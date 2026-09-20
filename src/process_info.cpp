#include "include/process_info.hpp"

#include "include/unique_fd.hpp"

#include <cerrno>
#include <charconv>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace shell {
namespace {

struct ReadResult {
    std::string data;
    int error_number{0};
};

ReadResult read_file(const std::string& path) {
    UniqueFd fd(::open(path.c_str(), O_RDONLY | O_CLOEXEC));
    if (!fd) {
        return {{}, errno};
    }

    std::string data;
    char buffer[4096];
    while (true) {
        const ssize_t count = ::read(fd.get(), buffer, sizeof(buffer));
        if (count > 0) {
            data.append(buffer, static_cast<std::size_t>(count));
            continue;
        }
        if (count == 0) {
            return {std::move(data), 0};
        }
        if (errno == EINTR) {
            continue;
        }
        return {{}, errno};
    }
}

std::string trim(std::string_view text) {
    const std::size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return {};
    }
    const std::size_t last = text.find_last_not_of(" \t\r\n");
    return std::string(text.substr(first, last - first + 1));
}

template <typename Integer>
bool parse_integer(std::string_view text, Integer& value) {
    text = std::string_view(text.data(), text.size());
    const std::string cleaned = trim(text);
    if (cleaned.empty()) {
        return false;
    }

    Integer parsed{};
    const char* begin = cleaned.data();
    const char* end = cleaned.data() + cleaned.size();
    const auto [ptr, ec] = std::from_chars(begin, end, parsed);
    if (ec != std::errc{} || ptr != end) {
        return false;
    }
    value = parsed;
    return true;
}

std::optional<std::uint64_t> parse_kb_value(std::string_view value) {
    std::istringstream stream{std::string(value)};
    std::uint64_t amount = 0;
    std::string unit;
    if (!(stream >> amount)) {
        return std::nullopt;
    }
    stream >> unit;
    if (!unit.empty() && unit != "kB") {
        return std::nullopt;
    }
    return amount;
}

std::string state_description(char state) {
    switch (state) {
        case 'R': return "R (running)";
        case 'S': return "S (sleeping)";
        case 'D': return "D (uninterruptible sleep)";
        case 'Z': return "Z (zombie)";
        case 'T': return "T (stopped)";
        case 't': return "t (tracing stop)";
        case 'X':
        case 'x': return "X (dead)";
        case 'I': return "I (idle)";
        default: return std::string(1, state) + " (unknown)";
    }
}

std::string normalize_cmdline(const std::string& raw) {
    std::string result;
    result.reserve(raw.size());
    for (char c : raw) {
        if (c == '\0') {
            if (!result.empty() && result.back() != ' ') {
                result.push_back(' ');
            }
        } else {
            result.push_back(c);
        }
    }
    while (!result.empty() && result.back() == ' ') {
        result.pop_back();
    }
    return result;
}

std::string read_symlink(const std::string& path) {
    std::vector<char> buffer(256);
    while (buffer.size() <= 64 * 1024) {
        const ssize_t count = ::readlink(path.c_str(), buffer.data(), buffer.size());
        if (count < 0) {
            return {};
        }
        if (static_cast<std::size_t>(count) < buffer.size()) {
            return std::string(buffer.data(), static_cast<std::size_t>(count));
        }
        buffer.resize(buffer.size() * 2);
    }
    return {};
}

std::optional<std::size_t> count_open_fds(const std::string& path) {
    DIR* directory = ::opendir(path.c_str());
    if (directory == nullptr) {
        return std::nullopt;
    }

    std::size_t count = 0;
    errno = 0;
    while (dirent* entry = ::readdir(directory)) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        ++count;
    }
    const int iteration_error = errno;
    ::closedir(directory);
    if (iteration_error != 0) {
        return std::nullopt;
    }
    return count;
}

bool parse_stat(const std::string& text, ProcessInfo& info, std::string& error) {
    const std::size_t open_paren = text.find('(');
    const std::size_t close_paren = text.rfind(')');
    if (open_paren == std::string::npos || close_paren == std::string::npos ||
        close_paren <= open_paren || close_paren + 2 >= text.size()) {
        error = "malformed /proc stat data";
        return false;
    }

    pid_t parsed_pid = -1;
    if (!parse_integer<pid_t>(std::string_view(text).substr(0, open_paren), parsed_pid)) {
        error = "invalid pid in /proc stat data";
        return false;
    }
    info.pid = parsed_pid;
    info.name = text.substr(open_paren + 1, close_paren - open_paren - 1);

    const char state = text[close_paren + 2];
    info.state = state_description(state);

    if (close_paren + 4 > text.size()) {
        error = "truncated /proc stat data";
        return false;
    }

    std::istringstream fields(text.substr(close_paren + 4));
    std::vector<std::string> tail;
    std::string field;
    while (fields >> field) {
        tail.push_back(field);
    }

    // tail[0] corresponds to field 4 (ppid) from proc_pid_stat(5).
    if (tail.size() <= 20) {
        error = "incomplete /proc stat data";
        return false;
    }

    long long ppid = 0;
    long long pgid = 0;
    long long session = 0;
    long long tpgid = 0;
    unsigned long long utime = 0;
    unsigned long long stime = 0;
    long priority = 0;
    long nice_value = 0;
    long threads = 0;

    if (!parse_integer(tail[0], ppid) ||
        !parse_integer(tail[1], pgid) ||
        !parse_integer(tail[2], session) ||
        !parse_integer(tail[4], tpgid) ||
        !parse_integer(tail[10], utime) ||
        !parse_integer(tail[11], stime) ||
        !parse_integer(tail[14], priority) ||
        !parse_integer(tail[15], nice_value) ||
        !parse_integer(tail[16], threads)) {
        error = "invalid numeric field in /proc stat data";
        return false;
    }

    info.ppid = static_cast<pid_t>(ppid);
    info.pgid = static_cast<pid_t>(pgid);
    info.session_id = static_cast<pid_t>(session);
    info.foreground_pgid = static_cast<pid_t>(tpgid);
    info.priority = priority;
    info.nice_value = nice_value;
    info.threads = threads;

    const long ticks_per_second = ::sysconf(_SC_CLK_TCK);
    if (ticks_per_second > 0) {
        info.user_cpu_seconds = static_cast<double>(utime) / static_cast<double>(ticks_per_second);
        info.system_cpu_seconds = static_cast<double>(stime) / static_cast<double>(ticks_per_second);
    }
    return true;
}

void parse_status(const std::string& text, ProcessInfo& info) {
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }

        const std::string key = line.substr(0, colon);
        const std::string value = trim(std::string_view(line).substr(colon + 1));

        if (key == "Name" && !value.empty()) {
            info.name = value;
        } else if (key == "State" && !value.empty()) {
            info.state = value;
        } else if (key == "Threads") {
            long threads = 0;
            if (parse_integer(value, threads)) {
                info.threads = threads;
            }
        } else if (key == "VmSize") {
            info.virtual_memory_kb = parse_kb_value(value);
        } else if (key == "VmRSS") {
            info.resident_memory_kb = parse_kb_value(value);
        } else if (key == "VmPeak") {
            info.peak_memory_kb = parse_kb_value(value);
        } else if (key == "Cpus_allowed_list") {
            info.cpu_allowed_list = value;
        } else if (key == "voluntary_ctxt_switches") {
            std::uint64_t switches = 0;
            if (parse_integer(value, switches)) {
                info.voluntary_context_switches = switches;
            }
        } else if (key == "nonvoluntary_ctxt_switches") {
            std::uint64_t switches = 0;
            if (parse_integer(value, switches)) {
                info.involuntary_context_switches = switches;
            }
        }
    }
}

std::string format_kb(const std::optional<std::uint64_t>& value) {
    if (!value.has_value()) {
        return "unavailable";
    }

    std::ostringstream output;
    if (*value >= 1024 * 1024) {
        output << std::fixed << std::setprecision(2)
               << static_cast<double>(*value) / (1024.0 * 1024.0) << " GiB";
    } else if (*value >= 1024) {
        output << std::fixed << std::setprecision(2)
               << static_cast<double>(*value) / 1024.0 << " MiB";
    } else {
        output << *value << " KiB";
    }
    return output.str();
}

std::string process_error(pid_t pid, int error_number) {
    if (error_number == ENOENT || error_number == ESRCH) {
        return "process " + std::to_string(pid) + " does not exist";
    }
    if (error_number == EACCES || error_number == EPERM) {
        return "permission denied while inspecting process " + std::to_string(pid);
    }
    return "failed to inspect process " + std::to_string(pid) + ": " +
           std::strerror(error_number);
}

}  // namespace

ProcessInfoResult inspect_process(pid_t pid) {
    if (pid <= 0) {
        return {std::nullopt, "pid must be a positive integer"};
    }

    const std::string root = "/proc/" + std::to_string(pid);
    const ReadResult stat = read_file(root + "/stat");
    if (stat.error_number != 0) {
        return {std::nullopt, process_error(pid, stat.error_number)};
    }

    ProcessInfo info;
    std::string parse_error;
    if (!parse_stat(stat.data, info, parse_error)) {
        return {std::nullopt,
                "failed to parse process " + std::to_string(pid) + ": " + parse_error};
    }

    const ReadResult status = read_file(root + "/status");
    if (status.error_number == 0) {
        parse_status(status.data, info);
    }

    const ReadResult cmdline = read_file(root + "/cmdline");
    if (cmdline.error_number == 0) {
        info.command_line = normalize_cmdline(cmdline.data);
    }
    if (info.command_line.empty()) {
        info.command_line = "[" + info.name + "]";
    }

    info.executable = read_symlink(root + "/exe");
    info.open_file_descriptors = count_open_fds(root + "/fd");
    // Opening /proc/<pid>/fd itself creates one descriptor when inspecting
    // this process, so discount that observer effect from the reported count.
    if (pid == ::getpid() && info.open_file_descriptors.has_value() &&
        *info.open_file_descriptors > 0) {
        --*info.open_file_descriptors;
    }

    return {std::move(info), {}};
}

std::string render_process_info(const ProcessInfo& info) {
    std::ostringstream output;
    output << "Process " << info.pid << " (" << info.name << ")\n";
    output << "  State:             " << info.state << "\n";
    output << "  PPID:              " << info.ppid << "\n";
    output << "  Process group:     " << info.pgid << "\n";
    output << "  Session:           " << info.session_id << "\n";
    output << "  Foreground PGID:   " << info.foreground_pgid << "\n";
    output << "  Threads:           " << info.threads << "\n";
    output << "  Priority / nice:   " << info.priority << " / " << info.nice_value << "\n";
    output << "  CPU affinity:      "
           << (info.cpu_allowed_list.empty() ? "unavailable" : info.cpu_allowed_list) << "\n";
    output << "\nCPU\n";
    output << "  User time:         " << std::fixed << std::setprecision(3)
           << info.user_cpu_seconds << " s\n";
    output << "  System time:       " << std::fixed << std::setprecision(3)
           << info.system_cpu_seconds << " s\n";
    output << "  Voluntary ctx:     ";
    if (info.voluntary_context_switches.has_value()) {
        output << *info.voluntary_context_switches;
    } else {
        output << "unavailable";
    }
    output << "\n  Involuntary ctx:   ";
    if (info.involuntary_context_switches.has_value()) {
        output << *info.involuntary_context_switches;
    } else {
        output << "unavailable";
    }
    output << "\n\nMemory\n";
    output << "  Virtual:           " << format_kb(info.virtual_memory_kb) << "\n";
    output << "  Resident:          " << format_kb(info.resident_memory_kb) << "\n";
    output << "  Peak:              " << format_kb(info.peak_memory_kb) << "\n";
    output << "\nI/O\n";
    output << "  Open FDs:          ";
    if (info.open_file_descriptors.has_value()) {
        output << *info.open_file_descriptors;
    } else {
        output << "unavailable";
    }
    output << "\n\nCommand\n";
    output << "  Executable:        " << (info.executable.empty() ? "unavailable" : info.executable) << "\n";
    output << "  Command line:      " << info.command_line << "\n";
    return output.str();
}

}  // namespace shell
