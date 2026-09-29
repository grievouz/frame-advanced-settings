#!/usr/bin/env bash
# Run on the PC in Git Bash/MSYS2 or Linux Bash, not on the headset.
set -euo pipefail

probe_only=false
launch=false
if [[ ${1:-} == --probe-only ]]; then
    probe_only=true
    shift
elif [[ ${1:-} == --launch ]]; then
    launch=true
    shift
fi
if [[ ${1:-} == --help || ${1:-} == -h ]]; then
    printf 'Usage: bash install-frame.sh [--probe-only | --launch] [FRAME_IP_OR_HOSTNAME]\n'
    printf 'Default: frame (steamos@frame). Uploads, installs, and runs the read-only probe.\n'
    printf 'With --probe-only: reports service status/logs and runs a temporary diagnostic; no installation.\n'
    printf 'With --launch: installs, opens the dashboard with saved preferences, and reports startup status/logs; no autostart.\n'
    exit 0
fi
if (( $# > 1 )); then
    printf 'Expected at most one argument: the Frame IP or hostname.\n' >&2
    exit 1
fi
frame_address=${1:-frame}
if [[ ! $frame_address =~ ^[a-zA-Z0-9][a-zA-Z0-9.-]*$ ]]; then
    printf 'Expected an IP address or hostname, such as frame.\n' >&2
    exit 1
fi
required_tools=(ssh sha256sum)
for tool in "${required_tools[@]}"; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        printf 'Missing command: %s. Run this from Git Bash with OpenSSH available.\n' "$tool" >&2
        exit 1
    fi
done

script_dir=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
project_root=$(CDPATH= cd -- "$script_dir/.." && pwd)
build_id_file=$project_root/dist/frame-advanced-settings/build-id.txt
if [[ ! -f $build_id_file ]]; then
    printf 'Built package identity missing. Build the package first.\n' >&2
    exit 1
fi
build_id=$(< "$build_id_file")
version=${build_id%%+*}
if [[ ! $version =~ ^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(-beta\.[1-9][0-9]*)?$ ]]; then
    printf 'Invalid built package version.\n' >&2
    exit 1
fi
archive=frame-advanced-settings-$version-linux-aarch64.tar.gz
checksum=$archive.sha256
if [[ ! -f $project_root/dist/$archive || ! -f $project_root/dist/$checksum ]]; then
    printf 'Built package/checksum missing in %s/dist. Build the package first.\n' "$project_root" >&2
    exit 1
fi

# Stream the verified package over SSH; nothing is uploaded to the home folder.
cd -- "$project_root/dist"
if ! sha256sum --check "$checksum"; then
    printf 'Package verification failed; nothing was uploaded.\n' >&2
    exit 1
fi
remote=steamos@$frame_address
if [[ $probe_only == true ]]; then
    printf 'Running the diagnostic on %s. Enter the Frame password at the SSH prompt.\n' "$remote"
    remote_command=$(cat <<'FRAME_PROBE'
set -eu
probe_root=$(mktemp -d /tmp/frame-advanced-settings-probe.XXXXXX)
cleanup() {
    rm -rf -- "$probe_root"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
tar -xzf - -C "$probe_root" frame-advanced-settings/frame-advanced-settings frame-advanced-settings/lib/libopenvr_api.so
chmod u+x "$probe_root/frame-advanced-settings/frame-advanced-settings"
printf '\n--- Installed dashboard service ---\n'
dashboard_service=frame-advanced-settings.service
systemctl --user show "$dashboard_service" --no-pager \
    --property=LoadState --property=ActiveState --property=SubState \
    --property=MainPID --property=ExecMainStatus --property=Result || :
printf '\n--- Recent dashboard startup logs ---\n'
journalctl --user -u "$dashboard_service" -n 60 --no-pager || :
printf '\n--- Last saved dashboard status (may be stale if stopped) ---\n'
state_home=${XDG_STATE_HOME:-}
case "$state_home" in /*) ;; *) state_home=$HOME/.local/state ;; esac
status_path="$state_home/frame-advanced-settings/status.json"
if [ -f "$status_path" ]; then cat "$status_path"; else printf 'No status file yet.\n'; fi
printf '\n--- Current runtime probe ---\n'
"$probe_root/frame-advanced-settings/frame-advanced-settings" --probe
FRAME_PROBE
)
    probe_result=0
    ssh "$remote" "$remote_command" < "$archive" || probe_result=$?
    if (( probe_result == 10 )); then
        printf '\nDiagnostic completed. Movement readiness is blocked; share the output above.\n'
    elif (( probe_result != 0 )); then
        printf '\nDiagnostic could not complete (exit %s); check the output above.\n' "$probe_result" >&2
    fi
    exit "$probe_result"
fi
printf 'Installing on %s. Enter the Frame password at the SSH prompt.\n' "$remote"
package_hash=$(sha256sum "$archive")
package_hash=${package_hash%% *}
[[ $package_hash =~ ^[a-fA-F0-9]{64}$ ]] || { printf 'Invalid package digest.\n' >&2; exit 1; }
remote_command=$(cat <<'FRAME_INSTALL'
set -eu
umask 077
stage=$(mktemp -d /tmp/frame-advanced-settings-install.XXXXXX)
cleanup() { rm -rf -- "$stage"; }
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
cat > "$stage/package.tar.gz"
printf '%s  %s\n' "$2" "$stage/package.tar.gz" | sha256sum -c -
tar -xzf "$stage/package.tar.gz" -C "$stage"
sh "$stage/frame-advanced-settings/install.sh"
probe_result=0
"$HOME/.local/share/frame-advanced-settings/frame-advanced-settings" --probe || probe_result=$?
if [ "$probe_result" -ne 0 ] && [ "$probe_result" -ne 10 ]; then exit "$probe_result"; fi
if [ "$1" = true ]; then
    printf '\n--- Starting the dashboard with saved preferences ---\n'
    started_at=$(date +%s)
    touch "$HOME/.local/share/frame-advanced-settings/show-dashboard"
    start_result=0
    systemctl --user reload-or-restart frame-advanced-settings.service || start_result=$?
    sleep 3
    systemctl --user status frame-advanced-settings.service --no-pager -l || :
    journalctl --user -u frame-advanced-settings.service --since "@$started_at" -n 50 --no-pager || :
    if [ "$start_result" -ne 0 ] || ! systemctl --user is-active --quiet frame-advanced-settings.service; then
        exit 20
    fi
    sh "$HOME/.local/share/frame-advanced-settings/verify-running.sh" || exit 21
fi
exit "$probe_result"
FRAME_INSTALL
)
# Only a boolean and a validated hex digest enter the remote shell arguments.
# The archive itself travels unchanged on stdin, including with Windows ssh.exe.
remote_command="set -- $launch $package_hash"$'\n'"$remote_command"
if ssh "$remote" "$remote_command" < "$archive"
then
    if [[ $launch == true ]]; then
        printf '\nInstalled and service is running. Check the headset for the Frame Advanced Settings panel.\n'
    else
        printf '\nInstalled. Open Frame Advanced Settings from Launch program (+) on the headset.\n'
    fi
    printf 'Your saved movement and startup choices are preserved.\n'
else
    result=$?
    if (( result == 10 )); then
        printf '\nInstallation succeeded, but the movement readiness check is blocked. Share the probe output above.\n' >&2
    elif (( result == 20 )); then
        printf '\nInstallation succeeded, but the dashboard service failed to start or exited. Share its startup logs above.\n' >&2
    elif (( result == 21 )); then
        printf '\nInstalled, but the running build could not be verified. Share the verification output above.\n' >&2
    else
        printf '\nInstallation or probe startup failed; check the output above.\n' >&2
    fi
    exit "$result"
fi
