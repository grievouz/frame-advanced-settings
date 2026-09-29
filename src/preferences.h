#pragma once
#include "playspace.h"
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <locale>
#include <sstream>
#include <system_error>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace drag {
// User preferences only. The movement choice is separate from temporary runtime
// pauses. Tracking reference, offset, held input and velocity are never saved.
struct Preferences {
    bool xyz = true;
    double gain = 2;
    bool gravity = false;
    double gravityStrength = 9.8;
    bool distanceLimit = false;
    double distanceLimitMeters = 2;
    bool movementEnabled = false;
    bool detailedLogging = false;
    bool automaticUpdateChecks = true;

    static Preferences capture(const Session &session) {
        return {session.engine.xyz,
                session.engine.gain,
                session.gravity.enabled(),
                session.gravity.strength(),
                session.engine.distanceLimit.enabled,
                session.engine.distanceLimit.meters(),
                session.movementRequested()};
    }
    void apply(Session &session) const {
        session.engine.xyz = xyz;
        session.engine.gain = gain;
        session.gravity.setEnabled(gravity);
        session.gravity.setStrength(gravityStrength);
        session.setDistanceLimit(distanceLimit, distanceLimitMeters);
        session.restoreMovementPreference(movementEnabled);
    }
    bool operator==(const Preferences &other) const {
        return xyz == other.xyz && gain == other.gain && gravity == other.gravity &&
               gravityStrength == other.gravityStrength && distanceLimit == other.distanceLimit &&
               distanceLimitMeters == other.distanceLimitMeters &&
               movementEnabled == other.movementEnabled && detailedLogging == other.detailedLogging &&
               automaticUpdateChecks == other.automaticUpdateChecks;
    }
};

inline std::filesystem::path preferencesPath(const char *configHome, const char *home) {
    if (configHome && *configHome && std::filesystem::path(configHome).is_absolute())
        return std::filesystem::path(configHome) / "frame-advanced-settings/settings.ini";
    if (home && *home)
        return std::filesystem::path(home) / ".config/frame-advanced-settings/settings.ini";
    throw std::runtime_error("HOME is not set");
}

namespace preferencesDetail {
inline std::string trim(const std::string &value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
}
inline bool boolean(const std::string &value, bool &result) {
    if (value != "true" && value != "false")
        return false;
    result = value == "true";
    return true;
}
inline bool number(const std::string &value, double &result, double low, double high) {
    std::istringstream input(value);
    input.imbue(std::locale::classic());
    double parsed;
    if (!(input >> parsed) || !std::isfinite(parsed))
        return false;
    input >> std::ws;
    if (!input.eof())
        return false;
    result = std::clamp(parsed, low, high);
    return true;
}
} // namespace preferencesDetail

struct PreferencesLoad {
    Preferences values;
    bool found = false;
    std::string problem;
};

inline PreferencesLoad loadPreferences(const std::filesystem::path &path) {
    PreferencesLoad result;
    std::error_code error;
    const bool exists = std::filesystem::exists(path, error);
    if (error) {
        result.problem = "Cannot inspect settings: " + error.message();
        return result;
    }
    if (!exists)
        return result;
    result.found = true;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size > 16384) {
        result.problem = "Settings file is unreadable or too large; using defaults.";
        return result;
    }
    std::ifstream input(path);
    if (!input) {
        result.problem = "Cannot read settings; using defaults.";
        return result;
    }
    std::string line;
    while (std::getline(input, line)) {
        line = preferencesDetail::trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';')
            continue;
        const auto equals = line.find('=');
        bool valid = equals != std::string::npos;
        if (valid) {
            const auto key = preferencesDetail::trim(line.substr(0, equals));
            const auto value = preferencesDetail::trim(line.substr(equals + 1));
            auto &p = result.values;
            if (key == "movement_axes") {
                valid = value == "height" || value == "xyz";
                if (valid)
                    p.xyz = value == "xyz";
            } else if (key == "drag_gain")
                valid = preferencesDetail::number(value, p.gain, .25, 5);
            else if (key == "gravity_enabled")
                valid = preferencesDetail::boolean(value, p.gravity);
            else if (key == "gravity_strength")
                valid = preferencesDetail::number(value, p.gravityStrength, 1, 20);
            else if (key == "distance_limit_enabled")
                valid = preferencesDetail::boolean(value, p.distanceLimit);
            else if (key == "distance_limit_m")
                valid = preferencesDetail::number(value, p.distanceLimitMeters, .5, 100);
            else if (key == "movement_enabled")
                valid = preferencesDetail::boolean(value, p.movementEnabled);
            else if (key == "detailed_logging")
                valid = preferencesDetail::boolean(value, p.detailedLogging);
            else if (key == "automatic_update_checks")
                valid = preferencesDetail::boolean(value, p.automaticUpdateChecks);
            // Unknown keys are ignored for forward compatibility.
        }
        if (!valid)
            result.problem = "Some saved settings are invalid; valid preferences were retained.";
    }
    if (input.bad()) {
        result.values = {};
        result.problem = "Could not finish reading settings; using defaults.";
    }
    return result;
}

inline bool savePreferences(const std::filesystem::path &path, const Preferences &p,
                            std::string &problem) {
    problem.clear();
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) {
        problem = "Cannot create settings directory: " + error.message();
        return false;
    }
    auto temporary = path;
    temporary += ".tmp";
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    output.imbue(std::locale::classic());
    output << std::boolalpha << std::setprecision(17) << "# Frame Advanced Settings preferences\n"
           << "movement_axes=" << (p.xyz ? "xyz" : "height") << '\n'
           << "drag_gain=" << p.gain << '\n'
           << "gravity_enabled=" << p.gravity << '\n'
           << "gravity_strength=" << p.gravityStrength << '\n'
           << "distance_limit_enabled=" << p.distanceLimit << '\n'
           << "distance_limit_m=" << p.distanceLimitMeters << '\n'
           << "movement_enabled=" << p.movementEnabled << '\n'
           << "detailed_logging=" << p.detailedLogging << '\n'
           << "automatic_update_checks=" << p.automaticUpdateChecks << '\n';
    output.close();
    if (!output) {
        problem = "Cannot write settings; previous saved preferences were kept.";
        return false;
    }
    // The app's instance lock serializes writers. Replace only after the entire
    // file is closed, leaving the previous file intact if staging fails.
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        error = std::error_code(GetLastError(), std::system_category());
#else
    std::filesystem::rename(temporary, path, error);
#endif
    if (error) {
        problem = "Cannot replace saved settings: " + error.message();
        return false;
    }
    return true;
}
} // namespace drag
