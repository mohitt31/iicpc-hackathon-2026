// ============================================================================
//  fix_bot.cpp — FIX 4.4 load bot (Phase 3.4)
// ============================================================================
//  An HTTP-less, minimal FIX 4.4 client that carries the SAME order as the
//  binary/WS/REST bots over the protocol the brief actually names. Same
//  intended-send-time Coordinated-Omission logic, same HDR pipeline (naive +
//  CO-corrected), same --snapshot-dir CSV so the FIX board is fed real data.
//
//  Transport: TCP + standard FIX tag=value framing (SOH-delimited), with a real
//  BodyLength (tag 9) and CheckSum (tag 10). Messages used:
//    Logon (35=A) once, then NewOrderSingle (35=D) per order; the engine side
//    (fix_adapter) replies ExecutionReport (35=8) echoing ClOrdID (11=seq),
//    which is the correlation key. Synchronous one-in-flight (honest latency
//    baseline; FIX is expected to land between WS and REST on the chart).
//
//  This is a load bot, not a full FIX engine: no session recovery / resend /
//  sequence-reset handling — it Logs on, fires orders, reads exec reports.
// ============================================================================
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <chrono>

#include <unistd.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include <hdr/hdr_histogram.h>

namespace {
constexpr char SOH = '\x01';

static int64_t now_ns() {
    using namespace std::chrono;
    return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}

static int tcp_connect(const std::string& host, uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one=1; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    sockaddr_in addr{}; addr.sin_family=AF_INET; addr.sin_port=htons(port);
    hostent* he=gethostbyname(host.c_str());
    if(!he){ ::close(fd); return -1; }
    memcpy(&addr.sin_addr, he->h_addr, he->h_length);
    if(::connect(fd,(sockaddr*)&addr,sizeof(addr))<0){ ::close(fd); return -1; }
    return fd;
}

// Wrap a body (the fields from tag 35 onward, each already SOH-terminated) with
// 8=FIX.4.4, a correct BodyLength (9), and a correct CheckSum (10).
static std::string frame_fix(const std::string& body) {
    std::string hdr = std::string("8=FIX.4.4") + SOH + "9=" + std::to_string(body.size()) + SOH;
    std::string m = hdr + body;                       // checksum covers 8.. up to SOH before 10
    unsigned sum = 0; for (unsigned char c : m) sum += c;
    char cs[16]; snprintf(cs, sizeof(cs), "10=%03u%c", sum % 256, SOH);
    return m + cs;
}

static bool send_all(int fd, const std::string& s) {
    size_t off=0; while(off<s.size()){ ssize_t n=::send(fd,s.data()+off,s.size()-off,0); if(n<=0) return false; off+=n; }
    return true;
}

// Read one complete FIX message (from "8=FIX" to just after the "10=ddd<SOH>"
// trailer) out of a persistent buffer. Returns "" on socket error.
static std::string read_fix_msg(int fd, std::string& buf) {
    for(;;){
        size_t s = buf.find("8=FIX");
        if(s != std::string::npos){
            // checksum trailer = SOH '1' '0' '=' ddd SOH
            size_t t = buf.find(std::string(1,SOH)+"10=", s);
            if(t != std::string::npos && buf.size() >= t+1+3+3+1){
                size_t end = t + 1 + 3 /*10=*/ + 3 /*ddd*/ + 1 /*SOH*/;
                std::string msg = buf.substr(s, end-s);
                buf.erase(0, end);
                return msg;
            }
        }
        char tmp[4096]; ssize_t n=::recv(fd,tmp,sizeof(tmp),0);
        if(n<=0) return std::string();
        buf.append(tmp,(size_t)n);
    }
}

// Pull the value of tag (e.g. "35" or "11") out of a FIX message.
static std::string field(const std::string& msg, const char* tag) {
    std::string key = std::string(1,SOH) + tag + "=";
    size_t p = msg.find(key);
    if(p==std::string::npos){ // could be the very first field (no leading SOH)
        std::string k2 = std::string(tag)+"=";
        if(msg.compare(0,k2.size(),k2)==0) p=0; else return std::string();
    } else p += 1;            // skip the leading SOH
    size_t eq = msg.find('=', p); if(eq==std::string::npos) return std::string();
    size_t soh = msg.find(SOH, eq); if(soh==std::string::npos) return std::string();
    return msg.substr(eq+1, soh-eq-1);
}

} // namespace

int main(int argc, char** argv) {
    std::string host="127.0.0.1", snapdir; uint16_t port=8083;
    uint64_t interval_us=100, duration_sec=5;
    for(int i=1;i<argc;i++){ std::string a=argv[i];
        if(a=="--ip"&&i+1<argc) host=argv[++i];
        else if(a=="--port"&&i+1<argc) port=(uint16_t)atoi(argv[++i]);
        else if(a=="--interval-us"&&i+1<argc) interval_us=strtoull(argv[++i],0,10);
        else if(a=="--duration-sec"&&i+1<argc) duration_sec=strtoull(argv[++i],0,10);
        else if(a=="--snapshot-dir"&&i+1<argc) snapdir=argv[++i];
    }

    int fd=tcp_connect(host,port);
    if(fd<0){ fprintf(stderr,"[fix_bot] connect failed\n"); return 1; }

    // Logon (35=A): EncryptMethod=0, HeartBtInt=30.
    uint64_t msgseq=1;
    std::string logon = std::string("35=A")+SOH+"49=BOT"+SOH+"56=ENGINE"+SOH+
        "34="+std::to_string(msgseq++)+SOH+"98=0"+SOH+"108=30"+SOH;
    if(!send_all(fd, frame_fix(logon))){ fprintf(stderr,"[fix_bot] logon send failed\n"); return 1; }
    std::string rxbuf;
    read_fix_msg(fd, rxbuf);   // consume the Logon ack (best-effort)

    hdr_histogram *naive=nullptr,*co=nullptr;
    hdr_init(1,60000000000LL,3,&naive);
    hdr_init(1,60000000000LL,3,&co);

    const int64_t interval_ns=(int64_t)interval_us*1000;
    const int64_t t_end=now_ns()+(int64_t)duration_sec*1000000000LL;
    int64_t intended=now_ns();
    uint64_t seq=0, sent=0, acked=0;

    FILE* snap=nullptr; int64_t start_ns=now_ns(), last_snap=start_ns;
    if(!snapdir.empty()){
        std::string sp=snapdir+"/fix_bot.csv"; snap=fopen(sp.c_str(),"w");
        if(snap) fprintf(snap,"elapsed_sec,sent,acked,naive_p50,naive_p90,naive_p99,naive_p99_9,naive_p99_99,naive_max,co_p50,co_p90,co_p99,co_p99_9,co_p99_99,co_max\n");
    }
    auto write_snap=[&]{
        if(!snap) return;
        fprintf(snap,"%lld,%llu,%llu,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld\n",
            (long long)((now_ns()-start_ns)/1000000000LL),(unsigned long long)sent,(unsigned long long)acked,
            (long long)hdr_value_at_percentile(naive,50),(long long)hdr_value_at_percentile(naive,90),
            (long long)hdr_value_at_percentile(naive,99),(long long)hdr_value_at_percentile(naive,99.9),
            (long long)hdr_value_at_percentile(naive,99.99),(long long)hdr_value_at_percentile(naive,100),
            (long long)hdr_value_at_percentile(co,50),(long long)hdr_value_at_percentile(co,90),
            (long long)hdr_value_at_percentile(co,99),(long long)hdr_value_at_percentile(co,99.9),
            (long long)hdr_value_at_percentile(co,99.99),(long long)hdr_value_at_percentile(co,100));
        fflush(snap);
    };

    while(now_ns()<t_end){
        intended += interval_ns;
        while(now_ns()<intended){ /* busy-wait to intended */ }
        ++seq;
        // NewOrderSingle (35=D): 11=ClOrdID, 55=Symbol, 54=Side(1=buy),
        // 38=OrderQty, 40=OrdType(2=limit), 44=Price.
        std::string ord = std::string("35=D")+SOH+"49=BOT"+SOH+"56=ENGINE"+SOH+
            "34="+std::to_string(msgseq++)+SOH+"11="+std::to_string(seq)+SOH+
            "55=1"+SOH+"54=1"+SOH+"38=10"+SOH+"40=2"+SOH+"44=10000"+SOH;
        if(!send_all(fd, frame_fix(ord))) break;
        // read until an ExecutionReport (35=8) returns (skip heartbeats etc.)
        bool ok=false;
        for(int g=0; g<4; g++){
            std::string m=read_fix_msg(fd, rxbuf);
            if(m.empty()) break;
            if(field(m,"35")=="8"){ ok=true; break; }
        }
        int64_t latency=now_ns()-intended; if(latency<0) latency=0;
        hdr_record_value(naive,latency);
        hdr_record_corrected_value(co,latency,interval_ns);
        sent++; if(ok) acked++;
        if(snap && now_ns()-last_snap>=1000000000LL){ write_snap(); last_snap=now_ns(); }
        if(!ok){ ::close(fd); fd=tcp_connect(host,port); if(fd<0) break; rxbuf.clear(); }
    }
    write_snap(); if(snap) fclose(snap);

    printf("[fix_bot] proto=FIX sent=%llu acked=%llu  naive_p99=%lld ns  co_p99=%lld ns\n",
        (unsigned long long)sent,(unsigned long long)acked,
        (long long)hdr_value_at_percentile(naive,99.0),
        (long long)hdr_value_at_percentile(co,99.0));
    ::close(fd);
    return 0;
}
