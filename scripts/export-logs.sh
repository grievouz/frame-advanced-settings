#!/usr/bin/env bash
# Run on the Frame. No runtime calls, service changes, or automatic uploads.
set -euo pipefail
umask 077

include_steam=false
case ${1:-} in
    --include-steam-logs) include_steam=true; shift ;;
    --help|-h)
        printf 'Usage: bash export-logs.sh [--include-steam-logs]\n'
        printf 'Exports bounded app diagnostics; prints the archive path on success.\n'
        printf 'Keeps the latest 2 reports (32 MiB each maximum). Nothing is uploaded.\n'
        printf 'Optional Steam logs may contain device, application and network details.\n'
        exit 0 ;;
esac
if (( $# )); then printf 'Unknown argument. See --help.\n' >&2; exit 2; fi
for tool in tar gzip stat dd tail head date mktemp find sort flock; do
    command -v "$tool" >/dev/null || { printf 'Missing command: %s\n' "$tool" >&2; exit 1; }
done

# Only walk real directories. Never traverse a symlink into unrelated data.
real_tree() {
    local path=$1 walk= part
    [[ $path == /* && $path != *$'\n'* && $path != *$'\r'* ]] || return 1
    local -a parts
    IFS=/ read -r -a parts <<< "$path"
    for part in "${parts[@]}"; do
        [[ -z $part ]] && continue
        [[ $part != . && $part != .. ]] || return 1
        walk+=/$part
        [[ ! -L $walk && -d $walk ]] || return 1
    done
}
make_real_tree() {
    local path=$1 parent
    [[ $path == /* && $path != *$'\n'* && $path != *$'\r'* ]] || return 1
    if [[ -e $path || -L $path ]]; then real_tree "$path"; return; fi
    parent=${path%/*}
    [[ -n $parent ]] || parent=/
    make_real_tree "$parent" || return 1
    mkdir -- "$path"
    real_tree "$path"
}

state_home=${XDG_STATE_HOME:-}
config_home=${XDG_CONFIG_HOME:-}
[[ $state_home == /* ]] || state_home=$HOME/.local/state
[[ $config_home == /* ]] || config_home=$HOME/.config
state_root=$state_home/frame-advanced-settings
config_root=$config_home/frame-advanced-settings
script_dir=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
report_root=$state_root/reports
if ! make_real_tree "$report_root"; then
    printf 'Report directory must have no symlink components: %s\n' "$report_root" >&2
    exit 1
fi
[[ ! -L $report_root/.export.lock && ( ! -e $report_root/.export.lock || -f $report_root/.export.lock ) ]] || {
    printf 'Refusing linked or non-regular export lock.\n' >&2; exit 1;
}
exec 9>>"$report_root/.export.lock"
flock -n 9 || { printf 'Another diagnostic export is still running.\n' >&2; exit 1; }

stage_marker='Frame Advanced Settings diagnostic export v1'
stage_marker_size=$((${#stage_marker} + 1))
shopt -s nullglob
# The exclusive lock means no exporter still owns a previous stage. Recover
# after power loss/SIGKILL without accumulating partial reports indefinitely.
for abandoned in "$report_root"/.export-*; do
    [[ ${abandoned##*/} =~ ^\.export-[a-zA-Z0-9]{8}$ ]] || continue
    real_tree "$abandoned" || continue
    owner=$abandoned/.frame-export
    [[ -f $owner && ! -L $owner ]] || continue
    [[ $(stat -c %s -- "$owner") == "$stage_marker_size" ]] || continue
    [[ $(head -c "$stage_marker_size" -- "$owner") == "$stage_marker" ]] || continue
    find "$abandoned" -depth -delete
done

stage=$(mktemp -d "$report_root/.export-XXXXXXXX")
cleanup() {
    # mktemp created this private directory; find does not follow links.
    if [[ $stage == "$report_root"/.export-* && -d $stage && ! -L $stage ]]; then
        find "$stage" -depth -delete
    fi
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
printf '%s\n' "$stage_marker" > "$stage/.frame-export"
payload=$stage/frame-advanced-settings-report
mkdir -- "$payload"
notes=$payload/collection.txt
printf 'Frame Advanced Settings diagnostic report\nUTC: %s\n' "$(date -u +%FT%TZ)" > "$notes"
printf 'Steam logs explicitly requested: %s\nSource files may be changing while the app runs.\n' "$include_steam" >> "$notes"
printf 'No headset runtime probe was run. Review files before sharing.\n\n' >> "$notes"
remaining=$((30 * 1024 * 1024))

# Copy only an explicitly named regular file, bounded even if it is growing.
# GNU dd O_NOFOLLOW refuses a final symlink substituted after the initial check.
copy_bounded() {
    local source=$1 name=$2 limit=$3 retry=${4:-false} size take skip=0 destination
    if ! real_tree "${source%/*}" || [[ -L $source || ! -f $source ]]; then
        printf 'Missing or skipped linked/non-regular file: %s\n' "$name" >> "$notes"
        return
    fi
    if ! size=$(stat -c %s -- "$source" 2>"$stage/copy-error.txt"); then
        if [[ $retry == true ]]; then copy_bounded "$source" "$name" "$limit" false; return; fi
        if [[ ! -e $source && ! -L $source ]]; then
            printf 'File rotated or disappeared during collection: %s\n' "$name" >> "$notes"
            return
        fi
        cat "$stage/copy-error.txt" >&2
        return 1
    fi
    take=$size
    (( take <= limit )) || take=$limit
    (( take <= remaining )) || take=$remaining
    if (( take == 0 && size != 0 )); then
        printf 'Skipped at report content limit: %s\n' "$name" >> "$notes"
        return
    fi
    if (( size > take )); then
        skip=$((size - take))
        printf 'Last %s of %s bytes only: %s\n' "$take" "$size" "$name" >> "$notes"
        # Truncated JSON is clearly marked instead of looking like valid JSON.
        [[ $name != *.json ]] || name+=.tail.txt
    fi
    destination=$payload/$name
    mkdir -p -- "${destination%/*}"
    if ! dd if="$source" of="$destination" iflag=nofollow,skip_bytes,count_bytes skip="$skip" count="$take" status=none 2>"$stage/copy-error.txt"; then
        rm -f -- "$destination"
        if [[ $retry == true ]]; then copy_bounded "$source" "$2" "$limit" false; return; fi
        if [[ ! -e $source && ! -L $source ]]; then
            printf 'File rotated or disappeared during collection: %s\n' "$name" >> "$notes"
            return
        fi
        printf 'Failed to read app diagnostic: %s\n' "$name" >&2
        cat "$stage/copy-error.txt" >&2
        return 1
    fi
    remaining=$((remaining - take))
}

copy_bounded "$state_root/status.json" status.json 131072
copy_bounded "$state_root/update-status.txt" update-status.txt 4096
copy_bounded "$state_root/update.log" update.log 262144
copy_bounded "$state_root/baseline.json" baseline.json 131072
copy_bounded "$config_root/settings.ini" settings.ini 65536
copy_bounded "$script_dir/build-id.txt" build-id.txt 4096
copy_bounded "$script_dir/frame-advanced-settings.sha256" frame-advanced-settings.sha256 4096
for snapshot in first-space-mismatch.json last-space-mismatch.json last-drag.json; do
    copy_bounded "$state_root/$snapshot" "$snapshot" 2097152
done

# Reserve report space for summaries and explicitly requested runtime logs
# before filling any remaining space with older rotating app log parts.
if command -v journalctl >/dev/null && command -v timeout >/dev/null; then
    set +e
    timeout 8s journalctl --user -u frame-advanced-settings.service -n 1500 --no-pager -o short-iso --quiet 2>/dev/null |
        tail -c 1048576 > "$stage/journal.txt"
    journal_result=${PIPESTATUS[0]}
    set -e
    printf 'App journal command exit code: %s\n' "$journal_result" >> "$notes"
    copy_bounded "$stage/journal.txt" app-journal.txt 1048576
else
    printf 'App journal unavailable (journalctl or timeout missing).\n' >> "$notes"
fi

if [[ $include_steam == true ]]; then
    # Steam uses these standard paths; symlink aliases are deliberately skipped.
    for steam_root in "$HOME/.local/share/Steam/logs" "$HOME/.steam/steam/logs"; do
        real_tree "$steam_root" || continue
        for steam_log in vrserver.txt vrcompositor.txt; do
            copy_bounded "$steam_root/$steam_log" "steam/$steam_log" 2097152
        done
        break
    done
fi

# Logger-owned sessions only; there is no recursive copy of arbitrary state.
log_root=$state_root/logs
if real_tree "$log_root"; then
    shopt -s nullglob
    sessions=("$log_root"/session-*)
    if (( ${#sessions[@]} )); then
        mapfile -t sessions < <(printf '%s\n' "${sessions[@]}" | sort -r)
    fi
    session_count=0
    marker_text='Frame Advanced Settings session logs v1'
    marker_size=$((${#marker_text} + 1))
    for session in "${sessions[@]}"; do
        name=${session##*/}
        [[ $name =~ ^session-[0-9]{8}T[0-9]{12}Z-[0-9a-f]{8}$ ]] || continue
        real_tree "$session" || continue
        marker=$session/.frame-session
        [[ -f $marker && ! -L $marker && $(stat -c %s -- "$marker") == "$marker_size" ]] || continue
        [[ $(head -c "$marker_size" -- "$marker") == "$marker_text" ]] || continue
        (( session_count < 3 )) || break
        session_count=$((session_count + 1))
        for part in events.jsonl events.1.jsonl events.2.jsonl events.3.jsonl; do
            copy_bounded "$session/$part" "logs/$name/$part" 2097152 true
        done
    done
fi

archive_name=frame-advanced-settings-report-$(date -u +%Y%m%dT%H%M%SZ)-$$.tar.gz
archive=$report_root/$archive_name
[[ ! -e $archive && ! -L $archive ]] || { printf 'Report filename collision.\n' >&2; exit 1; }
tar -czf "$stage/report.tar.gz" -C "$stage" frame-advanced-settings-report
archive_size=$(stat -c %s -- "$stage/report.tar.gz")
(( archive_size <= 32 * 1024 * 1024 )) || { printf 'Report exceeds 32 MiB; export cancelled.\n' >&2; exit 1; }
mv -- "$stage/report.tar.gz" "$archive"

# Remove only our exact archive pattern, after a complete new archive exists.
shopt -s nullglob
archives=("$report_root"/frame-advanced-settings-report-*.tar.gz)
mapfile -t archives < <(printf '%s\n' "${archives[@]}" | sort -r)
kept=1
for previous in "${archives[@]}"; do
    name=${previous##*/}
    [[ $name =~ ^frame-advanced-settings-report-[0-9]{8}T[0-9]{6}Z-[0-9]+\.tar\.gz$ ]] || continue
    [[ -f $previous && ! -L $previous ]] || continue
    [[ $previous != "$archive" ]] || continue
    if (( kept >= 2 )); then rm -f -- "$previous"; else kept=$((kept + 1)); fi
done
printf '%s\n' "$archive"
