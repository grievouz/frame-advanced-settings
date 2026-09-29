#include "app_registration.h"
#include <cstring>
#include <iostream>
#include <stdexcept>

static void check(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}

struct Applications {
    std::string binary, action, addedManifest;
    bool temporary = true;
    vr::EVRApplicationError addError = vr::VRApplicationError_None;
    vr::EVRApplicationError propertyError = vr::VRApplicationError_None;
    bool oversized = false;
    vr::EVRApplicationError AddApplicationManifest(const char *path, bool isTemporary) {
        addedManifest = path;
        temporary = isTemporary;
        return addError;
    }
    uint32_t GetApplicationPropertyString(const char *appKey, vr::EVRApplicationProperty property,
                                          char *buffer, uint32_t capacity,
                                          vr::EVRApplicationError *error) {
        check(std::string(appKey) == appregistration::key, "Query the stable binding identity");
        *error = propertyError;
        const auto &value =
            property == vr::VRApplicationProperty_BinaryPath_String ? binary : action;
        if (oversized)
            return capacity + 1;
        if (value.size() + 1 <= capacity)
            std::memcpy(buffer, value.c_str(), value.size() + 1);
        else
            *error = vr::VRApplicationError_BufferTooSmall;
        return static_cast<uint32_t>(value.size() + 1);
    }
};

int main() {
    const auto root = std::filesystem::path("/home/steamos/Frame Tools/frame-advanced-settings");
    const auto binary = root / "frame-advanced-settings";
    const auto action = root / "input/actions.json";
    const auto current = [&] {
        Applications apps;
        apps.binary = binary.string();
        apps.action = action.string();
        return apps;
    };

    auto apps = current();
    check(appregistration::registerCurrent(apps, root).empty(), "Current raw paths are accepted");
    check(!apps.temporary, "Current manifest is registered persistently");
    check(std::filesystem::path(apps.addedManifest) == root / "frame-advanced-settings.vrmanifest",
          "Register the current manifest filename");

    apps = current();
    apps.action = "file:///home/steamos/Frame%20Tools/frame-advanced-settings/input/actions.json";
    check(appregistration::registerCurrent(apps, root).empty(),
          "File URL with encoded spaces resolves to the current actions");

    apps = current();
    apps.binary = "/home/steamos/.local/share/frame-space-drag/frame-space-drag";
    apps.action = "file:///home/steamos/.local/share/frame-space-drag/input/actions.json";
    check(!appregistration::registerCurrent(apps, root).empty(),
          "Add returning success cannot hide a retained old manifest");

    apps = current();
    apps.action = "file:///home/steamos/.local/share/frame-space-drag/input/actions.json";
    check(appregistration::registerCurrent(apps, root).find("action manifest") != std::string::npos,
          "Current binary with a stale action URL is rejected");

    apps = current();
    apps.addError = vr::VRApplicationError_InvalidManifest;
    check(appregistration::registerCurrent(apps, root).find("Register application") !=
              std::string::npos,
          "Registration API failures are reported");

    apps = current();
    apps.propertyError = vr::VRApplicationError_UnknownProperty;
    check(appregistration::registerCurrent(apps, root).find("Cannot read registered") !=
              std::string::npos,
          "Property API failures are reported");

    apps = current();
    apps.oversized = true;
    check(appregistration::registerCurrent(apps, root).find("buffer") != std::string::npos,
          "Oversized property values fail clearly");

    apps = current();
    apps.action = "file:///home/steamos/Frame%00Tools/frame-advanced-settings/input/actions.json";
    check(!appregistration::registerCurrent(apps, root).empty(),
          "Encoded NUL cannot truncate a URL");
    apps.action = "file:///home/steamos/Frame%2Tools/frame-advanced-settings/input/actions.json";
    check(!appregistration::registerCurrent(apps, root).empty(),
          "Malformed percent escape is rejected");

    std::cout << "Application registration identity and path checks passed\n";
}
