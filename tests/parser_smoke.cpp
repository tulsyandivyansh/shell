#include "include/lexer.hpp"
#include "include/parser.hpp"

#include <cassert>
#include <string>

int main() {
    using namespace shell;

    {
        const auto lexed = lex("echo \"hello world\"|grep hello>>output.txt");
        assert(lexed.ok());
        assert(lexed.tokens.size() == 8);
        assert(lexed.tokens[0].type == TokenType::Word && lexed.tokens[0].text == "echo");
        assert(lexed.tokens[1].type == TokenType::Word && lexed.tokens[1].text == "hello world");
        assert(lexed.tokens[2].type == TokenType::Pipe);
        assert(lexed.tokens[5].type == TokenType::RedirectAppend);
    }

    {
        const auto parsed = parse_line("cat<input.txt|grep foo>out.txt&&echo ok||echo bad;pwd");
        assert(parsed.ok());
        assert(parsed.plan.steps.size() == 4);
        assert(parsed.plan.steps[0].pipeline.commands.size() == 2);
        assert(parsed.plan.steps[0].pipeline.commands[0].has_input_redirect);
        assert(parsed.plan.steps[0].pipeline.commands[0].input_file == "input.txt");
        assert(parsed.plan.steps[0].pipeline.commands[1].has_output_redirect);
        assert(parsed.plan.steps[0].pipeline.commands[1].output_file == "out.txt");
        assert(parsed.plan.steps[1].connector == ChainOperator::And);
        assert(parsed.plan.steps[2].connector == ChainOperator::Or);
        assert(parsed.plan.steps[3].connector == ChainOperator::Always);
    }

    {
        const auto parsed = parse_line("echo '' \"\"");
        assert(parsed.ok());
        assert(parsed.plan.steps[0].pipeline.commands[0].args.size() == 3);
        assert(parsed.plan.steps[0].pipeline.commands[0].args[1].empty());
        assert(parsed.plan.steps[0].pipeline.commands[0].args[2].empty());
    }

    {
        const auto parsed = parse_line("echo foo2>out.txt");
        assert(parsed.ok());
        const auto& command = parsed.plan.steps[0].pipeline.commands[0];
        assert(command.args.size() == 2);
        assert(command.args[1] == "foo2");
        assert(command.has_output_redirect);
        assert(!command.has_error_redirect);
    }

    {
        const auto parsed = parse_line("echo hi |");
        assert(!parsed.ok());
    }

    {
        const auto parsed = parse_line("echo \"unterminated");
        assert(!parsed.ok());
    }

    {
        const auto lexed = lex("sleep 1&echo done");
        assert(lexed.ok());
        assert(lexed.tokens[2].type == TokenType::Background);

        const auto parsed = parse(lexed.tokens);
        assert(parsed.ok());
        assert(parsed.plan.steps.size() == 2);
        assert(parsed.plan.steps[0].pipeline.background);
        assert(!parsed.plan.steps[1].pipeline.background);
        assert(parsed.plan.steps[1].connector == ChainOperator::Always);
    }

    return 0;
}
