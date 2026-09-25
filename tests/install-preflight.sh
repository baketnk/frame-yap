#!/bin/sh
# Offline, hardware-free contract test using a restricted PATH of fake host tools.
set -eu
script=$1
tmp=$(/bin/mktemp -d)
trap '/bin/rm -rf "$tmp"' EXIT HUP INT TERM
/bin/mkdir "$tmp/bin"
for dep in curl tar sha256sum mktemp mkdir mv; do
    printf '#!/bin/sh\nexit 0\n' > "$tmp/bin/$dep"
    /bin/chmod +x "$tmp/bin/$dep"
done
printf '#!/bin/sh\ncase "$1" in -s) printf "%%s\\n" "${MOCK_OS:-Linux}" ;; -m) printf "%%s\\n" "${MOCK_ARCH:-aarch64}" ;; esac\n' > "$tmp/bin/uname"
printf '#!/bin/sh\nprintf "%%s\\n" "${MOCK_LIBC:-glibc 2.39}"\n' > "$tmp/bin/getconf"
printf '#!/bin/sh\nprintf "%%s\\n" "${MOCK_PYTHON:-Python 3.12.3}"\n' > "$tmp/bin/python3"
/bin/chmod +x "$tmp/bin/uname" "$tmp/bin/getconf" "$tmp/bin/python3"

check() {
    expected=$1
    needle=$2
    shift 2
    status=0
    output=$(PATH="$tmp/bin" "$@" /bin/sh "$script" 2>&1) || status=$?
    if [ "$status" -ne "$expected" ]; then
        printf 'Expected exit %s, got %s: %s\n' "$expected" "$status" "$output" >&2
        exit 1
    fi
    case "$output" in
        *"$needle"*) ;;
        *) printf 'Missing expected text %s: %s\n' "$needle" "$output" >&2; exit 1 ;;
    esac
}

check 0 'Git: missing (not required' /usr/bin/env
check 0 'uv: missing (not required' /usr/bin/env
check 0 'Preflight passed' /usr/bin/env
check 1 'older than 3.12' /usr/bin/env MOCK_PYTHON='Python 3.11.9'
for dep in cmake c++ pkg-config wayland-scanner; do
    printf '#!/bin/sh\nexit 0\n' > "$tmp/bin/$dep"
    /bin/chmod +x "$tmp/bin/$dep"
done
output=$(PATH="$tmp/bin" /bin/sh "$script" --source 2>&1)
case "$output" in *'Source dependency: vulkan found'*) ;; *) echo "Source preflight missed Vulkan: $output" >&2; exit 1;; esac
printf '#!/bin/sh\n[ "$2" != vulkan ]\n' > "$tmp/bin/pkg-config"
/bin/chmod +x "$tmp/bin/pkg-config"
status=0
output=$(PATH="$tmp/bin" /bin/sh "$script" --source 2>&1) || status=$?
[ "$status" -eq 1 ] || { echo "Expected failing source preflight: $output" >&2; exit 1; }
case "$output" in *'Source dependency: vulkan MISSING'*) ;; *) echo "Missing Vulkan failure: $output" >&2; exit 1;; esac
check 1 'Linux AArch64 only' /usr/bin/env MOCK_ARCH=x86_64
check 1 'require glibc' /usr/bin/env MOCK_LIBC=musl
/bin/rm "$tmp/bin/python3"
check 1 'python3 MISSING' /usr/bin/env
