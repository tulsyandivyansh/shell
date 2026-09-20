#include "include/lexer.hpp"

#include <cctype>

namespace shell {
namespace {

void flush_word(std::vector<Token>& tokens, std::string& current, std::size_t start, bool& started) {
    if (started) {
        tokens.push_back(Token{TokenType::Word, std::move(current), start});
        current.clear();
        started = false;
    }
}

}  // namespace

LexResult lex(const std::string& input) {
    LexResult result;
    std::string current;
    std::size_t word_start = 0;
    bool in_single = false;
    bool in_double = false;
    bool word_started = false;

    auto begin_word_if_needed = [&](std::size_t position) {
        if (!word_started) {
            word_start = position;
            word_started = true;
        }
    };

    for (std::size_t i = 0; i < input.size(); ++i) {
        const char c = input[i];

        if (c == '\\') {
            begin_word_if_needed(i);
            if (in_single) {
                current += c;
                continue;
            }
            if (i + 1 >= input.size()) {
                result.error = "syntax error: trailing escape";
                return result;
            }
            const char next = input[++i];
            if (in_double) {
                if (next == '"' || next == '\\' || next == '$' || next == '`' || next == '\n') {
                    current += next;
                } else {
                    current += '\\';
                    current += next;
                }
            } else {
                current += next;
            }
            continue;
        }

        if (c == '\'' && !in_double) {
            begin_word_if_needed(i);
            in_single = !in_single;
            continue;
        }
        if (c == '"' && !in_single) {
            begin_word_if_needed(i);
            in_double = !in_double;
            continue;
        }

        if (in_single || in_double) {
            current += c;
            continue;
        }

        if (std::isspace(static_cast<unsigned char>(c))) {
            flush_word(result.tokens, current, word_start, word_started);
            continue;
        }

        auto emit_operator = [&](TokenType type, std::size_t length) {
            flush_word(result.tokens, current, word_start, word_started);
            result.tokens.push_back(Token{type, input.substr(i, length), i});
            i += length - 1;
        };

        if (c == '&' && i + 1 < input.size() && input[i + 1] == '&') {
            emit_operator(TokenType::AndIf, 2);
        } else if (c == '&') {
            emit_operator(TokenType::Background, 1);
        } else if (c == '|' && i + 1 < input.size() && input[i + 1] == '|') {
            emit_operator(TokenType::OrIf, 2);
        } else if (c == '|') {
            emit_operator(TokenType::Pipe, 1);
        } else if (c == ';') {
            emit_operator(TokenType::Semicolon, 1);
        } else if (c == '<') {
            emit_operator(TokenType::RedirectIn, 1);
        } else if (c == '2' && !word_started && i + 1 < input.size() && input[i + 1] == '>') {
            if (i + 2 < input.size() && input[i + 2] == '>') {
                emit_operator(TokenType::RedirectErrAppend, 3);
            } else {
                emit_operator(TokenType::RedirectErr, 2);
            }
        } else if (c == '1' && !word_started && i + 1 < input.size() && input[i + 1] == '>') {
            if (i + 2 < input.size() && input[i + 2] == '>') {
                emit_operator(TokenType::RedirectAppend, 3);
            } else {
                emit_operator(TokenType::RedirectOut, 2);
            }
        } else if (c == '>') {
            if (i + 1 < input.size() && input[i + 1] == '>') {
                emit_operator(TokenType::RedirectAppend, 2);
            } else {
                emit_operator(TokenType::RedirectOut, 1);
            }
        } else {
            begin_word_if_needed(i);
            current += c;
        }
    }

    if (in_single) {
        result.error = "syntax error: unmatched single quote";
        return result;
    }
    if (in_double) {
        result.error = "syntax error: unmatched double quote";
        return result;
    }

    flush_word(result.tokens, current, word_start, word_started);
    result.tokens.push_back(Token{TokenType::End, {}, input.size()});
    return result;
}

}  // namespace shell
