#!/usr/bin/env bash
# Isolated fixture homes and command stubs. No host audio or service changes.
set -euo pipefail
project=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
root=$(mktemp -d "${TMPDIR:-/tmp}/frame-microphone-tests.XXXXXXXX")
mkdir -p "$root/package/contrib" "$root/stubs"
cp -R "$project/contrib/wireplumber" "$root/package/contrib/"
# Replace the single read-only system probe with our fixture path; the installed
# helper never supports alternate system roots or changing /etc.
sed "s|stock_script=/etc/wireplumber/scripts/microphone-tracker.lua|stock_script=$root/stock.lua|" \
    "$project/scripts/microphone-setup.sh" > "$root/package/microphone-setup.sh"
printf '%s\n' 'steamos.mic_filter filter.smart.disabled Stream/Input/Audio' > "$root/stock.lua"
cat > "$root/stubs/wireplumber" <<'EOF'
#!/bin/sh
[ "$*" = --version ] || exit 19
echo "wireplumber ${TEST_WP_VERSION:-0.5.10}"
EOF
cat > "$root/stubs/wpctl" <<'EOF'
#!/bin/sh
echo 'Unexpected wpctl invocation: the installer must not change settings' >&2
exit 19
EOF
cat > "$root/stubs/systemctl" <<'EOF'
#!/bin/sh
echo 'Unexpected systemctl invocation: the installer must not restart audio' >&2
exit 19
EOF
chmod +x "$root/stubs/"*
fixture() {
    fixture_home="$root/$1"
    config="$fixture_home/custom config"
    data="$fixture_home/custom data & icons"
    conf="$config/wireplumber/wireplumber.conf.d/90-frame-advanced-settings-mic.conf"
    script="$data/wireplumber/scripts/frame-advanced-settings-mic.lua"
    mkdir -p "$fixture_home"
}
setup() {
    env HOME="$fixture_home" XDG_CONFIG_HOME="$config" XDG_DATA_HOME="$data" \
        PATH="$root/stubs:$PATH" sh "$root/package/microphone-setup.sh" "$@" > "$fixture_home/output" 2>&1
}
has() { grep -qF -- "$2" "$1" || { echo "Missing $2 in $1" >&2; exit 1; }; }
compatible_hook() {
    printf '%s\n' 'steamos.microphone-tracker = disabled' \
        'custom.frame-mic-tracker = required' \
        'frame-mic.echo-cancel = true' 'frame-mic.noise-suppression = false' \
        > "$(dirname "$conf")/external-hook.conf"
}
fixture fresh
setup
has "$conf" "name = \"$script\""
has "$script" 'Settings.subscribe ("frame-advanced-settings.mic-*"'
[[ $(grep -c 'default = true' "$conf") == 2 ]]
has "$fixture_home/output" 'Reboot the headset once'
before=$(stat -c %Y "$conf")
setup
has "$fixture_home/output" 'already up to date'
[[ $(stat -c %Y "$conf") == "$before" ]]
[[ -z $(find "$config/wireplumber" "$data/wireplumber" -name '.frame-advanced-settings-mic.*' -print) ]]
setup --remove
[[ ! -e "$conf" && ! -e "$script" ]]

fixture existing-controls
mkdir -p "$(dirname "$conf")"
compatible_hook
sum=$(sha256sum "$(dirname "$conf")/external-hook.conf")
setup
has "$fixture_home/output" 'using existing controls'
[[ ! -e "$conf" && ! -e "$script" ]]
[[ $(sha256sum "$(dirname "$conf")/external-hook.conf") == "$sum" ]]

fixture conflict
mkdir -p "$(dirname "$conf")"
printf 'steamos.microphone-tracker = disabled\n' > "$(dirname "$conf")/custom.conf"
if setup; then echo 'Expected conflict refusal' >&2; exit 1; fi
[[ ! -e "$conf" && ! -e "$script" ]]
has "$(dirname "$conf")/custom.conf" 'disabled'

fixture unowned
mkdir -p "$(dirname "$conf")"
printf 'user file\n' > "$conf"
if setup; then echo 'Expected unowned-file refusal' >&2; exit 1; fi
if setup --remove; then echo 'Expected unowned-file removal refusal' >&2; exit 1; fi
has "$conf" 'user file'

fixture unowned-script
mkdir -p "$(dirname "$script")"
printf '%s\n' 'frame-advanced-settings-mic-stream-added' > "$script"
if setup; then echo 'Expected partial script identity to be refused' >&2; exit 1; fi
if setup --remove; then echo 'Expected unowned script removal to be refused' >&2; exit 1; fi
has "$script" 'frame-advanced-settings-mic-stream-added'
[[ ! -e "$conf" ]]

fixture comment-independent-update
setup
printf '\n# Local comment\n' >> "$conf"
printf '\n-- Local comment\n' >> "$script"
setup
! grep -qF 'Local comment' "$conf"
! grep -qF 'Local comment' "$script"
setup --remove
[[ ! -e "$conf" && ! -e "$script" ]]

fixture unsupported-version
export TEST_WP_VERSION=0.4.17
if setup; then echo 'Expected version refusal' >&2; exit 1; fi
[[ ! -e "$conf" && ! -e "$script" ]]
unset TEST_WP_VERSION

fixture unsupported-tracker
printf 'unknown tracker\n' > "$root/stock.lua"
if setup; then echo 'Expected unsupported stock tracker refusal' >&2; exit 1; fi
[[ ! -e "$conf" && ! -e "$script" ]]

fixture replacement-controls
printf '%s\n' 'steamos.mic_filter filter.smart.disabled Stream/Input/Audio' > "$root/stock.lua"
setup
compatible_hook
setup
[[ ! -e "$conf" && ! -e "$script" ]]
has "$fixture_home/output" 'Reboot once to finish switching hooks'

printf 'Microphone setup fixtures passed: %s\n' "$root"
