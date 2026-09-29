#include "microphone.h"
#include <iostream>
#include <stdexcept>

using namespace microphone;
void check(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
std::string listing(bool echo, bool noise, Backend backend = Backend::AdvancedSettings) {
    return std::string("Settings:\n- Id: ") + key(backend, Filter::Echo) +
           "\n  Description: arbitrary text\n  Default: true\n  Value: " +
           (echo ? "true" : "false") +
           " (Saved: true)\n- Id: other.key\n  Value: false\n- Id: " + key(backend, Filter::Noise) +
           "\n  Value: " + (noise ? "true" : "false") + " [Saved: false]\n";
}
int main() {
    const auto own = Backend::AdvancedSettings;
    auto state = parseSettings(listing(false, true));
    check(state.backend == own && !state.echo && state.noise,
          "Use live values, not saved/default text");
    state = parseSettings(listing(true, false, Backend::Compatible));
    check(state.backend == Backend::Compatible && state.echo && !state.noise,
          "Existing compatible backend");
    state = parseSettings(listing(true, false, Backend::Compatible) + listing(false, true));
    check(state.backend == own && !state.echo, "Prefer own loaded schema");
    check(!parseSettings("").available(), "Missing hook unavailable");
    check(!parseSettings("- Id: frame-advanced-settings.mic-echo-cancel\n  Value: true\n")
               .available(),
          "Require both filters");
    check(!parseSetting("- Id: key.suffix\n  Value: true\n", "key"), "Exact keys only");
    check(!parseSetting("- Id: key\n  Saved: true\n", "key"), "Do not parse saved values");
    check(!parseSetting("- Id: key\n  Value: truegarbage\n", "key"), "Exact bool token only");
    check(!parseSetting("- Id: key\n  Value: false\n  Value: true\n", "key"),
          "Duplicate values rejected");
    check(!parseSetting("- Id: key\n  Value: false\n- Id: key\n  Value: true\n", "key"),
          "Duplicate blocks rejected");
    auto crlf = parseSetting("  - Id: key\r\n\tValue: false\t(Saved: true)\r\n", "key");
    check(crlf.has_value() && !*crlf, "Whitespace and CRLF");
    check(!parseSetting(std::string(65537, 'x'), "key"), "Bound parsing input");

    bool echo = false, noise = false, failWrite = false, ignoreWrite = false, failRead = false;
    Backend backend = Backend::Compatible;
    std::vector<std::vector<std::string>> writes;
    Service service([&](const std::vector<std::string> &args) -> CommandResult {
        check(args[0] == "wpctl" && args[1] == "settings", "Only settings utility");
        if (args.size() == 2)
            return {!failRead, listing(echo, noise, backend)};
        check(args.size() == 5 && args[2] == "--save", "Persist with direct argv");
        check(args[3] == key(backend, Filter::Echo) || args[3] == key(backend, Filter::Noise),
              "Known keys only");
        writes.push_back(args);
        if (failWrite)
            return {false, "Failed"};
        if (!ignoreWrite) {
            auto &value = args[3] == key(backend, Filter::Echo) ? echo : noise;
            value = args[4] == "true";
        }
        return {true, "ok"};
    });
    state = service.set(Filter::Echo, true);
    check(state.available() && state.echo && !state.noise && state.message.empty(),
          "Set only requested filter");
    check(writes.size() == 1 && writes[0][3] == "frame-mic.echo-cancel", "Reuse loaded backend");
    state = service.reset();
    check(state.echo && state.noise && state.message.empty(),
          "Reset matches stock both-on defaults");
    failWrite = true;
    state = service.set(Filter::Noise, false);
    check(state.noise && !state.message.empty(),
          "Failed write does not optimistically change displayed value");
    failWrite = false;
    ignoreWrite = true;
    state = service.set(Filter::Noise, false);
    check(state.noise && !state.message.empty(),
          "Successful exit still requires readback confirmation");
    failRead = true;
    const auto count = writes.size();
    state = service.reset();
    check(!state.available() && writes.size() == count, "Unavailable read never writes");
    std::cout
        << "Microphone parsing, backend selection, persistence and write confirmation passed.\n";
}
