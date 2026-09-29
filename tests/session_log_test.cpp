#include "session_log.h"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
static int checks = 0;
static void check(bool condition, const char *description) {
    ++checks;
    if (!condition)
        throw std::runtime_error(description);
}
static std::string read(const fs::path &path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), {});
}
static void write(const fs::path &path, const std::string &value) {
    std::ofstream file(path, std::ios::binary);
    file << value;
    check(bool(file), "fixture is writable");
}
static std::vector<fs::path> sessions(const fs::path &root) {
    std::vector<fs::path> result;
    for (const auto &entry : fs::directory_iterator(root))
        if (entry.is_directory() && fs::exists(entry.path() / ".frame-session"))
            result.push_back(entry.path());
    std::sort(result.begin(), result.end());
    return result;
}
static std::string allLogs(const fs::path &session) {
    std::string result;
    for (const auto &file : fs::directory_iterator(session))
        if (file.path().extension() == ".jsonl")
            result += read(file.path());
    return result;
}

int main() {
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = fs::current_path() / ("session-log-fixture-" + std::to_string(unique));
    fs::create_directories(root);
    try {
        sessionlog::Limits limits;
        limits.fileBytes = 4096;
        limits.recordBytes = 2048;
        limits.queueBytes = 16384;
        limits.queueRecords = 16;
        const auto logs = root / "logs";
        check(sessionlog::start(logs, "test-build", limits), "logger starts with bounded limits");
        check(!sessionlog::start(logs, "duplicate", limits), "cannot replace a running session");
        const auto first = sessionlog::sessionDirectory();
        sessionlog::setDetailed(false);
        sessionlog::debug("test", "hidden diagnostic");
        sessionlog::info("test", "newline\nquote\"slash\\");
        sessionlog::info("unicode", u8"Größe");
        sessionlog::info("invalid_utf8", std::string("broken ") + char(0xff));
        sessionlog::eventJson("snapshot", "{\"precise_universe\":5624895237226323188}");
        sessionlog::eventJson("pretty_json",
                              "{\n  \"value\": \"spaces stay, \\\"quote\\\" too\"\n}\n");
        sessionlog::setDetailed(true);
        sessionlog::debug("test", "visible diagnostic");
        check(sessionlog::flush(), "explicit flush completes queued writes");
        auto text = allLogs(first);
        check(text.find("hidden diagnostic") == std::string::npos &&
                  text.find("visible diagnostic") != std::string::npos,
              "only detailed text is gated by the developer toggle");
        check(text.find("\"data\":{\"precise_universe\":5624895237226323188}") != std::string::npos,
              "snapshot JSON is always recorded and keeps 64-bit IDs exact");
        check(text.find("\"data\":{\"value\":\"spaces stay, \\\"quote\\\" too\"}") !=
                  std::string::npos,
              "pretty-printed JSON becomes one complete line without changing string contents");
        check(text.find("newline\\u000aquote\\\"slash\\\\") != std::string::npos,
              "user text cannot inject new JSONL records");
        check(text.find(u8"Größe") != std::string::npos &&
                  text.find("broken \\ufffd") != std::string::npos,
              "valid UTF-8 survives and invalid native strings cannot corrupt JSON");
        sessionlog::eventJson("long_snapshot", "{\"value\":\"" + std::string(20000, 'x') + "\"}");
        sessionlog::info("long_message", std::string(30000, '\n'));
        check(sessionlog::flush(), "oversized entries flush");
        text = allLogs(first);
        check(text.find("\"data_preview\":") != std::string::npos &&
                  text.find("\"truncated\":true") != std::string::npos &&
                  text.find("\"original_bytes\":30000") != std::string::npos,
              "oversized data becomes an explicitly truncated valid JSON string");
        check(sessionlog::statusText().find("truncated") != std::string::npos,
              "developer status surfaces truncation");
        sessionlog::shutdown();
        check(allLogs(first).find("Session ended;") != std::string::npos,
              "shutdown drains the worker and writes session counters");

        std::vector<fs::path> generated{first};
        for (int session = 0; session != 4; ++session) {
            check(sessionlog::start(logs, "rotation-test", limits), "successive session starts");
            generated.push_back(sessionlog::sessionDirectory());
            for (int item = 0; item != 50; ++item) {
                sessionlog::eventJson("rotation", "{\"item\":" + std::to_string(item) +
                                                      ",\"padding\":\"" + std::string(1300, 'z') +
                                                      "\"}");
                check(sessionlog::flush(), "rotation workload reaches disk without queue loss");
            }
            sessionlog::shutdown();
            std::uintmax_t total = 0;
            const auto retained = sessions(logs);
            check(retained.size() <= 3, "retention keeps at most three sessions including current");
            for (const auto &directory : retained) {
                std::size_t parts = 0;
                for (const auto &file : fs::directory_iterator(directory)) {
                    if (file.path().extension() != ".jsonl")
                        continue;
                    ++parts;
                    const auto size = file.file_size();
                    total += size;
                    check(size <= limits.fileBytes,
                          "no rotated or active file exceeds the byte cap");
                    const auto body = read(file.path());
                    std::size_t begin = 0;
                    while (begin < body.size()) {
                        const auto end = body.find('\n', begin);
                        check(end != std::string::npos && body[begin] == '{' &&
                                  (body[end - 1] == '}' ||
                                   (body[end - 1] == '\r' && body[end - 2] == '}')),
                              "rotation preserves whole JSONL records");
                        const auto line = body.substr(begin, end - begin);
                        check(
                            line.find("\"build\":\"rotation-test\"") != std::string::npos ||
                                line.find("\"build\":\"test-build\"") != std::string::npos,
                            "every retained record identifies its build after startup rotates out");
                        check(line.find("\"pid\":") != std::string::npos,
                              "each record retains the running process identity");
                        begin = end + 1;
                    }
                }
                check(parts <= limits.parts, "rotation keeps current plus three parts");
            }
            check(total <= limits.fileBytes * limits.parts * limits.sessions,
                  "all retained log data fits the total storage cap");
        }
        check(!fs::exists(generated[0]) && !fs::exists(generated[1]) &&
                  fs::exists(generated.back()),
              "restart retention removes oldest sessions and preserves the latest");

        // Foreign content must neither be treated as owned nor removed.
        fs::create_directory(logs / "my-important-folder");
        write(logs / "my-important-folder/keep.txt", "leave me");
        check(sessionlog::start(logs, "retention-test", limits),
              "foreign content does not block logging");
        sessionlog::shutdown();
        check(read(logs / "my-important-folder/keep.txt") == "leave me",
              "cleanup preserves foreign data");
        auto unsafe = sessions(logs).front();
        write(unsafe / "unrecognized.txt", "preserve this");
        check(!sessionlog::start(logs, "unsafe-cleanup", limits),
              "unexpected owned content prevents cleanup");
        check(read(unsafe / "unrecognized.txt") == "preserve this",
              "failed cleanup preserves unknown content");
        check(sessionlog::statusText().find("unavailable") != std::string::npos,
              "startup failure is visible without throwing into movement code");

        write(root / "ordinary-file", "not a directory");
        check(!sessionlog::start(root / "ordinary-file/logs", "bad-path", limits),
              "unwritable path fails safely");
        sessionlog::error("test", "safe even when logging is unavailable");
        check(!sessionlog::flush(), "flush reports unavailable logger");

        // Sustained pressure cannot grow the queue indefinitely; losing records
        // is explicit in both the visible status and the session-end record.
        auto pressure = limits;
        pressure.queueRecords = 1;
        check(sessionlog::start(root / "pressure", "queue-test", pressure),
              "pressure fixture starts");
        const auto pressurePath = sessionlog::sessionDirectory();
        const auto payload = "{\"padding\":\"" + std::string(1400, 'p') + "\"}";
        for (int i = 0; i != 10000; ++i)
            sessionlog::eventJson("pressure", payload);
        check(sessionlog::droppedRecords() > 0,
              "bounded queue exposes records lost under pressure");
        check(sessionlog::statusText().find("dropped") != std::string::npos,
              "pressure loss appears in developer status");
        sessionlog::shutdown();
        check(allLogs(pressurePath).find("dropped_records=") != std::string::npos,
              "session-end counters persist queue losses for bug reports");

        check(sessionlog::start(root / "write-failure", "failure-test", limits),
              "write-failure fixture starts");
        const auto failedPath = sessionlog::sessionDirectory();
        fs::create_directory(failedPath / "events.1.jsonl");
        write(failedPath / "events.1.jsonl/blocker", "simulate a failed rotation");
        for (int i = 0; i != 8; ++i) {
            sessionlog::eventJson("failure", payload);
            sessionlog::flush();
        }
        check(sessionlog::statusText().find("unavailable") != std::string::npos &&
                  !sessionlog::flush(),
              "worker I/O failure is reported without escaping into application code");
        sessionlog::error("failure", "ordinary errors still reach the journal after file failure");
        sessionlog::shutdown();

        // Link tests are skipped only when Windows denies unprivileged symlinks.
        const auto external = root / "outside";
        fs::create_directory(external);
        write(external / "untouched.txt", "external data");
        std::error_code ec;
        fs::create_directory_symlink(external, root / "linked-root", ec);
        if (!ec) {
            check(!sessionlog::start(root / "linked-root/logs", "link-test", limits),
                  "logger refuses linked root ancestors");
            check(read(external / "untouched.txt") == "external data",
                  "no linked data was traversed or removed");
        } else {
            std::cout << "Symlink creation unavailable on this host; link test skipped\n";
        }

        // Fixtures live under this freshly created unique current-directory child.
        check(root.parent_path() == fs::current_path() &&
                  root.filename().string().find("session-log-fixture-") == 0,
              "cleanup target stays inside the fixture workspace");
        fs::remove_all(root);
        std::cout << checks << " session logging checks passed\n";
        return 0;
    } catch (const std::exception &error) {
        sessionlog::shutdown();
        std::cerr << "Session logging test failed: " << error.what() << " (fixture " << root
                  << ")\n";
        return 1;
    }
}
