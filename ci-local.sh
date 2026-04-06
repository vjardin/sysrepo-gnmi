#!/bin/bash
# Run CI checks locally in Docker for all CESNET versions.
# Usage: ./ci-local.sh [v2|v3|v4|v5|all] [none|address|undefined|all-sanitizers]
set -euo pipefail

VERSIONS="${1:-all}"
SANITIZER="${2:-none}"
IMAGE="ubuntu:noble"
PASS=0
FAIL=0
FAILED_COMBOS=""

if [ "$VERSIONS" = "all" ]; then
    VERSIONS="v2 v3 v4 v5"
fi

SANITIZERS="$SANITIZER"
if [ "$SANITIZER" = "all-sanitizers" ]; then
    SANITIZERS="address undefined"
fi

for V in $VERSIONS; do
  for S in $SANITIZERS; do
    LABEL="$V"
    [ "$S" != "none" ] && LABEL="$V+$S"

    echo ""
    echo "========================================"
    echo "  Building and testing: $LABEL"
    echo "========================================"

    if docker run --rm \
        -v "$(pwd):/src:ro" \
        -w /build \
        "$IMAGE" \
        bash -exc "
            # Install deps
            apt-get update -qq
            apt-get install -y -qq \
                build-essential meson ninja-build cmake pkg-config git \
                libgrpc-dev protobuf-compiler \
                libprotobuf-c-dev protobuf-c-compiler \
                libevent-dev libcjson-dev libpcre2-dev \
                python3-venv >/dev/null 2>&1

            # Copy source (exclude build artifacts from host)
            mkdir -p /build/src
            cd /src && tar cf - --exclude=builddir --exclude=.venv --exclude=__pycache__ . | tar xf - -C /build/src
            cd /build/src

            # Download subprojects
            git config --global --add safe.directory '*'
            meson subprojects download || true

            # Build
            meson setup builddir -Dcesnet_version=$V -Dsanitize=$S
            meson compile -C builddir

            # Setup Python
            python3 -m venv .venv
            .venv/bin/pip install -q grpcio grpcio-tools pytest

            # Generate Python stubs
            mkdir -p tests/proto
            .venv/bin/python -m grpc_tools.protoc \
                -Iproto --python_out=tests/proto --grpc_python_out=tests/proto \
                proto/gnmi.proto proto/gnmi_ext.proto

            # Run tests
            export LD_LIBRARY_PATH=\$(pwd)/builddir/deps/lib
            export GNMI_BUILD_DIR=\$(pwd)/builddir
            export GNMI_YANG_DIR=\$(pwd)/tests/yang
            export no_proxy=localhost,127.0.0.1
            export ASAN_OPTIONS=detect_leaks=0
            export UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1
            .venv/bin/pytest tests/ -v --tb=short
        "; then
        echo "=== $LABEL: PASSED ==="
        PASS=$((PASS + 1))
    else
        echo "=== $LABEL: FAILED ==="
        FAIL=$((FAIL + 1))
        FAILED_COMBOS="$FAILED_COMBOS $LABEL"
    fi
  done
done

echo ""
echo "========================================"
echo "  RESULTS: $PASS passed, $FAIL failed"
if [ $FAIL -gt 0 ]; then
    echo "  Failed:$FAILED_COMBOS"
fi
echo "========================================"
exit $FAIL
