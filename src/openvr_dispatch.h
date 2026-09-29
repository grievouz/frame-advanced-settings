#pragma once
#include <filesystem>
#include <string>

// Call before any OpenVR function. The loader remains resident until process exit.
bool loadOpenVR(const std::filesystem::path &applicationRoot, std::string &error);
// Optional runtime location for installed UI fonts; empty when unavailable.
std::filesystem::path openVRRuntimePath();
