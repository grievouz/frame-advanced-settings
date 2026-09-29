#!/bin/sh
# Read-only: compare the live service executable, even if its path was replaced.
set -eu
app_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd "$app_dir"
sha256sum -c frame-advanced-settings.sha256
printf 'Installed: '
./frame-advanced-settings --version
expected=$(cut -d ' ' -f 1 frame-advanced-settings.sha256)
pid=$(systemctl --user show frame-advanced-settings.service -p MainPID --value)
case "$pid" in ''|0|*[!0-9]*) echo 'No running service process to verify.' >&2; exit 21;; esac
if ! systemctl --user is-active --quiet frame-advanced-settings.service; then
    echo 'The service is not active.' >&2
    exit 21
fi
printf 'Running PID %s: ' "$pid"
readlink "/proc/$pid/exe"
actual=$(sha256sum "/proc/$pid/exe")
actual=${actual%% *}
printf 'Running SHA256: %s\n' "$actual"
if [ "$actual" != "$expected" ]; then
    echo 'MISMATCH: the running process is not the installed build.' >&2
    exit 21
fi
if [ "$(systemctl --user show frame-advanced-settings.service -p MainPID --value)" != "$pid" ]; then
    echo 'The service process changed during verification; check again.' >&2
    exit 21
fi
printf 'VERIFIED: the running process matches build %s\n' "$(cat build-id.txt)"
