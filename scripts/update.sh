#!/usr/bin/env bash
# Separate user service: stopping the dashboard must not stop its updater.
set -euo pipefail
export LC_ALL=C
umask 077
app=frame-advanced-settings
die() { printf '%s\n' "$*" >&2; exit 1; }
[[ $# == 1 && $1 =~ ^(0|[1-9][0-9]{0,8})\.(0|[1-9][0-9]{0,8})\.(0|[1-9][0-9]{0,8})(-beta\.[1-9][0-9]{0,8})?$ ]] || die 'Invalid release version'
version=$1
[[ ${HOME:-} == /* && $HOME != / ]] || die 'Invalid HOME'
[[ $(uname -m) == aarch64 ]] || die 'Updates require an ARM64 Steam Frame'
for tool in curl tar sha256sum flock timeout awk systemctl; do command -v "$tool" >/dev/null || die "Missing tool: $tool"; done
case ${XDG_STATE_HOME:-} in /*) state=$XDG_STATE_HOME/$app ;; *) state=$HOME/.local/state/$app ;; esac
destination=$HOME/.local/share/$app
check_path() {
    local path=$1
    while [[ $path != / ]]; do
        [[ ! -L $path ]] || die "Refusing symlinked path: $path"
        path=$(dirname -- "$path")
    done
}
for path in "$state/update.lock" "$state/update-status.txt" "$state/update-status.txt.tmp" "$state/update.log" "$destination"; do check_path "$path"; done
mkdir -p -- "$state"
exec 9>"$state/update.lock"
flock -n 9 || die 'An update is already running'
exec >"$state/update.log" 2>&1
stage=$(mktemp -d /tmp/frame-advanced-settings-update.XXXXXX)
status() {
    printf '%s\n%s\n' "$1" "$2" > "$state/update-status.txt.tmp"
    mv -f -- "$state/update-status.txt.tmp" "$state/update-status.txt"
    printf '%s\n' "$2"
}
installed=false
finished=false
cleanup() {
    local result=$?
    trap - EXIT
    if [[ $finished != true ]]; then
        status error 'Update failed. Your settings are kept; see update.log in the bug report.'
        if [[ -f $stage/previous ]]; then
            local previous
            previous=$(< "$stage/previous")
            if [[ $previous == "$destination".backup.* && $previous != *$'\n'* && -d $previous && ! -L $previous ]]; then
                systemctl --user stop "$app.service" || true
                if [[ -d $destination && ! -L $destination ]]; then
                    mv -- "$destination" "$destination.failed.$(date +%s).$$"
                fi
                mv -- "$previous" "$destination"
                if [[ -f $stage/old.service ]]; then cp -- "$stage/old.service" "$HOME/.config/systemd/user/$app.service"; fi
                if [[ -f $stage/old.desktop ]]; then cp -- "$stage/old.desktop" "$HOME/.local/share/applications/$app.desktop"; fi
                systemctl --user daemon-reload || true
            fi
        fi
        if [[ $installed == true ]]; then systemctl --user start "$app.service" || true; fi
    fi
    case $stage in /tmp/frame-advanced-settings-update.*) rm -rf -- "$stage" ;; esac
    exit "$result"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM HUP
status busy "Downloading version $version..."
archive=$app-$version-linux-aarch64.tar.gz
url=https://github.com/grievouz/frame-advanced-settings/releases/download/v$version
curl_args=(--disable --fail --silent --show-error --location --proto =https --proto-redir =https --connect-timeout 10 --max-time 120)
curl "${curl_args[@]}" --max-filesize 1024 --output "$stage/checksum" "$url/$archive.sha256"
curl "${curl_args[@]}" --max-filesize 33554432 --output "$stage/package.tar.gz" "$url/$archive"
[[ $(wc -c < "$stage/package.tar.gz") -le 33554432 ]] || die 'Package is too large'
checksum=$(< "$stage/checksum")
[[ $checksum =~ ^([a-fA-F0-9]{64})[[:space:]][[:space:]]([^[:space:]]+)$ ]] || die 'Invalid package checksum file'
expected=${BASH_REMATCH[1],,}
[[ ${BASH_REMATCH[2]} == "$archive" ]] || die 'Checksum names a different package'
actual=$(sha256sum "$stage/package.tar.gz")
[[ ${actual%% *} == "$expected" ]] || die 'Package checksum mismatch'
status busy 'Verifying update...'
# Validate member types, paths and expanded size before extracting anything.
(ulimit -f 4096; timeout 20 tar --list --verbose --gzip --numeric-owner --full-time --quoting-style=escape \
    --file "$stage/package.tar.gz" > "$stage/members")
awk '
    NF != 6 { exit 1 }
    {
        type = substr($1,1,1); path = $6
        if (type != "-" && type != "d") exit 1
        if ($3 !~ /^[0-9]+$/) exit 1
        if (path !~ /^frame-advanced-settings\/[A-Za-z0-9_.+\/-]*$/ || path ~ /(^|\/)\.\.?($|\/)/ || path ~ /\/\//) exit 1
        if (seen[path]++) exit 1
        total += $3; count++
        if (total > 134217728 || count > 2048) exit 1
    }
    END { if (count < 2) exit 1 }
' "$stage/members" || die 'Unsafe or oversized update archive'
timeout 30 tar --extract --gzip --file "$stage/package.tar.gz" --directory "$stage" \
    --no-same-owner --no-same-permissions --keep-old-files
package=$stage/$app
for file in install.sh "$app" "$app.sha256" build-id.txt "$app.vrmanifest" contrib/$app.service contrib/$app.desktop update.sh; do
    [[ -f $package/$file ]] || die "Incomplete update: $file"
done
identity=$(< "$package/build-id.txt")
[[ $identity =~ ^${version//./\.}\+[a-f0-9]{12}$ ]] || die 'Package version does not match release'
binary_checksum=$(< "$package/$app.sha256")
[[ $binary_checksum =~ ^([a-fA-F0-9]{64})[[:space:]][[:space:]*]frame-advanced-settings$ ]] || die 'Invalid executable checksum'
expected_binary=${BASH_REMATCH[1],,}
binary_hash=$(sha256sum "$package/$app")
[[ ${binary_hash%% *} == "$expected_binary" ]] || die 'Executable checksum mismatch'
for kind in service desktop; do
    if [[ $kind == service ]]; then original=$HOME/.config/systemd/user/$app.service
    else original=$HOME/.local/share/applications/$app.desktop; fi
    [[ ! -f $original ]] || cp -- "$original" "$stage/old.$kind"
done
status busy 'Installing update and restarting...'
installed=true
sh "$package/install.sh" --backup-record "$stage/previous"
touch "$destination/show-dashboard"
systemctl --user start "$app.service"
sleep 3
systemctl --user is-active --quiet "$app.service" || die 'Updated service did not stay running'
status done "Updated to version $version."
finished=true
