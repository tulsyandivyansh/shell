#include "include/trace.hpp"

#include "include/shell_utils.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace shell {
namespace {


struct SyscallEntryInfo {
    std::uint64_t nr;
    std::uint64_t args[6];
};

struct SyscallExitInfo {
    std::int64_t rval;
    std::uint8_t is_error;
};

struct SyscallSeccompInfo {
    std::uint64_t nr;
    std::uint64_t args[6];
    std::uint32_t ret_data;
};

struct KernelSyscallInfo {
    std::uint8_t op;
    std::uint8_t pad[3];
    std::uint32_t arch;
    std::uint64_t instruction_pointer;
    std::uint64_t stack_pointer;
    union {
        SyscallEntryInfo entry;
        SyscallExitInfo exit;
        SyscallSeccompInfo seccomp;
    };
};

struct SyscallEntry {
    long number{-1};
    std::array<unsigned long long, 6> args{};
    std::string rendered_args;
};

struct SyscallStats {
    unsigned long long calls{0};
    unsigned long long errors{0};
};

std::string syscall_name(long nr) {
    switch (nr) {
#ifdef SYS_read
        case SYS_read: return "read";
#endif
#ifdef SYS_write
        case SYS_write: return "write";
#endif
#ifdef SYS_open
        case SYS_open: return "open";
#endif
#ifdef SYS_close
        case SYS_close: return "close";
#endif
#ifdef SYS_stat
        case SYS_stat: return "stat";
#endif
#ifdef SYS_fstat
        case SYS_fstat: return "fstat";
#endif
#ifdef SYS_newfstatat
        case SYS_newfstatat: return "newfstatat";
#endif
#ifdef SYS_lstat
        case SYS_lstat: return "lstat";
#endif
#ifdef SYS_poll
        case SYS_poll: return "poll";
#endif
#ifdef SYS_lseek
        case SYS_lseek: return "lseek";
#endif
#ifdef SYS_mmap
        case SYS_mmap: return "mmap";
#endif
#ifdef SYS_mprotect
        case SYS_mprotect: return "mprotect";
#endif
#ifdef SYS_munmap
        case SYS_munmap: return "munmap";
#endif
#ifdef SYS_brk
        case SYS_brk: return "brk";
#endif
#ifdef SYS_rt_sigaction
        case SYS_rt_sigaction: return "rt_sigaction";
#endif
#ifdef SYS_rt_sigprocmask
        case SYS_rt_sigprocmask: return "rt_sigprocmask";
#endif
#ifdef SYS_rt_sigreturn
        case SYS_rt_sigreturn: return "rt_sigreturn";
#endif
#ifdef SYS_ioctl
        case SYS_ioctl: return "ioctl";
#endif
#ifdef SYS_pread64
        case SYS_pread64: return "pread64";
#endif
#ifdef SYS_pwrite64
        case SYS_pwrite64: return "pwrite64";
#endif
#ifdef SYS_readv
        case SYS_readv: return "readv";
#endif
#ifdef SYS_writev
        case SYS_writev: return "writev";
#endif
#ifdef SYS_access
        case SYS_access: return "access";
#endif
#ifdef SYS_pipe
        case SYS_pipe: return "pipe";
#endif
#ifdef SYS_select
        case SYS_select: return "select";
#endif
#ifdef SYS_sched_yield
        case SYS_sched_yield: return "sched_yield";
#endif
#ifdef SYS_mremap
        case SYS_mremap: return "mremap";
#endif
#ifdef SYS_msync
        case SYS_msync: return "msync";
#endif
#ifdef SYS_mincore
        case SYS_mincore: return "mincore";
#endif
#ifdef SYS_madvise
        case SYS_madvise: return "madvise";
#endif
#ifdef SYS_shmget
        case SYS_shmget: return "shmget";
#endif
#ifdef SYS_shmat
        case SYS_shmat: return "shmat";
#endif
#ifdef SYS_shmctl
        case SYS_shmctl: return "shmctl";
#endif
#ifdef SYS_dup
        case SYS_dup: return "dup";
#endif
#ifdef SYS_dup2
        case SYS_dup2: return "dup2";
#endif
#ifdef SYS_pause
        case SYS_pause: return "pause";
#endif
#ifdef SYS_nanosleep
        case SYS_nanosleep: return "nanosleep";
#endif
#ifdef SYS_getitimer
        case SYS_getitimer: return "getitimer";
#endif
#ifdef SYS_alarm
        case SYS_alarm: return "alarm";
#endif
#ifdef SYS_setitimer
        case SYS_setitimer: return "setitimer";
#endif
#ifdef SYS_getpid
        case SYS_getpid: return "getpid";
#endif
#ifdef SYS_sendfile
        case SYS_sendfile: return "sendfile";
#endif
#ifdef SYS_socket
        case SYS_socket: return "socket";
#endif
#ifdef SYS_connect
        case SYS_connect: return "connect";
#endif
#ifdef SYS_accept
        case SYS_accept: return "accept";
#endif
#ifdef SYS_sendto
        case SYS_sendto: return "sendto";
#endif
#ifdef SYS_recvfrom
        case SYS_recvfrom: return "recvfrom";
#endif
#ifdef SYS_sendmsg
        case SYS_sendmsg: return "sendmsg";
#endif
#ifdef SYS_recvmsg
        case SYS_recvmsg: return "recvmsg";
#endif
#ifdef SYS_shutdown
        case SYS_shutdown: return "shutdown";
#endif
#ifdef SYS_bind
        case SYS_bind: return "bind";
#endif
#ifdef SYS_listen
        case SYS_listen: return "listen";
#endif
#ifdef SYS_getsockname
        case SYS_getsockname: return "getsockname";
#endif
#ifdef SYS_getpeername
        case SYS_getpeername: return "getpeername";
#endif
#ifdef SYS_socketpair
        case SYS_socketpair: return "socketpair";
#endif
#ifdef SYS_setsockopt
        case SYS_setsockopt: return "setsockopt";
#endif
#ifdef SYS_getsockopt
        case SYS_getsockopt: return "getsockopt";
#endif
#ifdef SYS_clone
        case SYS_clone: return "clone";
#endif
#ifdef SYS_fork
        case SYS_fork: return "fork";
#endif
#ifdef SYS_vfork
        case SYS_vfork: return "vfork";
#endif
#ifdef SYS_execve
        case SYS_execve: return "execve";
#endif
#ifdef SYS_exit
        case SYS_exit: return "exit";
#endif
#ifdef SYS_wait4
        case SYS_wait4: return "wait4";
#endif
#ifdef SYS_kill
        case SYS_kill: return "kill";
#endif
#ifdef SYS_uname
        case SYS_uname: return "uname";
#endif
#ifdef SYS_fcntl
        case SYS_fcntl: return "fcntl";
#endif
#ifdef SYS_fsync
        case SYS_fsync: return "fsync";
#endif
#ifdef SYS_fdatasync
        case SYS_fdatasync: return "fdatasync";
#endif
#ifdef SYS_truncate
        case SYS_truncate: return "truncate";
#endif
#ifdef SYS_ftruncate
        case SYS_ftruncate: return "ftruncate";
#endif
#ifdef SYS_getdents
        case SYS_getdents: return "getdents";
#endif
#ifdef SYS_getcwd
        case SYS_getcwd: return "getcwd";
#endif
#ifdef SYS_chdir
        case SYS_chdir: return "chdir";
#endif
#ifdef SYS_fchdir
        case SYS_fchdir: return "fchdir";
#endif
#ifdef SYS_rename
        case SYS_rename: return "rename";
#endif
#ifdef SYS_mkdir
        case SYS_mkdir: return "mkdir";
#endif
#ifdef SYS_rmdir
        case SYS_rmdir: return "rmdir";
#endif
#ifdef SYS_creat
        case SYS_creat: return "creat";
#endif
#ifdef SYS_link
        case SYS_link: return "link";
#endif
#ifdef SYS_unlink
        case SYS_unlink: return "unlink";
#endif
#ifdef SYS_symlink
        case SYS_symlink: return "symlink";
#endif
#ifdef SYS_readlink
        case SYS_readlink: return "readlink";
#endif
#ifdef SYS_chmod
        case SYS_chmod: return "chmod";
#endif
#ifdef SYS_fchmod
        case SYS_fchmod: return "fchmod";
#endif
#ifdef SYS_chown
        case SYS_chown: return "chown";
#endif
#ifdef SYS_fchown
        case SYS_fchown: return "fchown";
#endif
#ifdef SYS_lchown
        case SYS_lchown: return "lchown";
#endif
#ifdef SYS_umask
        case SYS_umask: return "umask";
#endif
#ifdef SYS_gettimeofday
        case SYS_gettimeofday: return "gettimeofday";
#endif
#ifdef SYS_getrlimit
        case SYS_getrlimit: return "getrlimit";
#endif
#ifdef SYS_getrusage
        case SYS_getrusage: return "getrusage";
#endif
#ifdef SYS_sysinfo
        case SYS_sysinfo: return "sysinfo";
#endif
#ifdef SYS_times
        case SYS_times: return "times";
#endif
#ifdef SYS_ptrace
        case SYS_ptrace: return "ptrace";
#endif
#ifdef SYS_getuid
        case SYS_getuid: return "getuid";
#endif
#ifdef SYS_syslog
        case SYS_syslog: return "syslog";
#endif
#ifdef SYS_getgid
        case SYS_getgid: return "getgid";
#endif
#ifdef SYS_setuid
        case SYS_setuid: return "setuid";
#endif
#ifdef SYS_setgid
        case SYS_setgid: return "setgid";
#endif
#ifdef SYS_geteuid
        case SYS_geteuid: return "geteuid";
#endif
#ifdef SYS_getegid
        case SYS_getegid: return "getegid";
#endif
#ifdef SYS_setpgid
        case SYS_setpgid: return "setpgid";
#endif
#ifdef SYS_getppid
        case SYS_getppid: return "getppid";
#endif
#ifdef SYS_getpgrp
        case SYS_getpgrp: return "getpgrp";
#endif
#ifdef SYS_setsid
        case SYS_setsid: return "setsid";
#endif
#ifdef SYS_getsid
        case SYS_getsid: return "getsid";
#endif
#ifdef SYS_capget
        case SYS_capget: return "capget";
#endif
#ifdef SYS_capset
        case SYS_capset: return "capset";
#endif
#ifdef SYS_arch_prctl
        case SYS_arch_prctl: return "arch_prctl";
#endif
#ifdef SYS_setrlimit
        case SYS_setrlimit: return "setrlimit";
#endif
#ifdef SYS_chroot
        case SYS_chroot: return "chroot";
#endif
#ifdef SYS_sync
        case SYS_sync: return "sync";
#endif
#ifdef SYS_mount
        case SYS_mount: return "mount";
#endif
#ifdef SYS_umount2
        case SYS_umount2: return "umount2";
#endif
#ifdef SYS_gettid
        case SYS_gettid: return "gettid";
#endif
#ifdef SYS_futex
        case SYS_futex: return "futex";
#endif
#ifdef SYS_sched_setaffinity
        case SYS_sched_setaffinity: return "sched_setaffinity";
#endif
#ifdef SYS_sched_getaffinity
        case SYS_sched_getaffinity: return "sched_getaffinity";
#endif
#ifdef SYS_exit_group
        case SYS_exit_group: return "exit_group";
#endif
#ifdef SYS_epoll_wait
        case SYS_epoll_wait: return "epoll_wait";
#endif
#ifdef SYS_epoll_ctl
        case SYS_epoll_ctl: return "epoll_ctl";
#endif
#ifdef SYS_tgkill
        case SYS_tgkill: return "tgkill";
#endif
#ifdef SYS_openat
        case SYS_openat: return "openat";
#endif
#ifdef SYS_mkdirat
        case SYS_mkdirat: return "mkdirat";
#endif
#ifdef SYS_unlinkat
        case SYS_unlinkat: return "unlinkat";
#endif
#ifdef SYS_renameat
        case SYS_renameat: return "renameat";
#endif
#ifdef SYS_readlinkat
        case SYS_readlinkat: return "readlinkat";
#endif
#ifdef SYS_fchmodat
        case SYS_fchmodat: return "fchmodat";
#endif
#ifdef SYS_faccessat
        case SYS_faccessat: return "faccessat";
#endif
#ifdef SYS_pselect6
        case SYS_pselect6: return "pselect6";
#endif
#ifdef SYS_ppoll
        case SYS_ppoll: return "ppoll";
#endif
#ifdef SYS_unshare
        case SYS_unshare: return "unshare";
#endif
#ifdef SYS_set_robust_list
        case SYS_set_robust_list: return "set_robust_list";
#endif
#ifdef SYS_set_tid_address
        case SYS_set_tid_address: return "set_tid_address";
#endif
#ifdef SYS_get_robust_list
        case SYS_get_robust_list: return "get_robust_list";
#endif
#ifdef SYS_splice
        case SYS_splice: return "splice";
#endif
#ifdef SYS_tee
        case SYS_tee: return "tee";
#endif
#ifdef SYS_sync_file_range
        case SYS_sync_file_range: return "sync_file_range";
#endif
#ifdef SYS_vmsplice
        case SYS_vmsplice: return "vmsplice";
#endif
#ifdef SYS_utimensat
        case SYS_utimensat: return "utimensat";
#endif
#ifdef SYS_epoll_pwait
        case SYS_epoll_pwait: return "epoll_pwait";
#endif
#ifdef SYS_timerfd_create
        case SYS_timerfd_create: return "timerfd_create";
#endif
#ifdef SYS_eventfd
        case SYS_eventfd: return "eventfd";
#endif
#ifdef SYS_fallocate
        case SYS_fallocate: return "fallocate";
#endif
#ifdef SYS_timerfd_settime
        case SYS_timerfd_settime: return "timerfd_settime";
#endif
#ifdef SYS_timerfd_gettime
        case SYS_timerfd_gettime: return "timerfd_gettime";
#endif
#ifdef SYS_accept4
        case SYS_accept4: return "accept4";
#endif
#ifdef SYS_eventfd2
        case SYS_eventfd2: return "eventfd2";
#endif
#ifdef SYS_epoll_create1
        case SYS_epoll_create1: return "epoll_create1";
#endif
#ifdef SYS_dup3
        case SYS_dup3: return "dup3";
#endif
#ifdef SYS_pipe2
        case SYS_pipe2: return "pipe2";
#endif
#ifdef SYS_inotify_init1
        case SYS_inotify_init1: return "inotify_init1";
#endif
#ifdef SYS_preadv
        case SYS_preadv: return "preadv";
#endif
#ifdef SYS_pwritev
        case SYS_pwritev: return "pwritev";
#endif
#ifdef SYS_prlimit64
        case SYS_prlimit64: return "prlimit64";
#endif
#ifdef SYS_getrandom
        case SYS_getrandom: return "getrandom";
#endif
#ifdef SYS_memfd_create
        case SYS_memfd_create: return "memfd_create";
#endif
#ifdef SYS_execveat
        case SYS_execveat: return "execveat";
#endif
#ifdef SYS_statx
        case SYS_statx: return "statx";
#endif
#ifdef SYS_rseq
        case SYS_rseq: return "rseq";
#endif
#ifdef SYS_clone3
        case SYS_clone3: return "clone3";
#endif
#ifdef SYS_close_range
        case SYS_close_range: return "close_range";
#endif
#ifdef SYS_openat2
        case SYS_openat2: return "openat2";
#endif
#ifdef SYS_faccessat2
        case SYS_faccessat2: return "faccessat2";
#endif
        default: return "syscall_" + std::to_string(nr);
    }
}

std::string read_tracee_string(pid_t pid, unsigned long long address, std::size_t limit = 160) {
    if (address == 0) {
        return "NULL";
    }

    std::string value;
    value.reserve(std::min<std::size_t>(limit, 64));
    while (value.size() < limit) {
        errno = 0;
        const long word = ::ptrace(PTRACE_PEEKDATA, pid,
                                   reinterpret_cast<void*>(static_cast<uintptr_t>(address + value.size())),
                                   nullptr);
        if (word == -1 && errno != 0) {
            return {};
        }

        const char* bytes = reinterpret_cast<const char*>(&word);
        for (std::size_t i = 0; i < sizeof(long) && value.size() < limit; ++i) {
            if (bytes[i] == '\0') {
                return value;
            }
            const unsigned char c = static_cast<unsigned char>(bytes[i]);
            if (c == '\n') value += "\\n";
            else if (c == '\r') value += "\\r";
            else if (c == '\t') value += "\\t";
            else if (c == '"') value += "\\\"";
            else if (c == '\\') value += "\\\\";
            else if (c >= 0x20 && c < 0x7f) value.push_back(static_cast<char>(c));
            else value += '?';
        }
    }
    value += "...";
    return value;
}

std::string hex_arg(unsigned long long value) {
    std::ostringstream out;
    out << "0x" << std::hex << value;
    return out.str();
}

std::string quoted_tracee_string(pid_t pid, unsigned long long address) {
    const std::string value = read_tracee_string(pid, address);
    if (value.empty() && address != 0) {
        return hex_arg(address);
    }
    if (address == 0) {
        return "NULL";
    }
    return "\"" + value + "\"";
}

std::string format_arguments(pid_t pid, const SyscallEntry& call) {
    const auto& a = call.args;
    std::ostringstream out;

    switch (call.number) {
#ifdef SYS_execve
        case SYS_execve:
            out << quoted_tracee_string(pid, a[0]) << ", " << hex_arg(a[1]) << ", " << hex_arg(a[2]);
            return out.str();
#endif
#ifdef SYS_open
        case SYS_open:
            out << quoted_tracee_string(pid, a[0]) << ", " << hex_arg(a[1]) << ", " << hex_arg(a[2]);
            return out.str();
#endif
#ifdef SYS_openat
        case SYS_openat:
            out << static_cast<int>(a[0]) << ", " << quoted_tracee_string(pid, a[1])
                << ", " << hex_arg(a[2]) << ", " << hex_arg(a[3]);
            return out.str();
#endif
#ifdef SYS_access
        case SYS_access:
            out << quoted_tracee_string(pid, a[0]) << ", " << hex_arg(a[1]);
            return out.str();
#endif
#ifdef SYS_chdir
        case SYS_chdir:
            out << quoted_tracee_string(pid, a[0]);
            return out.str();
#endif
#ifdef SYS_unlink
        case SYS_unlink:
            out << quoted_tracee_string(pid, a[0]);
            return out.str();
#endif
#ifdef SYS_mkdir
        case SYS_mkdir:
            out << quoted_tracee_string(pid, a[0]) << ", " << hex_arg(a[1]);
            return out.str();
#endif
#ifdef SYS_readlink
        case SYS_readlink:
            out << quoted_tracee_string(pid, a[0]) << ", " << hex_arg(a[1]) << ", " << a[2];
            return out.str();
#endif
#ifdef SYS_newfstatat
        case SYS_newfstatat:
            out << static_cast<int>(a[0]) << ", " << quoted_tracee_string(pid, a[1])
                << ", " << hex_arg(a[2]) << ", " << hex_arg(a[3]);
            return out.str();
#endif
#ifdef SYS_write
        case SYS_write:
            out << a[0] << ", " << hex_arg(a[1]) << ", " << a[2];
            return out.str();
#endif
#ifdef SYS_read
        case SYS_read:
            out << a[0] << ", " << hex_arg(a[1]) << ", " << a[2];
            return out.str();
#endif
#ifdef SYS_close
        case SYS_close:
            out << a[0];
            return out.str();
#endif
        default:
            for (std::size_t i = 0; i < a.size(); ++i) {
                if (i) out << ", ";
                out << hex_arg(a[i]);
            }
            return out.str();
    }
}

bool is_error_return(long long value) {
    return value < 0 && value >= -4095;
}

std::string format_return(long long value) {
    if (!is_error_return(value)) {
        return std::to_string(value);
    }
    const int err = static_cast<int>(-value);
    return "-1 " + std::string(std::strerror(err)) + " (errno " + std::to_string(err) + ")";
}

void render_summary(int fd, const std::map<long, SyscallStats>& stats) {
    struct Row {
        long nr;
        SyscallStats stats;
    };
    std::vector<Row> rows;
    rows.reserve(stats.size());
    unsigned long long total_calls = 0;
    unsigned long long total_errors = 0;
    for (const auto& [nr, s] : stats) {
        rows.push_back(Row{nr, s});
        total_calls += s.calls;
        total_errors += s.errors;
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
        if (a.stats.calls != b.stats.calls) return a.stats.calls > b.stats.calls;
        return syscall_name(a.nr) < syscall_name(b.nr);
    });

    std::ostringstream out;
    out << "Syscall summary\n"
        << "------------------------------\n"
        << std::left << std::setw(22) << "syscall"
        << std::right << std::setw(8) << "calls"
        << std::setw(8) << "errors" << "\n";
    for (const Row& row : rows) {
        out << std::left << std::setw(22) << syscall_name(row.nr)
            << std::right << std::setw(8) << row.stats.calls
            << std::setw(8) << row.stats.errors << "\n";
    }
    out << "------------------------------\n"
        << std::left << std::setw(22) << "total"
        << std::right << std::setw(8) << total_calls
        << std::setw(8) << total_errors << "\n";
    write_all(fd, out.str());
}

[[noreturn]] void exec_tracee(const std::vector<std::string>& args, int error_fd) {
    if (::ptrace(PTRACE_TRACEME, 0, nullptr, nullptr) == -1) {
        write_all(error_fd, std::string("trace: PTRACE_TRACEME: ") + std::strerror(errno) + "\n");
        ::_exit(1);
    }

    if (::raise(SIGSTOP) != 0) {
        write_all(error_fd, "trace: failed to stop tracee before exec\n");
        ::_exit(1);
    }

    const std::string executable = find_executable(args.front());
    if (executable.empty()) {
        write_all(error_fd, args.front() + ": command not found\n");
        ::_exit(127);
    }

    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& arg : args) {
        argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);

    ::execv(executable.c_str(), argv.data());
    write_all(error_fd, "trace: exec " + args.front() + ": " + std::strerror(errno) + "\n");
    ::_exit(126);
}

int wait_status_to_exit_code(int status) {
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
}

int trace_process(const TraceConfig& cfg, int error_fd) {
    const pid_t child = ::fork();
    if (child == -1) {
        write_all(error_fd, std::string("trace: fork: ") + std::strerror(errno) + "\n");
        return 1;
    }
    if (child == 0) {
        exec_tracee(cfg.program_args, error_fd);
    }

    int status = 0;
    while (::waitpid(child, &status, 0) == -1) {
        if (errno == EINTR) continue;
        write_all(error_fd, std::string("trace: waitpid: ") + std::strerror(errno) + "\n");
        return 1;
    }

    if (!WIFSTOPPED(status)) {
        return wait_status_to_exit_code(status);
    }

    const unsigned long options = PTRACE_O_TRACESYSGOOD | PTRACE_O_EXITKILL;
    if (::ptrace(PTRACE_SETOPTIONS, child, nullptr, reinterpret_cast<void*>(options)) == -1) {
        write_all(error_fd, std::string("trace: PTRACE_SETOPTIONS: ") + std::strerror(errno) + "\n");
        (void)::kill(child, SIGKILL);
        (void)::waitpid(child, nullptr, 0);
        return 1;
    }

    std::optional<SyscallEntry> current;
    std::map<long, SyscallStats> stats;
    int signal_to_deliver = 0;

    while (true) {
        if (::ptrace(PTRACE_SYSCALL, child, nullptr,
                     reinterpret_cast<void*>(static_cast<intptr_t>(signal_to_deliver))) == -1) {
            if (errno == ESRCH) break;
            write_all(error_fd, std::string("trace: PTRACE_SYSCALL: ") + std::strerror(errno) + "\n");
            return 1;
        }
        signal_to_deliver = 0;

        while (::waitpid(child, &status, 0) == -1) {
            if (errno == EINTR) continue;
            write_all(error_fd, std::string("trace: waitpid: ") + std::strerror(errno) + "\n");
            return 1;
        }

        if (WIFEXITED(status) || WIFSIGNALED(status)) {
            if (cfg.summary) render_summary(error_fd, stats);
            return wait_status_to_exit_code(status);
        }
        if (!WIFSTOPPED(status)) {
            continue;
        }

        const int sig = WSTOPSIG(status);
        if (sig == (SIGTRAP | 0x80)) {
            KernelSyscallInfo info{};
            errno = 0;
            const long got = ::ptrace(PTRACE_GET_SYSCALL_INFO, child, sizeof(info), &info);
            if (got == -1) {
                write_all(error_fd,
                          std::string("trace: PTRACE_GET_SYSCALL_INFO: ") + std::strerror(errno) + "\n");
                return 1;
            }

            if (info.op == PTRACE_SYSCALL_INFO_ENTRY) {
                SyscallEntry entry;
                entry.number = static_cast<long>(info.entry.nr);
                for (std::size_t i = 0; i < entry.args.size(); ++i) {
                    entry.args[i] = info.entry.args[i];
                }
                entry.rendered_args = format_arguments(child, entry);
                current = std::move(entry);
            } else if (info.op == PTRACE_SYSCALL_INFO_EXIT && current.has_value()) {
                const long nr = current->number;
                const long long rval = static_cast<long long>(info.exit.rval);
                SyscallStats& s = stats[nr];
                ++s.calls;
                if (is_error_return(rval)) ++s.errors;

                if (!cfg.summary) {
                    const std::string line = syscall_name(nr) + "(" +
                                             current->rendered_args + ") = " +
                                             format_return(rval) + "\n";
                    write_all(error_fd, line);
                }
                current.reset();
            }
            continue;
        }

        // A plain SIGTRAP is generated around exec and by ptrace events. Do
        // not inject that synthetic tracer signal into the tracee.
        if (sig == SIGTRAP || sig == SIGSTOP) {
            continue;
        }

        // Real signals seen by the tracee are delivered on the next resume.
        signal_to_deliver = sig;
    }

    if (cfg.summary) render_summary(error_fd, stats);
    return 1;
}

}  // namespace

TraceParseResult parse_trace(const Command& command) {
    TraceParseResult result;
    if (command.args.empty() || command.args.front() != "trace") {
        result.error = "trace: internal parse error";
        return result;
    }

    bool separator = false;
    for (std::size_t i = 1; i < command.args.size(); ++i) {
        const std::string& arg = command.args[i];
        if (!separator) {
            if (arg == "--help" || arg == "-h") {
                result.config.show_help = true;
                continue;
            }
            if (arg == "--summary" || arg == "-c") {
                result.config.summary = true;
                continue;
            }
            if (arg == "--") {
                separator = true;
                continue;
            }
            result.error = "trace: unknown option or missing '--': " + arg;
            return result;
        }
        result.config.program_args.push_back(arg);
    }

    if (result.config.show_help && result.config.program_args.empty()) {
        return result;
    }
    if (!separator) {
        result.error = "trace: expected '--' before program";
        return result;
    }
    if (result.config.program_args.empty()) {
        result.error = "trace: missing program after '--'";
    }
    return result;
}

std::string trace_usage() {
    return
        "usage: trace [--summary|-c] -- PROGRAM [ARGS...]\n"
        "       trace --help\n"
        "\n"
        "Trace PROGRAM system calls with Linux ptrace. Live traces are written\n"
        "to stderr so PROGRAM stdout remains usable in pipelines. --summary\n"
        "prints per-syscall call/error counts when the program exits.\n";
}

int execute_trace_child(const Command& command, int output_fd, int error_fd) {
    (void)output_fd;
    const TraceParseResult parsed = parse_trace(command);
    if (!parsed.ok()) {
        write_all(error_fd, parsed.error + "\n" + trace_usage());
        return 2;
    }
    if (parsed.config.show_help) {
        write_all(output_fd, trace_usage());
        return 0;
    }
    return trace_process(parsed.config, error_fd);
}

}  // namespace shell
