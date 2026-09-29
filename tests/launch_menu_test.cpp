#include "launch_menu.h"
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
namespace fs = std::filesystem;
using launchmenu::Store;
static void check(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
static void put(const fs::path &path, const std::string &text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << text;
    check(bool(out), "Write fixture");
}
static std::string get(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    check(bool(in), "Read fixture");
    return std::string(std::istreambuf_iterator<char>(in), {});
}
static launchmenu::Entry entry(const launchmenu::View &view, const std::string &id) {
    for (const auto &item : view.entries)
        if (item.id == id)
            return item;
    throw std::runtime_error("Missing entry: " + id);
}
static bool contains(const launchmenu::View &view, const std::string &id) {
    for (const auto &item : view.entries)
        if (item.id == id)
            return true;
    return false;
}
template <typename F> static void rejects(F &&operation, const char *message) {
    bool failed = false;
    try {
        operation();
    } catch (const std::exception &) {
        failed = true;
    }
    check(failed, message);
}
int main() {
    const auto root =
        fs::absolute(fs::current_path() /
                     ("launch-menu-fixture-" +
                      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())))
            .lexically_normal();
    try {
        check(fs::create_directory(root), "Create isolated fixture");
        launchmenu::Paths paths{root / "user/applications",
                                root / "state/launch-menu.state",
                                {root / "system-high", root / "system-low"},
                                "de_DE.UTF-8",
                                "KDE"};
        const std::string original =
            "# Keep this comment\n[Desktop Entry]\nType=Application\nName=Files\n"
            "Name[de]=Dateien\nExec=/usr/bin/files --safe %U\nOnlyShowIn=KDE;\nIcon=folder\n"
            "Actions=New;\n\n[Desktop Action New]\nName=New\nExec=/usr/bin/files "
            "--new\nNoDisplay=false\n";
        put(paths.system[0] / "files.desktop", original);
        put(paths.system[1] / "files.desktop",
            "[Desktop Entry]\nType=Application\nName=Wrong priority\nExec=wrong\n");
        const std::string userOriginal =
            "[Desktop Entry]\r\nType=Application\r\nName=Player\r\nExec=player\r\n";
        put(paths.user / "player.desktop", userOriginal);
        put(paths.system[0] / "service.desktop",
            "[Desktop "
            "Entry]\nType=Application\nName=Background\nExec=background\nNoDisplay=true\n");
        put(paths.system[0] / "dbus.desktop",
            "[Desktop Entry]\nType=Application\nName=DBus App\nDBusActivatable=true\n"
            "X-Flatpak=org.example.DBus\nIcon=/tmp/My\\sApp/icon.png\n");
        put(paths.system[0] / "link.desktop",
            "[Desktop Entry]\nType=Link\nName=Link\nURL=https://example.org\n");
        put(paths.system[0] / "huge.desktop", std::string(256 * 1024 + 1, 'x'));
        put(paths.system[0] / "tombstone.desktop", original);
        const std::string tombstone = "# Partial user override\n[Desktop Entry]\nHidden=false\n";
        put(paths.user / "tombstone.desktop", tombstone);
        put(paths.user / "nested/app.desktop",
            "[Desktop Entry]\nType=Application\nName=Nested\nExec=nested\n");
        const auto nestedOriginal = get(paths.user / "nested/app.desktop");

        auto view = Store(paths).read();
        check(!view.enabled && view.entries.size() == 5,
              "Read-only default includes menu applications and DBus entries, not helpers");
        check(!fs::exists(paths.state), "Reading does not write configuration");
        check(entry(view, "files.desktop").name == "Dateien", "Localized name and source priority");
        check(entry(view, "files.desktop").appId == "files" &&
                  entry(view, "files.desktop").iconName == "folder",
              "App ID comes from desktop identity and themed Icon is preserved");
        check(entry(view, "dbus.desktop").appId == "org.example.DBus" &&
                  entry(view, "dbus.desktop").iconName == "/tmp/My App/icon.png",
              "Flatpak ID and escaped absolute icon path are decoded");
        check(!contains(view, "service.desktop"), "Unmanaged NoDisplay helper is omitted");
        check(entry(view, "tombstone.desktop").visible, "Partial visibility override honored");
        rejects([&] { Store(paths).visibility(entry(view, "files.desktop"), false); },
                "Off cannot edit");

        view = Store(paths).enable(true);
        view = Store(paths).visibility(entry(view, "files.desktop"), false);
        check(contains(view, "files.desktop") && !entry(view, "files.desktop").visible,
              "Hiding through the manager keeps the row available for Show/Reset");
        const auto hidden = get(paths.user / "files.desktop");
        check(hidden.find("Hidden=true") != std::string::npos &&
                  hidden.find("NoDisplay=true") != std::string::npos,
              "Hide sets both visibility keys in a user override");
        check(hidden.find("OnlyShowIn=KDE;") != std::string::npos &&
                  hidden.find("Exec=/usr/bin/files --safe %U") != std::string::npos,
              "Preserve restrictions and command without execution");
        check(
            hidden.find("# Keep this comment") == 0 &&
                hidden.find(
                    "[Desktop Action New]\nName=New\nExec=/usr/bin/files --new\nNoDisplay=false") !=
                    std::string::npos,
            "Preserve comments and action sections");
        check(get(paths.system[0] / "files.desktop") == original, "System shortcut untouched");
        view = Store(paths).visibility(entry(view, "player.desktop"), false);
        view = Store(paths).visibility(entry(view, "tombstone.desktop"), false);
        check(get(paths.user / "tombstone.desktop").find("Exec=/usr/bin/files") !=
                  std::string::npos,
              "Editing a partial override preserves inherited launch data");
        view = Store(paths).visibility(entry(view, "nested-app.desktop"), false);
        check(!fs::exists(paths.user / "nested-app.desktop"),
              "Edit existing nested override without creating duplicate ID");

        view = Store(paths).enable(false);
        check(!view.enabled && !entry(view, "files.desktop").visible,
              "Off remembers saved Hide choice");
        check(!fs::exists(paths.user / "files.desktop"), "Off removes only generated override");
        check(get(paths.user / "player.desktop") == userOriginal,
              "Off restores exact original user bytes including CRLF");
        check(get(paths.user / "tombstone.desktop") == tombstone,
              "Off restores original minimal tombstone");
        check(get(paths.user / "nested/app.desktop") == nestedOriginal,
              "Off restores nested original");
        view = Store(paths).enable(true);
        check(get(paths.user / "files.desktop") == hidden,
              "On reapplies remembered choice across Store restart");
        view = Store(paths).visibility(entry(view, "files.desktop"), true);
        check(entry(view, "files.desktop").visible, "Changing an already managed shortcut works");
        view = Store(paths).reset();
        check(view.enabled && entry(view, "files.desktop").visible &&
                  !fs::exists(paths.user / "files.desktop"),
              "Reset restores original visibility while keeping module On");
        Store(paths).enable(false);
        view = Store(paths).enable(true);
        check(!fs::exists(paths.user / "files.desktop"), "Reset clears remembered choices");

        const auto stale = entry(view, "player.desktop");
        put(paths.user / "player.desktop", userOriginal + "# External change\n");
        rejects([&] { Store(paths).visibility(stale, false); }, "Stale UI cannot overwrite edits");
        view = Store(paths).read();
        view = Store(paths).visibility(entry(view, "player.desktop"), false);
        const auto external = userOriginal + "# Changed while module active\n";
        put(paths.user / "player.desktop", external);
        view = Store(paths).enable(false);
        check(get(paths.user / "player.desktop") == external && !view.message.empty(),
              "Off leaves externally edited file alone");
        Store(paths).enable(true);
        check(get(paths.user / "player.desktop") == external, "On also refuses external conflict");
        Store(paths).reset();
        check(get(paths.user / "player.desktop") == external,
              "Reset forgets conflict without overwriting external file");

        view = Store(paths).read();
        view = Store(paths).visibility(entry(view, "files.desktop"), false);
        // Simulate a crash after the durable journal but before the override.
        fs::remove(paths.user / "files.desktop");
        Store(paths).resume();
        check(get(paths.user / "files.desktop") == hidden, "Resume finishes journaled write");
        Store(paths).enable(false);
        // Simulate a crash while restoring a generated override.
        put(paths.user / "files.desktop", hidden);
        Store(paths).resume();
        check(!fs::exists(paths.user / "files.desktop"), "Resume finishes journaled restoration");
        Store(paths).reset();

        put(paths.user / "collision-a.desktop", userOriginal);
        put(paths.user / "collision/a.desktop", userOriginal);
        view = Store(paths).read();
        check(!entry(view, "collision-a.desktop").editable,
              "Ambiguous desktop ID cannot be edited");
        std::error_code symlinkError;
        fs::create_symlink(paths.system[0] / "files.desktop", paths.user / "symlink.desktop",
                           symlinkError);
        if (!symlinkError) {
            view = Store(paths).enable(true);
            check(!entry(view, "symlink.desktop").editable, "Symlink shown but not writable");
            rejects([&] { Store(paths).visibility(entry(view, "symlink.desktop"), false); },
                    "Symlink edit refused");
            check(get(paths.system[0] / "files.desktop") == original, "Symlink source untouched");
        } else
            std::cout
                << "Symlink fixture unavailable on this host; runtime check still compiled.\n";

        const auto savedState = get(paths.state);
        std::ostringstream invalidRecord;
        invalidRecord << "FAS-LAUNCH-1\n"
                      << std::quoted(paths.user.generic_string()) << "\n1 1\n"
                      << std::quoted("escape.desktop") << ' ' << std::quoted("../escape.desktop")
                      << " 1\n0 \"\"\n1 " << std::quoted(original) << "\n0 \"\"\n";
        put(paths.state, invalidRecord.str());
        rejects([&] { Store(paths).resume(); }, "Journal cannot restore outside applications root");
        check(!fs::exists(paths.user.parent_path() / "escape.desktop"), "Escaped target untouched");
        put(paths.state, "not a valid state file");
        rejects([&] { Store(paths).enable(true); },
                "Corrupt journal must not be replaced silently");
        check(get(paths.state) == "not a valid state file", "Corrupt state left for recovery");
        put(paths.state, savedState);

        // Menu membership is separate from file syntax and editable visibility.
        // No commands are launched while checking desktop restrictions/TryExec.
        launchmenu::Paths filtered{
            root / "filtered/user", root / "filtered/state", {root / "filtered/system"}, "",
            "gamescope:KDE",        {root / "filtered/bin"}};
        auto app = [&](const char *id, const std::string &extra) {
            put(filtered.system[0] / id,
                "[Desktop Entry]\nType=Application\nName=Application\nExec=app\n" + extra);
        };
        app("visible.desktop", "");
        app("hidden.desktop", "Hidden=true\n");
        app("helper.desktop", "NoDisplay=true\n");
        app("kde.desktop", "OnlyShowIn=GNOME;KDE;\n");
        app("other.desktop", "OnlyShowIn=GNOME;\n");
        app("exact.desktop", "OnlyShowIn=KDE2;\n");
        app("excluded.desktop", "NotShowIn=KDE;\n");
        app("allowed.desktop", "NotShowIn=GNOME;\n");
        app("missing.desktop", "TryExec=missing-program\n");
        app("relative.desktop", "TryExec=./available\n");
        app("present.desktop", "TryExec=available\n");
        app("steam.desktop", "Name=Steam\n");
        app("steam-name.desktop", "Name=Steam Tools Example\n");
        put(filtered.executablePaths[0] / "available", "not executed\n");
        fs::permissions(filtered.executablePaths[0] / "available", fs::perms::owner_exec,
                        fs::perm_options::add);
        app("absolute.desktop",
            "TryExec=" + (filtered.executablePaths[0] / "available").generic_string() + "\n");
        auto filteredView = Store(filtered).read();
        check(filteredView.entries.size() == 6 && contains(filteredView, "visible.desktop") &&
                  contains(filteredView, "kde.desktop") &&
                  contains(filteredView, "allowed.desktop") &&
                  contains(filteredView, "present.desktop") &&
                  contains(filteredView, "absolute.desktop") &&
                  contains(filteredView, "steam-name.desktop"),
              "Menu filters hidden helpers, exact desktop restrictions, TryExec and Steam itself");
        check(!fs::exists(filtered.state), "Filtering is read-only");
        Store(filtered).enable(true);
        filteredView = Store(filtered).visibility(entry(filteredView, "visible.desktop"), false);
        check(contains(filteredView, "visible.desktop"), "Managed hidden row survives filtering");
        filteredView = Store(filtered).read();
        check(!entry(filteredView, "visible.desktop").visible,
              "Managed hidden row remains after reopening the store");
        filtered.currentDesktop = "GNOME";
        filteredView = Store(filtered).read();
        check(!contains(filteredView, "allowed.desktop") &&
                  contains(filteredView, "other.desktop") &&
                  contains(filteredView, "visible.desktop"),
              "Desktop changes re-evaluate membership without losing managed rows");
        filteredView = Store(filtered).enable(false);
        check(contains(filteredView, "visible.desktop") &&
                  !entry(filteredView, "visible.desktop").visible &&
                  !fs::exists(filtered.user / "visible.desktop"),
              "Off restores files and keeps the remembered hide choice in the list");
        filteredView = Store(filtered).reset();
        check(entry(filteredView, "visible.desktop").visible,
              "Reset restores menu visibility and clears the remembered override");

        check(root.parent_path() == fs::current_path().lexically_normal() &&
                  root.filename().string().rfind("launch-menu-fixture-", 0) == 0,
              "Cleanup stays inside this test's generated directory");
        fs::remove_all(root);
        std::cout << "Launch Menu priority, preservation, Off/On, Reset, conflict and restart "
                     "checks passed.\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << "\nFixture retained at " << root << '\n';
        return 1;
    }
}
