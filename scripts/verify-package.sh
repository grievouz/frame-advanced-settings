#!/usr/bin/env bash
set -euo pipefail
export LC_ALL=C
source "$(dirname -- "${BASH_SOURCE[0]}")/build-common.sh"
case "${1:-}" in
    --help|-h)
        printf 'Usage: bash verify-package.sh [PACKAGE_DIRECTORY]\nRequires CMake, GNU readelf, grep, sed, and sha256sum.\n'
        exit 0 ;;
esac
(( $# <= 1 )) || die 'Expected at most one package directory'
require_tools cmake readelf grep sed sha256sum
package_root=${1:-$project_root/dist/frame-advanced-settings}
[[ -d $package_root ]] || die "Package directory missing: $package_root"
package_root=$(CDPATH= cd -- "$package_root" && pwd)

required=(
    frame-advanced-settings lib/libopenvr_api.so frame-advanced-settings.vrmanifest
    input/actions.json input/bindings_frame.json install.sh README.md LICENSE
    contrib/frame-advanced-settings.service contrib/frame-advanced-settings.desktop
    assets/fonts/Inter-Regular.ttf assets/fonts/Inter-SemiBold.ttf licenses/inter.txt
    assets/icons/frame-advanced-settings.png assets/icons/frame-advanced-settings.svg
    build-id.txt frame-advanced-settings.sha256 verify-running.sh export-logs.sh collect-logs.sh
    microphone-setup.sh contrib/wireplumber/frame-advanced-settings-mic.lua
    contrib/wireplumber/90-frame-advanced-settings-mic.conf
    licenses/lucide.txt licenses/nanosvg.txt licenses/spdlog.txt licenses/spdlog-fmt.txt
)
for relative in "${required[@]}"; do
    [[ -f $package_root/$relative ]] || die "Package file missing: $relative"
done

verify_elf() {
    local path=$1 expected_interpreter=$2 report interpreter library
    report=$(readelf -W -h -l -d -- "$path" 2>&1) || die "Cannot read ELF: $path: $report"
    report=${report//$'\r'/}
    ! grep -qE '(^|[[:space:]])(Error|Warning):' <<< "$report" || die "Invalid ELF: $path: $report"
    grep -qE 'Class:[[:space:]]+ELF64$' <<< "$report" &&
        grep -qE 'Data:.*little endian$' <<< "$report" &&
        grep -qE 'Machine:[[:space:]]+AArch64$' <<< "$report" ||
        die "Expected a little-endian ARM64 ELF: $path"
    grep -qE '^[[:space:]]+DYNAMIC[[:space:]]' <<< "$report" || die "Missing dynamic section: $path"
    interpreter=$(sed -n 's/.*\[Requesting program interpreter: \(.*\)\]/\1/p' <<< "$report")
    [[ $interpreter == "$expected_interpreter" ]] || die "Unexpected Linux interpreter in $path: $interpreter"
    while IFS= read -r library; do
        [[ $library =~ ^[a-zA-Z0-9_.+-]+\.so(\.[0-9]+)*$ ]] ||
            die "Nonportable dependency '$library' embedded in $path"
    done < <(sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p' <<< "$report")
}
verify_elf "$package_root/frame-advanced-settings" /lib/ld-linux-aarch64.so.1
verify_elf "$package_root/lib/libopenvr_api.so" ''
metadata manifest "-DPACKAGE_ROOT=$(native_path "$package_root")"
for relative in install.sh verify-running.sh export-logs.sh collect-logs.sh microphone-setup.sh \
    contrib/wireplumber/frame-advanced-settings-mic.lua \
    contrib/wireplumber/90-frame-advanced-settings-mic.conf; do
    if grep -Uq $'\r' "$package_root/$relative"; then
        die "$relative contains CR line endings"
    fi
done
build_id=$(< "$package_root/build-id.txt")
build_id=${build_id%$'\r'}
[[ $build_id =~ ^${release_version_pattern}\+[a-f0-9]{12}$ ]] || die 'Invalid build identity'
grep -UaFq -- "$build_id" "$package_root/frame-advanced-settings" || die 'Packaged build ID does not match the executable'
binary_hash=$(sha256sum "$package_root/frame-advanced-settings")
binary_hash=${binary_hash%% *}
checksum=$(< "$package_root/frame-advanced-settings.sha256")
checksum=${checksum%$'\r'}
[[ $checksum == "$binary_hash  frame-advanced-settings" ||
   $checksum == "$binary_hash *frame-advanced-settings" ]] || die 'Packaged executable checksum mismatch'
printf 'Package verified: build %s, executable checksum, ARM64 Linux libraries, manifests, and scripts.\n' "$build_id"
