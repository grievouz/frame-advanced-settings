#!/usr/bin/env bash
# Run the real installer against isolated fixture homes; never contact systemd.
set -euo pipefail
project=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_root=$(mktemp -d "${TMPDIR:-/tmp}/frame-install-tests.XXXXXXXX")
package="$test_root/package"
stubs="$test_root/stubs"
mkdir -p "$package"/{lib,input,assets/fonts,assets/icons,licenses,contrib} "$stubs"
cp "$project/install.sh" "$package/"
cp -R "$project/contrib/"* "$package/contrib/"
cp "$project/scripts/microphone-setup.sh" "$package/"
for file in frame-advanced-settings frame-advanced-settings.vrmanifest README.md build-id.txt \
    verify-running.sh export-logs.sh collect-logs.sh update.sh LICENSE \
    lib/libopenvr_api.so input/actions.json assets/fonts/font.ttf licenses/license.txt \
    assets/icons/frame-advanced-settings.png assets/icons/frame-advanced-settings.svg; do
    printf 'fixture %s\n' "$file" > "$package/$file"
done
(cd "$package" && sha256sum frame-advanced-settings > frame-advanced-settings.sha256)
cat > "$stubs/uname" <<'EOF'
#!/bin/sh
echo aarch64
EOF
cat > "$stubs/flock" <<'EOF'
#!/bin/sh
echo "flock $*" >> "$TEST_COMMANDS"
[ "${FAIL_FLOCK:-0}" != 1 ]
EOF
cat > "$stubs/systemctl" <<'EOF'
#!/bin/sh
echo "systemctl $*" >> "$TEST_COMMANDS"
shift
operation=$1
shift
case "$operation" in
    list-units)
        for unit in "$@"; do :; done
        if [ -f "$HOME/.config/systemd/user/$unit" ]; then echo "$unit loaded"; fi
        ;;
    stop) [ "${FAIL_STOP:-}" != "$1" ] ;;
    show) if [ "${STUCK_UNIT:-}" = "$1" ]; then echo 567; else echo 0; fi ;;
    disable|daemon-reload) : ;;
    *) echo "Unexpected systemctl action: $operation" >&2; exit 9 ;;
esac
EOF
chmod +x "$stubs/"*
assert_file() { [ -f "$1" ] || { echo "Missing expected file: $1" >&2; exit 1; }; }
assert_absent() { [ ! -e "$1" ] || { echo "Unexpected active path: $1" >&2; exit 1; }; }
assert_text() { grep -qF -- "$2" "$1" || { echo "Missing expected content: $2 in $1" >&2; exit 1; }; }
fixture() {
    fixture_home="$test_root/$1"
    config="$fixture_home/.config"
    state="$fixture_home/.local/state"
    data="$fixture_home/.local/share"
    mkdir -p "$fixture_home"
    commands="$fixture_home/commands.txt"
    : > "$commands"
}
installed_fixture() {
    mkdir -p "$data/frame-advanced-settings" "$config/frame-advanced-settings" \
        "$state/frame-advanced-settings/logs/session-one" "$fixture_home/.config/systemd/user" "$data/applications"
    printf 'previous binary\n' > "$data/frame-advanced-settings/frame-advanced-settings"
    printf 'drag_gain=3\nmovement_enabled=true\n' > "$config/frame-advanced-settings/settings.ini"
    printf 'saved log survives\n' > "$state/frame-advanced-settings/logs/session-one/events.jsonl"
    printf 'previous service\n' > "$fixture_home/.config/systemd/user/frame-advanced-settings.service"
    printf 'previous launcher\n' > "$data/applications/frame-advanced-settings.desktop"
    : > "$state/frame-advanced-settings/instance.lock"
}
install_fixture() {
    env HOME="$fixture_home" XDG_CONFIG_HOME="$config" XDG_STATE_HOME="$state" \
        TEST_COMMANDS="$commands" PATH="$stubs:$PATH" "$@" \
        sh "$package/install.sh" > "$fixture_home/install-output.txt" 2>&1
}

fixture fresh
install_fixture
assert_file "$data/frame-advanced-settings/frame-advanced-settings"
assert_file "$config/systemd/user/frame-advanced-settings.service"
assert_file "$data/applications/frame-advanced-settings.desktop"
assert_text "$data/applications/frame-advanced-settings.desktop" 'reload-or-restart frame-advanced-settings.service'
assert_file "$data/frame-advanced-settings/assets/icons/frame-advanced-settings.png"
assert_text "$data/applications/frame-advanced-settings.desktop" "Icon=$data/frame-advanced-settings/assets/icons/frame-advanced-settings.png"
assert_text "$config/systemd/user/frame-advanced-settings.service" '%h/.local/share/frame-advanced-settings/frame-advanced-settings'
! grep -q ' enable ' "$commands"

fixture update
installed_fixture
install_fixture
assert_text "$config/frame-advanced-settings/settings.ini" 'drag_gain=3'
assert_text "$state/frame-advanced-settings/logs/session-one/events.jsonl" 'saved log survives'
assert_text "$data/frame-advanced-settings/frame-advanced-settings" 'fixture frame-advanced-settings'
assert_text "$commands" 'stop frame-advanced-settings.service'
! grep -qE 'systemctl --user (disable|enable) ' "$commands"
assert_text "$commands" 'flock -n 9'
compgen -G "$data/frame-advanced-settings.backup.*/frame-advanced-settings" > /dev/null

for failure in stop stuck lock; do
    fixture "$failure"
    installed_fixture
    case "$failure" in
        stop) failure_option=FAIL_STOP=frame-advanced-settings.service ;;
        stuck) failure_option=STUCK_UNIT=frame-advanced-settings.service ;;
        lock) failure_option=FAIL_FLOCK=1 ;;
    esac
    if install_fixture "$failure_option"; then echo "Expected $failure to abort installation" >&2; exit 1; fi
    assert_text "$config/frame-advanced-settings/settings.ini" 'drag_gain=3'
    assert_text "$state/frame-advanced-settings/logs/session-one/events.jsonl" 'saved log survives'
    assert_text "$data/frame-advanced-settings/frame-advanced-settings" 'previous binary'
    ! compgen -G "$data/frame-advanced-settings.backup.*" > /dev/null
done

fixture custom-xdg
config="$fixture_home/custom-config"
state="$fixture_home/custom-state"
installed_fixture
install_fixture
assert_text "$config/frame-advanced-settings/settings.ini" 'drag_gain=3'
assert_text "$state/frame-advanced-settings/logs/session-one/events.jsonl" 'saved log survives'

fixture symlink
mkdir -p "$state" "$fixture_home/outside"
if ln -s "$fixture_home/outside" "$state/frame-advanced-settings" && [ -L "$state/frame-advanced-settings" ]; then
    if install_fixture; then echo 'Expected a symlinked state directory to abort' >&2; exit 1; fi
    assert_absent "$data/frame-advanced-settings"
else
    echo 'Symlink fixture skipped: this host cannot create native symbolic links.'
fi

fixture relative-xdg
installed_fixture
install_fixture XDG_CONFIG_HOME=relative-config XDG_STATE_HOME=relative-state
assert_text "$config/frame-advanced-settings/settings.ini" 'drag_gain=3'
assert_text "$state/frame-advanced-settings/logs/session-one/events.jsonl" 'saved log survives'
printf 'Installer checks passed. Fixtures: %s\n' "$test_root"
