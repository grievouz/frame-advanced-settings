#include "update.h"
#include <stdexcept>
#include <iostream>
static void check(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
static std::string release(const std::string &v, bool beta = false, bool draft = false,
                           const std::string &host = "https://github.com", bool checksum = true) {
    const auto name = "frame-advanced-settings-" + v + "-linux-aarch64.tar.gz";
    const auto base = host + "/grievouz/frame-advanced-settings/releases/download/v" + v + "/";
    auto result = "{\"tag_name\":\"v" + v + "\",\"draft\":" + (draft ? "true" : "false") +
                  ",\"prerelease\":" + (beta ? "true" : "false") + ",\"assets\":[" +
                  "{\"name\":\"" + name + "\",\"size\":5000000,\"browser_download_url\":\"" + base + name + "\"}";
    if (checksum) result += ",{\"name\":\"" + name + ".sha256\",\"size\":130,\"browser_download_url\":\"" + base + name + ".sha256\"}";
    return result + "]}";
}
int main() {
    using namespace updates;
    for (const auto *bad : {"v1.2.3", "01.2.3", "1.2", "1.2.3;id", "1.2.3-beta.0", "1.2.3-beta.01",
                            "1.2.3-alpha.1", "9999999999999.0.0", "1.2.3\n"})
        check(!Version::parse(bad), "Invalid version rejected");
    check(*Version::parse("1.2.9") < *Version::parse("1.2.10"), "Numeric version ordering");
    check(*Version::parse("1.2.3-beta.10") < *Version::parse("1.2.3"), "Final beats beta");
    check(!(*Version::parse("1.2.3") < *Version::parse("1.2.3-beta.99")), "Beta does not replace final");
    auto result = select("[" + release("0.9.0") + "," + release("0.10.0") + "," + release("0.11.0-beta.1", true) + "]", "0.1.0");
    check(result && result->version == "0.10.0", "Select newest complete stable package, regardless of API order");
    check(!select("[" + release("0.1.0") + "]", "0.1.0"), "Equal version never overwrites development build");
    check(!select("[" + release("0.1.0") + "]", "0.2.0"), "No downgrade");
    check(!select("[" + release("1.0.0", false, true) + "]", "0.1.0"), "Draft is ineligible");
    check(!select("[" + release("1.0.0", true) + "]", "0.1.0"), "Inconsistent prerelease flags rejected");
    check(!select("[" + release("1.0.0", false, false, "https://example.com") + "]", "0.1.0"), "Third party URL rejected");
    check(!select("[" + release("1.0.0", false, false, "https://github.com", false) + "]", "0.1.0"), "Missing checksum means unpublished package");
    result = select("[" + release("1.0.0-beta.2", true) + "]", "1.0.0-beta.1");
    check(result && result->version == "1.0.0-beta.2", "Beta installation can advance on beta channel");
    result = select("[" + release("1.0.0") + "]", "1.0.0-beta.2");
    check(result && result->version == "1.0.0", "Beta upgrades to final");
    check(!select("[{},null,42,{\"tag_name\":12}]", "0.1.0"), "Malformed entries do not create updates");
    for (const auto *bad : {"not json", "{\"message\":\"API limit exceeded\"}"}) {
        bool rejected = false;
        try { select(bad, "0.1.0"); } catch (const std::exception &) { rejected = true; }
        check(rejected, "Malformed/API error responses are errors, never up-to-date");
    }
    std::cout << "Release selection and version checks passed\n";
}
