#!/bin/sh
set -eu

die() { printf '%s\n' "$*" >&2; exit 1; }
backup_record=''
if [ "$#" -gt 0 ]; then
    [ "$#" = 2 ] && [ "$1" = --backup-record ] || die 'Unknown install argument'
    backup_record=$2
fi
if [ -z "${HOME:-}" ] || [ "$HOME" = / ]; then die 'Invalid HOME'; fi
if [ "$(uname -m)" != aarch64 ]; then die 'This package requires an ARM64 Steam Frame.'; fi
source_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
app=frame-advanced-settings
destination="$HOME/.local/share/$app"
case "${XDG_CONFIG_HOME:-}" in /*) config_root=$XDG_CONFIG_HOME ;; *) config_root="$HOME/.config" ;; esac
case "${XDG_STATE_HOME:-}" in /*) state_root=$XDG_STATE_HOME ;; *) state_root="$HOME/.local/state" ;; esac
units="$HOME/.config/systemd/user"
desktop="$HOME/.local/share/applications"
stamp="$(date +%Y%m%d-%H%M%S).$$"

# Refuse redirected app directories, including symlinked ancestors. Never follow
# a link while checking app data or replacing binaries and launchers.
check_path() {
    checked_path=$1
    case "$checked_path" in /*) ;; *) die "Expected an absolute path: $checked_path" ;; esac
    while [ "$checked_path" != / ]; do
        [ ! -L "$checked_path" ] || die "Refusing a symlinked installation path: $checked_path"
        checked_path=$(dirname -- "$checked_path")
    done
}
for checked in "$destination" "$config_root/$app" "$state_root/$app/instance.lock" \
    "$units/$app.service" "$desktop/$app.desktop"; do
    check_path "$checked"
done
for checked in "$destination" "$config_root/$app" "$state_root/$app"; do
    [ ! -e "$checked" ] || [ -d "$checked" ] || die "Expected a directory: $checked"
done
[ -f "$source_dir/$app" ] || die 'Install from the built package.'
[ -f "$source_dir/contrib/$app.service" ] && [ -f "$source_dir/contrib/$app.desktop" ] ||
    die 'The package is missing its service or launcher.'
[ -f "$source_dir/assets/icons/$app.png" ] || die 'The package is missing its application icon.'
(cd "$source_dir" && sha256sum -c "$app.sha256")

# Stop and verify the installed service before moving any files. A graceful
# stop lets the previous app restore its movement offset.
unit="$app.service"
loaded_unit=$(systemctl --user list-units --all --plain --no-legend "$unit")
if [ -n "$loaded_unit" ] || [ -f "$units/$unit" ]; then
    systemctl --user stop "$unit"
    old_pid=$(systemctl --user show "$unit" -p MainPID --value)
    [ "${old_pid:-0}" = 0 ] || die "$unit is still running; installation stopped."
fi

# Service status does not catch an app launched directly. Hold its existing
# inode locked until the update is complete. Preferences and logs stay in place.
command -v flock >/dev/null 2>&1 || die 'The flock utility is required for a safe update.'
if [ -f "$state_root/$app/instance.lock" ]; then
    exec 9<>"$state_root/$app/instance.lock"
    flock -n 9 || die 'Frame Advanced Settings is still running outside systemd; close it before installing.'
fi

# Prepare and validate the replacement first. A failed copy leaves the installed
# files intact; an incomplete staging directory is retained for inspection.
staging="$destination.install.$stamp"
[ ! -e "$staging" ] && [ ! -L "$staging" ] || die "Staging path already exists: $staging"
mkdir -p "$staging/lib" "$staging/input" "$staging/licenses" "$staging/assets/fonts" "$staging/assets/icons" "$staging/contrib/wireplumber"
cp "$source_dir/$app" "$source_dir/$app.vrmanifest" "$source_dir/README.md" "$staging/"
cp "$source_dir/build-id.txt" "$source_dir/$app.sha256" "$source_dir/verify-running.sh" "$staging/"
cp "$source_dir/export-logs.sh" "$source_dir/collect-logs.sh" "$source_dir/microphone-setup.sh" "$staging/"
cp "$source_dir/update.sh" "$staging/"
cp "$source_dir/contrib/wireplumber/"* "$staging/contrib/wireplumber/"
cp "$source_dir/lib/libopenvr_api.so" "$staging/lib/"
cp "$source_dir/input/"*.json "$staging/input/"
cp "$source_dir/assets/fonts/"*.ttf "$staging/assets/fonts/"
cp "$source_dir/assets/icons/$app.png" "$source_dir/assets/icons/$app.svg" "$staging/assets/icons/"
cp "$source_dir/licenses/"*.txt "$staging/licenses/"
cp "$source_dir/LICENSE" "$staging/"
chmod 755 "$staging/$app" "$staging/export-logs.sh" "$staging/collect-logs.sh" "$staging/verify-running.sh"
(cd "$staging" && sha256sum -c "$app.sha256")

archive_path() {
    if [ -e "$1" ]; then
        archived="$1.backup.$stamp"
        [ ! -e "$archived" ] && [ ! -L "$archived" ] || die "Backup already exists: $archived"
        mv -- "$1" "$archived"
        printf 'Previous files saved at %s\n' "$archived"
    fi
}
archive_path "$destination"
if [ -n "$backup_record" ] && [ -d "$destination.backup.$stamp" ]; then
    printf '%s\n' "$destination.backup.$stamp" > "$backup_record"
fi
mv -- "$staging" "$destination"
mkdir -p "$units" "$desktop"
cp "$source_dir/contrib/$app.service" "$units/"
# Resolve the packaged icon for desktop launchers without relying on a theme cache.
while IFS= read -r line || [ -n "$line" ]; do
    case "$line" in
        Icon=*) printf 'Icon=%s/assets/icons/%s.png\n' "$destination" "$app" ;;
        *) printf '%s\n' "$line" ;;
    esac
done < "$source_dir/contrib/$app.desktop" > "$desktop/$app.desktop"
systemctl --user daemon-reload
if ! sh "$destination/microphone-setup.sh"; then
    echo "App installed; microphone setup was skipped. See the reason above." >&2
fi
echo 'Installed Frame Advanced Settings. Your startup choice and saved settings are preserved.'
echo 'Open it from Launch program (+), or run: systemctl --user reload-or-restart frame-advanced-settings'
echo 'Logs: journalctl --user -u frame-advanced-settings -n 80 --no-pager'
