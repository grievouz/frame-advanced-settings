#include "session_log.h"

#include <spdlog/logger.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <mutex>
#include <regex>
#include <sstream>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace sessionlog {
namespace {
namespace fs = std::filesystem;
using Clock = std::chrono::system_clock;
constexpr const char *owner = "Frame Advanced Settings session logs v1\n";
std::mutex lifecycleMutex;
std::mutex statusMutex;
std::string startupFailure = "File logging has not started";
std::atomic<bool> verbose{false};

std::uint64_t unixMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch())
        .count();
}
std::uint64_t processId() {
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return static_cast<std::uint64_t>(getpid());
#endif
}
bool linkOrReparse(const fs::path &path) {
    if (fs::is_symlink(fs::symlink_status(path)))
        return true;
#ifdef _WIN32
    const auto flags = GetFileAttributesW(path.c_str());
    if (flags != INVALID_FILE_ATTRIBUTES && (flags & FILE_ATTRIBUTE_REPARSE_POINT))
        return true;
#endif
    return false;
}
void safeAncestors(const fs::path &path) {
    fs::path part;
    const auto absolute = fs::absolute(path).lexically_normal();
    for (const auto &piece : absolute) {
        part /= piece;
        if (fs::exists(part) && (linkOrReparse(part) || !fs::is_directory(part)))
            throw std::runtime_error("Log path contains a link or non-directory component");
    }
}

std::string quote(std::string_view input, std::size_t maximum) {
    std::string result = "\"";
    const auto length = std::min(input.size(), maximum);
    for (std::size_t i = 0; i < length; ++i) {
        const auto c = static_cast<unsigned char>(input[i]);
        if (c == '"' || c == '\\') {
            result += '\\';
            result += char(c);
        } else if (c < 0x20) {
            static const char hex[] = "0123456789abcdef";
            result += "\\u00";
            result += hex[c >> 4];
            result += hex[c & 15];
        } else if (c >= 0x80) {
            const unsigned count = c >= 0xc2 && c <= 0xdf   ? 2
                                   : c >= 0xe0 && c <= 0xef ? 3
                                   : c >= 0xf0 && c <= 0xf4 ? 4
                                                            : 0;
            bool valid = count && i + count <= length;
            for (unsigned n = 1; valid && n < count; ++n) {
                const auto next = static_cast<unsigned char>(input[i + n]);
                valid = next >= 0x80 && next <= 0xbf;
            }
            if (valid && count >= 3) {
                const auto next = static_cast<unsigned char>(input[i + 1]);
                valid = !(c == 0xe0 && next < 0xa0) && !(c == 0xed && next >= 0xa0) &&
                        !(c == 0xf0 && next < 0x90) && !(c == 0xf4 && next >= 0x90);
            }
            if (valid) {
                result.append(input.substr(i, count));
                i += count - 1;
            } else {
                result += "\\ufffd";
            }
        } else {
            result += char(c);
        }
    }
    result += '"';
    return result;
}
std::string record(std::uint64_t sequence, std::string_view level, std::string_view category,
                   std::string_view payload, bool json, std::size_t cap, std::string_view build,
                   std::uint64_t pid, bool *wasTruncated = nullptr) {
    std::string prefix = "{\"unix_ms\":" + std::to_string(unixMs()) +
                         ",\"seq\":" + std::to_string(sequence) + ",\"build\":" + quote(build, 64) +
                         ",\"pid\":" + std::to_string(pid) + ",\"level\":" + quote(level, 16) +
                         ",\"category\":" + quote(category, 24);
    const std::size_t reserve = prefix.size() + 100;
    const std::size_t budget = cap > reserve ? cap - reserve : 0;
    if (json && payload.size() <= budget) {
        std::string compact;
        compact.reserve(payload.size());
        bool inString = false, escaped = false;
        for (const char value : payload) {
            if (!inString && (value == ' ' || value == '\n' || value == '\r' || value == '\t'))
                continue;
            compact += value;
            if (inString && escaped) {
                escaped = false;
            } else if (inString && value == '\\') {
                escaped = true;
            } else if (value == '"') {
                inString = !inString;
            }
        }
        return prefix + ",\"data\":" + compact + "}";
    }
    const auto text = quote(payload, std::min(payload.size(), budget / 6));
    const bool truncated = payload.size() > budget / 6;
    if (wasTruncated)
        *wasTruncated = truncated;
    return prefix + (json ? ",\"data_preview\":" : ",\"message\":") + text +
           (truncated ? ",\"truncated\":true,\"original_bytes\":" + std::to_string(payload.size())
                      : "") +
           "}";
}
bool ownedName(const fs::path &path) {
    static const std::regex pattern("session-[0-9]{8}T[0-9]{12}Z-[0-9a-f]{8}");
    return std::regex_match(path.filename().string(), pattern);
}
bool logName(const fs::path &path) {
    static const std::regex pattern("events(\\.[1-9][0-9]*)?\\.jsonl");
    return std::regex_match(path.filename().string(), pattern);
}
struct Previous {
    fs::path path;
    std::uintmax_t bytes = 0;
    bool overLimit = false;
};
std::vector<Previous> previousSessions(const fs::path &root, const Limits &limits) {
    std::vector<Previous> result;
    for (const auto &entry : fs::directory_iterator(root)) {
        if (!ownedName(entry.path()) || linkOrReparse(entry.path()) || !entry.is_directory())
            continue;
        const auto marker = entry.path() / ".frame-session";
        if (!fs::exists(marker) || linkOrReparse(marker) || !fs::is_regular_file(marker) ||
            fs::file_size(marker) != std::char_traits<char>::length(owner))
            continue;
        std::ifstream input(marker, std::ios::binary);
        std::string markerValue((std::istreambuf_iterator<char>(input)), {});
        if (markerValue != owner)
            continue;
        Previous session{entry.path()};
        std::size_t parts = 0;
        for (const auto &file : fs::directory_iterator(entry.path())) {
            if (linkOrReparse(file.path()) || !file.is_regular_file() ||
                (file.path().filename() != ".frame-session" && !logName(file.path())))
                throw std::runtime_error(
                    "An owned log session has an unexpected file or link; preserved it");
            if (file.path().filename() == ".frame-session")
                continue;
            ++parts;
            const auto size = file.file_size();
            session.bytes += size;
            session.overLimit = session.overLimit || size > limits.fileBytes;
        }
        session.overLimit = session.overLimit || parts > limits.parts;
        result.push_back(session);
    }
    std::sort(result.begin(), result.end(), [](const Previous &a, const Previous &b) {
        return a.path.filename() < b.path.filename();
    });
    return result;
}
void removeOwned(const fs::path &path) {
    // All children were allowlisted regular files above. Never recurse, and
    // recheck immediately before removal so a replaced link is not traversed.
    if (linkOrReparse(path) || !fs::is_directory(path))
        throw std::runtime_error("Log session changed during cleanup");
    for (const auto &file : fs::directory_iterator(path)) {
        if (linkOrReparse(file.path()) || !file.is_regular_file() ||
            (file.path().filename() != ".frame-session" && !logName(file.path())))
            throw std::runtime_error("Log session changed during cleanup");
    }
    for (const auto &file : fs::directory_iterator(path))
        fs::remove(file.path());
    fs::remove(path);
}
fs::path prepareSession(const fs::path &input, const Limits &limits) {
    if (input.empty() || !input.is_absolute())
        throw std::runtime_error("Log root must be an absolute app-owned directory");
    const auto root = input.lexically_normal();
    safeAncestors(root);
    fs::create_directories(root);
    auto sessions = previousSessions(root, limits);
    for (auto i = sessions.begin(); i != sessions.end();) {
        if (i->overLimit) {
            removeOwned(i->path);
            i = sessions.erase(i);
        } else {
            ++i;
        }
    }
    while (sessions.size() >= limits.sessions) {
        removeOwned(sessions.front().path);
        sessions.erase(sessions.begin());
    }
    for (unsigned attempt = 0; attempt != 16; ++attempt) {
        const auto now = Clock::now();
        const auto timestamp = Clock::to_time_t(now);
        std::tm utc{};
#ifdef _WIN32
        gmtime_s(&utc, &timestamp);
#else
        gmtime_r(&timestamp, &utc);
#endif
        const auto micros =
            std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count() %
            1000000;
        const auto unique =
            std::uint32_t(std::chrono::steady_clock::now().time_since_epoch().count());
        std::ostringstream name;
        name << "session-" << std::put_time(&utc, "%Y%m%dT%H%M%S") << std::setfill('0')
             << std::setw(6) << micros << "Z-" << std::hex << std::setw(8) << unique;
        const auto path = root / name.str();
        if (!fs::create_directory(path))
            continue;
#ifndef _WIN32
        fs::permissions(path, fs::perms::owner_all, fs::perm_options::replace);
#endif
        std::ofstream marker(path / ".frame-session", std::ios::binary);
        marker << owner;
        marker.close();
        if (!marker)
            throw std::runtime_error("Cannot mark the new log session");
        return path;
    }
    throw std::runtime_error("Cannot allocate a unique log session");
}

struct Entry {
    std::string json;
    std::string console;
    bool urgent = false;
};
void consoleMessage(std::string_view level, std::string_view category,
                    std::string_view message) noexcept {
    std::fprintf(stderr, "[frame-advanced-settings] [%.*s] [%.*s] %.*s\n", int(level.size()),
                 level.data(), int(std::min<std::size_t>(category.size(), 64)), category.data(),
                 int(std::min<std::size_t>(message.size(), 4096)), message.data());
}
struct State {
    Limits limits;
    std::string build;
    std::uint64_t pid = processId();
    fs::path directory;
    std::mutex mutex;
    std::condition_variable wake, flushed;
    std::deque<Entry> queue;
    std::size_t queuedBytes = 0;
    bool stopping = false;
    std::uint64_t flushRequested = 0, flushCompleted = 0;
    std::string failure;
    std::atomic<std::uint64_t> dropped{0}, sequence{0}, truncated{0};
    std::shared_ptr<spdlog::logger> logger;
    std::thread worker;

    void failed(const std::string &text) noexcept {
        try {
            std::lock_guard<std::mutex> lock(mutex);
            if (failure.empty()) {
                failure = text;
                std::fprintf(stderr, "[frame-advanced-settings] File logging unavailable: %s\n",
                             text.c_str());
            }
        } catch (...) {
        }
    }
    void run() noexcept {
        std::uint64_t reportedDrops = 0;
        auto nextFlush = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
        auto nextLossReport = std::chrono::steady_clock::now();
        try {
            for (;;) {
                Entry entry;
                std::uint64_t requested = 0;
                bool stop = false, haveEntry = false, fileUsable = true;
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    wake.wait_until(lock, nextFlush, [&] {
                        return stopping || !queue.empty() || flushRequested > flushCompleted;
                    });
                    if (!queue.empty()) {
                        entry = std::move(queue.front());
                        queuedBytes -= entry.json.size();
                        queue.pop_front();
                        haveEntry = true;
                    } else {
                        requested = flushRequested;
                        stop = stopping;
                    }
                    fileUsable = failure.empty();
                }
                const auto lost = dropped.load();
                if (lost != reportedDrops &&
                    (std::chrono::steady_clock::now() >= nextLossReport || !haveEntry)) {
                    const auto warning = "Queue overloaded; dropped " + std::to_string(lost) +
                                         " records in this session (oldest first)";
                    if (fileUsable)
                        logger->warn(record(++sequence, "warn", "logging", warning, false,
                                            limits.recordBytes, build, pid));
                    consoleMessage("warn", "logging", warning);
                    reportedDrops = lost;
                    nextLossReport = std::chrono::steady_clock::now() + std::chrono::seconds(1);
                }
                if (haveEntry && fileUsable)
                    logger->info(entry.json);
                if (!entry.console.empty())
                    std::fprintf(stderr, "[frame-advanced-settings] %s\n", entry.console.c_str());
                if (stop) {
                    const auto summary =
                        "Session ended; dropped_records=" + std::to_string(dropped.load()) +
                        "; truncated_records=" + std::to_string(truncated.load());
                    if (fileUsable)
                        logger->info(record(++sequence, "info", "session", summary, false,
                                            limits.recordBytes, build, pid));
                    consoleMessage("info", "session", summary);
                }
                const auto now = std::chrono::steady_clock::now();
                if (entry.urgent || now >= nextFlush || requested || stop) {
                    if (fileUsable)
                        logger->flush();
                    nextFlush = now + std::chrono::milliseconds(250);
                    if (requested) {
                        std::lock_guard<std::mutex> lock(mutex);
                        flushCompleted = requested;
                        flushed.notify_all();
                    }
                }
                if (stop)
                    break;
            }
        } catch (const std::exception &ex) {
            failed(ex.what());
        } catch (...) {
            failed("Unexpected log worker failure");
        }
        std::lock_guard<std::mutex> lock(mutex);
        stopping = true;
        flushed.notify_all();
    }
    void enqueue(std::string_view level, std::string_view category, std::string_view payload,
                 bool json) noexcept {
        try {
            bool wasTruncated = false;
            Entry entry{record(++sequence, level, category, payload, json, limits.recordBytes,
                               build, pid, &wasTruncated),
                        {},
                        level == "error"};
            if (!json && level != "debug")
                entry.console = "[" + std::string(level) + "] [" +
                                std::string(category.substr(0, 64)) + "] " +
                                std::string(payload.substr(0, 4096));
            if (wasTruncated)
                ++truncated;
            std::lock_guard<std::mutex> lock(mutex);
            if (stopping) {
                if (!entry.console.empty())
                    consoleMessage(level, category, payload);
                return;
            }
            while (!queue.empty() && (queue.size() >= limits.queueRecords ||
                                      queuedBytes + entry.json.size() > limits.queueBytes)) {
                queuedBytes -= queue.front().json.size();
                queue.pop_front();
                ++dropped;
            }
            queuedBytes += entry.json.size();
            queue.push_back(std::move(entry));
            wake.notify_one();
        } catch (...) {
            ++dropped;
        }
    }
    ~State() {
        if (worker.joinable()) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                stopping = true;
                wake.notify_one();
            }
            worker.join();
        }
    }
};
std::shared_ptr<State> current;
std::shared_ptr<State> state() {
    return std::atomic_load(&current);
}
void emit(std::string_view level, std::string_view category, std::string_view value,
          bool json = false) noexcept {
    try {
        if (auto valueState = state())
            valueState->enqueue(level, category, value, json);
        else if (!json && level != "debug")
            consoleMessage(level, category, value);
    } catch (...) {
    }
}
} // namespace

bool start(const fs::path &root, std::string_view build, Limits limits) noexcept {
    try {
        std::lock_guard<std::mutex> lifecycle(lifecycleMutex);
        if (state())
            return false;
        if (limits.sessions < 1 || limits.sessions > 3 || limits.parts < 1 || limits.parts > 4 ||
            limits.fileBytes < 1024 || limits.fileBytes > 2 * 1024 * 1024 ||
            limits.recordBytes < 1024 || limits.recordBytes + 2 > limits.fileBytes ||
            limits.queueBytes < limits.recordBytes || limits.queueBytes > 2 * 1024 * 1024 ||
            limits.queueRecords == 0 || limits.queueRecords > 256)
            throw std::runtime_error("Invalid bounded log limits");
        auto created = std::make_shared<State>();
        created->limits = limits;
        created->build = std::string(build.substr(0, 64));
        created->directory = prepareSession(root, limits);
        auto sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            (created->directory / "events.jsonl").string(), limits.fileBytes, limits.parts - 1);
        created->logger = std::make_shared<spdlog::logger>("frame-session", sink);
        created->logger->set_pattern("%v");
        created->logger->set_error_handler(
            [weak = std::weak_ptr<State>(created)](const std::string &message) {
                if (auto locked = weak.lock())
                    locked->failed(message);
            });
        created->worker = std::thread([pointer = created.get()] { pointer->run(); });
        std::atomic_store(&current, created);
        info("session", "Frame Advanced Settings " + std::string(build));
        eventJson("log_limits", "{\"file_bytes\":" + std::to_string(limits.fileBytes) +
                                    ",\"parts_per_session\":" + std::to_string(limits.parts) +
                                    ",\"retained_sessions\":" + std::to_string(limits.sessions) +
                                    ",\"queue_bytes\":" + std::to_string(limits.queueBytes) + "}");
        return true;
    } catch (const std::exception &ex) {
        try {
            std::lock_guard<std::mutex> lock(statusMutex);
            startupFailure = std::string("File logging unavailable: ") + ex.what();
            std::fprintf(stderr, "[frame-advanced-settings] %s\n", startupFailure.c_str());
        } catch (...) {
        }
        return false;
    } catch (...) {
        return false;
    }
}
void info(std::string_view category, std::string_view message) noexcept {
    emit("info", category, message);
}
void warn(std::string_view category, std::string_view message) noexcept {
    emit("warn", category, message);
}
void error(std::string_view category, std::string_view message) noexcept {
    emit("error", category, message);
}
void debug(std::string_view category, std::string_view message) noexcept {
    if (verbose.load())
        emit("debug", category, message);
}
void eventJson(std::string_view category, std::string_view json) noexcept {
    emit("info", category, json, true);
}
void setDetailed(bool value) noexcept {
    if (verbose.exchange(value) != value)
        info("logging", value ? "Detailed diagnostics enabled" : "Detailed diagnostics disabled");
}
bool detailed() noexcept {
    return verbose.load();
}
bool flush() noexcept {
    try {
        if (auto active = state()) {
            std::unique_lock<std::mutex> lock(active->mutex);
            const auto request = ++active->flushRequested;
            active->wake.notify_one();
            return active->flushed.wait_for(
                       lock, std::chrono::seconds(2),
                       [&] { return active->flushCompleted >= request || active->stopping; }) &&
                   active->flushCompleted >= request && active->failure.empty();
        }
    } catch (...) {
    }
    return false;
}
void shutdown() noexcept {
    try {
        std::lock_guard<std::mutex> lifecycle(lifecycleMutex);
        auto active = std::atomic_exchange(&current, std::shared_ptr<State>{});
        if (!active)
            return;
        // Hold ownership until the worker has joined, including while its error
        // handler temporarily locks a weak_ptr to this state.
        {
            std::lock_guard<std::mutex> lock(active->mutex);
            active->stopping = true;
            active->wake.notify_one();
        }
        if (active->worker.joinable())
            active->worker.join();
        active.reset();
    } catch (...) {
    }
}
fs::path sessionDirectory() {
    if (auto active = state())
        return active->directory;
    return {};
}
std::string statusText() {
    if (auto active = state()) {
        std::lock_guard<std::mutex> lock(active->mutex);
        if (!active->failure.empty())
            return "File logging unavailable: " + active->failure;
        const auto drops = active->dropped.load();
        const auto cuts = active->truncated.load();
        return std::string("Recording this session") +
               (drops ? "; " + std::to_string(drops) + " records dropped" : "") +
               (cuts ? "; " + std::to_string(cuts) + " records truncated" : "");
    }
    std::lock_guard<std::mutex> lock(statusMutex);
    return startupFailure;
}
std::uint64_t droppedRecords() noexcept {
    if (auto active = state())
        return active->dropped.load();
    return 0;
}
} // namespace sessionlog
