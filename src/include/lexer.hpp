#pragma once

#include <string>
#include <vector>

namespace shell {

enum class TokenType {
    Word,
    Pipe,
    AndIf,
    Background,
    OrIf,
    Semicolon,
    RedirectIn,
    RedirectOut,
    RedirectAppend,
    RedirectErr,
    RedirectErrAppend,
    End,
};

struct Token {
    TokenType type{TokenType::End};
    std::string text;
    std::size_t position{0};
};

struct LexResult {
    std::vector<Token> tokens;
    std::string error;

    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
};

LexResult lex(const std::string& input);

}  // namespace shell
