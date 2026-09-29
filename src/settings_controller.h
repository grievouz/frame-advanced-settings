#pragma once
#include "panel.h"
#include "process_command.h"
#include "update.h"
#include "build_version.h"
#include "session_log.h"
#include <future>
#include <fstream>

// All networking and systemd calls run away from the tracking/render loop.
class SettingsController {
    using Clock = std::chrono::steady_clock;
    struct StartupResult { bool available = false, enabled = false; std::string error; };
    struct UpdateResult {
        std::optional<updates::Release> release;
        bool installing = false;
        std::string message;
    };
    std::filesystem::path root_, state_;
    std::future<StartupResult> startupJob_;
    std::future<UpdateResult> updateJob_;
    StartupResult startup_;
    UpdateResult update_;
    Clock::time_point startupAt_{}, checkAt_ = Clock::now() + std::chrono::seconds(15), monitorAt_{};

    void startStartup(std::optional<bool> value = {}) {
        if (startupJob_.valid()) return;
        startupJob_ = std::async(std::launch::async, [value] {
            StartupResult result;
            if (value) {
                const auto changed = process::runCommand({"systemctl", "--user", *value ? "enable" : "disable",
                                                          "frame-advanced-settings.service"});
                sessionlog::info("startup", std::string(*value ? "Enable: " : "Disable: ") + changed.output);
                if (!changed.ok) result.error = "Couldn't change automatic startup. See the app log.";
            }
            const auto read = process::runCommand({"systemctl", "--user", "is-enabled", "frame-advanced-settings.service"});
            result.enabled = read.output == "enabled\n" || read.output == "enabled-runtime\n";
            result.available = result.enabled || read.output == "disabled\n";
            if (!result.available && result.error.empty())
                result.error = "Startup service unavailable. Install the app using its installer.";
            return result;
        });
        startupAt_ = Clock::now() + std::chrono::seconds(5);
    }
    void check() {
        if (!build::selfUpdates || updateJob_.valid() || update_.installing) return;
        update_.release.reset();
        update_.message = "Checking for updates...";
        checkAt_ = Clock::now() + std::chrono::hours(6);
        updateJob_ = std::async(std::launch::async, [] {
            UpdateResult result;
            try {
                const auto response = process::runCommand(
                    {"curl", "--disable", "--fail", "--silent", "--show-error", "--location",
                     "--proto", "=https", "--proto-redir", "=https", "--connect-timeout", "5", "--max-time", "15",
                     "--max-filesize", "2097152", "--user-agent", "frame-advanced-settings",
                     "--header", "Accept: application/vnd.github+json",
                     "https://api.github.com/repos/grievouz/frame-advanced-settings/releases?per_page=100"},
                    std::chrono::seconds(17), 2 * 1024 * 1024);
                if (!response.ok) throw std::runtime_error(response.output);
                result.release = updates::select(response.output, build::version);
                result.message = result.release ? "Version " + result.release->version + " is available."
                                                : "You're up to date.";
                sessionlog::info("updates", result.message);
            } catch (const std::exception &e) {
                sessionlog::warn("updates", std::string("Check failed: ") + e.what());
                result.message = "Couldn't check for updates. Check your connection and try again.";
            }
            return result;
        });
    }
    static UpdateResult installationStatus(const std::filesystem::path &state, UpdateResult result) {
        // The helper owns this small, atomically replaced status file.
        std::error_code error;
        const auto path = state / "update-status.txt";
        const auto size = std::filesystem::file_size(path, error);
        if (!error && size < 4096) {
            std::ifstream file(path);
            std::string phase, message;
            std::getline(file, phase);
            std::getline(file, message);
            if (phase == "done" || phase == "error" || phase == "busy") {
                result.installing = phase == "busy";
                result.message = message;
                if (phase == "done") result.release.reset();
            }
        }
        const auto active = process::runCommand({"systemctl", "--user", "is-active", "--quiet",
                                                "frame-advanced-settings-update.service"});
        if (active.ok) result.installing = true;
        else if (result.installing) {
            result.installing = false;
            result.message = "Update stopped before finishing. Check for updates to retry.";
        }
        return result;
    }
    void publish(PanelState &panel) const {
        panel.selfUpdates = build::selfUpdates;
        panel.startup = startup_.enabled;
        panel.startupAvailable = startup_.available;
        panel.startupBusy = startupJob_.valid() || update_.installing;
        panel.startupStatus = startup_.error;
        panel.updateBusy = updateJob_.valid() || update_.installing;
        panel.updateAvailable = update_.release.has_value();
        panel.updateStatus = update_.message;
    }
  public:
    SettingsController(std::filesystem::path root, std::filesystem::path state)
        : root_(std::move(root)), state_(std::move(state)) {
        startStartup();
        if (build::selfUpdates)
            updateJob_ = std::async(std::launch::async, [state = state_] { return installationStatus(state, {}); });
    }
    void command(Command command, PanelState &panel) {
        if (command == Command::StartupOff || command == Command::StartupOn) {
            if (startup_.available && !update_.installing) startStartup(command == Command::StartupOn);
        } else if (command == Command::CheckUpdate) check();
        else if (command == Command::InstallUpdate && build::selfUpdates &&
                 update_.release && !update_.installing && !updateJob_.valid()) {
            update_.installing = true;
            update_.message = "Starting update...";
            updateJob_ = std::async(std::launch::async, [root = root_, result = update_]() mutable {
                const auto started = process::runCommand(
                    {"systemd-run", "--user", "--collect", "--unit=frame-advanced-settings-update",
                     "--property=Type=exec", "/usr/bin/bash", (root / "update.sh").string(), result.release->version});
                if (!started.ok) {
                    result.installing = false;
                    result.message = "Couldn't start the update. See the app log.";
                    sessionlog::warn("updates", started.output);
                }
                return result;
            });
            monitorAt_ = Clock::now() + std::chrono::seconds(3);
        }
        publish(panel);
    }
    void poll(PanelState &panel, bool visible) {
        if (startupJob_.valid() && startupJob_.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
            startup_ = startupJob_.get();
        if (updateJob_.valid() && updateJob_.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
            update_ = updateJob_.get();
        if (visible && !startupJob_.valid() && Clock::now() >= startupAt_ && !update_.installing) startStartup();
        if (update_.installing && !updateJob_.valid() && Clock::now() >= monitorAt_) {
            monitorAt_ = Clock::now() + std::chrono::seconds(3);
            updateJob_ = std::async(std::launch::async, [state = state_, result = update_] {
                return installationStatus(state, result);
            });
        }
        if (build::selfUpdates && panel.automaticUpdateChecks && Clock::now() >= checkAt_) check();
        publish(panel);
    }
};
