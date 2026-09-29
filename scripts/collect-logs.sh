#!/usr/bin/env bash
# Run on the PC in Git Bash, MSYS2, or Linux Bash.
set -euo pipefail
include_steam=false
output=
frame_address=frame
have_host=false
while (( $# )); do
    case $1 in
        --help|-h)
            printf 'Usage: bash collect-logs.sh [--include-steam-logs] [--output DIRECTORY] [FRAME_HOST]\n'
            printf 'Default host: frame (steamos@frame); output: project diagnostics directory.\n'
            printf 'Creates and downloads a bounded report. Enter passwords at SSH/SCP prompts.\n'
            printf 'Nothing is uploaded or posted. Review the archive before sharing it.\n'
            exit 0 ;;
        --include-steam-logs) include_steam=true ;;
        --output)
            (( $# >= 2 )) || { printf '%s\n' '--output needs a directory.' >&2; exit 2; }
            output=$2; shift ;;
        --*) printf 'Unknown option: %s\n' "$1" >&2; exit 2 ;;
        *)
            [[ $have_host == false ]] || { printf 'Expected one Frame hostname.\n' >&2; exit 2; }
            frame_address=$1; have_host=true ;;
    esac
    shift
done
[[ $frame_address =~ ^[a-zA-Z0-9][a-zA-Z0-9.-]*$ ]] || { printf 'Invalid Frame hostname or IPv4 address.\n' >&2; exit 2; }
for tool in ssh scp mktemp tar stat; do
    command -v "$tool" >/dev/null || { printf 'Missing command: %s\n' "$tool" >&2; exit 1; }
done
script_dir=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
[[ -n $output ]] || output=$script_dir/../diagnostics
mkdir -p -- "$output"
cd -- "$output"
output=$(pwd -P)
remote=steamos@$frame_address
printf 'Creating a report on %s. Enter the Frame password at each SSH/SCP prompt.\n' "$remote"

# Fixed shell text on stdin prevents Windows/MSYS remote-path conversion and
# prevents values returned by SSH from becoming shell commands.
if ! archive=$(ssh "$remote" sh -s -- "$include_steam" <<'FRAME_EXPORT'
set -eu
script="$HOME/.local/share/frame-advanced-settings/export-logs.sh"
if [ ! -f "$script" ] || [ -L "$script" ]; then
    printf 'Installed exporter missing. Install the current Frame Advanced Settings package first.\n' >&2
    exit 1
fi
if [ "$1" = true ]; then
    exec bash "$script" --include-steam-logs
else
    exec bash "$script"
fi
FRAME_EXPORT
); then
    printf 'Remote export failed; nothing was downloaded.\n' >&2
    exit 1
fi
archive=${archive%$'\r'}
# Remote output is data. Only the exact generated path shape reaches scp.
if [[ ! $archive =~ ^/[a-zA-Z0-9_./-]+/reports/frame-advanced-settings-report-[0-9]{8}T[0-9]{6}Z-[0-9]+\.tar\.gz$ || $archive == */../* ]]; then
    printf 'Exporter returned an unexpected path; download cancelled.\n' >&2
    exit 1
fi
name=${archive##*/}
[[ ! -e $name && ! -L $name ]] || { printf 'Already exists: %s/%s\n' "$output" "$name" >&2; exit 1; }
partial=$(mktemp './.frame-report-XXXXXXXX')
trap 'rm -f -- "$partial"' EXIT
if ! scp "$remote:$archive" "$partial"; then printf 'Report download failed.\n' >&2; exit 1; fi
size=$(stat -c %s -- "$partial")
(( size <= 32 * 1024 * 1024 )) || { printf 'Downloaded report exceeds 32 MiB.\n' >&2; exit 1; }
tar -tzf "$partial" >/dev/null
mv -- "$partial" "./$name"
printf 'Saved: %s/%s\nReview the archive before attaching it to a bug report.\n' "$output" "$name"
