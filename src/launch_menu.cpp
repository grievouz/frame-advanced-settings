#include "launch_menu.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace launchmenu {
namespace fs = std::filesystem;
namespace {
constexpr size_t maxEntry = 256 * 1024, maxState = 8 * 1024 * 1024, maxRecords = 256;
using Text = std::optional<std::string>;
using Fields = std::map<std::string, std::string>;

std::string trim(std::string s) {
    const auto first = s.find_first_not_of(" \t\r");
    return first == std::string::npos ? ""
                                      : s.substr(first, s.find_last_not_of(" \t\r") - first + 1);
}
std::string lower(std::string s) {
    for (auto &c : s)
        if (c >= 'A' && c <= 'Z')
            c += 'a' - 'A';
    return s;
}
Text readFile(const fs::path &path, size_t limit = maxEntry) {
    std::error_code ec;
    const auto status = fs::symlink_status(path, ec);
    if (ec == std::errc::no_such_file_or_directory || status.type() == fs::file_type::not_found)
        return {};
    if (ec)
        throw std::runtime_error("Cannot inspect shortcut: " + path.string());
    if (!fs::is_regular_file(path, ec) || ec)
        throw std::runtime_error("Not a regular file: " + path.string());
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    const auto size = in ? in.tellg() : std::streampos(-1);
    if (size < 0 || size > static_cast<std::streamoff>(limit))
        throw std::runtime_error("Unreadable or oversized file: " + path.string());
    std::string result(static_cast<size_t>(size), '\0');
    in.seekg(0);
    in.read(result.data(), result.size());
    if (!in || result.find('\0') != std::string::npos)
        throw std::runtime_error("Invalid file: " + path.string());
    return result;
}
bool safeRelative(const fs::path &p) {
    if (p.empty() || p.is_absolute() || p.has_root_name() || p.extension() != ".desktop")
        return false;
    for (const auto &part : p)
        if (part == ".." || part == "." || part.empty() ||
            part.string().find('\\') != std::string::npos)
            return false;
    return true;
}
std::string desktopId(const fs::path &p) {
    auto id = p.generic_string();
    std::replace(id.begin(), id.end(), '/', '-');
    return id;
}
// The configured root may itself live on another disk. Never follow a symlink
// below it when writing an override (including a dangling shortcut symlink).
bool safeTarget(const fs::path &root, const fs::path &relative) {
    if (!safeRelative(relative))
        return false;
    fs::path current = root;
    for (const auto &part : relative) {
        current /= part;
        std::error_code ec;
        const auto status = fs::symlink_status(current, ec);
        if (ec && ec != std::errc::no_such_file_or_directory)
            return false;
        if (fs::is_symlink(status))
            return false;
        if (current != root / relative && fs::exists(status) && !fs::is_directory(status))
            return false;
        if (current == root / relative && fs::exists(status) && !fs::is_regular_file(status))
            return false;
    }
    return true;
}
void atomicWrite(const fs::path &path, const std::string &text) {
    fs::create_directories(path.parent_path());
    std::error_code permissionError;
    const auto originalStatus = fs::symlink_status(path, permissionError);
    static std::atomic<unsigned> sequence{0};
    auto temporary = path;
    temporary += ".fas-tmp-" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                 std::to_string(sequence++);
    bool ok = false;
#ifdef _WIN32
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        throw std::runtime_error("Cannot create temporary file");
    DWORD written = 0;
    ok = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) &&
         written == text.size() && FlushFileBuffers(file);
    CloseHandle(file);
    if (ok)
        ok = MoveFileExW(temporary.c_str(), path.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
#else
    const int fd =
        ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0)
        throw std::runtime_error("Cannot create temporary file");
    size_t offset = 0;
    while (offset < text.size()) {
        const auto count = ::write(fd, text.data() + offset, text.size() - offset);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            break;
        offset += static_cast<size_t>(count);
    }
    ok = offset == text.size() && ::fsync(fd) == 0;
    ::close(fd);
    if (ok && !permissionError && fs::is_regular_file(originalStatus)) {
        fs::permissions(temporary, originalStatus.permissions(), permissionError);
        ok = !permissionError;
    }
    if (ok)
        ok = ::rename(temporary.c_str(), path.c_str()) == 0;
    if (ok) {
        const int dir = ::open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (dir >= 0) {
            ::fsync(dir);
            ::close(dir);
        }
    }
#endif
    if (!ok) {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        throw std::runtime_error("Could not save " + path.string());
    }
}
Fields fields(const std::string &text) {
    Fields result;
    std::istringstream in(text);
    std::string line;
    bool active = false;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line.front() == '#')
            continue;
        if (line.front() == '[') {
            active = line == "[Desktop Entry]";
            continue;
        }
        const auto eq = line.find('=');
        if (active && eq != std::string::npos)
            result[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
    }
    return result;
}
std::string replaceFields(const std::string &source, const Fields &updates) {
    std::string result;
    bool active = false, inserted = false;
    auto insert = [&] {
        if (!result.empty() && result.back() != '\n')
            result += '\n';
        for (const auto &field : updates)
            result += field.first + "=" + field.second + "\n";
        inserted = true;
    };
    size_t pos = 0;
    while (pos < source.size()) {
        const auto end = source.find('\n', pos);
        const auto next = end == std::string::npos ? source.size() : end + 1;
        const auto raw = source.substr(pos, next - pos);
        const auto line =
            trim(source.substr(pos, (end == std::string::npos ? source.size() : end) - pos));
        if (!line.empty() && line.front() == '[') {
            if (active)
                insert();
            active = line == "[Desktop Entry]";
        }
        const auto eq = line.find('=');
        const bool replaced =
            active && eq != std::string::npos && updates.count(trim(line.substr(0, eq)));
        if (!replaced)
            result += raw;
        pos = next;
    }
    if (active)
        insert();
    if (!inserted)
        throw std::runtime_error("Shortcut has no Desktop Entry section");
    return result;
}
bool launchable(const Fields &f) {
    auto get = [&](const char *key) {
        auto i = f.find(key);
        return i == f.end() ? "" : i->second;
    };
    return get("Type") == "Application" && !get("Name").empty() &&
           (!get("Exec").empty() || get("DBusActivatable") == "true");
}
std::vector<std::string> split(const std::string &text, char separator) {
    std::vector<std::string> result;
    std::istringstream in(text);
    std::string item;
    while (std::getline(in, item, separator))
        if (!item.empty())
            result.push_back(item);
    return result;
}
bool menuEligible(const Fields &f, const Paths &paths) {
    auto get = [&](const char *key) {
        const auto found = f.find(key);
        return found == f.end() ? std::string{} : found->second;
    };
    const auto desktops = split(paths.currentDesktop, ':');
    const auto only = split(get("OnlyShowIn"), ';');
    const auto excluded = split(get("NotShowIn"), ';');
    auto matches = [&](const auto &list) {
        return std::any_of(desktops.begin(), desktops.end(), [&](const auto &desktop) {
            return std::find(list.begin(), list.end(), desktop) != list.end();
        });
    };
    if ((!only.empty() && !matches(only)) || matches(excluded))
        return false;
    // Steam's dashboard explicitly omits its own launcher. Do not match by
    // substring: unrelated applications containing "Steam" remain eligible.
    if (get("Name") == "Steam")
        return false;
    const auto tryExec = get("TryExec");
    if (!tryExec.empty()) {
        auto executable = [](const fs::path &path) {
            std::error_code ec;
            if (!fs::is_regular_file(path, ec) || ec)
                return false;
#ifndef _WIN32
            return ::access(path.c_str(), X_OK) == 0;
#else
            return true; // Host fixtures; the shipped Linux build checks X_OK.
#endif
        };
        const fs::path command(tryExec);
        if (command.is_absolute()) {
            if (!executable(command))
                return false;
        } else if (command.has_parent_path() ||
                   std::none_of(
                       paths.executablePaths.begin(), paths.executablePaths.end(),
                       [&](const auto &directory) { return executable(directory / command); }))
            return false;
    }
    return true;
}
std::string displayName(const Fields &f, std::string locale) {
    // Locale order follows the Desktop Entry specification, without loading a
    // locale into global process state (the scanner runs off the render thread).
    const auto encoding = locale.find('.');
    if (encoding != std::string::npos) {
        const auto modifier = locale.find('@', encoding);
        locale.erase(encoding,
                     modifier == std::string::npos ? std::string::npos : modifier - encoding);
    }
    std::vector<std::string> candidates{locale};
    const auto modifier = locale.find('@');
    const auto territory = locale.find('_');
    if (modifier != std::string::npos)
        candidates.push_back(locale.substr(0, modifier));
    if (territory != std::string::npos && modifier != std::string::npos)
        candidates.push_back(locale.substr(0, territory) + locale.substr(modifier));
    candidates.push_back(locale.substr(0, std::min(territory, modifier)));
    std::string name = f.at("Name");
    for (const auto &candidate : candidates) {
        auto found = f.find("Name[" + candidate + "]");
        if (found != f.end() && !found->second.empty()) {
            name = found->second;
            break;
        }
    }
    std::string clean;
    for (size_t i = 0; i < name.size() && clean.size() < 512; ++i) {
        unsigned char c = name[i];
        if (c == '\\' && i + 1 < name.size()) {
            c = name[++i];
            if (c == 's' || c == 'n' || c == 't' || c == 'r')
                c = ' ';
        }
        clean += c < 32 || c == 127 ? ' ' : static_cast<char>(c);
    }
    return clean;
}
fs::path envPath(const char *key, const fs::path &fallback) {
    const auto value = std::getenv(key);
    return value && fs::path(value).is_absolute() ? fs::path(value) : fallback;
}
void writeText(std::ostream &out, const Text &value) {
    out << value.has_value() << ' ' << std::quoted(value.value_or("")) << '\n';
}
Text readText(std::istream &in) {
    int present = 0;
    std::string value;
    if (!(in >> present >> std::quoted(value)) || (present != 0 && present != 1) ||
        value.size() > maxEntry)
        throw std::runtime_error("Invalid saved Launch Menu data");
    return present ? Text(value) : Text{};
}
} // namespace

Paths Paths::fromEnvironment() {
    const auto home = envPath("HOME", {});
    if (home.empty())
        throw std::runtime_error("HOME is unavailable");
    Paths result;
    result.user = envPath("XDG_DATA_HOME", home / ".local/share") / "applications";
    result.state = envPath("XDG_STATE_HOME", home / ".local/state") /
                   "frame-advanced-settings/launch-menu.state";
    const auto dirs = std::getenv("XDG_DATA_DIRS");
    std::istringstream stream(dirs && *dirs ? dirs : "/usr/local/share:/usr/share");
    std::string path;
    while (std::getline(stream, path, ':'))
        if (fs::path(path).is_absolute())
            result.system.push_back(fs::path(path) / "applications");
    result.system.push_back(home / ".local/share/flatpak/exports/share/applications");
    result.system.emplace_back("/var/lib/flatpak/exports/share/applications");
    for (const auto key : {"LC_ALL", "LC_MESSAGES", "LANG"}) {
        const auto value = std::getenv(key);
        if (value && *value) {
            result.locale = value;
            break;
        }
    }
    if (const auto desktop = std::getenv("XDG_CURRENT_DESKTOP"))
        result.currentDesktop = desktop;
    if (const auto executablePath = std::getenv("PATH"))
        for (const auto &directory : split(executablePath, ':'))
            if (fs::path(directory).is_absolute())
                result.executablePaths.emplace_back(directory);
    return result;
}
Store::Store(Paths paths) : paths_(std::move(paths)) {
    if (!paths_.user.is_absolute() || !paths_.state.is_absolute())
        throw std::runtime_error("Launch Menu paths must be absolute");
    load();
}
std::vector<Entry> Store::scan() const {
    std::vector<fs::path> roots{paths_.user};
    roots.insert(roots.end(), paths_.system.begin(), paths_.system.end());
    struct Source {
        fs::path relative;
        std::string text;
        bool user;
    };
    std::map<std::string, std::vector<Source>> sources;
    std::set<fs::path> seen;
    size_t examined = 0, bytesRead = 0;
    for (const auto &root : roots) {
        if (!root.is_absolute() || !seen.insert(root.lexically_normal()).second)
            continue;
        std::error_code ec;
        fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied,
                                            ec),
            end;
        for (; !ec && it != end; it.increment(ec)) {
            if (++examined > 20000)
                throw std::runtime_error("Too many shortcut files to scan");
            if (it.depth() >= 8)
                it.disable_recursion_pending();
            if (it->path().extension() != ".desktop")
                continue;
            const auto relative = it->path().lexically_relative(root);
            if (!safeRelative(relative))
                continue;
            try {
                auto text = readFile(it->path());
                if (text) {
                    bytesRead += text->size();
                    sources[desktopId(relative)].push_back({relative, *text, root == paths_.user});
                }
            } catch (
                const std::runtime_error &) { /* Unreadable entries cannot be offered for edits. */
            }
            if (bytesRead > 32 * 1024 * 1024)
                throw std::runtime_error("Shortcut scan exceeded its memory limit");
        }
    }
    std::vector<Entry> entries;
    for (const auto &group : sources) {
        const auto &stack = group.second;
        const auto &top = stack.front();
        auto meta = fields(top.text);
        auto text = top.text;
        if (!launchable(meta)) {
            // KDE may write only Hidden=true. Inherit launch data when removing
            // that tombstone, while preserving its other explicit overrides.
            if (meta.count("Type") && meta["Type"] != "Application")
                continue;
            for (size_t i = 1; i < stack.size(); ++i) {
                auto base = fields(stack[i].text);
                if (!launchable(base))
                    continue;
                text = replaceFields(stack[i].text, meta);
                meta = fields(text);
                break;
            }
        }
        if (!launchable(meta))
            continue;
        Entry entry;
        entry.id = group.first;
        entry.name = displayName(meta, paths_.locale);
        entry.appId = entry.id.substr(0, entry.id.size() - std::string(".desktop").size());
        const auto flatpakId = meta["X-Flatpak"];
        if (!flatpakId.empty() && flatpakId.size() <= 255 &&
            flatpakId.find_first_not_of(
                "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") ==
                std::string::npos)
            entry.appId = flatpakId;
        const auto iconValue = meta["Icon"];
        for (size_t i = 0; i < iconValue.size(); ++i) {
            char c = iconValue[i];
            if (c == '\\' && i + 1 < iconValue.size()) {
                c = iconValue[++i];
                if (c == 's')
                    c = ' ';
            }
            if (static_cast<unsigned char>(c) >= 32 && c != 127)
                entry.iconName += c;
        }
        entry.visible = meta["Hidden"] != "true" && meta["NoDisplay"] != "true";
        entry.menuEligible = menuEligible(meta, paths_);
        entry.text = std::move(text);
        entry.relative = top.user ? top.relative : fs::path(entry.id);
        if (top.user)
            entry.userText = top.text;
        entry.revision = top.text + '\0' + entry.text;
        entry.editable = safeTarget(paths_.user, entry.relative);
        // Ambiguous foo-bar.desktop vs foo/bar.desktop IDs are not safe to edit.
        if (std::count_if(stack.begin(), stack.end(), [](const Source &s) { return s.user; }) > 1)
            entry.editable = false;
        entries.push_back(std::move(entry));
    }
    std::sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) {
        const auto an = lower(a.name), bn = lower(b.name);
        return an == bn ? a.id < b.id : an < bn;
    });
    return entries;
}
View Store::read() const {
    View view;
    view.enabled = enabled_;
    view.entries = scan();
    // Keep our own hidden entries reachable for Show/Reset, but do not turn
    // every system helper with NoDisplay=true into a menu customization row.
    view.entries.erase(
        std::remove_if(view.entries.begin(), view.entries.end(),
                       [&](const Entry &e) {
                           const bool managed =
                               std::any_of(records_.begin(), records_.end(),
                                           [&](const Record &record) { return record.id == e.id; });
                           return !managed && (!e.visible || !e.menuEligible);
                       }),
        view.entries.end());
    // While Off the switches still show the remembered choices, dimmed.
    if (!enabled_)
        for (auto &entry : view.entries)
            for (const auto &record : records_)
                if (entry.id == record.id)
                    entry.visible = record.visible;
    return view;
}
void Store::load() {
    const auto raw = readFile(paths_.state, maxState);
    if (!raw)
        return;
    std::istringstream in(*raw);
    std::string magic, root;
    size_t count = 0;
    int enabled = 0;
    if (!(in >> magic >> std::quoted(root) >> enabled >> count) || magic != "FAS-LAUNCH-1" ||
        root != paths_.user.generic_string() || enabled < 0 || enabled > 1 || count > maxRecords)
        throw std::runtime_error(
            "Cannot read saved Launch Menu data; existing shortcuts were left alone");
    enabled_ = enabled != 0;
    std::set<std::string> ids;
    for (size_t i = 0; i < count; ++i) {
        Record record;
        std::string relative;
        int visible = 0;
        if (!(in >> std::quoted(record.id) >> std::quoted(relative) >> visible) || visible < 0 ||
            visible > 1)
            throw std::runtime_error("Invalid saved shortcut record");
        record.relative = fs::path(relative);
        record.visible = visible != 0;
        record.original = readText(in);
        const auto applied = readText(in);
        record.previous = readText(in);
        if (!safeRelative(record.relative) || desktopId(record.relative) != record.id || !applied ||
            !ids.insert(record.id).second || !launchable(fields(*applied)))
            throw std::runtime_error("Invalid saved shortcut record");
        record.applied = *applied;
        records_.push_back(std::move(record));
    }
    in >> std::ws;
    if (!in.eof())
        throw std::runtime_error("Unexpected data in saved Launch Menu settings");
}
void Store::save() const {
    std::ostringstream out;
    out << "FAS-LAUNCH-1\n"
        << std::quoted(paths_.user.generic_string()) << '\n'
        << enabled_ << ' ' << records_.size() << '\n';
    for (const auto &record : records_) {
        out << std::quoted(record.id) << ' ' << std::quoted(record.relative.generic_string()) << ' '
            << record.visible << '\n';
        writeText(out, record.original);
        writeText(out, record.applied);
        writeText(out, record.previous);
    }
    if (records_.size() > maxRecords || out.str().size() > maxState)
        throw std::runtime_error(
            "Launch Menu backup limit reached; reset unused customizations first");
    atomicWrite(paths_.state, out.str());
}
bool Store::reconcile(const Record &record, bool apply) const {
    if (!safeTarget(paths_.user, record.relative))
        return false;
    const auto target = paths_.user / record.relative;
    const auto actual = readFile(target);
    const Text desired = apply ? Text(record.applied) : record.original;
    if (actual == desired)
        return true;
    if (actual != record.original && actual != Text(record.applied) &&
        (!record.previous || actual != record.previous))
        return false;
    if (desired)
        atomicWrite(target, *desired);
    else
        fs::remove(target); // Only our exact recorded override can reach this branch.
    return true;
}
size_t Store::reconcileAll(bool apply) {
    size_t conflicts = 0;
    for (auto &record : records_) {
        if (reconcile(record, apply))
            record.previous.reset();
        else
            ++conflicts;
    }
    return conflicts;
}
View Store::resume() {
    if (records_.empty())
        return read();
    const auto conflicts = reconcileAll(enabled_);
    save();
    auto view = read();
    if (conflicts)
        view.message =
            "Some shortcuts changed elsewhere and were left alone. Reset to clear saved choices.";
    return view;
}
View Store::enable(bool value) {
    enabled_ = value;
    // Journal the intent and original bytes before changing any shortcut. On a
    // crash, resume can recognize either side without overwriting external edits.
    save();
    const auto conflicts = reconcileAll(value);
    save();
    auto view = read();
    view.message =
        conflicts
            ? "Some shortcuts changed elsewhere and were left alone. Reset to clear saved choices."
            : "";
    return view;
}
View Store::visibility(const Entry &requested, bool visible) {
    if (!enabled_)
        throw std::runtime_error("Turn Launch Menu on to edit shortcuts");
    const auto entries = scan();
    const auto found = std::find_if(entries.begin(), entries.end(),
                                    [&](const Entry &e) { return e.id == requested.id; });
    if (found == entries.end() || found->revision != requested.revision ||
        found->relative != requested.relative)
        throw std::runtime_error("Shortcut changed. Refresh the list and try again");
    if (!found->editable)
        throw std::runtime_error("This shortcut cannot be edited safely");
    auto record = std::find_if(records_.begin(), records_.end(),
                               [&](const Record &r) { return r.id == found->id; });
    const auto next = replaceFields(found->text, {{"Hidden", visible ? "false" : "true"},
                                                  {"NoDisplay", visible ? "false" : "true"}});
    if (next.size() > maxEntry)
        throw std::runtime_error("Shortcut is too large to edit");
    if (record == records_.end()) {
        records_.push_back({found->id, found->relative, found->userText, next, {}, visible});
    } else {
        if (record->relative != found->relative)
            throw std::runtime_error(
                "Shortcut location changed. Reset saved choices before editing it");
        const auto actual = readFile(paths_.user / record->relative);
        if (actual != record->original && actual != Text(record->applied))
            throw std::runtime_error(
                "Shortcut changed elsewhere. Reset saved choices before editing it");
        record->previous = record->applied;
        record->applied = next;
        record->visible = visible;
    }
    save();
    const auto conflicts = reconcileAll(true);
    save();
    auto view = read();
    view.message = conflicts ? "Some shortcuts changed elsewhere and were left alone." : "";
    return view;
}
View Store::reset() {
    const bool wasEnabled = enabled_;
    enabled_ = false;
    save();
    const auto conflicts = reconcileAll(false);
    records_.clear();
    enabled_ = wasEnabled;
    save();
    auto view = read();
    view.message = conflicts ? "Reset. Shortcuts edited elsewhere were left unchanged." : "";
    return view;
}
} // namespace launchmenu
