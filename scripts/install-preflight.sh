#!/bin/sh
# Read-only host check for binary/source installation. Does not install anything.
set -u

mode=binary
if [ "$#" -ne 0 ]; then
    if [ "$#" -eq 1 ] && [ "$1" = --help ]; then
        printf '%s\n' 'Usage: sh scripts/install-preflight.sh [--source]' \
            'Read-only Linux ARM64 binary/source dependency check; never installs anything.'
        exit 0
    fi
    if [ "$#" -eq 1 ] && [ "$1" = --source ]; then
        mode=source
    else
        printf '%s\n' 'Only --source or --help is supported.' >&2
        exit 2
    fi
fi

errors=0
os=$(uname -s 2>/dev/null) || os=unknown
arch=$(uname -m 2>/dev/null) || arch=unknown
printf 'Host: %s %s\n' "$os" "$arch"
if [ "$os" != Linux ] || [ "$arch" != aarch64 ]; then
    printf '%s\n' 'Unsupported host: planned release payload is Linux AArch64 only.' >&2
    errors=1
fi

libc=unknown
if command -v getconf >/dev/null 2>&1; then
    libc=$(getconf GNU_LIBC_VERSION 2>/dev/null) || libc=unknown
fi
printf 'C library: %s\n' "$libc"
case "$libc" in
    'glibc '*) ;;
    *) printf '%s\n' 'Unsupported/unknown C library: planned ARM64 wheels require glibc.' >&2
       errors=1 ;;
esac

# curl is needed for an explicitly approved GitHub download; local archives do
# not require it. The Python bootstrap handles tar/SHA-256 for both modes.
for dep in python3; do
    if command -v "$dep" >/dev/null 2>&1; then
        printf 'Required tool: %s found\n' "$dep"
    else
        printf 'Required tool: %s MISSING\n' "$dep" >&2
        errors=1
    fi
done

if command -v git >/dev/null 2>&1; then
    printf '%s\n' 'Git: found (not required for a release install)'
else
    printf '%s\n' 'Git: missing (not required for a release install)'
fi
if command -v uv >/dev/null 2>&1; then
    printf '%s\n' 'uv: found (not required for a bundled runtime)'
else
    printf '%s\n' 'uv: missing (not required for a bundled runtime)'
fi

if command -v python3 >/dev/null 2>&1; then
    version=$(python3 --version 2>&1) || version=unknown
    printf 'System Python: %s (3.12+ required for installer bootstrap)\n' "$version"
    case "$version" in
        'Python 3.'*)
            minor=${version#Python 3.}
            minor=${minor%%.*}
            case "$minor" in
                ''|*[!0-9]*) printf '%s\n' 'System Python version could not be parsed.' >&2; errors=1 ;;
                *) if [ "$minor" -lt 12 ]; then
                       printf '%s\n' 'System Python is older than 3.12; installer bootstrap requires 3.12+.' >&2
                       errors=1
                   fi ;;
            esac ;;
        *) printf '%s\n' 'System Python version could not be parsed.' >&2; errors=1 ;;
    esac
else
    printf '%s\n' 'System Python: missing (installer bootstrap requires 3.12+)' >&2
    errors=1
fi

if [ "$mode" = source ]; then
    for dep in cmake c++ pkg-config wayland-scanner; do
        if command -v "$dep" >/dev/null 2>&1; then
            printf 'Source tool: %s found\n' "$dep"
        else
            printf 'Source tool: %s MISSING\n' "$dep" >&2
            errors=1
        fi
    done
    if command -v pkg-config >/dev/null 2>&1; then
        for dep in sdl3 wayland-client xcb freetype2 vulkan; do
            if pkg-config --exists "$dep"; then
                printf 'Source dependency: %s found\n' "$dep"
            else
                printf 'Source dependency: %s MISSING\n' "$dep" >&2
                errors=1
            fi
        done
    fi
fi

if [ "$errors" -ne 0 ]; then
    printf '%s\n' 'Preflight failed. No changes were made.' >&2
    exit 1
fi
printf 'Preflight passed for %s prerequisites; no changes were made.\n' "$mode"
