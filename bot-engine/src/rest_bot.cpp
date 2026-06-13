// ============================================================================
//  rest_bot.cpp — REST/HTTP load bot (Phase 3.3)   ***DRAFT — UNVERIFIED***
// ============================================================================
//  ⚠️  STATUS: written but NEVER COMPILED OR RUN in the authoring environment.
//      Reviewed candidate, NOT a finished bot. Mergeable ONLY after it builds
//      green in CI (x86) AND a real run against a REST-capable engine produces
//      board data with Sent==Acked committed to verified_runs/. Until then the
//      "(Roadmap)" label on the REST board STAYS.
// ----------------------------------------------------------------------------
//  WHAT IT IS: an HTTP/1.1 keep-alive client that POSTs orders to /orders,
//  applies the SAME intended-send-time Coordinated-Omission logic and feeds the
//  SAME HDR pipeline (naive + CO-corrected) as the TCP and WS bots. Only the
//  transport + encoding differ: instead of the binary SBE frame, the order is a
//  small JSON body — a DOCUMENTED 1:1 mapping of the contract's NewOrder fields.
//
//  PEDAGOGICAL POINT (intended for the demo): REST is expected to be the SLOWEST
//  protocol (HTTP text framing + JSON parse on every order). The cross-protocol
//  chart binary << websocket << REST is the whole reason an HFT benchmark speaks
//  binary — this bot exists to make that gap measurable, not to win it.
//
//  DOCUMENTED JSON MAPPING (contracts/interface_contract_v1.h → JSON):
//    NewOrder.seq          -> "seq"        (uint64)
//    NewOrder.timestamp_ns -> "ts"         (uint64, ns; the intended-send time)
//    NewOrder.symbol_id    -> "symbol"     (uint32)
//    NewOrder.order_type   -> "type"       (0=LIMIT,1=MARKET)
//    NewOrder.side         -> "side"       (0=BUY,1=SELL)
//    NewOrder.price        -> "price"      (int64 ticks; 0 for MARKET)
//    NewOrder.quantity     -> "qty"        (uint64)
//  Engine REST contract (proposed; must match the engine's actual handler):
//    POST /orders  Content-Type: application/json   Body: {the object above}
//    200 OK  Body: { "order_seq": <echoed seq>, "status": "ACK"|"REJECT" }
//
//  KNOWN OPEN ITEMS for the verifier:
//   - Response parsing is minimal (status-line + body length via Content-Length
//     or read-until-close); chunked transfer-encoding is NOT handled.
//   - One in-flight request per connection in this draft (synchronous RTT). A
//     pipelined/parallel version is a follow-up; synchronous is the honest
//     baseline for a latency comparison anyway.
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

#include "contracts/interface_contract_v1.h"
#include <hdr/hdr_histogram.h>

namespace {

static int64_t now_ns() {
    using namespace std::chrono;
    return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}

static int http_connect(const std::string& host, uint16_t port) {
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

// Build the JSON body for one NewOrder (documented mapping above).
static int build_body(char* out, size_t cap, uint64_t seq, uint64_t ts) {
    return snprintf(out, cap,
        "{\"seq\":%llu,\"ts\":%llu,\"symbol\":1,\"type\":0,\"side\":0,\"price\":10000,\"qty\":10}",
        (unsigned long long)seq, (unsigned long long)ts);
}

// Send one keep-alive POST /orders and wait for the response head. Returns true
// on a 2xx. Minimal parser: reads until the status line + headers terminator,
// then drains Content-Length bytes if present.
static bool post_order(int fd, const std::string& host, uint16_t port,
                       const char* body, int blen) {
    char req[768];
    int rl = snprintf(req,sizeof(req),
        "POST /orders HTTP/1.1\r\nHost: %s:%u\r\nContent-Type: application/json\r\n"
        "Content-Length: %d\r\nConnection: keep-alive\r\n\r\n%.*s",
        host.c_str(), port, blen, blen, body);
    int off=0; while(off<rl){ ssize_t s=::send(fd,req+off,rl-off,0); if(s<=0) return false; off+=s; }

    char resp[1024]; int got=0;
    while(got<(int)sizeof(resp)-1){
        int r=::recv(fd,resp+got,sizeof(resp)-1-got,0);
        if(r<=0) return false;
        got+=r; resp[got]=0;
        if(strstr(resp,"\r\n\r\n")) break;   // got the head; good enough for RTT
    }
    // Accept 200/201/202.
    return strncmp(resp,"HTTP/1.1 2",10)==0;
}

} // namespace

// ============================================================================
//  main — single-thread draft; production reuses bot.cpp's launcher with this
//  transport. Flags mirror the other bots.
// ============================================================================
int main(int argc, char** argv) {
    std::string host="127.0.0.1", snapdir; uint16_t port=8082;
    uint64_t interval_us=100, duration_sec=5;
    for(int i=1;i<argc;i++){ std::string a=argv[i];
        if(a=="--ip"&&i+1<argc) host=argv[++i];
        else if(a=="--port"&&i+1<argc) port=(uint16_t)atoi(argv[++i]);
        else if(a=="--interval-us"&&i+1<argc) interval_us=strtoull(argv[++i],0,10);
        else if(a=="--duration-sec"&&i+1<argc) duration_sec=strtoull(argv[++i],0,10);
        else if(a=="--snapshot-dir"&&i+1<argc) snapdir=argv[++i];
    }

    int fd=http_connect(host,port);
    if(fd<0){ fprintf(stderr,"[rest_bot] connect failed\n"); return 1; }

    hdr_histogram *naive=nullptr,*co=nullptr;
    hdr_init(1,60000000000LL,3,&naive);
    hdr_init(1,60000000000LL,3,&co);

    const int64_t interval_ns=(int64_t)interval_us*1000;
    const int64_t t_end=now_ns()+(int64_t)duration_sec*1000000000LL;
    int64_t intended=now_ns();
    uint64_t seq=0, sent=0, acked=0;
    char body[256];

    // Optional per-second HDR snapshot CSV — same 15-column format the gateway
    // ingests, so the REST board shows REAL measured REST latency live.
    FILE* snap=nullptr; int64_t start_ns=now_ns(), last_snap=start_ns;
    if(!snapdir.empty()){
        std::string sp=snapdir+"/rest_bot.csv"; snap=fopen(sp.c_str(),"w");
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
        while(now_ns()<intended){ /* busy-wait to intended send time */ }
        int blen=build_body(body,sizeof(body),++seq,(uint64_t)intended);
        bool ok=post_order(fd,host,port,body,blen);
        int64_t latency=now_ns()-intended; if(latency<0) latency=0;
        hdr_record_value(naive,latency);
        hdr_record_corrected_value(co,latency,interval_ns);   // CO back-fill
        sent++; if(ok) acked++;
        if(snap && now_ns()-last_snap>=1000000000LL){ write_snap(); last_snap=now_ns(); }
        if(!ok){ /* draft: on failure, reconnect once */
            ::close(fd); fd=http_connect(host,port); if(fd<0) break;
        }
    }
    write_snap(); if(snap) fclose(snap);

    printf("[rest_bot DRAFT] proto=REST sent=%llu acked=%llu  naive_p99=%lld ns  co_p99=%lld ns\n",
        (unsigned long long)sent,(unsigned long long)acked,
        (long long)hdr_value_at_percentile(naive,99.0),
        (long long)hdr_value_at_percentile(co,99.0));
    fprintf(stderr,"[rest_bot] DRAFT — verify Sent==Acked and engine /orders contract before trusting numbers\n");
    ::close(fd);
    return 0;
}
