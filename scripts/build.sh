#!/usr/bin/env bash
set -euo pipefail
source "$(dirname -- "${BASH_SOURCE[0]}")/build-common.sh"
test_only=false
release_version=''
while (( $# )); do
    case $1 in
        --test-only) test_only=true; shift ;;
        --version)
            (( $# >= 2 )) || die '--version requires a version'
            release_version=$2
            valid_release_version "$release_version" || die 'Expected a version such as 0.1.0 or 0.2.0-beta.1'
            shift 2 ;;
        --help|-h)
            printf '%s\n' 'Usage: bash build.sh [--test-only] [--version VERSION]' \
                'Run host tests, then build and package the ARM64 Steam Frame application.' \
                'Hosts: x86_64 Linux or Windows Git Bash. Requires CMake 3.20+, Ninja, g++, curl, GNU readelf, tar, and sha256sum.' \
                'Compiler extraction also needs xz on Linux or unzip on Windows.' \
                '--version accepts 0.1.0 or 0.2.0-beta.1; omitted uses the CMake development version.'
            exit 0 ;;
        *) die "Unknown argument: $1" ;;
    esac
done
require_tools cmake ctest ninja g++
if [[ $test_only == false ]]; then
    [[ $(uname -m) == x86_64 ]] || die 'The cross-build requires an x86_64 host'
    case $(uname -s) in
        MINGW*|MSYS*) zig_host=x86_64-windows; zig_exe=zig.exe; require_tools unzip ;;
        Linux) zig_host=x86_64-linux; zig_exe=zig; require_tools xz ;;
        *) die 'Use Linux, Windows Git Bash, or --test-only' ;;
    esac
    require_tools curl readelf tar sha256sum
fi
bash "$script_dir/verify-dependencies.sh"
host_build=$project_root/build/host
host_compiler=$(command -v g++)
[[ ! -f $host_compiler.exe ]] || host_compiler=$host_compiler.exe
run_cmake -S "$(native_path "$project_root")" -B "$(native_path "$host_build")" -G Ninja \
    -DFRAME_BUILD_APP=OFF -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Debug \
    "-DFRAME_RELEASE_VERSION=$release_version" "-DCMAKE_CXX_COMPILER=$(native_path "$host_compiler")"
run_cmake --build "$(native_path "$host_build")"
ctest --test-dir "$(native_path "$host_build")" --output-on-failure
[[ $test_only == false ]] || exit 0

toolchain=$(metadata toolchain "-DTOOLCHAIN_HOST=$zig_host")
toolchain=${toolchain//$'\r'/}
mapfile -t zig_fields <<< "$toolchain"
(( ${#zig_fields[@]} == 3 )) || die 'Unexpected toolchain metadata'
zig_version=${zig_fields[0]}
zig_url=${zig_fields[1]}
zig_hash=${zig_fields[2],,}
tools_dir=$project_root/tools
zig_root=$tools_dir/zig-$zig_host-$zig_version
zig=$zig_root/$zig_exe
if [[ ! -f $zig ]]; then
    mkdir -p -- "$tools_dir"
    download=$tools_dir/${zig_url##*/}
    if [[ ! -f $download ]]; then
        curl --fail --location --retry 2 --output "$download.part" "$zig_url"
        actual=$(sha256sum "$download.part")
        [[ ${actual%% *} == "$zig_hash" ]] || die 'Downloaded Zig checksum mismatch'
        mv -- "$download.part" "$download"
    fi
    actual=$(sha256sum "$download")
    [[ ${actual%% *} == "$zig_hash" ]] || die 'Zig checksum mismatch'
    if [[ $zig_host == x86_64-windows ]]; then
        unzip -q -o "$download" -d "$tools_dir"
    else
        tar -xJf "$download" -C "$tools_dir"
    fi
    [[ -f $zig ]] || die 'Zig archive did not contain the expected compiler'
fi
cross_build=$project_root/build/aarch64
export ZIG_GLOBAL_CACHE_DIR
ZIG_GLOBAL_CACHE_DIR=$(native_path "$tools_dir/zig-cache")
# CMake expects executable paths for archiver subcommands.
if [[ $zig_host == x86_64-windows ]]; then
    ar_wrapper=$tools_dir/zig-ar.cmd
    ranlib_wrapper=$tools_dir/zig-ranlib.cmd
    printf '@"%%~dp0zig-x86_64-windows-%s\\zig.exe" ar %%*\r\n' "$zig_version" > "$ar_wrapper"
    printf '@"%%~dp0zig-x86_64-windows-%s\\zig.exe" ranlib %%*\r\n' "$zig_version" > "$ranlib_wrapper"
else
    ar_wrapper=$tools_dir/zig-ar.sh
    ranlib_wrapper=$tools_dir/zig-ranlib.sh
    printf '#!/usr/bin/env bash\nexec %q ar "$@"\n' "$zig" > "$ar_wrapper"
    printf '#!/usr/bin/env bash\nexec %q ranlib "$@"\n' "$zig" > "$ranlib_wrapper"
    chmod +x "$ar_wrapper" "$ranlib_wrapper"
fi
run_cmake -S "$(native_path "$project_root")" -B "$(native_path "$cross_build")" -G Ninja \
    -UCMAKE_CXX_ARCHIVE_CREATE -UCMAKE_CXX_ARCHIVE_FINISH \
    -DFRAME_BUILD_APP=ON -DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=Release \
    "-DFRAME_RELEASE_VERSION=$release_version" \
    -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
    "-DCMAKE_CXX_COMPILER=$(native_path "$zig")" -DCMAKE_CXX_COMPILER_ARG1=c++ \
    '-DCMAKE_CXX_FLAGS=-target aarch64-linux-gnu.2.35' \
    -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
    "-DCMAKE_AR=$(native_path "$ar_wrapper")" "-DCMAKE_RANLIB=$(native_path "$ranlib_wrapper")"
run_cmake --build "$(native_path "$cross_build")" --parallel 2
package_root=$project_root/dist/frame-advanced-settings
run_cmake --install "$(native_path "$cross_build")" --prefix "$(native_path "$package_root")"
cp -- "$project_root/install.sh" "$project_root/README.md" "$project_root/LICENSE" "$package_root/"
mkdir -p -- "$package_root/contrib"
cp -R -- "$project_root/contrib/." "$package_root/contrib/"
cp -- "$script_dir/verify-running.sh" "$package_root/"
binary_hash=$(sha256sum "$package_root/frame-advanced-settings")
printf '%s  frame-advanced-settings\n' "${binary_hash%% *}" > "$package_root/frame-advanced-settings.sha256"
licenses=$package_root/licenses
mkdir -p -- "$licenses"
for dependency in openvr stb spdlog; do
    cp -- "$project_root/vendor/$dependency/LICENSE" "$licenses/$dependency.txt"
done
cp -- "$project_root/vendor/vulkan/LICENSE.md" "$licenses/vulkan.txt"
cp -- "$project_root/vendor/lucide/LICENSE" "$licenses/lucide.txt"
cp -- "$project_root/vendor/nanosvg/LICENSE.txt" "$licenses/nanosvg.txt"
cp -- "$project_root/vendor/inter/LICENSE.txt" "$licenses/inter.txt"
cp -- "$project_root/vendor/spdlog/LICENSE.fmt" "$licenses/spdlog-fmt.txt"
for license_name in Apache-2.0 MIT; do
    cp -- "$project_root/vendor/vulkan/$license_name.txt" "$licenses/vulkan-$license_name.txt"
done
cp -- "$zig_root/LICENSE" "$licenses/zig.txt"
for runtime in libcxx libcxxabi libunwind; do
    cp -- "$zig_root/lib/$runtime/LICENSE.TXT" "$licenses/$runtime.txt"
done
bash "$script_dir/verify-package.sh" "$package_root"
build_id=$(< "$package_root/build-id.txt")
version=${build_id%%+*}
archive=frame-advanced-settings-$version-linux-aarch64.tar.gz
tar -czf "$project_root/dist/$archive" -C "$project_root/dist" frame-advanced-settings
archive_hash=$(sha256sum "$project_root/dist/$archive")
printf '%s  %s\n' "${archive_hash%% *}" "$archive" > "$project_root/dist/$archive.sha256"
printf 'Package: %s\nBuild: %s\n' "$project_root/dist/$archive" "$build_id"
