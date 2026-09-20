#include "include/parser.hpp"

namespace shell {
namespace {

class Parser {
public:
    explicit Parser(const std::vector<Token>& tokens) : tokens_(tokens) {}

    ParseResult run() {
        ParseResult result;
        if (peek().type == TokenType::End) {
            return result;
        }

        ChainOperator connector = ChainOperator::Always;
        while (peek().type != TokenType::End) {
            Pipeline pipeline;
            if (!parse_pipeline(pipeline)) {
                result.error = error_;
                return result;
            }

            // A single '&' terminates the current pipeline asynchronously and
            // also acts as a command-list separator, like a POSIX shell.
            if (peek().type == TokenType::Background) {
                pipeline.background = true;
                advance();
                result.plan.steps.push_back(PipelineStep{connector, std::move(pipeline)});
                connector = ChainOperator::Always;
                if (peek().type == TokenType::End) {
                    return result;
                }
                continue;
            }

            result.plan.steps.push_back(PipelineStep{connector, std::move(pipeline)});

            switch (peek().type) {
                case TokenType::AndIf:
                    connector = ChainOperator::And;
                    advance();
                    break;
                case TokenType::OrIf:
                    connector = ChainOperator::Or;
                    advance();
                    break;
                case TokenType::Semicolon:
                    connector = ChainOperator::Always;
                    advance();
                    break;
                case TokenType::End:
                    return result;
                default:
                    fail("unexpected token '" + peek().text + "'");
                    result.error = error_;
                    return result;
            }

            if (peek().type == TokenType::End) {
                fail("expected command after operator");
                result.error = error_;
                return result;
            }
        }
        return result;
    }

private:
    bool parse_pipeline(Pipeline& pipeline) {
        Command command;
        if (!parse_command(command)) {
            return false;
        }
        pipeline.commands.push_back(std::move(command));

        while (peek().type == TokenType::Pipe) {
            advance();
            if (peek().type == TokenType::Pipe || peek().type == TokenType::AndIf ||
                peek().type == TokenType::OrIf || peek().type == TokenType::Semicolon ||
                peek().type == TokenType::Background || peek().type == TokenType::End) {
                return fail("expected command after pipe");
            }
            Command next;
            if (!parse_command(next)) {
                return false;
            }
            pipeline.commands.push_back(std::move(next));
        }
        return true;
    }

    bool parse_command(Command& command) {
        bool saw_word = false;
        while (true) {
            const Token& token = peek();
            if (token.type == TokenType::Word) {
                command.args.push_back(token.text);
                saw_word = true;
                advance();
                continue;
            }

            if (is_redirection(token.type)) {
                const TokenType redirect = token.type;
                advance();
                if (peek().type != TokenType::Word) {
                    return fail("expected filename after redirection");
                }
                const std::string filename = peek().text;
                advance();
                switch (redirect) {
                    case TokenType::RedirectIn:
                        command.has_input_redirect = true;
                        command.input_file = filename;
                        break;
                    case TokenType::RedirectOut:
                        command.has_output_redirect = true;
                        command.append_output = false;
                        command.output_file = filename;
                        break;
                    case TokenType::RedirectAppend:
                        command.has_output_redirect = true;
                        command.append_output = true;
                        command.output_file = filename;
                        break;
                    case TokenType::RedirectErr:
                        command.has_error_redirect = true;
                        command.append_error = false;
                        command.error_file = filename;
                        break;
                    case TokenType::RedirectErrAppend:
                        command.has_error_redirect = true;
                        command.append_error = true;
                        command.error_file = filename;
                        break;
                    default:
                        break;
                }
                continue;
            }
            break;
        }

        if (!saw_word) {
            return fail("expected command");
        }
        return true;
    }

    static bool is_redirection(TokenType type) {
        return type == TokenType::RedirectIn || type == TokenType::RedirectOut ||
               type == TokenType::RedirectAppend || type == TokenType::RedirectErr ||
               type == TokenType::RedirectErrAppend;
    }

    const Token& peek() const { return tokens_[index_]; }
    void advance() {
        if (index_ + 1 < tokens_.size()) {
            ++index_;
        }
    }

    bool fail(const std::string& message) {
        error_ = "syntax error: " + message;
        return false;
    }

    const std::vector<Token>& tokens_;
    std::size_t index_{0};
    std::string error_;
};

}  // namespace

ParseResult parse(const std::vector<Token>& tokens) {
    if (tokens.empty()) {
        return ParseResult{{}, "syntax error: empty token stream"};
    }
    return Parser(tokens).run();
}

ParseResult parse_line(const std::string& input) {
    LexResult lexed = lex(input);
    if (!lexed.ok()) {
        return ParseResult{{}, lexed.error};
    }
    return parse(lexed.tokens);
}

}  // namespace shell
