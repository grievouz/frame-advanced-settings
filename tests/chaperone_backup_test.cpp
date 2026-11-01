#include "chaperone_backup.h"
#include <cstdio>
#include <stdexcept>
namespace fs = std::filesystem;
static void check(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
static void write(const fs::path &path, const std::string &data) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << data;
    out.close();
    check(bool(out), "fixture write");
}
int main() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = fs::temp_directory_path() / ("frame-chaperone-test-" + std::to_string(stamp));
    const auto config = root / "config", state = root / "state", runtime = root / "runtime-config";
    try {
        check(!chaperonebackup::save(config, state), "missing runtime registry refuses repair");
        nlohmann::json registry = {{"config", {runtime.generic_string()}}};
        write(config / "openvr/openvrpaths.vrpath", registry.dump());
        check(!chaperonebackup::save(config, state), "missing actual calibration refuses repair");
        const auto source = runtime / "chaperone_info.vrchap";
        write(source, "broken");
        check(!chaperonebackup::save(config, state),
              "invalid calibration is not backed up for commit");
        write(source, "{\"universes\":[]}");
        check(!chaperonebackup::save(config, state), "empty calibration refuses repair");
        for (int i = 0; i < 8; ++i) {
            const std::string data =
                "{\"universes\":[{\"universeID\":\"" + std::to_string(i + 1) + "\"}]}";
            write(source, data);
            const auto backup = chaperonebackup::save(config, state);
            check(backup && backup->source == source &&
                      chaperonebackup::read(backup->copy) == std::optional<std::string>{data},
                  "exact registered calibration is backed up");
            const auto second = chaperonebackup::save(config, state);
            check(second && second->copy == backup->copy, "identical backups are reused");
        }
        size_t count = 0;
        for (const auto &entry : fs::directory_iterator(state / "chaperone-backups"))
            if (entry.path().extension() == ".vrchap")
                ++count;
        check(count == 5, "calibration backups have bounded retention");
        write(source, std::string(chaperonebackup::maxFileBytes + 1, 'x'));
        check(!chaperonebackup::save(config, state),
              "oversized input cannot consume unbounded storage");
        write(config / "openvr/openvrpaths.vrpath", "{\"config\":[\"relative/path\"]}");
        check(!chaperonebackup::save(config, state), "unresolved runtime path refuses repair");
        fs::remove_all(root);
        std::puts("Calibration backup checks passed");
        return 0;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "%s (fixture: %s)\n", e.what(), root.string().c_str());
        return 1;
    }
}
