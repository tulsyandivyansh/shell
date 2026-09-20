#pragma once

#include <string>
#include <vector>

namespace shell {

struct Command {
    std::vector<std::string> args;
    std::string input_file;
    std::string output_file;
    std::string error_file;
    bool has_input_redirect{false};
    bool has_output_redirect{false};
    bool has_error_redirect{false};
    bool append_output{false};
    bool append_error{false};
};

struct Pipeline {
    std::vector<Command> commands;
    bool background{false};
};

enum class ChainOperator {
    Always,
    And,
    Or,
};

struct PipelineStep {
    ChainOperator connector{ChainOperator::Always};
    Pipeline pipeline;
};

struct ExecutionPlan {
    std::vector<PipelineStep> steps;
};

}  // namespace shell
