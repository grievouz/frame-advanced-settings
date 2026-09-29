#pragma once
#include "launch_menu.h"
#include "panel.h"
#include "session_log.h"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <future>
#include <thread>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

// All scanning, journaling and desktop cache tools stay off the pose/render loop.
// Only this controller's owner touches its view; the worker returns a new value.
class LaunchMenuController {
    enum class Job { Load, Refresh, Enable, Visibility, Reset };
    struct Result {
        launchmenu::View view;
        bool loaded = false;
    };
    std::future<Result> pending_;
    launchmenu::View view_;
    bool loaded_ = false;
    std::shared_ptr<appicons::Loader> icons_ =
        std::make_shared<appicons::Loader>(appicons::Options::fromEnvironment());
    size_t page_ = 0;
    static constexpr size_t pageSize = 6;

    static bool cacheTool(const std::vector<std::string> &args) {
        std::vector<char *> argv;
        for (const auto &arg : args)
            argv.push_back(const_cast<char *>(arg.c_str()));
        argv.push_back(nullptr);
        std::vector<char *> env;
        for (char **item = environ; item && *item; ++item) {
            const std::string value(*item);
            if (value.rfind("LD_LIBRARY_PATH=", 0) && value.rfind("LD_PRELOAD=", 0) &&
                value.rfind("QT_PLUGIN_PATH=", 0) && value.rfind("QT_QPA_PLATFORM_PLUGIN_PATH=", 0))
                env.push_back(*item);
        }
        env.push_back(nullptr);
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
        posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
        posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);
        posix_spawnattr_t attributes;
        posix_spawnattr_init(&attributes);
        posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
        posix_spawnattr_setpgroup(&attributes, 0);
        pid_t pid = -1;
        const int error =
            posix_spawnp(&pid, argv[0], &actions, &attributes, argv.data(), env.data());
        posix_spawnattr_destroy(&attributes);
        posix_spawn_file_actions_destroy(&actions);
        if (error) {
            sessionlog::warn("launch-menu",
                             args.front() + " could not start; error=" + std::to_string(error));
            return false;
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        int status = 0;
        while (std::chrono::steady_clock::now() < deadline) {
            const auto waited = waitpid(pid, &status, WNOHANG);
            if (waited == pid) {
                const bool ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
                sessionlog::info("launch-menu", args.front() + " status=" + std::to_string(status));
                return ok;
            }
            if (waited < 0 && errno != EINTR)
                return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        kill(-pid, SIGKILL);
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
        sessionlog::warn("launch-menu", args.front() + " timed out");
        return false;
    }
    void start(Job job, PanelState &panel, bool value = false, launchmenu::Entry entry = {}) {
        if (busy())
            return;
        try {
            pending_ = std::async(std::launch::async, [job, value, entry = std::move(entry),
                                                       icons = icons_] {
                Result result;
                const std::string action = job == Job::Load      ? "load"
                                           : job == Job::Refresh ? "refresh"
                                           : job == Job::Reset   ? "reset"
                                           : job == Job::Enable  ? (value ? "enable" : "disable")
                                           : value               ? "show"
                                                                 : "hide";
                sessionlog::info("launch-menu",
                                 "Requested " + action + (entry.id.empty() ? "" : " " + entry.id));
                try {
                    const auto paths = launchmenu::Paths::fromEnvironment();
                    launchmenu::Store store(paths);
                    switch (job) {
                    case Job::Load:
                        result.view = store.resume();
                        break;
                    case Job::Enable:
                        result.view = store.enable(value);
                        break;
                    case Job::Visibility:
                        result.view = store.visibility(entry, value);
                        break;
                    case Job::Reset:
                        result.view = store.reset();
                        break;
                    case Job::Refresh: {
                        icons->clear();
                        result.view = store.resume();
                        const bool desktop =
                            cacheTool({"update-desktop-database", paths.user.string()});
                        const bool kde = cacheTool({"kbuildsycoca6", "--noincremental"});
                        if (result.view.message.empty() && (!desktop || !kde))
                            result.view.message = "Menu cache refresh failed. See the app log.";
                        break;
                    }
                    }
                    for (auto &item : result.view.entries)
                        item.icon = icons->load(item.iconName);
                    result.loaded = true;
                    sessionlog::info("launch-menu",
                                     "Action=" + action +
                                         " enabled=" + std::to_string(result.view.enabled) +
                                         " entries=" + std::to_string(result.view.entries.size()) +
                                         " " + result.view.message);
                } catch (const std::exception &error) {
                    sessionlog::error("launch-menu", error.what());
                    try {
                        result.view =
                            launchmenu::Store(launchmenu::Paths::fromEnvironment()).read();
                        result.loaded = true;
                    } catch (...) {
                    }
                    result.view.message = "Could not update Launch Menu. Refresh and retry; "
                                          "details are in the app log.";
                }
                return result;
            });
        } catch (const std::exception &error) {
            sessionlog::error("launch-menu", error.what());
            view_.message = "Could not start the menu update. Try again.";
        }
        publish(panel);
    }
    void publish(PanelState &panel) const {
        panel.launchEnabled = view_.enabled;
        panel.launchLoaded = loaded_;
        panel.launchBusy = busy();
        panel.launchPage = page_;
        panel.launchPages = std::max(size_t(1), (view_.entries.size() + pageSize - 1) / pageSize);
        panel.launchRows.clear();
        for (size_t i = page_ * pageSize;
             i < std::min(view_.entries.size(), (page_ + 1) * pageSize); ++i) {
            const auto &entry = view_.entries[i];
            panel.launchRows.push_back(
                {entry.name, entry.visible, entry.editable, entry.appId, entry.icon});
        }
        panel.launchStatus = busy() ? "" : view_.message;
    }

  public:
    bool busy() const { return pending_.valid(); }
    void load(PanelState &panel) { start(Job::Load, panel); }
    bool poll(PanelState &panel) {
        if (!busy() || pending_.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            return false;
        auto result = pending_.get();
        view_ = std::move(result.view);
        loaded_ = result.loaded;
        page_ = std::min(page_,
                         view_.entries.empty() ? size_t(0) : (view_.entries.size() - 1) / pageSize);
        publish(panel);
        return true;
    }
    void command(Command command, PanelState &panel) {
        if (busy())
            return;
        if (command == Command::LaunchRefresh) {
            start(Job::Refresh, panel);
            return;
        }
        if (!loaded_)
            return;
        if (command == Command::LaunchOn || command == Command::LaunchOff) {
            start(Job::Enable, panel, command == Command::LaunchOn);
        } else if (command == Command::LaunchReset) {
            start(Job::Reset, panel);
        } else if (command == Command::LaunchPrevious || command == Command::LaunchNext) {
            if (command == Command::LaunchPrevious && page_)
                --page_;
            if (command == Command::LaunchNext && (page_ + 1) * pageSize < view_.entries.size())
                ++page_;
            publish(panel);
        } else if (command >= Command::LaunchHide0 && command <= Command::LaunchShow5 &&
                   view_.enabled) {
            const auto slot =
                static_cast<size_t>(command) - static_cast<size_t>(Command::LaunchHide0);
            const auto index = page_ * pageSize + slot / 2;
            const bool visible = slot % 2 != 0;
            if (index < view_.entries.size() && view_.entries[index].editable &&
                view_.entries[index].visible != visible)
                start(Job::Visibility, panel, visible, view_.entries[index]);
        }
    }
};
