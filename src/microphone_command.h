#pragma once
#include "microphone.h"
#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;
namespace microphone {
// Called only on the microphone worker. Bound both output and total time,
// including commands that close their output before exiting.
inline CommandResult runCommand(const std::vector<std::string> &args) {
    CommandResult result;
    if (args.empty())
        return result;
    int fd[2];
    if (pipe2(fd, O_CLOEXEC) != 0)
        return {false, "Could not create command pipe"};
    if (fcntl(fd[0], F_SETFL, O_NONBLOCK) < 0) {
        close(fd[0]);
        close(fd[1]);
        return {false, "Could not configure command pipe"};
    }
    std::vector<char *> argv, env;
    for (const auto &arg : args)
        argv.push_back(const_cast<char *>(arg.c_str()));
    argv.push_back(nullptr);
    for (char **item = environ; item && *item; ++item) {
        const std::string value(*item);
        if (value.rfind("LC_ALL=", 0) && value.rfind("LD_LIBRARY_PATH=", 0) &&
            value.rfind("LD_PRELOAD=", 0))
            env.push_back(*item);
    }
    char locale[] = "LC_ALL=C";
    env.push_back(locale);
    env.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attr;
    const int actionsError = posix_spawn_file_actions_init(&actions);
    const int attrError = posix_spawnattr_init(&attr);
    if (actionsError || attrError) {
        if (!actionsError)
            posix_spawn_file_actions_destroy(&actions);
        if (!attrError)
            posix_spawnattr_destroy(&attr);
        close(fd[0]);
        close(fd[1]);
        return {false, "Could not initialize command"};
    }
    int error = posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    error |= posix_spawn_file_actions_adddup2(&actions, fd[1], 1);
    error |= posix_spawn_file_actions_adddup2(&actions, fd[1], 2);
    error |= posix_spawn_file_actions_addclose(&actions, fd[0]);
    error |= posix_spawn_file_actions_addclose(&actions, fd[1]);
    error |= posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
    error |= posix_spawnattr_setpgroup(&attr, 0);
    pid_t pid = -1;
    if (!error)
        error = posix_spawnp(&pid, argv[0], &actions, &attr, argv.data(), env.data());
    posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&actions);
    close(fd[1]);
    if (error) {
        close(fd[0]);
        return {false, "Could not start command: " + std::to_string(error)};
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    bool exited = false, eof = false, failed = false;
    int status = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        char buffer[4096];
        // At most one read per iteration so a continuous writer cannot starve
        // the deadline or waitpid checks.
        if (!eof) {
            const auto count = read(fd[0], buffer, sizeof(buffer));
            if (count > 0) {
                if (result.output.size() + size_t(count) > 64 * 1024) {
                    failed = true;
                    break;
                }
                result.output.append(buffer, size_t(count));
            } else if (count == 0)
                eof = true;
            else if (errno != EINTR && errno != EAGAIN) {
                failed = true;
                break;
            }
        }
        if (!exited) {
            const auto waited = waitpid(pid, &status, WNOHANG);
            if (waited == pid)
                exited = true;
            else if (waited < 0 && errno != EINTR) {
                failed = true;
                break;
            }
        }
        if (exited && eof)
            break;
        pollfd pfd{fd[0], POLLIN, 0};
        // HUP is continuously ready after EOF; use an empty poll while waiting
        // for a process that has closed its output but is still running.
        poll(eof ? nullptr : &pfd, eof ? 0 : 1, 10);
    }
    close(fd[0]);
    if (failed || !exited || !eof) {
        kill(-pid, SIGKILL);
        if (!exited)
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
            }
        return {false, "Command exceeded its time/output limit or failed to read output"};
    }
    result.ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    return result;
}
} // namespace microphone
