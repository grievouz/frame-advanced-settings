#pragma once
#include "microphone.h"
#include "process_command.h"
namespace microphone {
inline CommandResult runCommand(const std::vector<std::string> &args) {
    auto result = process::runCommand(args);
    return {result.ok, std::move(result.output)};
}
} // namespace microphone
