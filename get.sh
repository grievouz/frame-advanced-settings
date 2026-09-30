#!/bin/sh
# Standalone bootstrap: keep this usable directly through curl | sh.
# Define everything before running so an interrupted script download cannot
# execute a partially received installer body.
main() (
    set -eu
    export LC_ALL=C
    umask 077
    die() { printf '%s\n' "$*" >&2; exit 1; }
    [ "$#" = 0 ] || die 'This installer takes no arguments; it installs the latest stable release.'
    [ "$(uname -s)" = Linux ] && [ "$(uname -m)" = aarch64 ] ||
        die 'Run this installer on your Steam Frame (ARM64 Linux).'
    [ "$(id -u)" != 0 ] || die 'Run as your normal SteamOS user, without sudo.'
    case ${HOME:-} in /*) [ "$HOME" != / ] || die 'Invalid HOME' ;; *) die 'Invalid HOME' ;; esac
    for tool in curl tar sha256sum awk grep mktemp timeout sh systemctl flock; do
        command -v "$tool" >/dev/null 2>&1 || die "Missing required command: $tool"
    done
    # Ignore local curl configuration and only follow HTTPS redirects.
    download() {
        curl --disable --fail --silent --show-error --location --proto '=https' --proto-redir '=https' \
            --connect-timeout 10 --max-time 120 --retry 2 --retry-max-time 180 "$@"
    }
    repo=https://github.com/grievouz/frame-advanced-settings
    printf 'Finding the latest Frame Advanced Settings release...\n'
    release_url=$(download --head --output /dev/null --write-out '%{url_effective}' "$repo/releases/latest")
    case $release_url in "$repo/releases/tag/v"*) version=${release_url#"$repo/releases/tag/v"} ;;
        *) die 'Could not resolve the latest stable release on GitHub.' ;; esac
    printf '%s\n' "$version" | grep -Eq '^(0|[1-9][0-9]{0,8})\.(0|[1-9][0-9]{0,8})\.(0|[1-9][0-9]{0,8})$' ||
        die 'GitHub returned an unexpected release version.'
    stage=$(mktemp -d /tmp/frame-advanced-settings-download.XXXXXX)
    cleanup() {
        result=$?
        trap - 0
        case $stage in /tmp/frame-advanced-settings-download.*) rm -rf -- "$stage" ;; esac
        exit "$result"
    }
    trap cleanup 0
    trap 'exit 129' HUP
    trap 'exit 130' INT
    trap 'exit 143' TERM
    archive=frame-advanced-settings-$version-linux-aarch64.tar.gz
    url=$repo/releases/download/v$version
    printf 'Downloading version %s...\n' "$version"
    download --max-filesize 1024 --output "$stage/checksum" "$url/$archive.sha256"
    download --max-filesize 33554432 --output "$stage/package.tar.gz" "$url/$archive"
    expected=$(awk -v archive="$archive" '
        NR != 1 || NF != 2 || length($1) != 64 || $1 ~ /[^a-fA-F0-9]/ || $2 != archive { exit 1 }
        { print tolower($1) }
        END { if (NR != 1) exit 1 }
    ' "$stage/checksum") || die 'Invalid release checksum file.'
    actual=$(sha256sum "$stage/package.tar.gz")
    [ "${actual%% *}" = "$expected" ] || die 'Package checksum mismatch; nothing was installed.'

    # Apply the same archive limits as the in-app updater before extraction.
    (ulimit -f 4096; timeout 20 tar --list --verbose --gzip --numeric-owner --full-time --quoting-style=escape \
        --file "$stage/package.tar.gz" > "$stage/members")
    awk '
        NF != 6 { exit 1 }
        {
            type = substr($1,1,1); path = $6
            if (type != "-" && type != "d") exit 1
            if ($3 !~ /^[0-9]+$/) exit 1
            if (path !~ /^frame-advanced-settings\/[A-Za-z0-9_.+\/-]*$/ || path ~ /(^|\/)\.\.?($|\/)/ || path ~ /\/\//) exit 1
            if (seen[path]++) exit 1
            total += $3; count++
            if (total > 134217728 || count > 2048) exit 1
        }
        END { if (count < 2) exit 1 }
    ' "$stage/members" || die 'Unsafe or oversized release archive; nothing was installed.'
    timeout 30 tar --extract --gzip --file "$stage/package.tar.gz" --directory "$stage" \
        --no-same-owner --no-same-permissions --keep-old-files
    package=$stage/frame-advanced-settings
    for file in install.sh build-id.txt frame-advanced-settings frame-advanced-settings.sha256; do
        [ -f "$package/$file" ] || die "Incomplete release package: $file"
    done
    identity=$(cat "$package/build-id.txt")
    case $identity in "$version+"*) build_hash=${identity#"$version+"} ;;
        *) die 'Package version does not match the release.' ;; esac
    printf '%s\n' "$build_hash" | grep -Eq '^[a-f0-9]{12}$' || die 'Invalid package build identity.'
    printf 'Verified version %s. Installing...\n' "$version"
    sh "$package/install.sh" < /dev/null
)
main "$@"
