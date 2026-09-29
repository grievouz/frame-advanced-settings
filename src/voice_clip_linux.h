#pragma once
#include "voice_clip_player.h"
#include "session_log.h"
#include <atomic>
#include <charconv>
#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <cerrno>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;
namespace voiceclip {
// Internal helper enters before OpenVR/log initialization. Parent death kills
// capture even if it is stalled and cannot notice a closed stdout pipe.
inline int helper(bool record, const std::string &parentText) {
    int parent = 0;
    const auto parsed =
        std::from_chars(parentText.data(), parentText.data() + parentText.size(), parent);
    if (parsed.ec != std::errc{} || parsed.ptr != parentText.data() + parentText.size() ||
        parent <= 1)
        return 2;
    if (prctl(PR_SET_PDEATHSIG, SIGKILL) != 0 || getppid() != parent)
        return 2;
    // The spawn uses an empty signal mask, including SIGPIPE's default action.
    // Raw '-' is a pipe, never a filename; capture uses the default processed mic.
    execlp("pw-cat", "pw-cat", record ? "--record" : "--playback", "--raw", "--rate=48000",
           "--channels=1", "--channel-map=mono", "--format=s16", "--latency=50ms", "--properties",
           "{ application.name = \"Frame Advanced Settings\" node.virtual = false }", "-",
           static_cast<char *>(nullptr));
    return 127;
}

inline Result run(bool record, const std::shared_ptr<Control> &control,
                  const std::shared_ptr<const Audio> &clip) {
    Result result;
    if (control->stop)
        return result;
    // A closed playback pipe must report EPIPE, not terminate the dashboard.
    // Only this temporary worker blocks SIGPIPE; the spawned helper resets it.
    sigset_t block;
    sigemptyset(&block);
    sigaddset(&block, SIGPIPE);
    if (pthread_sigmask(SIG_BLOCK, &block, nullptr) != 0)
        return {{}, "Could not start audio worker."};
    int fd[2];
    if (pipe2(fd, O_CLOEXEC) != 0)
        return {{}, "Could not create audio pipe."};
    const int parentFd = record ? fd[0] : fd[1];
    if (fcntl(parentFd, F_SETFL, O_NONBLOCK) < 0) {
        close(fd[0]);
        close(fd[1]);
        return {{}, "Could not configure audio pipe."};
    }
    std::string parentId = std::to_string(getpid());
    const char *mode = record ? "--voice-record-helper" : "--voice-play-helper";
    char *args[] = {const_cast<char *>("/proc/self/exe"), const_cast<char *>(mode), parentId.data(),
                    nullptr};
    std::vector<char *> env;
    for (char **item = environ; item && *item; ++item) {
        const std::string value(*item);
        if (value.rfind("LC_ALL=", 0) && value.rfind("LD_PRELOAD=", 0) &&
            value.rfind("LD_LIBRARY_PATH=", 0))
            env.push_back(*item);
    }
    char locale[] = "LC_ALL=C";
    env.push_back(locale);
    env.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attr;
    const int actionError = posix_spawn_file_actions_init(&actions);
    const int attrError = posix_spawnattr_init(&attr);
    if (actionError || attrError) {
        if (!actionError)
            posix_spawn_file_actions_destroy(&actions);
        if (!attrError)
            posix_spawnattr_destroy(&attr);
        close(fd[0]);
        close(fd[1]);
        return {{}, "Could not initialize audio process."};
    }
    sigset_t empty, defaults;
    sigemptyset(&empty);
    sigemptyset(&defaults);
    sigaddset(&defaults, SIGPIPE);
    int error = posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
    error |= posix_spawnattr_setsigmask(&attr, &empty);
    error |= posix_spawnattr_setsigdefault(&attr, &defaults);
    error |= posix_spawn_file_actions_addopen(&actions, record ? 0 : 1, "/dev/null",
                                              record ? O_RDONLY : O_WRONLY, 0);
    error |= posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0);
    error |= posix_spawn_file_actions_adddup2(&actions, record ? fd[1] : fd[0], record ? 1 : 0);
    error |= posix_spawn_file_actions_addclose(&actions, fd[0]);
    error |= posix_spawn_file_actions_addclose(&actions, fd[1]);
    pid_t pid = -1;
    if (!error)
        error = posix_spawn(&pid, "/proc/self/exe", &actions, &attr, args, env.data());
    posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&actions);
    close(record ? fd[1] : fd[0]);
    if (error) {
        close(parentFd);
        sessionlog::warn("microphone", "Voice process spawn error=" + std::to_string(error));
        return {{}, "Could not start voice check."};
    }
    Capture capture;
    size_t written = 0;
    bool pipeOpen = true, exited = false, finished = false, failed = false;
    int status = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!control->stop && std::chrono::steady_clock::now() < deadline) {
        if (record) {
            uint8_t bytes[8192];
            const auto count = read(parentFd, bytes, sizeof(bytes));
            if (count > 0) {
                capture.append(bytes, size_t(count));
                control->captured = capture.size();
                if (capture.full()) {
                    finished = true;
                    break;
                }
            } else if (count == 0 || (errno != EAGAIN && errno != EINTR)) {
                failed = true;
                break;
            }
        } else if (pipeOpen) {
            const auto count = write(parentFd, clip->data() + written,
                                     std::min(size_t(8192), clip->size() - written));
            if (count > 0)
                written += size_t(count);
            else if (count < 0 && errno != EAGAIN && errno != EINTR) {
                failed = true;
                break;
            }
            if (written == clip->size()) {
                close(parentFd);
                pipeOpen = false;
            }
        }
        const auto waited = waitpid(pid, &status, WNOHANG);
        if (waited == pid) {
            exited = true;
            finished = !record && !pipeOpen && WIFEXITED(status) && WEXITSTATUS(status) == 0;
            break;
        }
        if (waited < 0 && errno != EINTR) {
            failed = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (pipeOpen)
        close(parentFd);
    if (!exited) {
        kill(pid, SIGTERM);
        const auto stopDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
        while (std::chrono::steady_clock::now() < stopDeadline) {
            const auto waited = waitpid(pid, &status, WNOHANG);
            if (waited == pid || (waited < 0 && errno == ECHILD)) {
                exited = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!exited) {
            kill(pid, SIGKILL);
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
            }
        }
    }
    if (control->stop)
        return result; // Never offer a partial/cancelled recording.
    if (finished && !failed) {
        if (record)
            result.samples = capture.take();
    } else {
        sessionlog::warn("microphone", "Voice check failed; capture=" + std::to_string(record) +
                                           " process_status=" + std::to_string(status));
        result.error = WIFEXITED(status) && WEXITSTATUS(status) == 127
                           ? "PipeWire audio tool is unavailable on this headset."
                       : record ? "Recording failed. Check that the microphone is available."
                                : "Playback failed. Check the headset audio output.";
    }
    return result;
}

} // namespace voiceclip
