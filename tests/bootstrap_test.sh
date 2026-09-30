#!/usr/bin/env bash
# Run the streamed bootstrap and real package installer against isolated homes.
# Network, architecture and systemd are fixtures; tar and checksums are real.
set -euo pipefail
project=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_shell=sh
if [[ $OSTYPE == msys* ]]; then
    # MSYS lacks RLIMIT_FSIZE. Linux CI exercises /bin/sh and the actual limit.
    ulimit() { :; }; export -f ulimit
    test_shell=bash
fi
test_root=$(mktemp -d "${TMPDIR:-/tmp}/frame-bootstrap-tests.XXXXXXXX")
app=frame-advanced-settings
package=$test_root/package/$app
stubs=$test_root/stubs
mkdir -p "$package"/{lib,input,assets/fonts,assets/icons,licenses,contrib} "$stubs"
cp "$project/install.sh" "$package/"
cp -R "$project/contrib/"* "$package/contrib/"
cp "$project/scripts/"{update,microphone-setup}.sh "$package/"
for file in "$app" "$app.vrmanifest" README.md verify-running.sh export-logs.sh collect-logs.sh LICENSE \
    lib/libopenvr_api.so input/actions.json assets/fonts/font.ttf licenses/license.txt \
    assets/icons/$app.png assets/icons/$app.svg; do printf 'new binary %s\n' "$file" > "$package/$file"; done
printf '0.2.0+0123456789ab\n' > "$package/build-id.txt"
(cd "$package" && sha256sum "$app" > "$app.sha256")
tar -czf "$test_root/good.tar.gz" -C "$test_root/package" "$app"
real_mktemp=$(command -v mktemp)
cat > "$stubs/mktemp" <<'EOF'
#!/bin/sh
set -eu
[ "$#" = 2 ] && [ "$1" = -d ] && [ "$2" = /tmp/frame-advanced-settings-download.XXXXXX ] || exit 90
stage=$("$REAL_MKTEMP" "$@")
printf '%s\n' "$stage" >> "$TEST_STAGES"
printf '%s\n' "$stage"
EOF
cat > "$stubs/curl" <<'EOF'
#!/usr/bin/env bash
set -eu
out='' effective=false
[[ $1 == --disable ]] || exit 91
while (( $# )); do
    case $1 in
        --output) out=$2; shift 2;;
        --write-out) [[ $2 == '%{url_effective}' ]] || exit 92; effective=true; shift 2;;
        --proto|--proto-redir) [[ $2 == =https ]] || exit 93; shift 2;;
        --connect-timeout|--max-time|--retry|--retry-max-time|--max-filesize) shift 2;;
        --disable|--fail|--silent|--show-error|--location|--head) shift;;
        https://*) url=$1; shift;;
        *) exit 94;;
    esac
done
printf '%s\n' "$url" >> "$TEST_REQUESTS"
[[ ${FAIL_DOWNLOAD:-0} == 0 || $url != *tar.gz ]] || exit 22
if [[ $effective == true ]]; then
    [[ $url == https://github.com/grievouz/frame-advanced-settings/releases/latest ]] || exit 95
    printf '%s' "${LATEST_URL:-https://github.com/grievouz/frame-advanced-settings/releases/tag/v0.2.0}"
else
    base=https://github.com/grievouz/frame-advanced-settings/releases/download/v0.2.0/frame-advanced-settings-0.2.0-linux-aarch64.tar.gz
    case $url in
        "$base.sha256") cp "$PAYLOAD.sha256" "$out";;
        "$base") cp "$PAYLOAD" "$out";;
        *) exit 96;;
    esac
fi
EOF
cat > "$stubs/systemctl" <<'EOF'
#!/bin/sh
set -eu
printf '%s\n' "$*" >> "$TEST_COMMANDS"
shift
case $1 in
    list-units) printf 'frame-advanced-settings.service loaded\n';;
    show) printf '0\n';;
    stop|daemon-reload) :;;
    *) exit 97;;
esac
EOF
cat > "$stubs/uname" <<'EOF'
#!/bin/sh
case $1 in -s) printf '%s\n' "${TEST_OS:-Linux}";; -m) printf '%s\n' "${TEST_ARCH:-aarch64}";; *) exit 98;; esac
EOF
printf '#!/bin/sh\nprintf "%%s\\n" "${TEST_UID:-1000}"\n' > "$stubs/id"
printf '#!/bin/sh\nexit 0\n' > "$stubs/flock"
chmod +x "$stubs/"*
fixture() {
    fixture_home=$test_root/$1
    data=$fixture_home/.local/share/$app
    mkdir -p "$fixture_home/Documents" "$fixture_home/.config/$app"
    printf 'keep\n' > "$fixture_home/Documents/keep.txt"
    printf 'movement_enabled=false\ndrag_gain=4\n' > "$fixture_home/.config/$app/settings.ini"
    payload=$fixture_home/package.tar.gz
    cp "$test_root/good.tar.gz" "$payload"
    : > "$fixture_home/commands"
    : > "$fixture_home/stages"
    : > "$fixture_home/requests"
    checksum
}
checksum() {
    digest=$(sha256sum "$payload")
    printf '%s  frame-advanced-settings-0.2.0-linux-aarch64.tar.gz\n' "${digest%% *}" > "$payload.sha256"
}
run_bootstrap() {
    local expected=$1 actual=0
    shift
    cat "$project/get.sh" | env HOME="$fixture_home" XDG_CONFIG_HOME="$fixture_home/.config" \
        XDG_STATE_HOME="$fixture_home/.local/state" PATH="$stubs:$PATH" PAYLOAD="$payload" \
        REAL_MKTEMP="$real_mktemp" TEST_COMMANDS="$fixture_home/commands" TEST_STAGES="$fixture_home/stages" \
        TEST_REQUESTS="$fixture_home/requests" "$@" "$test_shell" > "$fixture_home/output" 2>&1 || actual=$?
    if [[ $actual != "$expected" ]]; then cat "$fixture_home/output"; echo "Expected $expected, got $actual" >&2; exit 1; fi
    while IFS= read -r stage; do [[ ! -e $stage ]] || { echo "Download directory not cleaned: $stage" >&2; exit 1; }; done < "$fixture_home/stages"
    grep -qx 'keep' "$fixture_home/Documents/keep.txt"
    grep -qx 'drag_gain=4' "$fixture_home/.config/$app/settings.ini"
    if [[ $expected != 0 ]]; then [[ ! -s $fixture_home/commands ]]; fi
    printf 'Passed: %s\n' "${fixture_home##*/}"
}
fixture success
run_bootstrap 0
grep -q 'new binary' "$data/$app"
[[ $(wc -l < "$fixture_home/requests") == 3 ]]
# Installing again uses the existing backup mechanism and keeps startup choices.
mkdir -p "$fixture_home/.config/systemd/user/steamvr.service.wants"
printf 'startup stays enabled\n' > "$fixture_home/.config/systemd/user/steamvr.service.wants/$app.service"
printf 'old binary\n' > "$data/$app"
run_bootstrap 0
grep -q 'old binary' "$data".backup.*/"$app"
grep -q 'startup stays enabled' "$fixture_home/.config/systemd/user/steamvr.service.wants/$app.service"
fixture wrong-arch
run_bootstrap 1 TEST_ARCH=x86_64
[[ ! -s $fixture_home/requests ]]
fixture wrong-os
run_bootstrap 1 TEST_OS=Darwin
fixture root
run_bootstrap 1 TEST_UID=0
fixture unexpected-url
run_bootstrap 1 LATEST_URL=https://example.com/v0.2.0
fixture prerelease
run_bootstrap 1 LATEST_URL=https://github.com/grievouz/frame-advanced-settings/releases/tag/v0.2.0-beta.1
fixture download-failure
run_bootstrap 22 FAIL_DOWNLOAD=1
fixture corrupt
printf 'broken transfer' >> "$payload"
run_bootstrap 1
fixture checksum-name
sed -i 's/linux-aarch64/linux-x86_64/' "$payload.sha256"
run_bootstrap 1
fixture traversal
tar -czf "$payload" --transform='s@frame-advanced-settings/build-id.txt@frame-advanced-settings/../escape@' -C "$test_root/package" "$app"
checksum
run_bootstrap 1
fixture wrong-version
printf '0.3.0+0123456789ab\n' > "$package/build-id.txt"
tar -czf "$payload" -C "$test_root/package" "$app"
checksum
run_bootstrap 1
printf 'Bootstrap install, reinstall, download validation and cleanup checks passed. Fixtures: %s\n' "$test_root"
