#include "include/shell_utils.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <dirent.h>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace shell {

const std::vector<std::string>& builtin_commands() {
    static const std::vector<std::string> commands = {
        "echo", "exit", "type", "pwd", "cd", "history", "jobs", "fg", "bg", "pinfo", "run", "limits", "sandbox", "trace"
    };
    return commands;
}

bool is_builtin(const std::string& command) {
    const auto& commands = builtin_commands();
    return std::find(commands.begin(), commands.end(), command) != commands.end();
}

std::string find_executable(const std::string& command) {
    if (command.find('/') != std::string::npos) {
        struct stat file_info {};
        if (::stat(command.c_str(), &file_info) == 0 &&
            S_ISREG(file_info.st_mode) &&
            ::access(command.c_str(), X_OK) == 0) {
            return command;
        }
        return {};
    }

    const char* path_env = std::getenv("PATH");
    if (path_env == nullptr) {
        return {};
    }

    std::istringstream path_stream(path_env);
    std::string directory;
    while (std::getline(path_stream, directory, ':')) {
        if (directory.empty()) {
            directory = ".";
        }
        const std::string full_path = directory + "/" + command;
        struct stat file_info {};
        if (::stat(full_path.c_str(), &file_info) == 0 &&
            S_ISREG(file_info.st_mode) &&
            ::access(full_path.c_str(), X_OK) == 0) {
            return full_path;
        }
    }

    return {};
}

std::vector<std::string> get_path_executables() {
    std::vector<std::string> executables;
    const char* path_env = std::getenv("PATH");
    if (path_env == nullptr) {
        return executables;
    }

    std::istringstream path_stream(path_env);
    std::string directory;
    while (std::getline(path_stream, directory, ':')) {
        if (directory.empty()) {
            continue;
        }

        DIR* dir = ::opendir(directory.c_str());
        if (dir == nullptr) {
            continue;
        }

        while (dirent* entry = ::readdir(dir)) {
            if (entry->d_name[0] == '.') {
                continue;
            }

            const std::string full_path = directory + "/" + entry->d_name;
            struct stat file_info {};
            if (::stat(full_path.c_str(), &file_info) == 0 &&
                S_ISREG(file_info.st_mode) &&
                ::access(full_path.c_str(), X_OK) == 0) {
                const std::string filename(entry->d_name);
                if (std::find(executables.begin(), executables.end(), filename) == executables.end()) {
                    executables.push_back(filename);
                }
            }
        }
        ::closedir(dir);
    }

    return executables;
}

std::string process_echo_escapes(const std::string& input) {
    std::string result;
    for (std::size_t i = 0; i < input.length(); ++i) {
        if (input[i] == '\\' && i + 1 < input.length()) {
            const char next = input[i + 1];
            if (next == 'n') {
                result += '\n';
                ++i;
            } else if (next == 't') {
                result += '\t';
                ++i;
            } else if (next == '\\') {
                result += '\\';
                ++i;
            } else {
                result += input[i];
            }
        } else {
            result += input[i];
        }
    }
    return result;
}

bool write_all(int fd, const std::string& data) {
    std::size_t written = 0;
    while (written < data.size()) {
        const ssize_t result = ::write(fd, data.data() + written, data.size() - written);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        written += static_cast<std::size_t>(result);
    }
    return true;
}

}  // namespace shell
