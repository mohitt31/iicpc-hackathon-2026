// MALICIOUS FIXTURE — the documented evasion: invoke socket via syscall(2) BY
// NUMBER so the source contains no "socket"/"connect" token for the static scan
// to grep. Expected: PASSES intake (scan defeated, exactly as intake.sh warns),
// then KILLED in the microVM by seccomp on the syscall number itself. This is
// the proof that the runtime seccomp profile — not the lint scan — is the boundary.
#include <sys/syscall.h>
#include <unistd.h>
int main() {
    // 41 = __NR_socket on x86_64. No banned token appears in this source.
    long fd = syscall(41, 2 /*AF_INET*/, 1 /*SOCK_STREAM*/, 0);
    (void)fd;
    return 0;
}
