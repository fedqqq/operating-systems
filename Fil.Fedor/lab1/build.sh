#!/bin/sh
set -eu
cd "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
if [ "$(uname -s)" != Linux ]; then
    echo 'lab1 requires Linux (/proc, inotify, signalfd, pidfd); use Docker on macOS.' >&2
    exit 1
fi
temporary=$(mktemp -d)
trap 'rm -rf "$temporary"' EXIT HUP INT TERM
"${CXX:-g++}" -std=c++17 -Wall -Wextra -Werror ${CXXFLAGS:-} -Iinclude \
    src/main.cpp src/config.cpp src/watcher.cpp src/daemon.cpp \
    ${LDFLAGS:-} -o "$temporary/lab1"
cp "$temporary/lab1" ./lab1
printf '%s\n' 'Built lab1 (-std=c++17 -Wall -Wextra -Werror); temporary files removed.'
