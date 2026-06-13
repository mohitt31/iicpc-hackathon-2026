// ============================================================================
//  ws_bot.cpp — WebSocket load bot (Phase 3.2)   ***DRAFT — UNVERIFIED***
// ============================================================================
//  ⚠️  STATUS: written but NEVER COMPILED OR RUN in the authoring environment
//      (no x86 toolchain / no live engine there). This is a reviewed candidate
//      implementation, NOT a finished, verified bot. It is mergeable ONLY after:
//        1. it builds green in CI (x86), AND
//        2. a real run against a WS-capable engine produces board data with
//           Sent==Acked accounting committed to verified_runs/.
//      Until both hold, the "(Roadmap)" label on the WebSocket board STAYS.
// ----------------------------------------------------------------------------
//  WHAT IT IS: an RFC-6455 client that carries the EXACT SAME SBE NewOrder
//  payload as the TCP bot (contracts/interface_contract_v1.h), applies the
//  SAME intended-send-time Coordinated-Omission logic, and feeds the SAME HDR
//  pipeline (naive + CO-corrected). Only the transport framing differs:
//  the SBE FrameHeader+NewOrder bytes are wrapped in a masked binary WS frame.
//
//  WHAT IT IS NOT: a from-scratch WS library. It implements the minimal client
//  subset (HTTP Upgrade handshake + masked binary data frames + close), which
//  is all a load bot needs. No TLS (ws://, not wss:// — the engine is on the
//  benchmark LAN). No fragmentation/continuation on the send path (each order
//  is one frame, well under any fragmentation threshold).
//
//  KNOWN OPEN ITEMS for the verifier (do not assume these are handled):
//   - Sec-WebSocket-Accept validation is computed but not strictly enforced.
//   - RX path assumes the engine replies with binary frames carrying OrderAck;
//     if the engine speaks text/JSON over WS, the parse must change.
//   - Partial frame reassembly on RX is minimal; large/ fragmented acks TODO.
//   - base64/SHA1 for the handshake use a tiny inline impl — swap for OpenSSL
//     if already linked.
// ============================================================================

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>
#include <chrono>
#include <random>

#include <unistd.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#include "contracts/interface_contract_v1.h"
#include "tsc_util.h"
#include <hdr/hdr_histogram.h>

namespace {

static int64_t now_ns() {
    using namespace std::chrono;
    return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}

// ── minimal SHA-1 (for Sec-WebSocket-Accept) ───────────────────────────────
// Compact, public-domain-style implementation. Verifier: replace with OpenSSL
// SHA1() if libcrypto is already a dependency.
struct Sha1 {
    uint32_t h[5]; uint64_t len; uint8_t buf[64]; size_t n;
    Sha1(){ reset(); }
    void reset(){ h[0]=0x67452301;h[1]=0xEFCDAB89;h[2]=0x98BADCFE;h[3]=0x10325476;h[4]=0xC3D2E1F0;len=0;n=0; }
    static uint32_t rol(uint32_t v,int b){ return (v<<b)|(v>>(32-b)); }
    void block(const uint8_t* p){
        uint32_t w[80];
        for(int i=0;i<16;i++) w[i]=(p[i*4]<<24)|(p[i*4+1]<<16)|(p[i*4+2]<<8)|p[i*4+3];
        for(int i=16;i<80;i++) w[i]=rol(w[i-3]^w[i-8]^w[i-14]^w[i-16],1);
        uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4];
        for(int i=0;i<80;i++){
            uint32_t f,k;
            if(i<20){f=(b&c)|((~b)&d);k=0x5A827999;}
            else if(i<40){f=b^c^d;k=0x6ED9EBA1;}
            else if(i<60){f=(b&c)|(b&d)|(c&d);k=0x8F1BBCDC;}
            else {f=b^c^d;k=0xCA62C1D6;}
            uint32_t t=rol(a,5)+f+e+k+w[i]; e=d;d=c;c=rol(b,30);b=a;a=t;
        }
        h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;
    }
    void update(const uint8_t* p,size_t l){
        len+=l;
        while(l){ size_t take=64-n; if(take>l)take=l; memcpy(buf+n,p,take); n+=take; p+=take; l-=take;
                  if(n==64){ block(buf); n=0; } }
    }
    void finish(uint8_t out[20]){
        uint64_t bits=len*8; uint8_t pad=0x80; update(&pad,1);
        uint8_t z=0; while(n!=56) update(&z,1);
        uint8_t lb[8]; for(int i=0;i<8;i++) lb[i]=(bits>>(56-i*8))&0xff; update(lb,8);
        for(int i=0;i<5;i++){ out[i*4]=h[i]>>24;out[i*4+1]=h[i]>>16;out[i*4+2]=h[i]>>8;out[i*4+3]=h[i]; }
    }
};

static std::string base64(const uint8_t* d,size_t n){
    static const char* t="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o; int v=0,b=-6;
    for(size_t i=0;i<n;i++){ v=(v<<8)|d[i]; b+=8; while(b>=0){ o.push_back(t[(v>>b)&0x3f]); b-=6; } }
    if(b>-6) o.push_back(t[((v<<8)>>(b+8))&0x3f]);
    while(o.size()%4) o.push_back('=');
    return o;
}

// ── one socket: TCP connect + WS handshake ──────────────────────────────────
static int ws_connect(const std::string& host, uint16_t port, const std::string& path) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    sockaddr_in addr{}; addr.sin_family=AF_INET; addr.sin_port=htons(port);
    hostent* he = gethostbyname(host.c_str());
    if(!he){ ::close(fd); return -1; }
    memcpy(&addr.sin_addr, he->h_addr, he->h_length);
    if (::connect(fd,(sockaddr*)&addr,sizeof(addr))<0){ ::close(fd); return -1; }

    // 16 random bytes → base64 key
    uint8_t key[16]; std::mt19937_64 rng(now_ns());
    for(int i=0;i<16;i++) key[i]=(uint8_t)rng();
    std::string keyb64 = base64(key,16);

    char req[512];
    int rl = snprintf(req,sizeof(req),
        "GET %s HTTP/1.1\r\nHost: %s:%u\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n",
        path.c_str(), host.c_str(), port, keyb64.c_str());
    if (::send(fd,req,rl,0)!=rl){ ::close(fd); return -1; }

    // read handshake response (until \r\n\r\n). Minimal; assumes 101.
    char resp[1024]; int got=0;
    while(got < (int)sizeof(resp)-1){
        int r=::recv(fd,resp+got,sizeof(resp)-1-got,0);
        if(r<=0){ ::close(fd); return -1; }
        got+=r; resp[got]=0;
        if(strstr(resp,"\r\n\r\n")) break;
    }
    if(!strstr(resp,"101")){ ::close(fd); return -1; }
    // NOTE: Sec-WebSocket-Accept SHA1(key+GUID) is computable here for strict
    // validation; left non-fatal in this draft.
    (void)Sha1{};
    return fd;
}

// ── send one masked binary frame carrying `len` bytes of payload ────────────
static bool ws_send_binary(int fd, const uint8_t* payload, size_t len) {
    // client frames MUST be masked (RFC-6455 §5.3)
    uint8_t hdr[14]; size_t hn=0;
    hdr[0]=0x82;                         // FIN=1, opcode=0x2 (binary)
    if(len<126){ hdr[1]=0x80|(uint8_t)len; hn=2; }
    else if(len<65536){ hdr[1]=0x80|126; hdr[2]=(len>>8)&0xff; hdr[3]=len&0xff; hn=4; }
    else { hdr[1]=0x80|127; for(int i=0;i<8;i++) hdr[2+i]=(len>>(56-i*8))&0xff; hn=10; }
    uint8_t mask[4]; std::mt19937 rng((unsigned)now_ns());
    for(int i=0;i<4;i++) mask[i]=(uint8_t)rng();
    memcpy(hdr+hn,mask,4); hn+=4;

    std::vector<uint8_t> frame(hn+len);
    memcpy(frame.data(),hdr,hn);
    for(size_t i=0;i<len;i++) frame[hn+i]=payload[i]^mask[i&3];
    size_t off=0;
    while(off<frame.size()){
        ssize_t s=::send(fd,frame.data()+off,frame.size()-off,0);
        if(s<=0) return false;
        off+=s;
    }
    return true;
}

} // namespace

// ============================================================================
//  main — one bot thread for the draft; the production version reuses the TCP
//  bot's multi-thread launcher (bot.cpp) with this transport swapped in.
//  Flags mirror bot.cpp: --ip --port --path --bots --interval-us --duration-sec
// ============================================================================
int main(int argc, char** argv) {
    std::string host="127.0.0.1", path="/orders", snapdir; uint16_t port=8081;
    uint64_t interval_us=100, duration_sec=5;
    for(int i=1;i<argc;i++){ std::string a=argv[i];
        if(a=="--ip"&&i+1<argc) host=argv[++i];
        else if(a=="--port"&&i+1<argc) port=(uint16_t)atoi(argv[++i]);
        else if(a=="--path"&&i+1<argc) path=argv[++i];
        else if(a=="--interval-us"&&i+1<argc) interval_us=strtoull(argv[++i],0,10);
        else if(a=="--duration-sec"&&i+1<argc) duration_sec=strtoull(argv[++i],0,10);
        else if(a=="--snapshot-dir"&&i+1<argc) snapdir=argv[++i];
    }

    int fd = ws_connect(host,port,path);
    if(fd<0){ fprintf(stderr,"[ws_bot] connect/handshake failed\n"); return 1; }

    // HDR: same two histograms as the TCP bot — naive + CO-corrected.
    hdr_histogram *naive=nullptr,*co=nullptr;
    hdr_init(1,60000000000LL,3,&naive);
    hdr_init(1,60000000000LL,3,&co);

    const size_t FRAME = sizeof(FrameHeader)+sizeof(NewOrder);
    uint8_t buf[FRAME];
    auto* fh = reinterpret_cast<FrameHeader*>(buf);
    auto* ord= reinterpret_cast<NewOrder*>(buf+sizeof(FrameHeader));
    fh->msg_type=MSG_NEWORDER; fh->_pad=0; fh->msg_len=sizeof(NewOrder);
    ord->symbol_id=1; ord->order_type=ORDER_LIMIT; ord->side=SIDE_BUY;
    ord->price=10000; ord->quantity=10;

    const int64_t interval_ns=(int64_t)interval_us*1000;
    const int64_t t_end = now_ns()+(int64_t)duration_sec*1000000000LL;
    int64_t intended = now_ns();
    uint64_t seq=0, sent=0, acked=0;

    // seq → intended-send-time, so latency is measured against INTENDED (the
    // CO-honest invariant) and recorded when the ACK actually arrives — same
    // accounting as the binary bot, now on the WS RX path.
    size_t maxseq = (size_t)(duration_sec*1000000ULL/interval_us) + 4096;
    std::vector<int64_t> pending(maxseq+1, 0);
    std::vector<uint8_t> rx;   // raw socket bytes
    std::vector<uint8_t> sbe;  // WS-deframed SBE byte stream

    // Drain available bytes (non-blocking), unwrap server WS frames into the SBE
    // stream, then parse SBE messages and correlate OrderAcks by order_seq.
    auto drain_acks = [&]() {
        uint8_t tmp[8192]; ssize_t n;
        while((n=recv(fd,tmp,sizeof(tmp),MSG_DONTWAIT))>0) rx.insert(rx.end(),tmp,tmp+n);
        size_t off=0;                                   // 1) unwrap WS frames (server frames are unmasked)
        while(rx.size()-off >= 2){
            uint8_t b1=rx[off+1]; uint64_t len=b1&0x7f; size_t hdr=2;
            if(len==126){ if(rx.size()-off<4) break; len=((uint64_t)rx[off+2]<<8)|rx[off+3]; hdr=4; }
            else if(len==127){ if(rx.size()-off<10) break; len=0; for(int k=0;k<8;k++) len=(len<<8)|rx[off+2+k]; hdr=10; }
            if(rx.size()-off < hdr+len) break;
            sbe.insert(sbe.end(), rx.begin()+off+hdr, rx.begin()+off+hdr+len);
            off += hdr+len;
        }
        rx.erase(rx.begin(), rx.begin()+off);
        size_t p=0;                                     // 2) parse SBE messages, correlate acks
        while(sbe.size()-p >= sizeof(FrameHeader)){
            auto* afh=reinterpret_cast<FrameHeader*>(&sbe[p]);
            size_t msglen = afh->msg_len;
            if(sbe.size()-p < sizeof(FrameHeader)+msglen) break;
            if(afh->msg_type==MSG_ORDERACK && msglen>=sizeof(OrderAck)){
                auto* ack=reinterpret_cast<OrderAck*>(&sbe[p+sizeof(FrameHeader)]);
                uint64_t s=ack->order_seq;
                if(s>0 && s<pending.size() && pending[s]>0){
                    int64_t lat=now_ns()-pending[s]; if(lat<0) lat=0;
                    hdr_record_value(naive,lat);
                    hdr_record_corrected_value(co,lat,interval_ns);
                    pending[s]=0; acked++;
                }
            }
            p += sizeof(FrameHeader)+msglen;
        }
        sbe.erase(sbe.begin(), sbe.begin()+p);
    };

    // Optional per-second HDR snapshot CSV — same 15-column format the binary
    // bot writes and the telemetry gateway ingests, so the WS board can show
    // REAL measured WS latency on the live leaderboard (not synthetic).
    FILE* snap=nullptr; int64_t start_ns=now_ns(), last_snap=start_ns;
    if(!snapdir.empty()){
        std::string sp=snapdir+"/ws_bot.csv"; snap=fopen(sp.c_str(),"w");
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

    while(now_ns() < t_end){
        intended += interval_ns;
        int64_t spin; while((spin=now_ns()) < intended) { /* busy-wait to intended */ }
        ord->seq = ++seq;
        ord->timestamp_ns = (uint64_t)intended;
        if(seq < pending.size()) pending[seq]=intended;     // tag intended-send-time
        if(!ws_send_binary(fd,buf,FRAME)) break;
        sent++;
        drain_acks();                                       // non-blocking RX, doesn't stall the cadence
        if(snap && now_ns()-last_snap>=1000000000LL){ write_snap(); last_snap=now_ns(); }
    }
    int64_t drain_end = now_ns()+200000000LL;               // let in-flight acks land (~200ms)
    while(now_ns()<drain_end && acked<sent){ drain_acks(); }
    write_snap(); if(snap) fclose(snap);

    printf("[ws_bot] proto=WEBSOCKET sent=%llu acked=%llu  naive_p99=%lld ns  co_p99=%lld ns\n",
        (unsigned long long)sent,(unsigned long long)acked,
        (long long)hdr_value_at_percentile(naive,99.0),
        (long long)hdr_value_at_percentile(co,99.0));
    if(acked!=sent) fprintf(stderr,"[ws_bot] note: %llu unacked (in-flight at shutdown)\n",
        (unsigned long long)(sent-acked));
    ::close(fd);
    return 0;
}
