#include "panel.h"
#include "build_version.h"
#include <cstdio>
#include <fstream>
#include <stdexcept>
static void check(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
int main(int argc, char **argv) {
    check(argc == 3 || argc == 4,
          "Usage: panel-preview OUTPUT.ppm FONT_DIRECTORY [STEAMVR_FONT_DIRECTORY]");
    check(Panel::hit(1180, 78) == Command::Disable, "Heading Off segment hit");
    check(Panel::hit(1400, 78) == Command::Enable, "Heading On segment hit");
    check(Panel::hit(440, 698) == Command::Reset, "Reset hit");
    check(Panel::hit(1180, 180) == Command::Height, "Height segment replaces old enable row");
    check(Panel::hit(1400, 180) == Command::XYZ, "XYZ segment replaces old enable row");
    check(Panel::hit(1200, 493) == Command::Bindings, "Native binding editor button hit");
    check(Panel::hit(890, 415) == Command::GravityOff, "Gravity off hit");
    check(Panel::hit(980, 415) == Command::GravityOn, "Gravity on hit");
    check(Panel::hit(1130, 415) == Command::GravityWeaker, "Gravity strength decrease hit");
    check(Panel::hit(1500, 415) == Command::GravityStronger, "Gravity strength increase hit");
    check(Panel::hit(890, 337) == Command::LimitOff, "Distance limit off hit");
    check(Panel::hit(980, 337) == Command::LimitOn, "Distance limit on hit");
    check(Panel::hit(1130, 337) == Command::LimitSmaller, "Distance limit decrease hit");
    check(Panel::hit(1500, 337) == Command::LimitLarger, "Distance limit increase hit");
    check(Panel::hit(1200, 450) == Command::None, "Gap is not clickable");
    check(Panel::hit(310, 10) == Command::None, "Background is not clickable");
    for (const auto page : {PanelPage::SpaceDrag, PanelPage::LaunchMenu, PanelPage::Microphone, PanelPage::Settings}) {
        check(Panel::hit(100, 60, page) == Command::PageSpaceDrag, "Space Drag navigation");
        check(Panel::hit(100, 132, page) == Command::PageLaunchMenu, "Launch Menu navigation");
        check(Panel::hit(100, 348, page) == Command::None, "About lives inside Settings");
        check(Panel::hit(0, 24, page) == Command::PageSpaceDrag,
              "Sidebar row starts at the left edge");
        check(Panel::hit(339, 95, page) == Command::PageSpaceDrag,
              "Sidebar row includes the entire selected rectangle");
        check(Panel::hit(2, 276, page) == Command::PageSettings,
              "Settings row is clickable before its icon");
        check(Panel::hit(338, 348, page) == Command::None,
              "Removed About sidebar row has no hit target");
        check(Panel::hit(340, 60, page) == Command::None,
              "Sidebar hit region does not enter the content gutter");
        check(Panel::hit(100, 96, page) == Command::PageLaunchMenu &&
                  Panel::hit(100, 168, page) == Command::PageMicrophone &&
                  Panel::hit(100, 240, page) == Command::PageSettings &&
                  Panel::hit(100, 312, page) == Command::None,
              "Adjacent sidebar row boundaries have no gaps");
        check(Panel::hit(100, 820, page) == Command::None, "Removed Quit area is not clickable");
    }
    check(Panel::hit(1400, 78, PanelPage::Settings) == Command::None,
          "Settings cannot enable hidden movement controls");
    check(Panel::hit(1200, 493, PanelPage::Settings) == Command::None,
          "Settings cannot open hidden controller bindings");
    check(Panel::hit(1180, 460, PanelPage::Settings) == Command::DebugLoggingOff,
          "Settings off changes logging, not movement");
    check(Panel::hit(1400, 460, PanelPage::Settings) == Command::DebugLoggingOn,
          "Settings on changes logging, not movement");
    check(Panel::hit(440, 698, PanelPage::Settings) == Command::None,
          "Settings cannot activate hidden reset or old export buttons");
    check(Panel::hit(1200, 555, PanelPage::Settings) == Command::ExportLogs,
          "Compact Settings export button hit");
    check(Panel::hit(1180, 78, PanelPage::LaunchMenu) == Command::LaunchOff, "Launch Menu Off control");
    check(Panel::hit(1400, 78, PanelPage::LaunchMenu) == Command::LaunchOn, "Launch Menu On control");
    check(Panel::hit(1180, 214, PanelPage::LaunchMenu) == Command::LaunchHide0, "First shortcut Hide");
    check(Panel::hit(1400, 604, PanelPage::LaunchMenu) == Command::LaunchShow5, "Last shortcut Show");
    check(Panel::hit(1400, 604, PanelPage::Settings) == Command::None, "Shortcut controls scoped to Launch Menu");
    check(Panel::hit(1200, 698, PanelPage::LaunchMenu) == Command::LaunchRefresh, "Refresh does not reset movement");
    check(Panel::hit(1400, 698, PanelPage::LaunchMenu) == Command::LaunchReset, "Launch Menu reset");
    check(Panel::hit(1180, 180, PanelPage::Microphone) == Command::MicEchoOff, "Echo switch");
    check(Panel::hit(1400, 280, PanelPage::Microphone) == Command::MicNoiseOn, "Noise switch");
    check(Panel::hit(1200, 389, PanelPage::Microphone) == Command::MicReset, "Filter reset");
    check(Panel::hit(1400, 78, PanelPage::Microphone) == Command::None, "Mic cannot toggle hidden movement");
    check(Panel::hit(600, 530, PanelPage::Microphone) == Command::VoiceRecord, "Record clip hit");
    check(Panel::hit(1040, 530, PanelPage::Microphone) == Command::VoicePlay, "Play clip hit");
    check(Panel::hit(1400, 530, PanelPage::Microphone) == Command::VoiceStop, "Stop clip hit");
    check(Panel::hit(600, 530, PanelPage::Settings) == Command::None, "Hidden recording control cannot activate");
    const auto base = std::filesystem::path(argv[1]).parent_path();
    Panel fallback(argv[2], base / "absent-steamvr-fonts");
    check(fallback.fontFamily() == "Inter (bundled)", "Missing runtime fonts use bundled fallback");
    // Test plain and encoded runtime fonts without redistributing Valve's assets.
    const auto fixture = base / "font-load-fixture";
    std::filesystem::create_directories(fixture);
    for (const auto &names : {std::make_pair("Inter-Regular.ttf", "motiva-sans-regular.ttf"),
                              std::make_pair("Inter-SemiBold.ttf", "motiva-sans-bold.ttf")}) {
        std::ifstream source(std::filesystem::path(argv[2]) / names.first, std::ios::binary);
        std::ofstream destination(fixture / names.second, std::ios::binary | std::ios::trunc);
        destination << source.rdbuf();
        check(source && destination, "Write plain font fixture");
    }
    Panel plainFonts(argv[2], fixture);
    check(plainFonts.fontFamily() == "Motiva Sans (installed SteamVR)",
          "Plain runtime font pair is selected");
    {
        std::ofstream encoded(fixture / "motiva-sans-regular.ttf", std::ios::binary);
        const std::string invalidHeader(32, '\x6d');
        encoded.write(invalidHeader.data(), invalidHeader.size());
    }
    Panel encodedFonts(argv[2], fixture);
    check(encodedFonts.fontFamily() == "Inter (bundled)",
          "Encoded runtime font is rejected before rasterization");
    Panel panel(argv[2], argc == 4 ? argv[3] : "");
    {
        PanelState settings;
        settings.page = PanelPage::Settings;
        settings.settingsScroll = 156;
        check(panel.hit(1200, 304, settings) == Command::DebugLoggingOff,
              "Scrolling keeps logging controls and their hit targets aligned");
        check(panel.hit(1200, 100, settings) == Command::None,
              "Scrolled controls cannot activate through the fixed heading");
        check(panel.hit(100, 270, settings) == Command::PageSettings,
              "Scrolling content does not move the sidebar");
        settings.updateAvailable = true;
        settings.updateBusy = true;
        check(panel.hit(1400, 214, settings) == Command::None,
              "Cannot install twice while update is busy");
        settings.updateBusy = false;
        check(panel.hit(1400, 214, settings) == Command::InstallUpdate,
              "Available update can install from its scrolled row");
        settings.selfUpdates = false;
        check(panel.hit(1400, 214, settings) == Command::None,
              "Steam builds have no hidden update action");
        settings.settingsScroll = 0;
        check(panel.hit(1180, 180, settings) == Command::None,
              "Startup control waits for actual service state");
    }
    {
        PanelState resets;
        auto resetX = [&](Command command, int y) {
            for (int x = 400; x < 828; ++x)
                if (panel.hit(x, y, resets) == command) return x;
            return -1;
        };
        check(resetX(Command::ResetDirection, 180) == -1 &&
                  resetX(Command::ResetGain, 259) == -1 &&
                  resetX(Command::ResetLimit, 337) == -1 &&
                  resetX(Command::ResetGravity, 415) == -1,
              "Default rows have no invisible reset hit targets");
        resets.xyz = false;
        resets.gain = .75;
        resets.distanceLimitMeters = 5; // Still Off: the row is nevertheless changed.
        resets.gravityStrength = 12; // Still Off.
        const int directionX = resetX(Command::ResetDirection, 180);
        check(directionX > 400 && resetX(Command::ResetGain, 259) > 400 &&
                  resetX(Command::ResetLimit, 337) > 400 && resetX(Command::ResetGravity, 415) > 400,
              "Changed row resets are beside labels, before the unchanged control columns");
        check(panel.hit(1180, 180, resets) == Command::Height &&
                  panel.hit(1500, 259, resets) == Command::Faster &&
                  panel.hit(890, 337, resets) == Command::LimitOff &&
                  panel.hit(1500, 415, resets) == Command::GravityStronger,
              "Existing controls stay in their original hit regions");
        panel.render(resets);
        check(panel.save((base / "panel-row-resets.ppm").string().c_str()), "Save row reset preview");
        resets.xyz = true;
        check(panel.hit(directionX, 180, resets) == Command::None,
              "Restoring defaults removes the reset target immediately");
        resets.page = PanelPage::Settings;
        resets.debugLogging = true;
        check(resetX(Command::ResetDebugLogging, 460) > 400 &&
                  resetX(Command::ResetGain, 259) == -1,
              "Logging reset is page scoped");
        resets.page = PanelPage::Microphone;
        resets.micAvailable = true;
        resets.micEcho = false;
        resets.micNoise = false;
        const int micX = resetX(Command::MicResetEcho, 180);
        check(micX > 400 && resetX(Command::MicResetNoise, 275) > 400,
              "Each changed microphone filter has its own reset");
        resets.voiceBusy = true;
        check(panel.hit(micX, 180, resets) == Command::None,
              "Microphone reset cannot change processing during clip capture/playback");
        resets.voiceBusy = false;
        resets.micBusy = true;
        check(panel.hit(micX, 180, resets) == Command::None,
              "Microphone reset cannot overlap another settings write");
        resets.micBusy = false;
        resets.micAvailable = false;
        check(panel.hit(micX, 180, resets) == Command::None,
              "Unavailable microphone values are not treated as changed defaults");
    }
    PanelState state;
    PanelState changed = state;
    changed.distanceLimitMeters = 5;
    check(!(changed == state), "Distance value change redraws panel");
    changed = state;
    changed.distanceLimit = true;
    check(!(changed == state), "Distance toggle redraws panel");
    changed = state;
    changed.page = PanelPage::Settings;
    check(!(changed == state), "Navigation redraws panel");
    changed = state;
    changed.runtimeVersion = "2.15.6";
    check(!(changed == state), "Runtime information redraws panel");
    changed = state;
    changed.exportStatus = "Saved bug-report.tar.gz";
    check(!(changed == state), "Export result redraws panel");
    changed = state;
    changed.loggingStatus = "Log file could not be opened.";
    check(!(changed == state), "Logging failure redraws panel");
    state.status = "Ready when you are. Turn Space Drag on, then close the dashboard.";
    panel.render(state);
    const auto pixel = [&](int x, int y) {
        const auto at = static_cast<size_t>((y * Panel::width + x) * 4);
        return uint32_t(panel.pixels[at]) << 16 | uint32_t(panel.pixels[at + 1]) << 8 |
               uint32_t(panel.pixels[at + 2]);
    };
    check(pixel(0, 24) == 0x3d4450 && pixel(339, 95) == 0x3d4450,
          "Selected sidebar row is a full-width neutral rectangle");
    check(pixel(340, 60) == 0x0a0f14, "Selected row does not extend beyond the sidebar");
    for (const auto row : {24, 96, 168, 240}) {
        const uint32_t background = row == 24 ? 0x3d4450 : 0x22272b;
        bool iconVisible = false;
        for (int y = row + 23; y < row + 49; ++y)
            for (int x = 30; x < 56; ++x)
                iconVisible = iconVisible || pixel(x, y) != background;
        check(iconVisible, "Each sidebar page renders its Lucide icon");
    }
    check(panel.save(argv[1]), "Save paused preview");
    state.enabled = true;
    state.gravity = true;
    state.distanceLimit = true;
    state.distanceLimitMeters = 100;
    state.offset.y = .25;
    state.hover = Command::Reset;
    state.status = "Close the dashboard; hold your drag binding and move that controller.";
    panel.render(state);
    check(panel.save((base / "panel-enabled.ppm").string().c_str()), "Save enabled preview");
    state.enabled = false;
    state.distanceLimit = false;
    state.hover = Command::None;
    state.status = "Playspace changed externally. Paused; recalibrate, then enable.";
    state.statusAttention = true;
    panel.render(state);
    check(panel.save((base / "panel-warning.ppm").string().c_str()), "Save warning preview");
    state.notice = "Could not open bindings. Use SteamVR Settings > Controllers > Manage bindings.";
    panel.render(state);
    check(panel.save((base / "panel-bindings.ppm").string().c_str()), "Save binding error preview");
    state.notice.clear();
    state.page = PanelPage::Settings;
    state.appVersion = build::version;
    state.buildId = build::id;
    state.buildTarget = "Linux aarch64";
    state.openVRVersion = "2.15.6";
    state.runtimeVersion = "2.15.6 (Steam Frame)";
    state.settingsScroll = Panel::settingsScrollMax;
    panel.render(state);
    check(panel.save((base / "panel-about.ppm").string().c_str()), "Save About preview");
    state.page = PanelPage::Settings;
    state.settingsScroll = 0;
    state.startupAvailable = true;
    state.updateAvailable = true;
    state.updateStatus = "Version 0.2.0 is available.";
    state.debugLogging = true;
    panel.render(state);
    check(panel.save((base / "panel-settings.ppm").string().c_str()), "Save Settings preview");
    state.updateBusy = true;
    state.updateStatus = "Downloading version 0.2.0...";
    panel.render(state);
    check(panel.save((base / "panel-settings-updating.ppm").string().c_str()), "Save update progress preview");
    state.updateBusy = false;
    state.updateStatus = "Version 0.2.0 is available.";
    state.exportStatus = "Logs exported. Ready to attach to a bug report.";
    state.hover = Command::ExportLogs;
    panel.render(state);
    check(panel.save((base / "panel-export.ppm").string().c_str()), "Save export result preview");
    state.page = PanelPage::LaunchMenu;
    state.hover = Command::None;
    state.launchLoaded = true;
    state.launchEnabled = true;
    state.launchPages = 3;
    appicons::Loader appIcons({});
    const auto appIcon = appIcons.load((std::filesystem::absolute(argv[2]).parent_path().parent_path() /
                                      "assets/icons/frame-advanced-settings.png").string());
    check(bool(appIcon), "Load actual application icon for preview");
    state.launchRows = {{"Dateien", true, true, "org.kde.dolphin"},
                        {"Firefox", true, true, "org.mozilla.firefox"},
                        {"Frame Advanced Settings", true, true, "frame-advanced-settings", appIcon},
                        {"Konsole", false, true, "org.kde.konsole"},
                        {"Systemeinstellungen", true, true, "systemsettings"},
                        {"A very long application name that must stay inside its row and never overlap the switches", false, true,
                         "org.example.averylongapplicationidthatmustneveroverlapthevisibilitycontrols"}};
    state.launchStatus.clear();
    panel.render(state);
    check(panel.save((base / "panel-launch-menu.ppm").string().c_str()), "Save Launch Menu preview");
    changed = state;
    changed.launchRows[0].visible = false;
    check(!(changed == state), "Visibility change redraws panel");
    changed = state;
    changed.launchRows[0].appId = "org.example.changed";
    check(!(changed == state), "App ID change redraws panel");
    changed = state;
    changed.launchBusy = true;
    check(!(changed == state), "Worker activity redraws panel");
    state.launchEnabled = false;
    state.launchStatus.clear();
    panel.render(state);
    check(panel.save((base / "panel-launch-menu-off.ppm").string().c_str()), "Save disabled Launch Menu preview");
    state.launchRows.clear();
    state.launchPages = 1;
    panel.render(state);
    check(panel.save((base / "panel-launch-menu-empty.ppm").string().c_str()), "Save empty Launch Menu preview");
    state.launchLoaded = false;
    state.launchStatus = "Could not update Launch Menu. Refresh and retry; details are in the app log.";
    panel.render(state);
    check(panel.save((base / "panel-launch-menu-error.ppm").string().c_str()), "Save failed Launch Menu preview");
    state.page = PanelPage::Microphone;
    state.micAvailable = true;
    state.micNoise = false;
    panel.render(state);
    check(panel.save((base / "panel-microphone.ppm").string().c_str()), "Save microphone preview");
    state.voiceBusy = true;
    state.voiceStatus = "Recording... 4 s remaining";
    panel.render(state);
    check(panel.save((base / "panel-microphone-recording.ppm").string().c_str()), "Save recording preview");
    state.voiceBusy = false;
    state.voiceReady = true;
    state.voiceStatus.clear();
    panel.render(state);
    check(panel.save((base / "panel-microphone-ready.ppm").string().c_str()), "Save ready clip preview");
    state.voiceBusy = true;
    state.voiceStatus = "Playing...";
    panel.render(state);
    check(panel.save((base / "panel-microphone-playing.ppm").string().c_str()), "Save playing preview");
    state.voiceBusy = false;
    state.voiceReady = false;
    state.voiceStatus.clear();
    state.micAvailable = false;
    state.micStatus = "Microphone controls are not loaded. Reboot the headset after installing this update.";
    panel.render(state);
    check(panel.save((base / "panel-microphone-unavailable.ppm").string().c_str()), "Save unavailable microphone preview");
    std::printf(
        "Page-aware hit regions, font fallback, and sixteen visual states rendered. Font: %s\n",
        panel.fontFamily().c_str());
}
