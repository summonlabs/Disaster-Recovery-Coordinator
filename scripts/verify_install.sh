#!/usr/bin/env bash
# Build the package, install it into a scratch prefix, build an independent
# downstream consumer against that prefix (never against the build tree), and
# run the consumer. Any failure stops the script with a non-zero exit code.
#
# There is no timeout anywhere in this script: a hang is a defect.
set -euo pipefail

BUILD_DIR="${1:-build/release}"
PREFIX="${2:-_install}"
CONFIG="${3:-Release}"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

cmake --build "$BUILD_DIR"
cmake --install "$BUILD_DIR" --prefix "$PREFIX" --config "$CONFIG"

if [ ! -f "$PREFIX/lib/cmake/DisasterRecoveryCoordinator/DisasterRecoveryCoordinatorConfig.cmake" ]; then
    echo "the installed package has no CMake package config" >&2
    exit 1
fi
if [ ! -f "$PREFIX/include/drc/engine.hpp" ]; then
    echo "the installed package has no public headers" >&2
    exit 1
fi

rm -rf "$PREFIX/consumer-build"
cmake -S tests/downstream -B "$PREFIX/consumer-build" -DCMAKE_BUILD_TYPE="$CONFIG" -DCMAKE_PREFIX_PATH="$ROOT/$PREFIX"
cmake --build "$PREFIX/consumer-build"
"$PREFIX/consumer-build/downstream_consumer"
echo "install and downstream consumption verified against $PREFIX"
