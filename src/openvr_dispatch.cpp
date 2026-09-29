#include "openvr_dispatch.h"
#include <openvr.h>
#include <dlfcn.h>

namespace {
void *library = nullptr;
decltype(&vr::VR_InitInternal2) init = nullptr;
decltype(&vr::VR_ShutdownInternal) shutdown = nullptr;
decltype(&vr::VR_GetGenericInterface) getInterface = nullptr;
decltype(&vr::VR_IsInterfaceVersionValid) validInterface = nullptr;
decltype(&vr::VR_GetInitToken) initToken = nullptr;
decltype(&vr::VR_GetVRInitErrorAsEnglishDescription) errorDescription = nullptr;

template <class T> bool resolve(void *handle, T &function, const char *name, std::string &error) {
    dlerror();
    function = reinterpret_cast<T>(dlsym(handle, name));
    const char *detail = dlerror();
    if (!function || detail) {
        error = std::string("OpenVR loader is missing ") + name;
        if (detail)
            error += std::string(": ") + detail;
        return false;
    }
    return true;
}
} // namespace

bool loadOpenVR(const std::filesystem::path &applicationRoot, std::string &error) {
    if (library)
        return true;
    const auto path = applicationRoot / "lib/libopenvr_api.so";
    void *candidate = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!candidate) {
        error = std::string("Cannot load ") + path.string() + ": " + dlerror();
        return false;
    }
    if (!resolve(candidate, init, "VR_InitInternal2", error) ||
        !resolve(candidate, shutdown, "VR_ShutdownInternal", error) ||
        !resolve(candidate, getInterface, "VR_GetGenericInterface", error) ||
        !resolve(candidate, validInterface, "VR_IsInterfaceVersionValid", error) ||
        !resolve(candidate, initToken, "VR_GetInitToken", error) ||
        !resolve(candidate, errorDescription, "VR_GetVRInitErrorAsEnglishDescription", error)) {
        dlclose(candidate);
        return false;
    }
    library = candidate;
    return true;
}

std::filesystem::path openVRRuntimePath() {
    if (!library)
        return {};
    const auto getPath =
        reinterpret_cast<decltype(&vr::VR_GetRuntimePath)>(dlsym(library, "VR_GetRuntimePath"));
    if (!getPath)
        return {};
    char path[4096]{};
    uint32_t required = 0;
    if (!getPath(path, sizeof(path), &required) || required == 0 || required > sizeof(path) ||
        path[required - 1] != '\0')
        return {};
    return path;
}

// CMake renames these declarations/definitions to Frame_VR_* throughout this app.
// dlsym above always resolves the original, unmodified Valve entry points.
namespace vr {
uint32_t VR_CALLTYPE VR_InitInternal2(EVRInitError *error, EVRApplicationType type,
                                      const char *startup) {
    return init(error, type, startup);
}
void VR_CALLTYPE VR_ShutdownInternal() {
    shutdown();
}
void *VR_CALLTYPE VR_GetGenericInterface(const char *version, EVRInitError *error) {
    return getInterface(version, error);
}
bool VR_CALLTYPE VR_IsInterfaceVersionValid(const char *version) {
    return validInterface(version);
}
uint32_t VR_CALLTYPE VR_GetInitToken() {
    return initToken();
}
const char *VR_CALLTYPE VR_GetVRInitErrorAsEnglishDescription(EVRInitError error) {
    return errorDescription(error);
}
} // namespace vr
