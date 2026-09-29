#!/bin/sh
# Install/remove only our user-level WirePlumber hook. Never restart audio here.
set -eu
die() { printf 'Microphone setup: %s\n' "$*" >&2; exit 1; }
case "${HOME:-}" in /*) [ "$HOME" != / ] || die 'Invalid HOME' ;; *) die 'Invalid HOME' ;; esac
case "${XDG_CONFIG_HOME:-}" in /*) config_root=$XDG_CONFIG_HOME ;; *) config_root="$HOME/.config" ;; esac
case "${XDG_DATA_HOME:-}" in /*) data_root=$XDG_DATA_HOME ;; *) data_root="$HOME/.local/share" ;; esac
source_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
source_files="$source_dir/contrib/wireplumber"
# Source-tree invocation is also useful for fixture tests.
[ -d "$source_files" ] || source_files="$source_dir/../contrib/wireplumber"
script="$data_root/wireplumber/scripts/frame-advanced-settings-mic.lua"
conf_dir="$config_root/wireplumber/wireplumber.conf.d"
conf="$conf_dir/90-frame-advanced-settings-mic.conf"
owned_hook() {
    [ -f "$1" ] || return 1
    case "$1" in
        "$conf")
            grep -qF 'custom.frame-advanced-settings-mic' "$1" &&
                grep -qF 'frame-advanced-settings.mic-echo-cancel' "$1" &&
                grep -qF 'frame-advanced-settings.mic-noise-suppression' "$1" ;;
        "$script")
            grep -qF 'frame-advanced-settings-mic-stream-added' "$1" &&
                grep -qF 'frame-advanced-settings-mic-stream-removed' "$1" &&
                grep -qF 'Settings.subscribe ("frame-advanced-settings.mic-*"' "$1" ;;
        *) return 1 ;;
    esac
}
check_path() {
    checked=$1
    while [ "$checked" != / ]; do
        [ ! -L "$checked" ] || die "Refusing symlink: $checked"
        checked=$(dirname -- "$checked")
    done
}
for target in "$script" "$conf"; do
    check_path "$target"
    if [ -e "$target" ]; then
        owned_hook "$target" || die "Not an owned hook: $target"
    fi
done
case "${1:-}" in
    --remove)
        # Explicit, nonrecursive file removal; no other app's hook/settings.
        rm -f -- "$conf" "$script"
        echo 'Microphone hook removed. Reboot the headset to restore the stock tracker.'
        exit 0 ;;
    '') ;;
    *) die 'Usage: microphone-setup.sh [--remove]' ;;
esac
command -v wpctl >/dev/null 2>&1 || die 'wpctl is unavailable; microphone controls were not installed.'
command -v wireplumber >/dev/null 2>&1 || die 'WirePlumber is unavailable; microphone controls were not installed.'
version=$(wireplumber --version)
printf '%s\n' "$version" | grep -qE '(^|[^0-9])0\.5\.[0-9]+' || die 'This hook requires WirePlumber 0.5; setup skipped.'

# Refuse competing user overrides; reuse an existing compatible settings schema.
reuse=0
for candidate in "$conf_dir/"*.conf; do
    [ -f "$candidate" ] || continue
    [ "$candidate" != "$conf" ] || continue
    if grep -qF 'steamos.microphone-tracker' "$candidate"; then
        if grep -qF 'custom.frame-mic-tracker' "$candidate" &&
            grep -qF 'frame-mic.echo-cancel' "$candidate" &&
            grep -qF 'frame-mic.noise-suppression' "$candidate"; then
            reuse=1
        else
            die 'Another microphone-tracker override exists. Existing audio configuration was left untouched.'
        fi
    fi
done
if [ "$reuse" = 1 ]; then
    # If a compatible hook was installed later, retire our files so the next boot
    # cannot load two trackers. Currently loaded scripts remain until reboot.
    if [ -f "$conf" ] || [ -f "$script" ]; then
        rm -f -- "$conf" "$script"
        echo 'Microphone: using existing controls. Reboot once to finish switching hooks.'
    else
        echo 'Microphone: using existing controls; no additional hook installed.'
    fi
    exit 0
fi
stock_script=/etc/wireplumber/scripts/microphone-tracker.lua
[ -r "$stock_script" ] || die 'Steam Frame microphone tracker not found; setup skipped.'
for token in 'steamos.mic_filter' 'filter.smart.disabled' 'Stream/Input/Audio'; do
    grep -qF "$token" "$stock_script" || die 'Unrecognized SteamOS microphone tracker; setup skipped.'
done
[ -f "$source_files/frame-advanced-settings-mic.lua" ] &&
    [ -f "$source_files/90-frame-advanced-settings-mic.conf" ] || die 'Packaged microphone hook is missing.'
# Reject control characters rather than inserting them into a config string.
case "$script" in *'
'*|*'\'*|*'"'*) die 'Unsupported character in the WirePlumber script path.' ;; esac
mkdir -p -- "$(dirname -- "$script")" "$conf_dir"
staging=$(mktemp -d "$conf_dir/.frame-advanced-settings-mic.XXXXXXXX")
trap 'rm -f -- "$staging/script" "$staging/conf"; rmdir -- "$staging"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
cp -- "$source_files/frame-advanced-settings-mic.lua" "$staging/script"
escaped_script=$(printf '%s' "$script" | sed 's/[&|]/\\&/g')
sed "s|@SCRIPT_PATH@|$escaped_script|g" "$source_files/90-frame-advanced-settings-mic.conf" > "$staging/conf"
chmod 644 "$staging/script" "$staging/conf"
changed=0
if ! cmp -s "$staging/script" "$script"; then
    # Stage beside the target so the rename is atomic even with separate XDG mounts.
    tmp_script=$(mktemp "$(dirname -- "$script")/.frame-advanced-settings-mic.XXXXXXXX")
    if ! cp -- "$staging/script" "$tmp_script" || ! chmod 644 "$tmp_script" ||
        ! mv -f -- "$tmp_script" "$script"; then
        rm -f -- "$tmp_script"
        die 'Could not install microphone script.'
    fi
    changed=1
fi
if ! cmp -s "$staging/conf" "$conf"; then
    mv -f -- "$staging/conf" "$conf"
    changed=1
fi
if [ "$changed" = 1 ]; then
    echo 'Microphone controls installed. Reboot the headset once to load the hook. Audio services were not restarted.'
else
    echo 'Microphone hook is already up to date.'
fi
