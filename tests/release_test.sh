#!/usr/bin/env bash
# Exercise release validation in a disposable repository, never the real tags.
set -euo pipefail
source "$(dirname -- "${BASH_SOURCE[0]}")/../scripts/build-common.sh"
test_root=$(mktemp -d "${TMPDIR:-/tmp}/frame-release-tests.XXXXXXXX")
export GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null
git init -q --initial-branch=main "$test_root/repo"
cd "$test_root/repo"
git config user.name 'Release fixture'
git config user.email fixture@example.invalid
printf 'first\n' > source.txt
git add source.txt
git commit -q -m 'initial fixture'
git update-ref refs/remotes/origin/main HEAD
git tag -a v0.1.0 -m v0.1.0
git tag v0.2.0-beta.1

check_release() {
    local tag=$1 channel=$2 prerelease=$3 info
    info=$(bash "$project_root/scripts/release-info.sh" "$tag")
    grep -qxF "version=${tag#v}" <<< "$info"
    grep -qxF "channel=$channel" <<< "$info"
    grep -qxF "prerelease=$prerelease" <<< "$info"
    grep -qxF "archive=frame-advanced-settings-${tag#v}-linux-aarch64.tar.gz" <<< "$info"
    grep -qxF "tag_object=$(git rev-parse "refs/tags/$tag")" <<< "$info"
}
reject() {
    local expected=$1
    shift
    if "$@" > "$test_root/failure.log" 2>&1; then
        printf 'Expected rejection: %s\n' "$*" >&2
        exit 1
    fi
    grep -qF "$expected" "$test_root/failure.log" || { cat "$test_root/failure.log" >&2; exit 1; }
}
check_release v0.1.0 stable false
check_release v0.2.0-beta.1 beta true
for tag in v01.2.3 v1.02.3 v1.2.03 v1.2 v1.2.3-beta.0 v1.2.3-beta.01 \
    v1.2.3-rc.1 v1.2.3+metadata app/v1.2.3 'v1.2.3;echo injected' $'v1.2.3\nextra'; do
    reject 'Expected a tag' bash "$project_root/scripts/release-info.sh" "$tag"
done
reject 'does not exist' bash "$project_root/scripts/release-info.sh" v9.9.9
printf 'second\n' >> source.txt
git commit -qam 'second fixture'
reject 'Checkout does not match' bash "$project_root/scripts/release-info.sh" v0.1.0
git tag v0.3.0
reject 'must already be on origin/main' bash "$project_root/scripts/release-info.sh" v0.3.0
git update-ref refs/remotes/origin/main HEAD
check_release v0.3.0 stable false

# Confirm CMake embeds the complete release version, including beta suffixes,
# and clears a previous override for an ordinary development build.
compiler=$(command -v g++)
[[ ! -f $compiler.exe ]] || compiler=$compiler.exe
configure=(cmake -S "$(native_path "$project_root")" -B "$(native_path "$test_root/configure")"
    -G Ninja -DFRAME_BUILD_APP=OFF -DBUILD_TESTING=OFF
    "-DCMAKE_CXX_COMPILER=$(native_path "$compiler")")
export MSYS2_ARG_CONV_EXCL='*'
for version in 2.3.4 2.4.0-beta.2; do
    "${configure[@]}" "-DFRAME_RELEASE_VERSION=$version" > "$test_root/configure.log" 2>&1 ||
        { cat "$test_root/configure.log" >&2; exit 1; }
    [[ $(< "$test_root/configure/build-id.txt") == "$version+"* ]]
    grep -qF "version = \"$version\"" "$test_root/configure/generated/build_version.h"
done
reject 'Expected a release version' "${configure[@]}" -DFRAME_RELEASE_VERSION=2.4.0-beta.01
"${configure[@]}" -DFRAME_RELEASE_VERSION= > "$test_root/configure.log" 2>&1
default_version=$(sed -n 's/^CMAKE_PROJECT_VERSION:STATIC=//p' "$test_root/configure/CMakeCache.txt")
[[ -n $default_version ]]
grep -qF "version = \"$default_version\"" "$test_root/configure/generated/build_version.h"
printf 'Release tag, branch, and version tests passed: %s\n' "$test_root"
