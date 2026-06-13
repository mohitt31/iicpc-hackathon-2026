/*
 * seccomp_init.c — the guest PID 1 for the contestant microVM.
 *
 * This is the piece that makes the sandbox's seccomp boundary REAL instead of a
 * decorative JSON file. Until this existed, /etc/seccomp_profile.json was never
 * installed and nothing filtered the contestant's syscalls. Now the boot chain is:
 *
 *     kernel → /init (this) → mount /proc,/sys,/dev
 *                           → prctl(NO_NEW_PRIVS)
 *                           → install a seccomp-bpf filter (default = KILL)
 *                           → execve(/bin/contestant)
 *
 * The filter is inherited across the execve, so the contestant runs under it.
 *
 * DESIGN — why this allowlist (the important part):
 *   A contestant is a *matching engine*: it must accept inbound TCP connections
 *   from the bot fleet and speak the SBE wire. So it legitimately needs the
 *   server-side network syscalls (socket/bind/listen/accept), epoll, threads
 *   (clone), memory, timing, and read/write. A naive "only read/write/futex"
 *   allowlist would kill a *legitimate* engine on its first socket() call.
 *
 *   So the boundary is defence-in-depth, and seccomp is ONE layer:
 *     - seccomp KILLs the syscalls an engine never needs and an attacker does:
 *       connect (outbound exfil), fork/vfork, execve-of-others is neutered by a
 *       single-binary read-only rootfs, ptrace, mount, chroot, pivot_root,
 *       kexec, reboot, *module, setuid/setgid, bpf, etc. Default action = KILL,
 *       so anything not explicitly allowed dies with SIGSYS.
 *     - `connect` is deliberately NOT allowed: a server accepts, it never dials
 *       out. Omitting it kills data-exfil at the syscall layer (belt) on top of
 *       network-egress being blocked at the host (suspenders, see run_vm.sh).
 *     - no-new-privs blocks privilege escalation via setuid binaries.
 *     - read-only rootfs (run_vm.sh: is_read_only) blocks tampering; the rootfs
 *       holds one binary and no /etc/shadow, so credential theft has no target.
 *
 *   The malicious fixtures in sandbox/test/malicious/ map onto this exactly:
 *     forkbomb       → fork not allowed   → KILL (also rejected at intake)
 *     exfil_connect  → connect not allowed→ KILL (also no route out)
 *     read_shadow    → openat allowed but /etc/shadow absent + rootfs read-only → fails
 *     syscall_evasion→ raw socket-by-number still hits the filter by NUMBER → the
 *                      evasion that defeats the intake lint is futile here.
 *
 * BRING-UP: if a legitimate engine traps on a syscall we forgot, compile with
 *   -DSECCOMP_PERMISSIVE: the default action becomes "return ENOSYS" instead of
 *   KILL, and the offending syscall number is written to the console — run the
 *   engine once, read the missing numbers off ttyS0, add them below, then drop
 *   -DSECCOMP_PERMISSIVE for the real (KILL) build.
 *
 * No libc/libseccomp dependency assumptions beyond a static build — the raw BPF
 * is hand-rolled so the minimal rootfs needs nothing but this one binary.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stddef.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <linux/unistd.h>

extern char **environ;

#ifndef SECCOMP_RET_KILL_PROCESS
#define SECCOMP_RET_KILL_PROCESS 0x80000000U
#endif

/* default action for a disallowed syscall */
#ifdef SECCOMP_PERMISSIVE
#  define DEFAULT_ACTION (SECCOMP_RET_ERRNO | (ENOSYS & SECCOMP_RET_DATA))
#else
#  define DEFAULT_ACTION SECCOMP_RET_KILL_PROCESS
#endif

/* ALLOW(name): if the loaded syscall nr == __NR_name, return ALLOW; else fall
 * through to the next check. Two BPF instructions each. */
#define ALLOW(name) \
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_##name, 0, 1), \
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW)

static int install_filter(void) {
    struct sock_filter filter[] = {
        /* 1. reject any non-x86_64 personality outright */
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_X86_64, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),

        /* 2. load the syscall number */
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),

        /* 3. allowlist — what a TCP epoll matching engine legitimately needs */
        /* core I/O */
        ALLOW(read), ALLOW(write), ALLOW(readv), ALLOW(writev),
        ALLOW(close), ALLOW(lseek), ALLOW(pread64), ALLOW(pwrite64),
        /* server-side networking (NOTE: connect is intentionally absent) */
        ALLOW(socket), ALLOW(bind), ALLOW(listen), ALLOW(accept), ALLOW(accept4),
        ALLOW(setsockopt), ALLOW(getsockopt), ALLOW(getsockname),
        ALLOW(getpeername), ALLOW(shutdown), ALLOW(recvfrom), ALLOW(sendto),
        ALLOW(recvmsg), ALLOW(sendmsg),
        /* readiness / event loop */
        ALLOW(epoll_create1), ALLOW(epoll_ctl), ALLOW(epoll_wait),
        ALLOW(epoll_pwait), ALLOW(poll), ALLOW(ppoll), ALLOW(pselect6),
        ALLOW(eventfd2), ALLOW(timerfd_create), ALLOW(timerfd_settime),
        /* memory */
        ALLOW(mmap), ALLOW(munmap), ALLOW(mprotect), ALLOW(brk),
        ALLOW(madvise), ALLOW(mremap),
        /* threads + synchronisation (pthreads use clone, never fork) */
        ALLOW(clone), ALLOW(clone3), ALLOW(futex), ALLOW(set_robust_list),
        ALLOW(get_robust_list), ALLOW(rseq), ALLOW(sched_yield),
        ALLOW(sched_getaffinity), ALLOW(sched_setaffinity),
        /* signals */
        ALLOW(rt_sigaction), ALLOW(rt_sigprocmask), ALLOW(rt_sigreturn),
        ALLOW(sigaltstack),
        /* fd / file metadata (rootfs is read-only; openat damage is bounded) */
        ALLOW(fcntl), ALLOW(dup), ALLOW(dup2), ALLOW(dup3), ALLOW(pipe2),
        ALLOW(openat), ALLOW(fstat), ALLOW(newfstatat), ALLOW(statx),
        ALLOW(getdents64), ALLOW(uname), ALLOW(statfs), ALLOW(readlink), ALLOW(readlinkat), ALLOW(ioctl),
        /* timing */
        ALLOW(clock_gettime), ALLOW(clock_getres), ALLOW(clock_nanosleep),
        ALLOW(nanosleep), ALLOW(gettimeofday),
        /* process startup / identity / teardown */
        ALLOW(arch_prctl), ALLOW(set_tid_address), ALLOW(gettid),
        ALLOW(getpid), ALLOW(getppid), ALLOW(getrandom), ALLOW(prlimit64),
        ALLOW(getuid), ALLOW(geteuid), ALLOW(getgid), ALLOW(getegid),
        ALLOW(exit), ALLOW(exit_group),
        /* the single execve that launches the contestant (read-only single-
         * binary rootfs makes re-exec pointless) */
        ALLOW(execve),

        /* 4. default: kill (or, with -DSECCOMP_PERMISSIVE, ENOSYS) */
        BPF_STMT(BPF_RET | BPF_K, DEFAULT_ACTION),
    };

    struct sock_fprog prog = {
        .len = (unsigned short)(sizeof(filter) / sizeof(filter[0])),
        .filter = filter,
    };

    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
        perror("[init] PR_SET_NO_NEW_PRIVS");
        return -1;
    }
    /* prefer the seccomp() syscall; fall back to prctl() on older kernels */
    if (syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0, &prog) != 0) {
        if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog) != 0) {
            perror("[init] seccomp install");
            return -1;
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    (void)argc;

    /* minimal pseudo-filesystems the engine + libc expect */
    mount("proc", "/proc", "proc", 0, NULL);
    mount("sysfs", "/sys", "sysfs", 0, NULL);
    mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);

    fputs("[init] installing seccomp allowlist (default=KILL)\n", stderr);
    if (install_filter() != 0) {
        fputs("[init] FATAL: could not install seccomp filter; refusing to run "
              "the contestant unsandboxed\n", stderr);
        _exit(1);                 /* fail closed — never run without the boundary */
    }

    fputs("[init] sandbox active → exec /bin/contestant\n", stderr);
    char *const cargv[] = { "/bin/contestant", NULL };
    execve("/bin/contestant", cargv, environ);

    perror("[init] execve(/bin/contestant)");
    _exit(127);
}
