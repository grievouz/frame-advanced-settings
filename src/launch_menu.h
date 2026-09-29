#pragma once
#include "app_icons.h"
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

// Desktop entries are data, never shell commands. This module only edits
// Hidden/NoDisplay in user overrides; it never launches or removes applications.
namespace launchmenu {
struct Paths {
    std::filesystem::path user, state;
    // Highest priority first. User overrides always take precedence.
    std::vector<std::filesystem::path> system;
    std::string locale;
    std::string currentDesktop;
    std::vector<std::filesystem::path> executablePaths;
    static Paths fromEnvironment();
};
struct Entry {
    std::string id, name, text, revision, appId, iconName;
    std::shared_ptr<const appicons::Bitmap> icon;
    std::filesystem::path relative;
    std::optional<std::string> userText;
    bool visible = true, editable = true;
    bool menuEligible = true;
};
struct View {
    bool enabled = false;
    std::vector<Entry> entries;
    std::string message;
};
class Store {
  public:
    explicit Store(Paths paths);
    View read() const;
    // Finish an interrupted, previously requested change after restarting.
    View resume();
    View enable(bool value);
    View visibility(const Entry &entry, bool visible);
    View reset();

  private:
    struct Record {
        std::string id;
        std::filesystem::path relative;
        std::optional<std::string> original;
        std::string applied;
        std::optional<std::string> previous;
        bool visible = true;
    };
    Paths paths_;
    bool enabled_ = false;
    std::vector<Record> records_;
    std::vector<Entry> scan() const;
    void load();
    void save() const;
    bool reconcile(const Record &record, bool apply) const;
    size_t reconcileAll(bool apply);
};
} // namespace launchmenu
