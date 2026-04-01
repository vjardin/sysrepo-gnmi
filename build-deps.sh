#!/bin/sh
# Build CESNET C dependencies (libyang, sysrepo) with cmake.
# Called by meson as a custom_target.
# Arguments: $1=source_dir  $2=build_dir  $3=stamp_file  $4=version (v3 or v4)
set -e

SRC="$1/subprojects"
BLD="$2/deps_build"
PFX="$2/deps"
STAMP="$3"
VERSION="${4:-v4}"
NPROC=$(nproc 2>/dev/null || echo 4)

# Skip if already built
if [ -f "$STAMP" ]; then
    exit 0
fi

# Select subproject directories based on version
if [ "$VERSION" = "v3" ]; then
    LIBYANG_DIR="$SRC/libyang-v3"
    SYSREPO_DIR="$SRC/sysrepo-v3"
    # Fallback: if -v3 dirs don't exist, try renaming from download
    [ -d "$LIBYANG_DIR" ] || LIBYANG_DIR="$SRC/libyang"
    [ -d "$SYSREPO_DIR" ] || SYSREPO_DIR="$SRC/sysrepo"
else
    LIBYANG_DIR="$SRC/libyang"
    SYSREPO_DIR="$SRC/sysrepo"
fi

echo "=== Building CESNET deps ($VERSION) ==="
echo "  libyang: $LIBYANG_DIR"
echo "  sysrepo: $SYSREPO_DIR"

mkdir -p "$BLD" "$PFX"

# Ensure cmake and pkg-config find locally-installed deps
export PKG_CONFIG_PATH="$PFX/lib/pkgconfig:$PFX/lib/x86_64-linux-gnu/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export LD_LIBRARY_PATH="$PFX/lib:$PFX/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

echo "=== Building libyang ==="
cmake -S "$LIBYANG_DIR" -B "$BLD/libyang" \
    -DCMAKE_INSTALL_PREFIX="$PFX" \
    -DENABLE_TESTS=OFF -DENABLE_VALGRIND_TESTS=OFF -DENABLE_TOOLS=OFF \
    -DBUILD_SHARED_LIBS=ON \
    -DCMAKE_INSTALL_RPATH_USE_LINK_PATH=ON
cmake --build "$BLD/libyang" -j"$NPROC"
cmake --install "$BLD/libyang"

echo "=== Building sysrepo ==="
cmake -S "$SYSREPO_DIR" -B "$BLD/sysrepo" \
    -DCMAKE_INSTALL_PREFIX="$PFX" -DCMAKE_PREFIX_PATH="$PFX" \
    -DENABLE_TESTS=OFF -DENABLE_VALGRIND_TESTS=OFF -DENABLE_EXAMPLES=OFF \
    -DENABLE_SYSREPOCTL=ON -DENABLE_SYSREPOCFG=OFF -DENABLE_SYSREPO_PLUGIND=OFF \
    -DBUILD_SHARED_LIBS=ON \
    -DCMAKE_INSTALL_RPATH_USE_LINK_PATH=ON
cmake --build "$BLD/sysrepo" -j"$NPROC"
cmake --install "$BLD/sysrepo"

touch "$STAMP"
echo "=== All CESNET dependencies built ($VERSION) ==="
