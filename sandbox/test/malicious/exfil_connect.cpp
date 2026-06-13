// MALICIOUS FIXTURE — outbound connection (data exfiltration attempt).
// `socket`/`connect` are NOT on the intake banned list, so this PASSES the
// static scan and builds. Expected: KILLED in the microVM — `socket` is not in
// the seccomp allowlist (default action SCMP_ACT_KILL), and the VM has no route
// out (network-none). Demonstrates the static scan is a heuristic, not the boundary.
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <cstring>
int main() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);   // not in seccomp allowlist → KILL
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(80);
    inet_pton(AF_INET, "1.1.1.1", &a.sin_addr);
    connect(fd, (sockaddr*)&a, sizeof(a));
    return 0;
}
