#pragma once

#include <string>

namespace shell::history {

void initialize();
void load_from_file(const std::string& filename);
void write_to_file(const std::string& filename);
void append_new_to_file(const std::string& filename);
void save_session();
std::string render(int max_entries = -1);

}  // namespace shell::history
