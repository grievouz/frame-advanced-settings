#include "microphone.h"
#include <sstream>
#include <string_view>

namespace microphone {
namespace {
std::string_view trim(std::string_view value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == value.npos)
        return {};
    return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
}
} // namespace
const char *key(Backend backend, Filter filter) {
    if (backend == Backend::AdvancedSettings)
        return filter == Filter::Echo ? "frame-advanced-settings.mic-echo-cancel"
                                      : "frame-advanced-settings.mic-noise-suppression";
    if (backend == Backend::Compatible)
        return filter == Filter::Echo ? "frame-mic.echo-cancel" : "frame-mic.noise-suppression";
    return "";
}
std::optional<bool> parseSetting(const std::string &output, const std::string &keyName) {
    if (output.size() > 64 * 1024)
        return std::nullopt;
    std::istringstream lines(output);
    std::string line;
    bool found = false, seen = false;
    std::optional<bool> result;
    while (std::getline(lines, line)) {
        auto value = trim(line);
        if (value.substr(0, 5) == "- Id:") {
            found = trim(value.substr(5)) == keyName;
            if (found && seen)
                return std::nullopt; // Ambiguous duplicate block.
            if (found)
                seen = true;
        } else if (found && value.substr(0, 6) == "Value:") {
            if (result)
                return std::nullopt;
            value = trim(value.substr(6));
            const auto end = value.find_first_of(" \t");
            const auto token = value.substr(0, end);
            if (token == "true")
                result = true;
            else if (token == "false")
                result = false;
            else
                return std::nullopt;
        }
    }
    return result;
}
State parseSettings(const std::string &output) {
    for (auto backend : {Backend::AdvancedSettings, Backend::Compatible}) {
        auto echo = parseSetting(output, key(backend, Filter::Echo));
        auto noise = parseSetting(output, key(backend, Filter::Noise));
        if (echo && noise)
            return {backend, *echo, *noise, {}};
    }
    return {Backend::None, true, true,
            "Microphone controls are not loaded. Reboot the headset after installing this update."};
}
State Service::read() {
    const auto result = run_({"wpctl", "settings"});
    if (!result.ok)
        return {Backend::None, true, true, "Could not read microphone settings. See the app log."};
    return parseSettings(result.output);
}
State Service::apply(std::optional<Filter> filter, bool value) {
    const auto before = read();
    if (!before.available())
        return before;
    bool saved = true;
    for (auto target : {Filter::Echo, Filter::Noise}) {
        if (filter && *filter != target)
            continue;
        if (!run_({"wpctl", "settings", "--save", key(before.backend, target),
                   value ? "true" : "false"})
                 .ok) {
            saved = false;
            break;
        }
    }
    auto after = read();
    const bool confirmed = after.backend == before.backend &&
                           (filter == Filter::Noise || after.echo == value) &&
                           (filter == Filter::Echo || after.noise == value);
    if (!saved || !confirmed)
        after.message = "Could not save all filter changes. See the app log and try again.";
    return after;
}
} // namespace microphone
