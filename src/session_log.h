#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace sessionlog {
struct Limits {
    std::size_t fileBytes = 2 * 1024 * 1024;
    std::size_t parts = 4;    // Current file plus rotated files.
    std::size_t sessions = 3; // Current session plus two previous sessions.
    std::size_t recordBytes = 512 * 1024;
    std::size_t queueBytes = 2 * 1024 * 1024;
    std::size_t queueRecords = 256;
};

// Startup/teardown are called from the application's owning thread, outside its
// motion loop. Logging never propagates exceptions; an unavailable log is shown
// in statusText(). Logs use a private worker, never disk I/O on the calling loop.
bool start(const std::filesystem::path &root, std::string_view build, Limits limits = {}) noexcept;
void info(std::string_view category, std::string_view message) noexcept;
void warn(std::string_view category, std::string_view message) noexcept;
void error(std::string_view category, std::string_view message) noexcept;
void debug(std::string_view category, std::string_view message) noexcept;
// The caller supplies a complete, valid serialized JSON value. This is always
// recorded, irrespective of the detailed toggle; use it for important snapshots.
// Oversized values become a quoted preview instead of invalid partial JSON.
void eventJson(std::string_view category, std::string_view json) noexcept;
void setDetailed(bool value) noexcept;
bool detailed() noexcept;
// Requests a disk flush and waits at most two seconds for the worker. Do not call
// from the motion loop. Ordinary records flush automatically every 250 ms.
bool flush() noexcept;
void shutdown() noexcept;
std::filesystem::path sessionDirectory();
std::string statusText();
std::uint64_t droppedRecords() noexcept;
} // namespace sessionlog
