// MALICIOUS FIXTURE — reads /etc/shadow (credential theft attempt).
// `open`/`openat` are NOT in the seccomp allowlist. Expected: KILLED in the
// microVM the instant it calls open(); and /etc/shadow does not exist in the
// minimal contestant rootfs anyway. Passes the intake static scan.
#include <cstdio>
int main() {
    FILE* f = std::fopen("/etc/shadow", "r");    // openat → not in allowlist → KILL
    if (!f) return 1;
    char buf[256];
    while (std::fgets(buf, sizeof(buf), f)) std::fputs(buf, stdout);
    std::fclose(f);
    return 0;
}
