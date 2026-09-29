#pragma once
#include "session_log.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <fcntl.h>
#include <spawn.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

// The archive runs in a separate process; compression never blocks pose polling.
// Arguments are passed directly to bash, without constructing a shell command.
class LogExport {
    pid_t child_ = -1;
    int output_ = -1;
    std::string captured_;
    std::chrono::steady_clock::time_point started_;
    bool timedOut_ = false;

    void drain() {
        std::array<char, 1024> buffer{};
        for (int i = 0; i < 8; ++i) {
            const auto count = read(output_, buffer.data(), buffer.size());
            if (count <= 0)
                break;
            captured_.append(buffer.data(), static_cast<size_t>(count));
            if (captured_.size() > 8192)
                captured_.erase(0, captured_.size() - 8192);
        }
    }

  public:
    std::string status;
    bool running() const { return child_ > 0; }
    bool start(const std::filesystem::path &installRoot) {
        if (running())
            return false;
        int pipeFds[2];
        if (pipe2(pipeFds, O_CLOEXEC) != 0) {
            status = "Could not start export. See the app log.";
            sessionlog::error("export", "pipe failed: " + std::to_string(errno));
            return false;
        }
        const int flags = fcntl(pipeFds[0], F_GETFL);
        if (flags < 0 || fcntl(pipeFds[0], F_SETFL, flags | O_NONBLOCK) < 0) {
            close(pipeFds[0]);
            close(pipeFds[1]);
            status = "Could not prepare export output. See the app log.";
            sessionlog::error("export", "Cannot make export output nonblocking");
            return false;
        }
        posix_spawn_file_actions_t actions;
        posix_spawnattr_t attributes;
        const int actionInit = posix_spawn_file_actions_init(&actions);
        const int attrInit = posix_spawnattr_init(&attributes);
        if (actionInit || attrInit) {
            if (!actionInit)
                posix_spawn_file_actions_destroy(&actions);
            if (!attrInit)
                posix_spawnattr_destroy(&attributes);
            close(pipeFds[0]);
            close(pipeFds[1]);
            status = "Could not prepare export process.";
            sessionlog::error("export", status);
            return false;
        }
        int setupError = posix_spawn_file_actions_adddup2(&actions, pipeFds[1], STDOUT_FILENO);
        if (!setupError)
            setupError = posix_spawn_file_actions_adddup2(&actions, pipeFds[1], STDERR_FILENO);
        if (!setupError)
            setupError = posix_spawn_file_actions_addclose(&actions, pipeFds[0]);
        if (!setupError)
            setupError = posix_spawn_file_actions_addclose(&actions, pipeFds[1]);
        if (!setupError)
            setupError = posix_spawnattr_setpgroup(&attributes, 0);
        if (!setupError)
            setupError = posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
        if (setupError) {
            posix_spawn_file_actions_destroy(&actions);
            posix_spawnattr_destroy(&attributes);
            close(pipeFds[0]);
            close(pipeFds[1]);
            status = "Could not prepare export process. See the app log.";
            sessionlog::error("export", "Process setup failed: " + std::to_string(setupError));
            return false;
        }
        std::string script = (installRoot / "export-logs.sh").string();
        std::array<char *, 3> args{const_cast<char *>("/bin/bash"), script.data(), nullptr};
        const int error =
            posix_spawn(&child_, "/bin/bash", &actions, &attributes, args.data(), environ);
        posix_spawn_file_actions_destroy(&actions);
        posix_spawnattr_destroy(&attributes);
        close(pipeFds[1]);
        if (error) {
            close(pipeFds[0]);
            child_ = -1;
            status = "Could not start export. See the app log.";
            sessionlog::error("export", "spawn failed: " + std::to_string(error));
            return false;
        }
        output_ = pipeFds[0];
        captured_.clear();
        timedOut_ = false;
        started_ = std::chrono::steady_clock::now();
        status = "Creating bug report...";
        sessionlog::info("export", "Started local bug-report archive");
        return true;
    }
    void poll() {
        if (!running())
            return;
        drain();
        if (!timedOut_ && std::chrono::steady_clock::now() - started_ > std::chrono::minutes(2)) {
            kill(-child_, SIGTERM);
            timedOut_ = true;
        }
        if (timedOut_ && std::chrono::steady_clock::now() - started_ > std::chrono::seconds(122))
            kill(-child_, SIGKILL);
        int result = 0;
        const pid_t done = waitpid(child_, &result, WNOHANG);
        if (done == 0 || (done < 0 && errno == EINTR))
            return;
        // The child may have written its final path between EAGAIN and waitpid.
        drain();
        close(output_);
        output_ = -1;
        child_ = -1;
        if (done > 0 && WIFEXITED(result) && WEXITSTATUS(result) == 0 && !timedOut_) {
            while (!captured_.empty() && (captured_.back() == '\n' || captured_.back() == '\r'))
                captured_.pop_back();
            const auto lastLine = captured_.find_last_of('\n');
            const auto path = captured_.substr(lastLine == std::string::npos ? 0 : lastLine + 1);
            std::error_code pathError;
            if (!std::filesystem::path(path).is_absolute() || path.size() < 7 ||
                path.substr(path.size() - 7) != ".tar.gz" ||
                !std::filesystem::is_regular_file(path, pathError)) {
                status = "Export returned no archive. See the app log.";
                sessionlog::error("export", status + " " + captured_);
                return;
            }
            status = "Logs exported.";
            sessionlog::info("export", "Bug report saved: " + path);
        } else {
            status = timedOut_ ? "Export timed out. See the app log."
                               : "Export failed. See the app log.";
            sessionlog::error("export", status + " " + captured_);
        }
    }
    ~LogExport() {
        if (output_ >= 0)
            close(output_);
        if (child_ > 0) {
            // Don't leave a compressor holding temporary staging after exit.
            kill(-child_, SIGTERM);
            waitpid(child_, nullptr, WNOHANG);
        }
    }
};
