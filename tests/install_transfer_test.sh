#!/usr/bin/env bash
# Exercise the real PC wrapper with local SSH/systemd fixtures; no headset access.
set -euo pipefail
project=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_root=$(mktemp -d "${TMPDIR:-/tmp}/frame-transfer-tests.XXXXXXXX")
client=$test_root/client
stubs=$test_root/stubs
payload=$test_root/payload/frame-advanced-settings
mkdir -p "$client/scripts" "$client/dist" "$stubs" "$payload/lib"
cp "$project/scripts/install-frame.sh" "$client/scripts/"
cat > "$payload/frame-advanced-settings" <<'EOF'
#!/bin/sh
exit "${TEST_PROBE_RESULT:-0}"
EOF
cat > "$payload/install.sh" <<'EOF'
#!/bin/sh
set -eu
[ "${TEST_INSTALL_FAIL:-0}" = 0 ] || exit 32
source_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
destination=$HOME/.local/share/frame-advanced-settings
mkdir -p "$destination"
cp "$source_dir/frame-advanced-settings" "$source_dir/verify-running.sh" "$destination/"
chmod u+x "$destination/frame-advanced-settings"
EOF
cat > "$payload/verify-running.sh" <<'EOF'
#!/bin/sh
exit "${TEST_VERIFY_FAIL:-0}"
EOF
printf 'fixture library\n' > "$payload/lib/libopenvr_api.so"
archive=frame-advanced-settings-0.1.0-aarch64.tar.gz
tar -czf "$client/dist/$archive" -C "$test_root/payload" frame-advanced-settings
(cd "$client/dist" && sha256sum "$archive" > "$archive.sha256")
real_mktemp=$(command -v mktemp)
cat > "$stubs/mktemp" <<'EOF'
#!/usr/bin/env bash
set -eu
[[ $# == 2 && $1 == -d && $2 == /tmp/frame-advanced-settings-*.XXXXXX ]] || exit 92
exec "$REAL_MKTEMP" -d "$TEST_RUNTIME/${2##*/}"
EOF
cat > "$stubs/ssh" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
[[ $# == 2 && $1 == steamos@frame ]] || exit 93
printf 'ssh\n' >> "$TEST_CALLS"
if [[ ${TEST_CORRUPT:-0} == 1 ]]; then
    cat > /dev/null
    printf 'broken transfer\n' | HOME="$TEST_HOME" bash -c "$2"
else
    HOME="$TEST_HOME" bash -c "$2"
fi
EOF
cat > "$stubs/systemctl" <<'EOF'
#!/bin/sh
case "$*" in
    *reload-or-restart*|*is-active*) exit "${TEST_START_FAIL:-0}" ;;
esac
exit 0
EOF
printf '#!/bin/sh\nexit 0\n' > "$stubs/journalctl"
printf '#!/bin/sh\nexit 0\n' > "$stubs/sleep"
printf '#!/bin/sh\necho "Unexpected SCP upload" >&2\nexit 91\n' > "$stubs/scp"
chmod +x "$stubs/"*
run_case() {
    local name=$1 expected=$2 option=$3
    shift 3
    local case_root=$test_root/$name
    mkdir -p "$case_root/home/Documents" "$case_root/runtime"
    printf 'keep\n' > "$case_root/home/Documents/keep.txt"
    local actual=0
    env PATH="$stubs:$PATH" REAL_MKTEMP="$real_mktemp" TEST_HOME="$case_root/home" \
        TEST_RUNTIME="$case_root/runtime" TEST_CALLS="$case_root/calls" "$@" \
        bash "$client/scripts/install-frame.sh" ${option:+"$option"} > "$case_root/output" 2>&1 || actual=$?
    if [[ $actual != "$expected" ]]; then
        cat "$case_root/output"
        printf '%s: expected %s, got %s\n' "$name" "$expected" "$actual" >&2
        exit 1
    fi
    [[ $(wc -l < "$case_root/calls") == 1 ]] || { printf 'Expected one SSH call\n' >&2; exit 1; }
    shopt -s nullglob dotglob
    local leftovers=("$case_root/runtime"/* "$case_root/home"/frame-*)
    [[ ${#leftovers[@]} == 0 ]] || { printf 'Leftover: %s\n' "${leftovers[@]}" >&2; exit 1; }
    [[ $(cat "$case_root/home/Documents/keep.txt") == keep ]]
    printf 'Passed: %s\n' "$name"
}
run_case success 0 ''
run_case launch 0 --launch
run_case blocked 10 '' TEST_PROBE_RESULT=10
run_case probe-failed 2 '' TEST_PROBE_RESULT=2
run_case install-failed 32 '' TEST_INSTALL_FAIL=1
run_case corrupt-transfer 1 '' TEST_CORRUPT=1
run_case launch-failed 20 --launch TEST_START_FAIL=1
run_case verification-failed 21 --launch TEST_VERIFY_FAIL=1
run_case probe-only 0 --probe-only
run_case probe-only-failed 2 --probe-only TEST_PROBE_RESULT=2
printf 'Transfer/cleanup fixtures passed: %s\n' "$test_root"
