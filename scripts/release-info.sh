#!/usr/bin/env bash
# Validate the tag and release commit before building or publishing anything.
set -euo pipefail
source "$(dirname -- "${BASH_SOURCE[0]}")/build-common.sh"
(( $# == 1 )) || die 'Usage: bash release-info.sh TAG'
tag=$1
[[ $tag == v* ]] && valid_release_version "${tag#v}" ||
    die 'Expected a tag such as v0.1.0 or v0.2.0-beta.1'
require_tools git
commit=$(git rev-parse --verify "refs/tags/$tag^{commit}") || die 'Release tag does not exist'
[[ $(git rev-parse HEAD) == "$commit" ]] || die 'Checkout does not match the release tag'
git merge-base --is-ancestor "$commit" refs/remotes/origin/main ||
    die 'Release commit must already be on origin/main'
channel=stable
prerelease=false
if [[ $tag == *-beta.* ]]; then
    channel=beta
    prerelease=true
fi
printf 'version=%s\nchannel=%s\nprerelease=%s\ncommit=%s\ntag_object=%s\narchive=%s\n' \
    "${tag#v}" "$channel" "$prerelease" "$commit" "$(git rev-parse "refs/tags/$tag")" \
    "frame-advanced-settings-${tag#v}-linux-aarch64.tar.gz"
