#!/usr/bin/env bash
# Exercise the actual updater/installer with local releases and isolated homes.
# No network, host systemd or installed application is touched.
set -euo pipefail
# MSYS has no RLIMIT_FSIZE; production and Linux CI retain the real limit.
if [[ $OSTYPE == msys* ]]; then ulimit() { :; }; export -f ulimit; fi
project=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_root=$(mktemp -d "${TMPDIR:-/tmp}/frame-update-tests.XXXXXXXX")
app=frame-advanced-settings
package=$test_root/package/$app
stubs=$test_root/stubs
mkdir -p "$package"/{lib,input,assets/fonts,assets/icons,licenses,contrib} "$stubs"
cp "$project/install.sh" "$package/"
cp -R "$project/contrib/"* "$package/contrib/"
cp "$project/scripts/"{update,microphone-setup}.sh "$package/"
for file in "$app" "$app.vrmanifest" README.md verify-running.sh export-logs.sh collect-logs.sh LICENSE \
    lib/libopenvr_api.so input/actions.json assets/fonts/font.ttf licenses/license.txt \
    assets/icons/$app.png assets/icons/$app.svg; do printf 'new binary %s\n' "$file" > "$package/$file"; done
printf '0.2.0+0123456789ab\n' > "$package/build-id.txt"
(cd "$package" && sha256sum "$app" > "$app.sha256")
tar -czf "$test_root/good.tar.gz" -C "$test_root/package" "$app"
cat > "$stubs/curl" <<'EOF'
#!/usr/bin/env bash
set -eu
out=''
while (( $# )); do
    case $1 in --output) out=$2; shift 2;; *) url=$1; shift;; esac
done
if [[ $url == *.sha256 ]]; then cp "$PAYLOAD.sha256" "$out"; else cp "$PAYLOAD" "$out"; fi
EOF
cat > "$stubs/systemctl" <<'EOF'
#!/usr/bin/env bash
set -eu
echo "$*" >> "$TEST_COMMANDS"
shift
case $1 in
    list-units) echo 'frame-advanced-settings.service loaded' ;;
    show) echo 0 ;;
    start)
        if [[ ${FAIL_START:-0} == 1 ]] && grep -q 'new binary' "$HOME/.local/share/frame-advanced-settings/frame-advanced-settings"; then exit 1; fi ;;
    stop|daemon-reload|is-active) : ;;
    *) echo "Unexpected operation: $*" >&2; exit 1 ;;
esac
EOF
printf '#!/bin/sh\necho aarch64\n' > "$stubs/uname"
printf '#!/bin/sh\nexit 0\n' > "$stubs/flock"
printf '#!/bin/sh\nexit 0\n' > "$stubs/sleep"
printf '#!/bin/sh\nshift\nexec "$@"\n' > "$stubs/timeout"
chmod +x "$stubs/"*
fixture() {
    fixture_home=$test_root/$1
    state=$fixture_home/.local/state/$app
    data=$fixture_home/.local/share/$app
    mkdir -p "$data" "$state" "$fixture_home/.config/$app" \
        "$fixture_home/.config/systemd/user/steamvr.service.wants" "$fixture_home/.local/share/applications"
    printf 'original binary\n' > "$data/$app"
    printf 'original service\n' > "$fixture_home/.config/systemd/user/$app.service"
    printf 'original launcher\n' > "$fixture_home/.local/share/applications/$app.desktop"
    printf 'startup stays enabled\n' > "$fixture_home/.config/systemd/user/steamvr.service.wants/$app.service"
    printf 'drag_gain=2\nautomatic_update_checks=false\n' > "$fixture_home/.config/$app/settings.ini"
    commands=$fixture_home/commands
    : > "$commands"
    cp "$test_root/good.tar.gz" "$fixture_home/package.tar.gz"
    payload=$fixture_home/package.tar.gz
    checksum
}
checksum() {
    digest=$(sha256sum "$payload")
    printf '%s  frame-advanced-settings-0.2.0-linux-aarch64.tar.gz\n' "${digest%% *}" > "$payload.sha256"
}
run_update() {
    env HOME="$fixture_home" XDG_STATE_HOME="$fixture_home/.local/state" XDG_CONFIG_HOME="$fixture_home/.config" \
        TEST_COMMANDS="$commands" PAYLOAD="$payload" PATH="$stubs:$PATH" "$@" \
        bash "$project/scripts/update.sh" 0.2.0
}
assert_preserved() {
    grep -q 'automatic_update_checks=false' "$fixture_home/.config/$app/settings.ini"
    grep -q 'startup stays enabled' "$fixture_home/.config/systemd/user/steamvr.service.wants/$app.service"
}
fixture success
run_update
grep -q 'new binary' "$data/$app"
grep -q '^done$' "$state/update-status.txt"
test -f "$data/show-dashboard"
assert_preserved
fixture rollback
if run_update FAIL_START=1; then echo 'Expected failed startup' >&2; exit 1; fi
grep -q 'original binary' "$data/$app"
grep -q 'original service' "$fixture_home/.config/systemd/user/$app.service"
grep -q 'original launcher' "$fixture_home/.local/share/applications/$app.desktop"
grep -q '^error$' "$state/update-status.txt"
assert_preserved
fixture corrupt
printf 'changed archive bytes' >> "$payload"
if run_update; then echo 'Expected checksum rejection' >&2; exit 1; fi
test ! -s "$commands"
grep -q 'original binary' "$data/$app"
assert_preserved
fixture traversal
tar -czf "$payload" --transform='s@frame-advanced-settings/build-id.txt@frame-advanced-settings/../escape@' -C "$test_root/package" "$app"
checksum
if run_update; then echo 'Expected traversal rejection' >&2; exit 1; fi
test ! -s "$commands"
fixture wrong-version
printf '0.3.0+0123456789ab\n' > "$package/build-id.txt"
tar -czf "$payload" -C "$test_root/package" "$app"
checksum
if run_update; then echo 'Expected version rejection' >&2; exit 1; fi
test ! -s "$commands"
printf 'Update install, rollback, integrity, traversal and preservation checks passed. Fixtures: %s\n' "$test_root"
