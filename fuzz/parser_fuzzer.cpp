#include "include/lexer.hpp"
#include "include/parser.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>

namespace {

void check_plan(const shell::ExecutionPlan& plan) {
    for (const auto& step : plan.steps) {
        if (step.pipeline.commands.empty()) {
            std::abort();
        }
        for (const auto& command : step.pipeline.commands) {
            if (command.args.empty()) {
                std::abort();
            }
        }
    }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    // Bound individual fuzz cases so malformed inputs exercise parser state
    // rather than consuming unbounded memory/time.
    if (size > 4096) {
        return 0;
    }

    const std::string input(reinterpret_cast<const char*>(data), size);
    const auto lexed = shell::lex(input);
    if (!lexed.ok()) {
        return 0;
    }
    if (lexed.tokens.empty() || lexed.tokens.back().type != shell::TokenType::End) {
        std::abort();
    }

    const auto parsed = shell::parse(lexed.tokens);
    if (parsed.ok()) {
        check_plan(parsed.plan);
    }
    return 0;
}
