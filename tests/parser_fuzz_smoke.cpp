#include "include/lexer.hpp"
#include "include/parser.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <random>
#include <string>

namespace {

void validate_plan(const shell::ExecutionPlan& plan) {
    for (const auto& step : plan.steps) {
        assert(!step.pipeline.commands.empty());
        for (const auto& command : step.pipeline.commands) {
            assert(!command.args.empty());
            for (const auto& arg : command.args) {
                (void)arg;
            }
        }
    }
}

}  // namespace

int main() {
    constexpr std::size_t kIterations = 30000;
    constexpr std::size_t kMaxLength = 256;
    constexpr char kAlphabet[] =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
        " \\t\\n'\"\\\\|&;<>-_/.$`()[]{}:=+*?!#%^,@";

    std::mt19937_64 rng(0x5EEDC0FFEEULL);
    std::uniform_int_distribution<std::size_t> length_dist(0, kMaxLength);
    std::uniform_int_distribution<std::size_t> char_dist(0, sizeof(kAlphabet) - 2);

    for (std::size_t iteration = 0; iteration < kIterations; ++iteration) {
        const std::size_t length = length_dist(rng);
        std::string input;
        input.reserve(length);
        for (std::size_t i = 0; i < length; ++i) {
            input.push_back(kAlphabet[char_dist(rng)]);
        }

        const auto lexed = shell::lex(input);
        if (!lexed.ok()) {
            continue;
        }

        assert(!lexed.tokens.empty());
        assert(lexed.tokens.back().type == shell::TokenType::End);

        const auto parsed = shell::parse(lexed.tokens);
        if (parsed.ok()) {
            validate_plan(parsed.plan);
        }

        // Parsing is deterministic: the same input must produce the same
        // success/failure classification and high-level plan size.
        const auto reparsed = shell::parse_line(input);
        assert(reparsed.ok() == parsed.ok());
        if (parsed.ok()) {
            assert(reparsed.plan.steps.size() == parsed.plan.steps.size());
        }
    }

    return 0;
}
