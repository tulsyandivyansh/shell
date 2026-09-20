#include "include/executor.hpp"
#include "include/history.hpp"
#include "include/parser.hpp"
#include "include/shell_utils.hpp"

#include <algorithm>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <readline/history.h>
#include <readline/readline.h>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

std::vector<std::string> completion_matches;
std::string completion_prefix;
bool first_tab_pressed = false;
volatile sig_atomic_t prompt_interrupted = 0;
shell::Executor* active_executor = nullptr;

extern "C" void handle_prompt_sigint(int) {
    prompt_interrupted = 1;
    const char newline = '\n';
    ::write(STDOUT_FILENO, &newline, 1);
}

int readline_event_hook() {
    // Readline invokes this in normal execution context, so it is a safe place
    // to reap SIGCHLD events without touching C++ state in the signal handler.
    if (active_executor != nullptr) {
        active_executor->reap_background_jobs(false);
    }

    if (prompt_interrupted != 0) {
        ::rl_replace_line("", 0);
        ::rl_done = 1;
    }
    return 0;
}

void ring_bell() {
    std::cout << '\a' << std::flush;
}

std::string longest_common_prefix(const std::vector<std::string>& matches) {
    if (matches.empty()) {
        return {};
    }

    std::string prefix = matches.front();
    for (std::size_t i = 1; i < matches.size(); ++i) {
        std::size_t common = 0;
        while (common < prefix.size() &&
               common < matches[i].size() &&
               prefix[common] == matches[i][common]) {
            ++common;
        }
        prefix.resize(common);
    }
    return prefix;
}

char* command_completion(const char* text, int state) {
    static std::vector<std::string> matches;
    static std::size_t index = 0;

    if (state == 0) {
        index = 0;
        matches.clear();
        const std::string prefix(text);

        std::vector<std::string> commands = shell::builtin_commands();
        auto executables = shell::get_path_executables();
        commands.insert(commands.end(), executables.begin(), executables.end());

        for (const auto& command : commands) {
            if (command.rfind(prefix, 0) == 0) {
                matches.push_back(command);
            }
        }
        std::sort(matches.begin(), matches.end());
        matches.erase(std::unique(matches.begin(), matches.end()), matches.end());

        completion_matches = matches;
        completion_prefix = prefix;
    }

    if (index >= matches.size()) {
        return nullptr;
    }

    const std::string& match = matches[index++];
    char* result = static_cast<char*>(std::malloc(match.size() + 1));
    if (result != nullptr) {
        std::memcpy(result, match.c_str(), match.size() + 1);
    }
    return result;
}

char** shell_completion(const char* text, int start, int /*end*/) {
    if (start != 0) {
        return nullptr;
    }

    char** matches = ::rl_completion_matches(text, command_completion);

    if (completion_matches.empty()) {
        ring_bell();
        first_tab_pressed = false;
        return matches;
    }

    if (completion_matches.size() == 1) {
        first_tab_pressed = false;
        return matches;
    }

    if (!first_tab_pressed) {
        const std::string common = longest_common_prefix(completion_matches);
        if (common.size() > completion_prefix.size()) {
            first_tab_pressed = false;
            return matches;
        }

        ring_bell();
        first_tab_pressed = true;
        if (matches != nullptr) {
            for (int i = 0; matches[i] != nullptr; ++i) {
                std::free(matches[i]);
            }
            std::free(matches);
        }
        return nullptr;
    }

    std::cout << '\n';
    for (std::size_t i = 0; i < completion_matches.size(); ++i) {
        if (i > 0) {
            std::cout << "  ";
        }
        std::cout << completion_matches[i];
    }
    std::cout << '\n';
    ::rl_forced_update_display();
    first_tab_pressed = false;

    if (matches != nullptr) {
        for (int i = 0; matches[i] != nullptr; ++i) {
            std::free(matches[i]);
        }
        std::free(matches);
    }
    return nullptr;
}

void initialize_readline() {
    struct sigaction action {};
    action.sa_handler = handle_prompt_sigint;
    ::sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    ::sigaction(SIGINT, &action, nullptr);

    ::rl_attempted_completion_function = shell_completion;
    ::rl_event_hook = readline_event_hook;
    ::rl_set_keyboard_input_timeout(100000);
    ::rl_completion_entry_function = nullptr;
    shell::history::initialize();
}

}  // namespace

int main() {
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;

    shell::Executor executor;
    active_executor = &executor;
    initialize_readline();

    while (true) {
        executor.reap_background_jobs(true);
        first_tab_pressed = false;
        char* input_cstr = ::readline("$ ");

        if (input_cstr == nullptr) {
            std::cout << '\n';
            shell::history::save_session();
            return 0;
        }

        executor.reap_background_jobs(true);

        std::string input(input_cstr);
        const bool interrupted = prompt_interrupted != 0;
        prompt_interrupted = 0;
        if (!interrupted && !input.empty()) {
            ::add_history(input_cstr);
        }
        std::free(input_cstr);

        if (interrupted) {
            continue;
        }

        if (input.empty()) {
            continue;
        }

        const shell::ParseResult parsed = shell::parse_line(input);
        if (!parsed.ok()) {
            std::cerr << parsed.error << '\n';
            continue;
        }
        if (parsed.plan.steps.empty()) {
            continue;
        }

        const shell::ExecutionResult result = executor.execute(parsed.plan);
        if (result.exit_requested) {
            shell::history::save_session();
            return result.exit_code;
        }
    }
}
