#include "include/history.hpp"

#include <cstdlib>
#include <fstream>
#include <readline/history.h>

namespace shell::history {
namespace {
std::string histfile_path;
int persisted_history_length = 0;
}

void load_from_file(const std::string& filename) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        return;
    }

    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty()) {
            ::add_history(line.c_str());
        }
    }
    persisted_history_length = history_length;
}

void write_to_file(const std::string& filename) {
    std::ofstream file(filename);
    if (!file.is_open()) {
        return;
    }

    for (int i = 1; i <= history_length; ++i) {
        HIST_ENTRY* entry = ::history_get(i);
        if (entry != nullptr && entry->line != nullptr) {
            file << entry->line << '\n';
        }
    }
}

void append_new_to_file(const std::string& filename) {
    if (history_length <= persisted_history_length) {
        return;
    }

    std::ofstream file(filename, std::ios::app);
    if (!file.is_open()) {
        return;
    }

    for (int i = persisted_history_length + 1; i <= history_length; ++i) {
        HIST_ENTRY* entry = ::history_get(i);
        if (entry != nullptr && entry->line != nullptr) {
            file << entry->line << '\n';
        }
    }
    persisted_history_length = history_length;
}

void initialize() {
    ::using_history();
    if (const char* env = std::getenv("HISTFILE"); env != nullptr) {
        histfile_path = env;
        load_from_file(histfile_path);
    }
}

void save_session() {
    if (histfile_path.empty()) {
        return;
    }

    std::ifstream check(histfile_path);
    const bool exists = check.good();
    check.close();

    if (exists) {
        append_new_to_file(histfile_path);
    } else {
        write_to_file(histfile_path);
    }
}

std::string render(int max_entries) {
    const int hist_len = history_length;
    if (hist_len <= 0) {
        return {};
    }

    int start_index = 1;
    if (max_entries > 0 && max_entries < hist_len) {
        start_index = hist_len - max_entries + 1;
    }

    std::string output;
    for (int i = start_index; i <= hist_len; ++i) {
        HIST_ENTRY* entry = ::history_get(i);
        if (entry != nullptr && entry->line != nullptr) {
            output += "    " + std::to_string(i) + "  " + std::string(entry->line) + "\n";
        }
    }
    return output;
}

}  // namespace shell::history
