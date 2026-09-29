#!/usr/bin/env bash
set -euo pipefail
source "$(dirname -- "${BASH_SOURCE[0]}")/build-common.sh"
case "${1:-}" in
    --help|-h) printf 'Usage: bash verify-dependencies.sh\nRequires CMake 3.20 or newer.\n'; exit 0 ;;
    '') ;;
    *) die 'Usage: bash verify-dependencies.sh' ;;
esac
(( $# == 0 )) || die 'Usage: bash verify-dependencies.sh'
require_tools cmake
metadata dependencies
