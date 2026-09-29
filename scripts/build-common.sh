#!/usr/bin/env bash
# Shared host-side helpers; sourced by the build and verification entry points.
script_dir=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
project_root=$(CDPATH= cd -- "$script_dir/.." && pwd)

die() { printf '%s\n' "$*" >&2; exit 1; }
release_version_pattern='(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(-beta\.[1-9][0-9]*)?'
valid_release_version() { [[ $1 =~ ^${release_version_pattern}$ ]]; }
require_tools() {
    local tool
    for tool in "$@"; do
        command -v "$tool" >/dev/null 2>&1 || die "Missing required tool: $tool"
    done
}
native_path() {
    if command -v cygpath >/dev/null 2>&1; then
        cygpath -am -- "$1"
    else
        printf '%s\n' "$1"
    fi
}
run_cmake() {
    # Paths are explicitly converted above, including -D values. Do not let
    # MSYS rewrite compiler flags or Linux paths passed to native CMake.
    MSYS2_ARG_CONV_EXCL='*' cmake "$@"
}
metadata() {
    run_cmake "-DMODE=$1" "-DPROJECT_ROOT=$(native_path "$project_root")" \
        "${@:2}" -P "$(native_path "$script_dir/metadata.cmake")"
}
