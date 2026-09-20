#pragma once

#include <string>
#include <vector>

namespace shell {

const std::vector<std::string>& builtin_commands();
bool is_builtin(const std::string& command);
std::string find_executable(const std::string& command);
std::vector<std::string> get_path_executables();
std::string process_echo_escapes(const std::string& input);
bool write_all(int fd, const std::string& data);

}  // namespace shell
