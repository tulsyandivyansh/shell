#pragma once

#include "command.hpp"
#include "lexer.hpp"

#include <string>
#include <vector>

namespace shell {

struct ParseResult {
    ExecutionPlan plan;
    std::string error;

    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
};

ParseResult parse(const std::vector<Token>& tokens);
ParseResult parse_line(const std::string& input);

}  // namespace shell
