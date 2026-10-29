#!/bin/sh
set -eu
cd "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
./build.sh
temporary=$(mktemp -d)
trap 'rm -rf "$temporary"' EXIT HUP INT TERM
"${CXX:-g++}" -std=c++17 -Wall -Wextra -Werror ${CXXFLAGS:-} -Iinclude \
    tests/singleton.cpp src/daemon.cpp src/config.cpp src/watcher.cpp \
    ${LDFLAGS:-} -o "$temporary/singleton"
"$temporary/singleton"
python3 tests/test_lab1.py "$@"
