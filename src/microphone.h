#pragma once
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace microphone {
enum class Backend { None, AdvancedSettings, Compatible };
enum class Filter { Echo, Noise };
struct State {
    Backend backend = Backend::None;
    bool echo = true, noise = true;
    std::string message;
    bool available() const { return backend != Backend::None; }
};
struct CommandResult {
    bool ok = false;
    std::string output;
};
using Runner = std::function<CommandResult(const std::vector<std::string> &)>;
std::optional<bool> parseSetting(const std::string &output, const std::string &key);
State parseSettings(const std::string &output);
const char *key(Backend backend, Filter filter);

// No shell, recording, audio-device routing, or app-owned preference files.
// Every write is confirmed by reading WirePlumber's current setting again.
class Service {
    Runner run_;
    State apply(std::optional<Filter> filter, bool value);

  public:
    explicit Service(Runner runner) : run_(std::move(runner)) {}
    State read();
    State set(Filter filter, bool value) { return apply(filter, value); }
    State reset() { return apply(std::nullopt, true); }
};
} // namespace microphone
