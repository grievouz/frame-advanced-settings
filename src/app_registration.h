#pragma once
#include "openvr.h"
#include <algorithm>
#include <array>
#include <filesystem>
#include <string>
#include <utility>

namespace appregistration {
// This stable application identifier owns the user's SteamVR bindings.
inline constexpr const char *key = "local.frame-space-drag";

namespace detail {
inline int hexDigit(char value) {
    if (value >= '0' && value <= '9')
        return value - '0';
    if (value >= 'a' && value <= 'f')
        return value - 'a' + 10;
    if (value >= 'A' && value <= 'F')
        return value - 'A' + 10;
    return -1;
}

inline bool matchesPath(const std::string &value, const std::filesystem::path &expected) {
    std::string path = value;
    if (path.compare(0, 7, "file://") == 0) {
        path.erase(0, 7);
        std::string decoded;
        for (size_t i = 0; i < path.size(); ++i) {
            if (path[i] != '%') {
                decoded += path[i];
                continue;
            }
            if (i + 2 >= path.size())
                return false;
            const int high = hexDigit(path[i + 1]), low = hexDigit(path[i + 2]);
            if (high < 0 || low < 0 || (high == 0 && low == 0))
                return false;
            decoded += static_cast<char>((high << 4) | low);
            i += 2;
        }
        path = std::move(decoded);
#ifdef _WIN32
        // file:///C:/... is the standard URL form of a Windows absolute path.
        if (path.size() > 3 && path[0] == '/' && path[2] == ':')
            path.erase(0, 1);
#endif
    }
    return std::filesystem::path(path).lexically_normal() == expected.lexically_normal();
}
} // namespace detail

// A successful AddApplicationManifest can retain another manifest with the same
// key. Check the resolved properties before using that identity for input.
template <class Applications>
std::string registerCurrent(Applications &apps, const std::filesystem::path &root) {
    const auto manifest = root / "frame-advanced-settings.vrmanifest";
    const auto added = apps.AddApplicationManifest(manifest.string().c_str(), false);
    if (added != vr::VRApplicationError_None)
        return "Register application manifest failed (OpenVR error " +
               std::to_string(static_cast<int>(added)) + "): " + manifest.string();

    const auto readProperty = [&](vr::EVRApplicationProperty property, const char *label,
                                  std::string &value) -> std::string {
        std::array<char, 16384> buffer{};
        vr::EVRApplicationError error = vr::VRApplicationError_None;
        const auto required = apps.GetApplicationPropertyString(
            key, property, buffer.data(), static_cast<uint32_t>(buffer.size()), &error);
        if (error != vr::VRApplicationError_None)
            return std::string("Cannot read registered ") + label + " (OpenVR error " +
                   std::to_string(static_cast<int>(error)) + ")";
        const auto end = std::find(buffer.begin(), buffer.end(), '\0');
        if (required > buffer.size() || end == buffer.end())
            return std::string("Registered ") + label + " exceeds the application path buffer";
        value.assign(buffer.begin(), end);
        if (value.empty())
            return std::string("Registered ") + label + " is empty";
        return {};
    };

    std::string binary, action;
    if (auto error =
            readProperty(vr::VRApplicationProperty_BinaryPath_String, "binary path", binary);
        !error.empty())
        return error;
    if (auto error = readProperty(vr::VRApplicationProperty_ActionManifestURL_String,
                                  "action manifest URL", action);
        !error.empty())
        return error;
    const auto expectedBinary = root / "frame-advanced-settings";
    const auto expectedAction = root / "input/actions.json";
    if (!detail::matchesPath(binary, expectedBinary))
        return "Application registration has a different binary: " + binary + "; expected " +
               expectedBinary.string();
    if (!detail::matchesPath(action, expectedAction))
        return "Application registration has a different action manifest: " + action +
               "; expected " + expectedAction.string();
    return {};
}
} // namespace appregistration
