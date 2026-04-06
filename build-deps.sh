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

# Select subproject directories and git refs based on version
LIBYANG_URL="https://github.com/CESNET/libyang.git"
SYSREPO_URL="https://github.com/sysrepo/sysrepo.git"

case "$VERSION" in
    v2)
        LIBYANG_DIR="$SRC/libyang-v2"
        SYSREPO_DIR="$SRC/sysrepo-v2"
        LIBYANG_REF="v2.1.148"
        SYSREPO_REF="v2.2.36"
        ;;
    v3)
        LIBYANG_DIR="$SRC/libyang-v3"
        SYSREPO_DIR="$SRC/sysrepo-v3"
        LIBYANG_REF="v3.13.6"
        SYSREPO_REF="v3.7.11"
        ;;
    v5)
        LIBYANG_DIR="$SRC/libyang-v5"
        SYSREPO_DIR="$SRC/sysrepo-v5"
        LIBYANG_REF="v5.4.9"
        SYSREPO_REF="v4.5.4"
        ;;
    *)  # v4 (default)
        LIBYANG_DIR="$SRC/libyang"
        SYSREPO_DIR="$SRC/sysrepo"
        LIBYANG_REF="v4.2.2"
        SYSREPO_REF="v4.2.10"
        ;;
esac

# Auto-download if subproject dirs don't exist
for dir_url_ref in "$LIBYANG_DIR $LIBYANG_URL $LIBYANG_REF" "$SYSREPO_DIR $SYSREPO_URL $SYSREPO_REF"; do
    dir=$(echo "$dir_url_ref" | cut -d' ' -f1)
    url=$(echo "$dir_url_ref" | cut -d' ' -f2)
    ref=$(echo "$dir_url_ref" | cut -d' ' -f3)
    if [ ! -d "$dir" ]; then
        echo "=== Cloning $url ($ref) into $dir ==="
        git clone --depth 1 --branch "$ref" "$url" "$dir"
    fi
done

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
