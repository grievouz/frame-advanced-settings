#!/usr/bin/env bash
# Run against fixture copies only; never change real dependencies or packages.
set -euo pipefail
project=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
package=${1:-$project/dist/frame-advanced-settings}
root=$(mktemp -d "${TMPDIR:-/tmp}/frame-build-script-tests.XXXXXXXX")
expect_failure() {
    local name=$1 expected=$2
    shift 2
    if "$@" > "$root/failure.log" 2>&1; then
        printf 'Expected failure: %s\n' "$name" >&2
        exit 1
    fi
    grep -qF -- "$expected" "$root/failure.log" || {
        cat "$root/failure.log" >&2
        printf 'Unexpected failure: %s\n' "$name" >&2
        exit 1
    }
    printf 'Passed: %s\n' "$name"
}

# JSON parsing and checksum verification must work from a path with spaces,
# reject changed/missing files, and reject paths escaping the vendor folder.
fixture=$root/dependency\ fixture
mkdir -p "$fixture/scripts" "$fixture/vendor/local"
cp "$project/scripts/"{build-common.sh,metadata.cmake,verify-dependencies.sh} "$fixture/scripts/"
printf 'dependency fixture\n' > "$fixture/vendor/local/header.h"
hash=$(sha256sum "$fixture/vendor/local/header.h")
hash=${hash%% *}
write_lock() {
    printf '{"files":[{"path":"%s","sha256":"%s"}]}\n' "$1" "$hash" > "$fixture/dependencies.lock.json"
}
write_lock vendor/local/header.h
bash "$fixture/scripts/verify-dependencies.sh"
printf 'changed\n' >> "$fixture/vendor/local/header.h"
expect_failure changed-dependency 'Dependency changed' bash "$fixture/scripts/verify-dependencies.sh"
write_lock vendor/local/missing.h
expect_failure missing-dependency 'Missing dependency' bash "$fixture/scripts/verify-dependencies.sh"
write_lock vendor/../outside.h
expect_failure escaped-dependency 'Invalid dependency path' bash "$fixture/scripts/verify-dependencies.sh"
printf 'not json\n' > "$fixture/dependencies.lock.json"
expect_failure malformed-lock 'JSON' bash "$fixture/scripts/verify-dependencies.sh"

fixture=$root/package\ fixture
mkdir -p "$fixture"
cp -R "$package/." "$fixture/"
verify=(bash "$project/scripts/verify-package.sh" "$fixture")
"${verify[@]}"
binary_hash=$(sha256sum "$fixture/frame-advanced-settings")
printf '%s *frame-advanced-settings\n' "${binary_hash%% *}" > "$fixture/frame-advanced-settings.sha256"
"${verify[@]}"
cp "$package/frame-advanced-settings.sha256" "$fixture/frame-advanced-settings.sha256"
mv "$fixture/assets/icons/frame-advanced-settings.png" "$root/icon.png"
expect_failure missing-package-file 'Package file missing' "${verify[@]}"
mv "$root/icon.png" "$fixture/assets/icons/frame-advanced-settings.png"
printf '\r\n' >> "$fixture/install.sh"
expect_failure script-line-endings 'CR line endings' "${verify[@]}"
cp "$package/install.sh" "$fixture/install.sh"
sed 's/local.frame-space-drag/unrelated.app/' "$package/frame-advanced-settings.vrmanifest" > "$fixture/frame-advanced-settings.vrmanifest"
expect_failure application-identity 'exactly one' "${verify[@]}"
cp "$package/frame-advanced-settings.vrmanifest" "$fixture/frame-advanced-settings.vrmanifest"
printf '0.1.0+000000000000\n' > "$fixture/build-id.txt"
expect_failure wrong-build 'build ID does not match' "${verify[@]}"
cp "$package/build-id.txt" "$fixture/build-id.txt"
printf 'invalid checksum\n' > "$fixture/frame-advanced-settings.sha256"
expect_failure wrong-checksum 'executable checksum mismatch' "${verify[@]}"
cp "$package/frame-advanced-settings.sha256" "$fixture/frame-advanced-settings.sha256"
printf '\076\000' | dd of="$fixture/frame-advanced-settings" bs=1 seek=18 conv=notrunc status=none
expect_failure wrong-architecture 'Expected a little-endian ARM64 ELF' "${verify[@]}"
cp "$package/frame-advanced-settings" "$fixture/frame-advanced-settings"
locations=$(grep -aobF 'libm.so.6' "$fixture/frame-advanced-settings")
library_offset=${locations%%:*}
[[ $library_offset =~ ^[0-9]+$ ]]
printf '/bad/x.so' | dd of="$fixture/frame-advanced-settings" bs=1 seek="$library_offset" conv=notrunc status=none
expect_failure nonportable-library 'Nonportable dependency' "${verify[@]}"
printf 'Build script verification fixtures passed: %s\n' "$root"
