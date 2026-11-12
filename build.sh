#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# Build LEDGER-4 for the FM-1 with fm1-base (base/). macOS runs the JieLi toolchain in Docker (base/tools/build.py).
#   ./build.sh                  a development build: app/build/ledger-4.fwsc
#   ./build.sh --release 0.1    a release build: app/build/ledger-4-0.1.fwsc
#   ./build.sh --host           the host simulator, then the tests in app/tests/
set -e
cd "$(dirname "$0")"
export JIELI_TOOLCHAIN="${JIELI_TOOLCHAIN:-$HOME/.jieli/toolchain}"
export AC79_SDK="${AC79_SDK:-$HOME/.jieli/ac79-sdk}"
if [ -z "$PYTHON" ]; then
    if command -v python3.11 >/dev/null 2>&1; then PYTHON=python3.11; else PYTHON=python3; fi
fi
NAME=$(sed -n 's/^name = "\(.*\)".*/\1/p' app/fm1.toml | tr 'A-Z' 'a-z')
if [ "$1" = "--host" ]; then
    FM1B_BOARD=fm1 "$PYTHON" base/tools/build.py app --host
    fail=0
    for t in app/tests/*.txt; do
        case "$t" in *screens.txt) continue;; esac
        echo "== $t"
        app/build/host/${NAME}_host "$t" app/build/host || fail=1
    done
    exit $fail
fi
if [ "$1" = "--shots" ]; then
    FM1B_BOARD=fm1 "$PYTHON" base/tools/build.py app --host
    mkdir -p app/docs/img
    app/build/host/${NAME}_host app/tests/screens.txt app/docs/img
    exit 0
fi
REL=""
if [ "$1" = "--release" ]; then REL="--release $2"; fi
FM1B_BOARD=fm1 "$PYTHON" base/tools/build.py app $REL
mkdir -p build/firmware
find app/build -maxdepth 1 -name '*.fwsc' -exec cp {} build/firmware/ \;
(cd build/firmware && shasum -a 256 *.fwsc > SHA256SUMS && cat SHA256SUMS)
