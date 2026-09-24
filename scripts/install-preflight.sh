#!/bin/sh
# Read-only host check for the proposed release installer. Does not install anything.
set -u

if [ "$#" -ne 0 ]; then
    if [ "$#" -eq 1 ] && [ "$1" = --help ]; then
        printf '%s\n' 'Usage: sh scripts/install-preflight.sh' \
            'Checks the proposed Linux ARM64 installation prerequisites; makes no changes.'
        exit 0
    fi
    printf '%s\n' 'No arguments are supported (except --help).' >&2
    exit 2
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

# Expected for a future curl/tar release bootstrap, not for building from source.
for dep in curl tar sha256sum mktemp mkdir mv; do
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
    printf 'System Python: %s (not required for a bundled runtime)\n' "$version"
    case "$version" in
        'Python 3.'*)
            minor=${version#Python 3.}
            minor=${minor%%.*}
            case "$minor" in
                ''|*[!0-9]*) printf '%s\n' 'System Python version could not be parsed.' ;;
                *) if [ "$minor" -lt 12 ]; then
                       printf '%s\n' 'System Python is older than 3.12; unsuitable for the proposed optional source worker.'
                   fi ;;
            esac ;;
        *) printf '%s\n' 'System Python version could not be parsed.' ;;
    esac
else
    printf '%s\n' 'System Python: missing (not required for a bundled runtime)'
fi

if [ "$errors" -ne 0 ]; then
    printf '%s\n' 'Preflight failed. No changes were made.' >&2
    exit 1
fi
printf '%s\n' 'Preflight passed for the proposed package format. No installer or release payload exists yet; nothing was installed.'
