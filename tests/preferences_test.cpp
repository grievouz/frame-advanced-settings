#include "preferences.h"
#include "setting_resets.h"
#include <chrono>
#include <cstdio>
#include <stdexcept>

namespace fs = std::filesystem;
static int checks = 0;
static void check(bool value, const char *message) {
    ++checks;
    if (!value)
        throw std::runtime_error(message);
}
static void write(const fs::path &path, const std::string &value) {
    std::ofstream output(path, std::ios::binary);
    output << value;
    check(bool(output), "write preference fixture");
}
struct Runtime : drag::SpaceAccess {
    int calls = 0;
    int writes = 0;
    bool readable = false, backupOk = true;
    drag::Space current;
    Runtime() {
        current.universe = 1;
        current.bounds.push_back(
            {drag::Vec{-1, 0, -1}, drag::Vec{-1, 2, -1}, drag::Vec{1, 2, -1}, drag::Vec{1, 0, -1}});
    }
    bool read(drag::Space &result) override {
        ++calls;
        result = current;
        return readable;
    }
    bool write(const drag::Space &target, drag::WriteMode) override {
        ++calls;
        ++writes;
        current = target;
        return true;
    }
    bool saveBaseline(const drag::Space &) override {
        ++calls;
        return backupOk;
    }
    void cancelPreview() override { ++calls; }
};
struct CommaLocale : std::numpunct<char> {
    char do_decimal_point() const override { return ','; }
};
int main() {
    using namespace drag;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = fs::current_path() / ("preferences-fixture-" + std::to_string(stamp));
    const auto path = root / "config/frame-advanced-settings/settings.ini";
    const Preferences defaults;
    check(defaults.xyz && defaults.gain == 2 && !defaults.distanceLimit && !defaults.gravity &&
              !defaults.movementEnabled,
          "first-launch defaults are all axes, two times speed, limit/gravity/movement off");
    auto loaded = loadPreferences(path);
    check(!loaded.found && loaded.problem.empty() && loaded.values == defaults && !fs::exists(root),
          "first launch uses defaults without writing configuration");
    const auto customConfig = (root / "xdg").string();
    const auto userHome = (root / "home").string();
    check(preferencesPath(customConfig.c_str(), userHome.c_str()) ==
              fs::path(customConfig) / "frame-advanced-settings/settings.ini",
          "absolute XDG_CONFIG_HOME selects the preference directory");
    check(preferencesPath("relative-path", userHome.c_str()) ==
              fs::path(userHome) / ".config/frame-advanced-settings/settings.ini",
          "relative XDG_CONFIG_HOME falls back to the home configuration directory");

    Runtime runtime;
    {
        Runtime resetRuntime;
        Session resetSession(resetRuntime);
        resetSession.engine.xyz = false;
        resetSession.engine.gain = .75;
        resetSession.gravity.setEnabled(true);
        resetSession.gravity.setStrength(7.8);
        resetSession.setDistanceLimit(true, 18.5);
        resetSession.restoreMovementPreference(true);
        resetSession.enabled = true;
        resetSession.engine.offset = {1, 2, -1};
        auto preserved = [&] {
            check(resetSession.enabled && resetSession.movementRequested() &&
                      resetSession.engine.offset.x == 1 && resetSession.engine.offset.y == 2 &&
                      resetSession.engine.offset.z == -1 && resetRuntime.calls == 0,
                  "A row reset preserves active movement/position and never touches the runtime");
            const auto saved = Preferences::capture(resetSession);
            std::string problem;
            check(savePreferences(root / "row-reset.ini", saved, problem) &&
                      loadPreferences(root / "row-reset.ini").values == saved,
                  "Row-reset values survive the existing preference persistence path");
        };
        check(resetSpaceDragSetting(Command::ResetDirection, resetSession) &&
                  resetSession.engine.xyz && resetSession.engine.gain == .75 &&
                  resetSession.gravity.enabled() && resetSession.engine.distanceLimit.enabled,
              "Direction reset changes only the axes row");
        preserved();
        check(resetSpaceDragSetting(Command::ResetGain, resetSession) &&
                  resetSession.engine.gain == 2 && resetSession.gravity.strength() == 7.8 &&
                  resetSession.engine.distanceLimit.meters() == 18.5,
              "Speed reset preserves gravity and distance settings");
        preserved();
        check(resetSpaceDragSetting(Command::ResetLimit, resetSession) &&
                  !resetSession.engine.distanceLimit.enabled &&
                  resetSession.engine.distanceLimit.meters() == 2 &&
                  resetSession.gravity.enabled() && resetSession.gravity.strength() == 7.8,
              "Limit reset restores both values in its row only");
        preserved();
        check(resetSpaceDragSetting(Command::ResetGravity, resetSession) &&
                  !resetSession.gravity.enabled() && resetSession.gravity.strength() == 9.8 &&
                  resetSession.engine.xyz && resetSession.engine.gain == 2,
              "Gravity reset restores both values in its row only");
        preserved();
        check(!resetSpaceDragSetting(Command::MicResetEcho, resetSession),
              "Other pages cannot reset movement preferences");
    }
    Session running(runtime);
    defaults.apply(running);
    check(running.engine.xyz && running.engine.gain == 2 && !running.engine.distanceLimit.enabled &&
              !running.gravity.enabled() && !running.enabled && !running.startupEnablePending() &&
              runtime.calls == 0,
          "first-launch defaults apply without enabling movement or querying the playspace");
    running.engine.xyz = true;
    running.engine.gain = 4.75;
    running.gravity.setEnabled(true);
    running.gravity.setStrength(7.8);
    running.setDistanceLimit(false, 18.5);
    running.restoreMovementPreference(true);
    running.enabled = true;
    running.engine.offset = {1, 2, -1};
    running.gravity.launch({1, 2, 0});
    const auto preferences = Preferences::capture(running);
    std::string problem;
    check(savePreferences(path, preferences, problem) && problem.empty(),
          "first preference change creates the directory and settings file");
    loaded = loadPreferences(path);
    check(loaded.found && loaded.problem.empty() && loaded.values == preferences,
          "all preferences including drag speed above 2x and unlimited distance round-trip exactly");
    {
        auto diagnosticPreferences = preferences;
        diagnosticPreferences.detailedLogging = true;
        diagnosticPreferences.automaticUpdateChecks = false;
        check(!defaults.detailedLogging && !(diagnosticPreferences == preferences),
              "detailed logging defaults off and changes preference identity");
        const auto diagnosticPath = root / "diagnostic-settings.ini";
        check(savePreferences(diagnosticPath, diagnosticPreferences, problem) &&
                  loadPreferences(diagnosticPath).values == diagnosticPreferences,
              "logging and automatic update choices survive restart");
    }
    Session restarted(runtime);
    loaded.values.apply(restarted);
    check(Preferences::capture(restarted) == preferences && !restarted.enabled &&
              restarted.startupEnablePending() && !restarted.needsRestore() &&
              length(restarted.engine.offset) == 0 && restarted.gravity.speed() == 0 &&
              !restarted.engine.held() && runtime.calls == 0,
          "saved On schedules activation without restoring motion or touching the playspace");

    Runtime startupRuntime;
    Session startup(startupRuntime);
    loaded.values.apply(startup);
    check(!startup.watch() && startup.startupEnablePending() && !startup.enabled &&
              startup.movementRequested() && startupRuntime.writes == 0 &&
              Preferences::capture(startup) == preferences,
          "unavailable startup calibration waits without losing the saved On preference");
    startupRuntime.readable = true;
    check(startup.watch() && startup.enabled && !startup.startupEnablePending() &&
              !startup.needsRestore() && startupRuntime.writes == 0 &&
              length(startup.engine.offset) == 0 && startup.gravity.speed() == 0,
          "saved On restores after validating and backing up the current playspace");
    startup.updateMotion(true, true, true, {0, -.1, 0}, .01);
    check(!startup.engine.held() && startupRuntime.writes == 0,
          "a drag button held across restart cannot start movement");
    startup.updateMotion(true, false, true, {}, .01);
    startup.updateMotion(true, true, true, {}, .01);
    startup.updateMotion(true, true, true, {.05, -.05, 0}, .01);
    check(startupRuntime.writes == 1 && startup.engine.offset.x < 0 && startup.engine.offset.y > 0,
          "fresh release and press permits dragging with the restored preferences");
    startup.disable();
    check(!startup.enabled && Preferences::capture(startup) == preferences,
          "shutdown cleanup does not replace the saved On choice with Off");
    startup.requestMovement(true);
    startup.abandon("Tracking stopped.");
    check(!startup.enabled && Preferences::capture(startup) == preferences,
          "automatic runtime pauses do not change the saved movement choice");
    startup.requestMovement(false);
    check(!startup.enabled && !startup.movementRequested() && !startup.startupEnablePending(),
          "explicit Off changes the saved choice and cancels pending activation");
    auto off = Preferences::capture(startup);
    check(savePreferences(path, off, problem) && !loadPreferences(path).values.movementEnabled,
          "explicit Off is persisted to disk");
    Session offStartup(startupRuntime);
    loadPreferences(path).values.apply(offStartup);
    check(offStartup.watch() && !offStartup.enabled && !offStartup.startupEnablePending(),
          "saved Off remains off on the next launch");
    preferences.apply(startup);
    startup.requestMovement(false);
    startup.watch();
    check(!startup.enabled && !startup.startupEnablePending(),
          "Off while startup activation is pending prevents automatic re-enabling");
    preferences.apply(startup);
    startupRuntime.backupOk = false;
    check(!startup.watch() && !startup.enabled && !startup.startupEnablePending() &&
              startup.movementRequested(),
          "failed startup backup stops activation without erasing the saved preference");

    auto changed = preferences;
    changed.xyz = false;
    changed.gravity = false;
    changed.distanceLimit = true;
    changed.distanceLimitMeters = .5;
    changed.movementEnabled = false;
    check(savePreferences(path, changed, problem) && loadPreferences(path).values == changed,
          "later changes replace an existing settings file and preserve false values");
    auto temporary = path;
    temporary += ".tmp";
    fs::create_directory(temporary);
    check(!savePreferences(path, preferences, problem) && !problem.empty() &&
              loadPreferences(path).values == changed,
          "failed staging leaves the previously saved preferences intact");
    fs::remove(temporary);
    check(savePreferences(path, preferences, problem) && problem.empty() &&
              loadPreferences(path).values == preferences,
          "a retry saves successfully once the write problem is resolved");

    write(path, "# user preferences\r\n  movement_axes = xyz \r\n"
                "drag_gain=1.5garbage\ngravity_enabled=maybe\ngravity_strength=nan\n"
                "distance_limit_enabled=false\ndistance_limit_m=-9\n"
                "enabled=true\noffset=2,3,4\nunknown_option=future\n");
    loaded = loadPreferences(path);
    check(!loaded.problem.empty() && loaded.values.xyz && loaded.values.gain == 2 &&
              !loaded.values.gravity && loaded.values.gravityStrength == 9.8 &&
              !loaded.values.distanceLimit && loaded.values.distanceLimitMeters == .5,
          "invalid values fall back individually while valid fields and bounded values load");
    Session malformed(runtime);
    loaded.values.apply(malformed);
    check(!malformed.enabled && length(malformed.engine.offset) == 0 && runtime.calls == 0,
          "unknown run-state keys cannot reactivate movement or restore an offset");
    check(!loaded.values.movementEnabled,
          "old preference files without movement_enabled default to Off");
    write(path, "movement_enabled=true\n");
    check(loadPreferences(path).values.movementEnabled,
          "new movement_enabled key restores the explicit preference");
    write(path, "movement_enabled=maybe\n");
    loaded = loadPreferences(path);
    check(!loaded.problem.empty() && !loaded.values.movementEnabled,
          "invalid movement choice defaults to Off");
    write(path, "drag_gain=99\ngravity_strength=-1\ndistance_limit_m=10000\n");
    loaded = loadPreferences(path);
    check(loaded.values.gain == 5 && loaded.values.gravityStrength == 1 &&
              loaded.values.distanceLimitMeters == 100,
          "manual numeric settings remain within supported ranges");
    write(path, "distance_limit_m=inf\ngravity_strength=7,8\n");
    loaded = loadPreferences(path);
    check(!loaded.problem.empty() && loaded.values == defaults,
          "non-finite and locale-dependent values are rejected");
    write(path, std::string(16385, 'x'));
    loaded = loadPreferences(path);
    check(!loaded.problem.empty() && loaded.values == defaults,
          "oversized settings files fall back without parsing");

    const auto previousLocale = std::locale();
    std::locale::global(std::locale(previousLocale, new CommaLocale));
    check(savePreferences(path, preferences, problem) &&
              loadPreferences(path).values == preferences,
          "saved decimal values round-trip independently of the system locale");
    std::locale::global(previousLocale);
    const auto blocker = root / "blocked";
    write(blocker, "ordinary file");
    check(!savePreferences(blocker / "settings.ini", preferences, problem) && !problem.empty(),
          "unavailable configuration directory reports a failure");
    check(!fs::exists(temporary), "successful replacement leaves no partial settings file");
    std::printf("%d preference checks passed\n", checks);
}
