#pragma once
#include "drag.h"
#include "app_icons.h"
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

enum class PanelPage { SpaceDrag, LaunchMenu, Microphone, Settings };
enum class Command {
    None,
    Disable,
    Enable,
    Reset,
    Height,
    XYZ,
    Slower,
    Faster,
    Bindings,
    GravityOff,
    GravityOn,
    GravityWeaker,
    GravityStronger,
    LimitOff,
    LimitOn,
    LimitSmaller,
    LimitLarger,
    PageSpaceDrag,
    PageLaunchMenu,
    PageMicrophone,
    PageSettings,
    DebugLoggingOff,
    DebugLoggingOn,
    ExportLogs,
    MicEchoOff, MicEchoOn, MicNoiseOff, MicNoiseOn, MicReset,
    VoiceRecord, VoicePlay, VoiceStop,
    LaunchOff, LaunchOn, LaunchRefresh, LaunchReset, LaunchPrevious, LaunchNext,
    LaunchHide0, LaunchShow0, LaunchHide1, LaunchShow1, LaunchHide2, LaunchShow2,
    LaunchHide3, LaunchShow3, LaunchHide4, LaunchShow4, LaunchHide5, LaunchShow5,
    ResetDirection, ResetGain, ResetLimit, ResetGravity, ResetDebugLogging,
    MicResetEcho, MicResetNoise,
    SettingsUp, SettingsDown, StartupOff, StartupOn,
    AutoUpdateOff, AutoUpdateOn, CheckUpdate, InstallUpdate, ResetAutoUpdate
};
struct LaunchRow {
    std::string name;
    bool visible = true, editable = true;
    std::string appId;
    std::shared_ptr<const appicons::Bitmap> icon;
    bool operator==(const LaunchRow &other) const {
        return name == other.name && visible == other.visible && editable == other.editable &&
               appId == other.appId && icon == other.icon;
    }
};
struct PanelState {
    PanelPage page = PanelPage::SpaceDrag;
    bool enabled = false, xyz = true, dragging = false;
    double gain = 2;
    bool gravity = false;
    double gravityStrength = 9.8;
    bool distanceLimit = false;
    double distanceLimitMeters = 2;
    drag::Vec offset;
    std::string status = "PAUSED";
    bool statusAttention = false;
    std::string notice;
    std::string appVersion = "Unavailable", buildId = "Unavailable", buildTarget = "Unavailable";
    std::string openVRVersion = "Unavailable", runtimeVersion = "Unavailable";
    bool debugLogging = false;
    bool startup = false, startupAvailable = false, startupBusy = false;
    bool selfUpdates = true, automaticUpdateChecks = true, updateBusy = false, updateAvailable = false;
    std::string startupStatus, updateStatus;
    int settingsScroll = 0;
    std::string loggingStatus, exportStatus;
    bool launchEnabled = false, launchBusy = false, launchLoaded = false;
    size_t launchPage = 0, launchPages = 1;
    std::vector<LaunchRow> launchRows;
    std::string launchStatus;
    bool micAvailable = false, micBusy = false, micEcho = true, micNoise = true;
    std::string micStatus;
    bool voiceBusy = false, voiceReady = false;
    std::string voiceStatus;
    Command hover = Command::None, pressed = Command::None;
    bool operator==(const PanelState &other) const;
};
struct Panel {
    static constexpr int width = 1600, height = 900;
    static constexpr int settingsScrollMax = 230;
    // SteamVR's settings panel uses 16:9 and a 1.5 m base dashboard height.
    // The runtime applies the user's dashboard distance/scale around this.
    static constexpr float widthMeters = 1.5f * width / height;
    std::vector<uint8_t> pixels;
    explicit Panel(const std::filesystem::path &fontDirectory,
                   const std::filesystem::path &steamVRFontDirectory = {});
    ~Panel();
    Panel(const Panel &) = delete;
    Panel &operator=(const Panel &) = delete;
    void render(const PanelState &state);
    static Command hit(double x, double y, PanelPage page = PanelPage::SpaceDrag);
    Command hit(double x, double y, const PanelState &state);
    bool save(const char *path) const;
    const std::string &fontFamily() const;

  private:
    struct Fonts;
    std::unique_ptr<Fonts> fonts_;
    int scrollY_ = 0, clipTop_ = 0, clipBottom_ = height;
    void blend(int x, int y, uint32_t color, float alpha);
    void rect(int x, int y, int w, int h, uint32_t color);
    void rounded(int x, int y, int w, int h, int radius, uint32_t color);
    void text(int x, int y, const std::string &text, int size = 26, uint32_t color = 0xdcdedf,
              bool bold = false);
    int textWidth(const std::string &text, int size, bool bold = false);
    void centered(int x, int y, int w, int h, const std::string &text, int size, uint32_t color,
                  bool bold = false);
    void wrapped(int x, int y, int width, const std::string &text, int size, uint32_t color);
};
