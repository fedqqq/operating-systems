#!/bin/sh
set -eu
cd "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
if [ "$(uname -s)" != Linux ]; then
    echo 'lab1 requires Linux (/proc, inotify, signalfd, pidfd).' >&2
    exit 1
fi
temporary=$(mktemp -d)
trap 'rm -rf "$temporary"' EXIT HUP INT TERM
cmake -S . -B "$temporary" -DBUILD_TESTING=OFF "$@"
cmake --build "$temporary" --parallel --verbose
cp "$temporary/lab1" ./lab1
printf '%s\n' 'Built lab1 with CMake (-Wall -Wextra -Werror).'
