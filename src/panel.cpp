#include "panel.h"
#include "sidebar_icons.h"
#include "preferences.h"
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <tuple>

namespace {
// SteamVR settings colors and scaled layout metrics; rendered independently.
// Keep the same 16:9 panel and readable SteamVR-sized rows across every page.
constexpr uint32_t background = 0x0a0f14, surface = 0x15191f, control = 0x23262e;
constexpr uint32_t sidebarBackground = 0x22272b, sidebarSelected = 0x3d4450;
constexpr uint32_t primary = 0xdee2e5, secondary = 0x8b929a;
constexpr uint32_t segmentTrack = 0x1e242c, selected = 0x353945, inactive = 0x6b7178;
constexpr int sidebarWidth = 340, sidebarTop = 24, sidebarRowHeight = 72;
constexpr int inset = 400, contentWidth = 1136;
constexpr int controlsLeft = 1096, controlsWidth = 440, rowHeight = 55, bodySize = 22;
struct Button {
    Command command;
    int x, y, w, h;
    PanelPage page = PanelPage::SpaceDrag;
    bool shared = false;
};
constexpr Button buttons[] = {
    {Command::PageSpaceDrag, 0, sidebarTop, sidebarWidth, sidebarRowHeight, PanelPage::SpaceDrag, true},
    {Command::PageLaunchMenu, 0, sidebarTop + sidebarRowHeight, sidebarWidth, sidebarRowHeight, PanelPage::LaunchMenu, true},
    {Command::PageMicrophone, 0, sidebarTop + 2 * sidebarRowHeight, sidebarWidth, sidebarRowHeight, PanelPage::Microphone, true},
    {Command::PageSettings, 0, sidebarTop + 3 * sidebarRowHeight, sidebarWidth, sidebarRowHeight, PanelPage::Settings, true},
    {Command::StartupOff, controlsLeft, 154, 220, rowHeight, PanelPage::Settings},
    {Command::StartupOn, controlsLeft + 220, 154, 220, rowHeight, PanelPage::Settings},
    {Command::AutoUpdateOff, controlsLeft, 248, 220, rowHeight, PanelPage::Settings},
    {Command::AutoUpdateOn, controlsLeft + 220, 248, 220, rowHeight, PanelPage::Settings},
    {Command::CheckUpdate, controlsLeft, 342, 208, rowHeight, PanelPage::Settings},
    {Command::InstallUpdate, controlsLeft + 232, 342, 208, rowHeight, PanelPage::Settings},
    {Command::SettingsUp, 1554, 148, 32, 40, PanelPage::Settings},
    {Command::SettingsDown, 1554, 830, 32, 40, PanelPage::Settings},
    {Command::Disable, controlsLeft, 52, 220, rowHeight},
    {Command::Enable, controlsLeft + 220, 52, 220, rowHeight},
    {Command::Height, controlsLeft, 154, 220, rowHeight},
    {Command::XYZ, controlsLeft + 220, 154, 220, rowHeight},
    {Command::Slower, controlsLeft, 232, 70, rowHeight},
    {Command::Faster, controlsLeft + 370, 232, 70, rowHeight},
    {Command::LimitOff, 828, 310, 122, rowHeight},
    {Command::LimitOn, 950, 310, 122, rowHeight},
    {Command::LimitSmaller, controlsLeft, 310, 70, rowHeight},
    {Command::LimitLarger, controlsLeft + 370, 310, 70, rowHeight},
    {Command::GravityOff, 828, 388, 122, rowHeight},
    {Command::GravityOn, 950, 388, 122, rowHeight},
    {Command::GravityWeaker, controlsLeft, 388, 70, rowHeight},
    {Command::GravityStronger, controlsLeft + 370, 388, 70, rowHeight},
    {Command::Bindings, controlsLeft, 466, controlsWidth, rowHeight},
    {Command::Reset, inset, 672, contentWidth, rowHeight},
    {Command::MicEchoOff, controlsLeft, 154, 220, rowHeight, PanelPage::Microphone},
    {Command::MicEchoOn, controlsLeft + 220, 154, 220, rowHeight, PanelPage::Microphone},
    {Command::MicNoiseOff, controlsLeft, 248, 220, rowHeight, PanelPage::Microphone},
    {Command::MicNoiseOn, controlsLeft + 220, 248, 220, rowHeight, PanelPage::Microphone},
    {Command::MicReset, inset, 364, contentWidth, rowHeight, PanelPage::Microphone},
    {Command::VoiceRecord, inset, 506, 528, rowHeight, PanelPage::Microphone},
    {Command::VoicePlay, 952, 506, 280, rowHeight, PanelPage::Microphone},
    {Command::VoiceStop, 1256, 506, 280, rowHeight, PanelPage::Microphone},
    {Command::DebugLoggingOff, controlsLeft, 436, 220, rowHeight, PanelPage::Settings},
    {Command::DebugLoggingOn, controlsLeft + 220, 436, 220, rowHeight, PanelPage::Settings},
    {Command::ExportLogs, controlsLeft, 530, controlsWidth, rowHeight, PanelPage::Settings},
    {Command::LaunchOff, controlsLeft, 52, 220, rowHeight, PanelPage::LaunchMenu},
    {Command::LaunchOn, controlsLeft + 220, 52, 220, rowHeight, PanelPage::LaunchMenu},
    {Command::LaunchHide0, controlsLeft, 188, 220, rowHeight, PanelPage::LaunchMenu},
    {Command::LaunchShow0, controlsLeft + 220, 188, 220, rowHeight, PanelPage::LaunchMenu},
    {Command::LaunchHide1, controlsLeft, 266, 220, rowHeight, PanelPage::LaunchMenu},
    {Command::LaunchShow1, controlsLeft + 220, 266, 220, rowHeight, PanelPage::LaunchMenu},
    {Command::LaunchHide2, controlsLeft, 344, 220, rowHeight, PanelPage::LaunchMenu},
    {Command::LaunchShow2, controlsLeft + 220, 344, 220, rowHeight, PanelPage::LaunchMenu},
    {Command::LaunchHide3, controlsLeft, 422, 220, rowHeight, PanelPage::LaunchMenu},
    {Command::LaunchShow3, controlsLeft + 220, 422, 220, rowHeight, PanelPage::LaunchMenu},
    {Command::LaunchHide4, controlsLeft, 500, 220, rowHeight, PanelPage::LaunchMenu},
    {Command::LaunchShow4, controlsLeft + 220, 500, 220, rowHeight, PanelPage::LaunchMenu},
    {Command::LaunchHide5, controlsLeft, 578, 220, rowHeight, PanelPage::LaunchMenu},
    {Command::LaunchShow5, controlsLeft + 220, 578, 220, rowHeight, PanelPage::LaunchMenu},
    {Command::LaunchPrevious, inset, 674, 64, rowHeight, PanelPage::LaunchMenu},
    {Command::LaunchNext, inset + 192, 674, 64, rowHeight, PanelPage::LaunchMenu},
    {Command::LaunchRefresh, controlsLeft, 674, 208, rowHeight, PanelPage::LaunchMenu},
    {Command::LaunchReset, controlsLeft + 232, 674, 208, rowHeight, PanelPage::LaunchMenu}};
const Button &button(Command command) {
    for (const auto &item : buttons)
        if (item.command == command)
            return item;
    throw std::logic_error("Unknown panel command");
}
struct RowReset {
    Command command, anchor;
    const char *label;
};
constexpr RowReset rowResets[] = {
    {Command::ResetDirection, Command::Height, "Movement direction"},
    {Command::ResetGain, Command::Slower, "Drag speed"},
    {Command::ResetLimit, Command::LimitOff, "Distance limit"},
    {Command::ResetGravity, Command::GravityOff, "Gravity"},
    {Command::ResetDebugLogging, Command::DebugLoggingOff, "Detailed logging"},
    {Command::ResetAutoUpdate, Command::AutoUpdateOff, "Check for updates automatically"},
    {Command::MicResetEcho, Command::MicEchoOff, "Echo cancellation"},
    {Command::MicResetNoise, Command::MicNoiseOff, "Noise suppression"}};
constexpr int resetTargetSize = 36, resetLabelGap = 6;
bool rowChanged(Command command, const PanelState &s) {
    const drag::Preferences defaults;
    switch (command) {
    case Command::ResetDirection: return s.xyz != defaults.xyz;
    case Command::ResetGain: return std::abs(s.gain - defaults.gain) > 1e-9;
    case Command::ResetLimit:
        return s.distanceLimit != defaults.distanceLimit ||
               std::abs(s.distanceLimitMeters - defaults.distanceLimitMeters) > 1e-9;
    case Command::ResetGravity:
        return s.gravity != defaults.gravity ||
               std::abs(s.gravityStrength - defaults.gravityStrength) > 1e-9;
    case Command::ResetDebugLogging: return s.debugLogging != defaults.detailedLogging;
    case Command::ResetAutoUpdate: return s.selfUpdates && !s.automaticUpdateChecks;
    case Command::MicResetEcho: return s.micAvailable && !s.micEcho;
    case Command::MicResetNoise: return s.micAvailable && !s.micNoise;
    default: return false;
    }
}
bool resetEnabled(Command command, const PanelState &s) {
    return (command != Command::MicResetEcho && command != Command::MicResetNoise) ||
           (s.micAvailable && !s.micBusy && !s.voiceBusy);
}
std::vector<int> codepoints(const std::string &text) {
    std::vector<int> result;
    for (size_t i = 0; i < text.size();) {
        const auto c = static_cast<unsigned char>(text[i++]);
        if (c < 0x80) { result.push_back(c); continue; }
        const int count = c >= 0xc2 && c <= 0xdf ? 1 : c >= 0xe0 && c <= 0xef ? 2 : c >= 0xf0 && c <= 0xf4 ? 3 : 0;
        int code = c & (count == 1 ? 0x1f : count == 2 ? 0x0f : 0x07);
        bool valid = count && i + count <= text.size();
        for (int n = 0; valid && n < count; ++n) {
            const auto next = static_cast<unsigned char>(text[i + n]);
            valid = (next & 0xc0) == 0x80;
            code = (code << 6) | (next & 0x3f);
        }
        valid = valid && code <= 0x10ffff && !(code >= 0xd800 && code <= 0xdfff) &&
                code >= (count == 1 ? 0x80 : count == 2 ? 0x800 : 0x10000);
        if (valid) i += count;
        result.push_back(valid ? code : 0xfffd);
    }
    return result;
}
void dropLastCodepoint(std::string &text) {
    if (text.empty()) return;
    size_t end = text.size() - 1;
    while (end && (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80) --end;
    text.resize(end);
}
} // namespace
struct Panel::Fonts {
    struct Face {
        std::vector<unsigned char> bytes;
        stbtt_fontinfo info{};
        explicit Face(const std::filesystem::path &path) {
            std::ifstream stream(path, std::ios::binary | std::ios::ate);
            const auto size = stream ? stream.tellg() : std::streampos(-1);
            if (size < 12 || size > 4 * 1024 * 1024)
                throw std::runtime_error("Missing/invalid font: " + path.string());
            bytes.resize(static_cast<size_t>(size));
            stream.seekg(0);
            stream.read(reinterpret_cast<char *>(bytes.data()), bytes.size());
            // Some SteamVR builds store encoded data even under a .ttf filename.
            // stb_truetype assumes a valid sfnt directory and is not a file validator.
            auto u32 = [&](size_t at) {
                return (uint32_t(bytes[at]) << 24) | (uint32_t(bytes[at + 1]) << 16) |
                       (uint32_t(bytes[at + 2]) << 8) | uint32_t(bytes[at + 3]);
            };
            const auto signature = u32(0);
            const size_t tables = (size_t(bytes[4]) << 8) | bytes[5];
            if (!stream ||
                (signature != 0x00010000 && signature != 0x4f54544f && signature != 0x74727565) ||
                tables == 0 || tables > (bytes.size() - 12) / 16)
                throw std::runtime_error("Unsupported font format: " + path.string());
            for (size_t i = 0; i < tables; ++i) {
                const size_t offset = u32(12 + i * 16 + 8);
                const size_t length = u32(12 + i * 16 + 12);
                if (offset > bytes.size() || length > bytes.size() - offset)
                    throw std::runtime_error("Truncated font: " + path.string());
            }
            if (!stbtt_InitFont(&info, bytes.data(), 0))
                throw std::runtime_error("Cannot load font: " + path.string());
        }
    } regular, bold;
    struct Glyph {
        int w = 0, h = 0, x = 0, y = 0;
        float advance = 0;
        std::vector<unsigned char> bitmap;
    };
    std::map<std::tuple<bool, int, int>, Glyph> glyphs;
    std::string family;
    Fonts(const std::filesystem::path &regularPath, const std::filesystem::path &boldPath,
          std::string name)
        : regular(regularPath), bold(boldPath), family(std::move(name)) {}
    Face &face(bool weight) { return weight ? bold : regular; }
    float scale(int size, bool weight) {
        return stbtt_ScaleForMappingEmToPixels(&face(weight).info, static_cast<float>(size));
    }
    const Glyph &glyph(int code, int size, bool weight) {
        auto key = std::make_tuple(weight, size, code);
        auto found = glyphs.find(key);
        if (found != glyphs.end())
            return found->second;
        Glyph g;
        auto &font = face(weight).info;
        const float s = scale(size, weight);
        int advance = 0;
        stbtt_GetCodepointHMetrics(&font, code, &advance, nullptr);
        g.advance = advance * s;
        unsigned char *bitmap = stbtt_GetCodepointBitmap(&font, s, s, code, &g.w, &g.h, &g.x, &g.y);
        if (bitmap) {
            g.bitmap.assign(bitmap, bitmap + g.w * g.h);
            stbtt_FreeBitmap(bitmap, nullptr);
        }
        return glyphs.emplace(key, std::move(g)).first->second;
    }
};
bool PanelState::operator==(const PanelState &other) const {
    return page == other.page && settingsScroll == other.settingsScroll && enabled == other.enabled && xyz == other.xyz &&
           dragging == other.dragging && gain == other.gain && gravity == other.gravity &&
           gravityStrength == other.gravityStrength && distanceLimit == other.distanceLimit &&
           distanceLimitMeters == other.distanceLimitMeters && offset.x == other.offset.x &&
           offset.y == other.offset.y && offset.z == other.offset.z && status == other.status &&
           statusAttention == other.statusAttention && notice == other.notice &&
           hover == other.hover && pressed == other.pressed && appVersion == other.appVersion &&
           buildId == other.buildId && buildTarget == other.buildTarget &&
           openVRVersion == other.openVRVersion && runtimeVersion == other.runtimeVersion &&
           debugLogging == other.debugLogging &&
           startup == other.startup && startupAvailable == other.startupAvailable && startupBusy == other.startupBusy &&
           selfUpdates == other.selfUpdates && automaticUpdateChecks == other.automaticUpdateChecks &&
           updateBusy == other.updateBusy && updateAvailable == other.updateAvailable &&
           startupStatus == other.startupStatus && updateStatus == other.updateStatus &&
           loggingStatus == other.loggingStatus && exportStatus == other.exportStatus &&
           micAvailable == other.micAvailable && micBusy == other.micBusy &&
           voiceBusy == other.voiceBusy && voiceReady == other.voiceReady && voiceStatus == other.voiceStatus &&
           micEcho == other.micEcho && micNoise == other.micNoise && micStatus == other.micStatus &&
           launchEnabled == other.launchEnabled && launchBusy == other.launchBusy &&
           launchLoaded == other.launchLoaded && launchPage == other.launchPage &&
           launchPages == other.launchPages && launchRows == other.launchRows && launchStatus == other.launchStatus;
}
Panel::Panel(const std::filesystem::path &fontDirectory,
             const std::filesystem::path &steamVRFontDirectory)
    : pixels(width * height * 4) {
    if (!steamVRFontDirectory.empty()) {
        try {
            fonts_ = std::make_unique<Fonts>(steamVRFontDirectory / "motiva-sans-regular.ttf",
                                             steamVRFontDirectory / "motiva-sans-bold.ttf",
                                             "Motiva Sans (installed SteamVR)");
        } catch (const std::runtime_error &) {
            // An absent/incomplete runtime font set must not prevent the dashboard opening.
        }
    }
    if (!fonts_)
        fonts_ = std::make_unique<Fonts>(fontDirectory / "Inter-Regular.ttf",
                                         fontDirectory / "Inter-SemiBold.ttf", "Inter (bundled)");
}
Panel::~Panel() = default;
const std::string &Panel::fontFamily() const {
    return fonts_->family;
}
void Panel::blend(int x, int y, uint32_t color, float alpha) {
    y -= scrollY_;
    if (x < 0 || y < clipTop_ || x >= width || y >= clipBottom_ || alpha <= 0)
        return;
    alpha = std::min(1.f, alpha);
    const int p = (y * width + x) * 4;
    for (int channel = 0; channel < 3; ++channel) {
        const unsigned component = (color >> (16 - channel * 8)) & 255;
        pixels[p + channel] = static_cast<uint8_t>(
            std::lround(pixels[p + channel] * (1 - alpha) + component * alpha));
    }
    pixels[p + 3] = 255;
}
void Panel::rect(int x, int y, int w, int h, uint32_t color) {
    y -= scrollY_;
    for (int yy = std::max(clipTop_, y); yy < std::min(clipBottom_, y + h); ++yy)
        for (int xx = std::max(0, x); xx < std::min(width, x + w); ++xx) {
            const int p = (yy * width + xx) * 4;
            pixels[p] = color >> 16;
            pixels[p + 1] = color >> 8;
            pixels[p + 2] = color;
            pixels[p + 3] = 255;
        }
}
void Panel::rounded(int x, int y, int w, int h, int radius, uint32_t color) {
    for (int yy = std::max(clipTop_ + scrollY_, y); yy < std::min(clipBottom_ + scrollY_, y + h); ++yy)
        for (int xx = std::max(0, x); xx < std::min(width, x + w); ++xx) {
            const float dx = std::max(std::abs(xx + .5f - x - w * .5f) - (w * .5f - radius), 0.f);
            const float dy = std::max(std::abs(yy + .5f - y - h * .5f) - (h * .5f - radius), 0.f);
            blend(xx, yy, color, std::clamp(radius + .5f - std::sqrt(dx * dx + dy * dy), 0.f, 1.f));
        }
}
int Panel::textWidth(const std::string &value, int size, bool bold) {
    float pen = 0;
    const float scale = fonts_->scale(size, bold);
    auto &face = fonts_->face(bold).info;
    const auto codes = codepoints(value);
    for (size_t i = 0; i < codes.size(); ++i) {
        const auto code = codes[i];
        pen += fonts_->glyph(code, size, bold).advance;
        if (i + 1 < codes.size())
            pen += scale * stbtt_GetCodepointKernAdvance(&face, code, codes[i + 1]);
    }
    return static_cast<int>(std::ceil(pen));
}
void Panel::text(int x, int y, const std::string &value, int size, uint32_t color, bool bold) {
    auto &face = fonts_->face(bold).info;
    const float scale = fonts_->scale(size, bold);
    int ascent = 0;
    stbtt_GetFontVMetrics(&face, &ascent, nullptr, nullptr);
    const int baseline = y + static_cast<int>(std::lround(ascent * scale));
    float pen = static_cast<float>(x);
    const auto codes = codepoints(value);
    for (size_t i = 0; i < codes.size(); ++i) {
        const auto code = codes[i];
        const auto &g = fonts_->glyph(code, size, bold);
        const int start = static_cast<int>(std::lround(pen)) + g.x;
        for (int yy = 0; yy < g.h; ++yy)
            for (int xx = 0; xx < g.w; ++xx)
                blend(start + xx, baseline + g.y + yy, color, g.bitmap[yy * g.w + xx] / 255.f);
        pen += g.advance;
        if (i + 1 < codes.size())
            pen += scale * stbtt_GetCodepointKernAdvance(&face, code, codes[i + 1]);
    }
}
void Panel::centered(int x, int y, int w, int h, const std::string &value, int size, uint32_t color,
                     bool bold) {
    text(x + (w - textWidth(value, size, bold)) / 2, y + (h - static_cast<int>(size * 1.2)) / 2,
         value, size, color, bold);
}
void Panel::wrapped(int x, int y, int maxWidth, const std::string &value, int size,
                    uint32_t color) {
    std::istringstream words(value);
    std::string word, line;
    int row = 0;
    while (words >> word) {
        const std::string candidate = line.empty() ? word : line + " " + word;
        if (!line.empty() && textWidth(candidate, size) > maxWidth) {
            text(x, y + row * (size + 5), line, size, color);
            if (++row == 2)
                return;
            line = word;
        } else
            line = candidate;
    }
    text(x, y + row * (size + 5), line, size, color);
}
void Panel::render(const PanelState &s) {
    rect(0, 0, width, height, background);
    rect(0, 0, sidebarWidth, height, sidebarBackground);
    auto colorFor = [&](Command command, uint32_t normal) {
        return s.pressed == command && s.hover == command ? 0x1e2126u
               : s.hover == command                       ? 0x3d4450u
                                                          : normal;
    };
    auto drawButton = [&](Command command, const std::string &label, uint32_t normal = control,
                          int size = bodySize, bool enabled = true) {
        const auto &b = button(command);
        rounded(b.x, b.y, b.w, b.h, 8, enabled ? colorFor(command, normal) : surface);
        centered(b.x, b.y, b.w, b.h, label, size, enabled ? primary : inactive);
    };
    auto segment = [&](Command left, const char *leftLabel, Command right, const char *rightLabel,
                       bool rightSelected, bool enabled = true) {
        const auto &a = button(left);
        const auto &b = button(right);
        rounded(a.x, a.y, a.w + b.w, a.h, 8, segmentTrack);
        for (const auto &item :
             {std::make_pair(left, leftLabel), std::make_pair(right, rightLabel)}) {
            const auto &r = button(item.first);
            const bool active = (item.first == right) == rightSelected;
            if (active || (enabled && s.hover == item.first))
                rounded(r.x, r.y, r.w, r.h, 8,
                        enabled ? colorFor(item.first, active ? selected : segmentTrack) : control);
            centered(r.x, r.y, r.w, r.h, item.second, bodySize,
                     enabled && (active || s.hover == item.first) ? primary : inactive);
        }
    };
    auto label = [&](Command command, const char *value) {
        const auto &b = button(command);
        text(inset, b.y + (b.h - static_cast<int>(bodySize * 1.2)) / 2, value, bodySize, primary);
        for (const auto &reset : rowResets) {
            if (reset.anchor != command || !rowChanged(reset.command, s)) continue;
            const int left = inset + textWidth(value, bodySize) + resetLabelGap;
            const int top = b.y + (b.h - resetTargetSize) / 2;
            const bool enabled = resetEnabled(reset.command, s);
            if (enabled && s.hover == reset.command)
                rounded(left, top, resetTargetSize, resetTargetSize, 5,
                        colorFor(reset.command, background));
            const auto icon = sidebar_icons::get(sidebar_icons::Icon::RotateCcw);
            const auto color = !enabled ? inactive : s.hover == reset.command ? primary : secondary;
            for (int y = 0; y < icon.height; ++y)
                for (int x = 0; x < icon.width; ++x)
                    blend(left + (resetTargetSize - icon.width) / 2 + x,
                          top + (resetTargetSize - icon.height) / 2 + y,
                          color, icon.alpha[y * icon.width + x] / 255.f);
        }
    };
    for (const auto &item : {std::make_tuple(Command::PageSpaceDrag, PanelPage::SpaceDrag,
                                             "Space Drag", sidebar_icons::Icon::Move3d),
                             std::make_tuple(Command::PageLaunchMenu, PanelPage::LaunchMenu, "Launch Menu",
                                             sidebar_icons::Icon::List),
                             std::make_tuple(Command::PageMicrophone, PanelPage::Microphone, "Microphone",
                                             sidebar_icons::Icon::Mic),
                             std::make_tuple(Command::PageSettings, PanelPage::Settings, "Settings",
                                             sidebar_icons::Icon::Settings)}) {
        const auto command = std::get<0>(item);
        const auto &b = button(command);
        const bool active = s.page == std::get<1>(item);
        if (active || s.hover == command) {
            const auto fill = s.pressed == command && s.hover == command ? 0x303741u
                              : s.hover == command                       ? 0x454d5bu
                                                                         : sidebarSelected;
            rect(b.x, b.y, b.w, b.h, fill);
        }
        const auto foreground = active || s.hover == command ? primary : secondary;
        const auto icon = sidebar_icons::get(std::get<3>(item));
        const int iconTop = b.y + (b.h - icon.height) / 2;
        for (int y = 0; y < icon.height; ++y)
            for (int x = 0; x < icon.width; ++x)
                blend(30 + x, iconTop + y, foreground, icon.alpha[y * icon.width + x] / 255.f);
        text(74, b.y + (b.h - static_cast<int>(bodySize * 1.2)) / 2,
             std::get<2>(item), bodySize, foreground);
    }
    const char *pageTitle = s.page == PanelPage::Settings ? "Settings"
                            : s.page == PanelPage::Microphone ? "Microphone"
                            : s.page == PanelPage::LaunchMenu ? "Launch Menu"
                                                            : "Space Drag";
    text(inset, 56, pageTitle, 32, primary, true);
    rect(inset, 118, contentWidth, 1, control);


    if (s.page == PanelPage::LaunchMenu) {
        const bool ready = s.launchLoaded && !s.launchBusy;
        segment(Command::LaunchOff, "Off", Command::LaunchOn, "On", s.launchEnabled, ready);
        text(inset, 142, s.launchEnabled ? "Choose what appears in Launch Program and the desktop app menu."
                                        : "Off restores your original shortcuts and keeps your choices.", 18, secondary);
        for (size_t i = 0; i < std::min(size_t(6), s.launchRows.size()); ++i) {
            const auto &row = s.launchRows[i];
            const auto hide = static_cast<Command>(static_cast<int>(Command::LaunchHide0) + i * 2);
            const auto show = static_cast<Command>(static_cast<int>(hide) + 1);
            const auto &b = button(hide);
            const bool editable = ready && s.launchEnabled && row.editable;
            auto shortened = [&](std::string value, int size) {
                if (textWidth(value, size) > 594) {
                    while (!value.empty() && textWidth(value + "...", size) > 594) dropLastCodepoint(value);
                    value += "...";
                }
                return value;
            };
            const int iconTop = b.y + (b.h - appicons::Bitmap::size) / 2;
            if (row.icon && row.icon->rgba.size() == appicons::Bitmap::size * appicons::Bitmap::size * 4) {
                for (int y = 0; y < appicons::Bitmap::size; ++y)
                    for (int x = 0; x < appicons::Bitmap::size; ++x) {
                        const auto *rgba = row.icon->rgba.data() + (y * appicons::Bitmap::size + x) * 4;
                        blend(inset + x, iconTop + y, uint32_t(rgba[0]) << 16 | uint32_t(rgba[1]) << 8 | rgba[2],
                              rgba[3] / 255.f * (s.launchEnabled ? 1.f : .5f));
                    }
            } else {
                // Quiet generic app tile when a shortcut has no usable icon.
                rounded(inset, iconTop, 48, 48, 8, control);
                for (int y : {13, 27}) for (int x : {13, 27})
                    rounded(inset + x, iconTop + y, 8, 8, 2, s.launchEnabled ? secondary : inactive);
            }
            text(inset + 68, b.y + 1, shortened(row.name, bodySize), bodySize, s.launchEnabled ? primary : secondary);
            text(inset + 68, b.y + 32, shortened(row.appId, 16), 16, secondary);
            segment(hide, "Hide", show, "Show", row.visible, editable);
        }
        if (s.launchRows.empty())
            text(inset, 260, s.launchBusy ? "Loading shortcuts..." : s.launchLoaded ? "No shortcuts found." : "Shortcuts unavailable.", bodySize, secondary);
        if (s.launchPages > 1) {
            drawButton(Command::LaunchPrevious, "<", control, bodySize, ready && s.launchPage > 0);
            centered(inset + 64, 674, 128, rowHeight, std::to_string(s.launchPage + 1) + " / " +
                     std::to_string(s.launchPages), 20, secondary);
            drawButton(Command::LaunchNext, ">", control, bodySize, ready && s.launchPage + 1 < s.launchPages);
        }
        drawButton(Command::LaunchRefresh, "REFRESH", control, bodySize, !s.launchBusy);
        drawButton(Command::LaunchReset, "RESET", control, bodySize, ready);
        if (!s.launchStatus.empty()) wrapped(inset, 760, contentWidth, s.launchStatus, 18, secondary);
        return;
    }

    if (s.page == PanelPage::Microphone) {
        label(Command::MicEchoOff, "Echo cancellation");
        label(Command::MicNoiseOff, "Noise suppression");
        text(inset, 201, "Reduces sound from the headset speakers.", 18, secondary);
        text(inset, 295, "Filters background noise from your microphone.", 18, secondary);
        if (s.micAvailable) {
            segment(Command::MicEchoOff, "Off", Command::MicEchoOn, "On", s.micEcho, !s.micBusy && !s.voiceBusy);
            segment(Command::MicNoiseOff, "Off", Command::MicNoiseOn, "On", s.micNoise, !s.micBusy && !s.voiceBusy);
        } else {
            for (int y : {154, 248}) {
                rounded(controlsLeft, y, controlsWidth, rowHeight, 8, segmentTrack);
                centered(controlsLeft, y, controlsWidth, rowHeight, "Unavailable", bodySize, inactive);
            }
        }
        drawButton(Command::MicReset, "RESET FILTERS", control, bodySize, s.micAvailable && !s.micBusy && !s.voiceBusy);
        rect(inset, 452, contentWidth, 1, surface);
        text(inset, 466, "Mic test", bodySize, primary, true);
        drawButton(Command::VoiceRecord, "RECORD 6 SECONDS", control, bodySize, !s.voiceBusy && !s.micBusy);
        drawButton(Command::VoicePlay, "PLAY", control, bodySize, s.voiceReady && !s.voiceBusy && !s.micBusy);
        drawButton(Command::VoiceStop, "STOP", control, bodySize, s.voiceBusy);
        if (!s.voiceStatus.empty()) wrapped(inset, 584, contentWidth, s.voiceStatus, 18, secondary);
        if (!s.micStatus.empty()) wrapped(inset, 830, contentWidth, s.micStatus, 18, secondary);
        return;
    }

    if (s.page == PanelPage::Settings) {
        scrollY_ = std::clamp(s.settingsScroll, 0, settingsScrollMax);
        clipTop_ = 138;
        clipBottom_ = 882;
        label(Command::StartupOff, "Start with SteamVR");
        segment(Command::StartupOff, "Off", Command::StartupOn, "On", s.startup, s.startupAvailable && !s.startupBusy);
        text(inset, 201, "Start automatically when the headset's VR session starts.", 18, secondary);
        if (s.selfUpdates) {
            label(Command::AutoUpdateOff, "Check for updates automatically");
            segment(Command::AutoUpdateOff, "Off", Command::AutoUpdateOn, "On", s.automaticUpdateChecks);
            text(inset, 295, "You'll choose when to install.", 18, secondary);
            label(Command::CheckUpdate, "Updates");
            drawButton(Command::CheckUpdate, "CHECK NOW", control, 20, !s.updateBusy);
            drawButton(Command::InstallUpdate, "INSTALL & RESTART", control, 18, s.updateAvailable && !s.updateBusy);
            if (!s.updateStatus.empty()) text(inset, 394, s.updateStatus, 18, secondary);
        }
        label(Command::DebugLoggingOff, "Detailed logging");
        segment(Command::DebugLoggingOff, "Off", Command::DebugLoggingOn, "On", s.debugLogging);
        text(inset, 483, "Include tracking and movement details for debugging.", 18, secondary);
        label(Command::ExportLogs, "Bug report logs");
        text(inset, 577, "Create an archive to attach to a bug report.", 18, secondary);
        drawButton(Command::ExportLogs, "EXPORT LOGS");
        const auto message = !s.startupStatus.empty() ? s.startupStatus : !s.loggingStatus.empty()
                                ? s.loggingStatus : !s.exportStatus.empty() ? s.exportStatus : s.notice;
        if (!message.empty()) wrapped(inset, 620, contentWidth, message, 18, secondary);
        text(inset, 676, "About", 26, primary, true);
        const auto information = {
            std::make_pair("Version", s.appVersion),
            std::make_pair("Build", s.buildId),
            std::make_pair("Target", s.buildTarget),
            std::make_pair("OpenVR SDK", s.openVRVersion),
            std::make_pair("SteamVR runtime", s.runtimeVersion)};
        int row = 742;
        for (const auto &entry : information) {
            text(inset, row, entry.first, bodySize, primary);
            wrapped(controlsLeft, row, controlsWidth, entry.second, bodySize, secondary);
            row += 78;
        }
        scrollY_ = 0;
        clipTop_ = 0;
        clipBottom_ = height;
        drawButton(Command::SettingsUp, "^", background, 20, s.settingsScroll > 0);
        drawButton(Command::SettingsDown, "v", background, 20, s.settingsScroll < settingsScrollMax);
        rounded(1566, 204, 6, 604, 3, surface);
        rounded(1566, 204 + s.settingsScroll * 168 / settingsScrollMax, 6, 436, 3, sidebarSelected);
        return;
    }
    segment(Command::Disable, "Off", Command::Enable, "On", s.enabled);
    label(Command::Height, "Movement direction");
    segment(Command::Height, "Height only", Command::XYZ, "All axes", s.xyz);
    label(Command::Slower, "Drag speed");
    const int speedY = button(Command::Slower).y;
    rounded(controlsLeft, speedY, controlsWidth, rowHeight, 8, surface);
    drawButton(Command::Slower, "-");
    drawButton(Command::Faster, "+");
    char value[96];
    std::snprintf(value, sizeof(value), "%.2f x", s.gain);
    centered(controlsLeft + 70, speedY, controlsWidth - 140, rowHeight, value, bodySize, primary);
    label(Command::LimitOff, "Distance limit");
    segment(Command::LimitOff, "Off", Command::LimitOn, "On", s.distanceLimit);
    const int limitY = button(Command::LimitSmaller).y;
    rounded(controlsLeft, limitY, controlsWidth, rowHeight, 8, surface);
    drawButton(Command::LimitSmaller, "-");
    drawButton(Command::LimitLarger, "+");
    std::snprintf(value, sizeof(value), "%.1f m", s.distanceLimitMeters);
    centered(controlsLeft + 70, limitY, controlsWidth - 140, rowHeight, value, bodySize,
             s.distanceLimit ? primary : secondary);
    label(Command::GravityOff, "Gravity");
    segment(Command::GravityOff, "Off", Command::GravityOn, "On", s.gravity);
    const int gravityY = button(Command::GravityWeaker).y;
    rounded(controlsLeft, gravityY, controlsWidth, rowHeight, 8, surface);
    drawButton(Command::GravityWeaker, "-");
    drawButton(Command::GravityStronger, "+");
    std::snprintf(value, sizeof(value), "%.1f m/s", s.gravityStrength);
    const int unitWidth = textWidth(value, bodySize);
    const int unitLeft = controlsLeft + (controlsWidth - unitWidth - textWidth("2", 14)) / 2;
    const auto unitColor = s.gravity ? primary : secondary;
    text(unitLeft, gravityY + 14, value, bodySize, unitColor);
    text(unitLeft + unitWidth, gravityY + 12, "2", 14, unitColor);
    label(Command::Bindings, "Controller bindings");
    drawButton(Command::Bindings, "EDIT CONTROLLER BINDINGS");

    rounded(inset, 550, contentWidth, 98, 12, surface);
    text(inset + 26, 562, "Position offset", bodySize, primary);
    const double offsets[] = {s.offset.x, s.offset.y, s.offset.z};
    const char *labels[] = {"X", "Y", "Z"};
    for (int i = 0; i < 3; ++i) {
        const int left = inset + 26 + i * contentWidth / 3;
        if (i)
            rect(left - 24, 596, 1, 34, 0x25282e);
        text(left, 600, labels[i], bodySize, secondary);
        std::snprintf(value, sizeof(value), "%+.2f m", offsets[i]);
        text(left + 46, 596, value, 28, primary);
    }
    drawButton(Command::Reset, "RESET OFFSET");
    if (!s.notice.empty() || s.statusAttention)
        wrapped(inset, 830, contentWidth, s.notice.empty() ? s.status : s.notice, bodySize,
                secondary);
}
Command Panel::hit(double x, double y, PanelPage page) {
    for (const auto &b : buttons)
        if ((b.shared || b.page == page) && x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h)
            return b.command;
    return Command::None;
}
Command Panel::hit(double x, double y, const PanelState &state) {
    if (state.page == PanelPage::Settings && x >= sidebarWidth) {
        if (x >= 1554) {
            const auto fixed = hit(x, y, state.page);
            if (fixed == Command::SettingsUp && state.settingsScroll > 0) return fixed;
            if (fixed == Command::SettingsDown && state.settingsScroll < settingsScrollMax) return fixed;
            return Command::None;
        }
        if (y < 138 || y >= 882) return Command::None;
        y += std::clamp(state.settingsScroll, 0, settingsScrollMax);
    }
    for (const auto &reset : rowResets) {
        const auto &anchor = button(reset.anchor);
        if (anchor.page != state.page || !rowChanged(reset.command, state) ||
            !resetEnabled(reset.command, state)) continue;
        const int left = inset + textWidth(reset.label, bodySize) + resetLabelGap;
        const int top = anchor.y + (anchor.h - resetTargetSize) / 2;
        if (x >= left && x < left + resetTargetSize && y >= top && y < top + resetTargetSize)
            return reset.command;
    }
    const auto command = hit(x, y, state.page);
    if ((command == Command::StartupOn || command == Command::StartupOff) &&
        (!state.startupAvailable || state.startupBusy)) return Command::None;
    if ((command == Command::CheckUpdate || command == Command::InstallUpdate) &&
        (!state.selfUpdates || state.updateBusy)) return Command::None;
    if (command == Command::InstallUpdate && !state.updateAvailable) return Command::None;
    if ((command == Command::AutoUpdateOff || command == Command::AutoUpdateOn) && !state.selfUpdates)
        return Command::None;
    return command;
}
bool Panel::save(const char *path) const {
    FILE *file = std::fopen(path, "wb");
    if (!file)
        return false;
    std::fprintf(file, "P6\n%d %d\n255\n", width, height);
    for (size_t i = 0; i < pixels.size(); i += 4)
        if (std::fwrite(&pixels[i], 1, 3, file) != 3) {
            std::fclose(file);
            return false;
        }
    return std::fclose(file) == 0;
}
