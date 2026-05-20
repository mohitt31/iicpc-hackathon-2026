#!/bin/bash
# build_contestant.sh — Runs inside the hermetic builder container
#
# Per doc §7 stage 2:
#   source → compile → static-link
#   → record compiler version + flags into run metadata
#   → output: single contestant ELF
#
# Environment:
#   /src    — contestant source (read-only)
#   /output — output directory for ELF + metadata

set -euo pipefail

SRC_DIR="/src"
OUTPUT_DIR="/output"
BUILD_DIR="/build/workspace"

echo "[Builder] Starting hermetic build..."
echo "[Builder] GCC version: $(gcc --version | head -1)"

# ── Copy source to writable build directory ─────────────────
rm -rf "$BUILD_DIR"
cp -r "$SRC_DIR" "$BUILD_DIR"

# ── Detect build system ─────────────────────────────────────
CXX_FLAGS="-std=c++20 -O2 -Wall -Wextra -static -march=x86-64-v2"
COMPILER="g++"
COMPILER_VERSION="$(gcc --version | head -1)"

cd "$BUILD_DIR"

if [[ -f "CMakeLists.txt" ]]; then
    echo "[Builder] Detected CMake build system"
    mkdir -p cmake_build && cd cmake_build
    cmake .. -DCMAKE_CXX_FLAGS="$CXX_FLAGS" \
             -DCMAKE_EXE_LINKER_FLAGS="-static" \
             -DCMAKE_BUILD_TYPE=Release 2>&1
    make -j"$(nproc)" 2>&1

    # Find the first ELF executable
    ELF=$(find . -type f -executable -exec file {} \; | grep 'ELF' | head -1 | cut -d: -f1)
    if [[ -z "$ELF" ]]; then
        echo "[Builder] ERROR: No ELF binary produced by CMake build" >&2
        exit 1
    fi
    cp "$ELF" "$OUTPUT_DIR/contestant.elf"

elif [[ -f "Makefile" ]] || [[ -f "makefile" ]]; then
    echo "[Builder] Detected Makefile build system"
    make CXX="$COMPILER" CXXFLAGS="$CXX_FLAGS" LDFLAGS="-static" -j"$(nproc)" 2>&1

    ELF=$(find . -maxdepth 1 -type f -executable -exec file {} \; | grep 'ELF' | head -1 | cut -d: -f1)
    if [[ -z "$ELF" ]]; then
        echo "[Builder] ERROR: No ELF binary produced by make" >&2
        exit 1
    fi
    cp "$ELF" "$OUTPUT_DIR/contestant.elf"

else
    echo "[Builder] No build system detected, compiling all .cpp files"
    # Find all .cpp/.cc files and compile them together
    SOURCES=$(find . -name '*.cpp' -o -name '*.cc' -o -name '*.cxx' | head -50)
    if [[ -z "$SOURCES" ]]; then
        echo "[Builder] ERROR: No C++ source files found" >&2
        exit 1
    fi

    echo "[Builder] Compiling: $SOURCES"
    # shellcheck disable=SC2086
    $COMPILER $CXX_FLAGS $SOURCES -o "$OUTPUT_DIR/contestant.elf" 2>&1
fi

# ── Verify output ───────────────────────────────────────────
if [[ ! -f "$OUTPUT_DIR/contestant.elf" ]]; then
    echo "[Builder] ERROR: contestant.elf not produced" >&2
    exit 1
fi

chmod +x "$OUTPUT_DIR/contestant.elf"

# ── Check it's statically linked ────────────────────────────
FILE_INFO=$(file "$OUTPUT_DIR/contestant.elf")
echo "[Builder] Output: $FILE_INFO"

LDD_CHECK=$(ldd "$OUTPUT_DIR/contestant.elf" 2>&1 || true)
if echo "$LDD_CHECK" | grep -q "not a dynamic executable\|statically linked"; then
    echo "[Builder] ✓ Binary is statically linked"
    STATIC="true"
else
    echo "[Builder] ⚠ Binary appears dynamically linked (may still work in microVM)"
    STATIC="false"
fi

# ── Compute hash ────────────────────────────────────────────
ELF_HASH=$(sha256sum "$OUTPUT_DIR/contestant.elf" | awk '{print $1}')

# ── Record metadata (§7 stage 2 requirement) ───────────────
cat > "$OUTPUT_DIR/run_metadata.json" <<EOF
{
    "compiler": "$COMPILER",
    "compiler_version": "$COMPILER_VERSION",
    "cxx_flags": "$CXX_FLAGS",
    "static_linked": $STATIC,
    "elf_hash": "$ELF_HASH",
    "elf_size_bytes": $(stat -c%s "$OUTPUT_DIR/contestant.elf"),
    "build_timestamp": "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
}
EOF

echo "[Builder] Build complete."
echo "[Builder] ELF hash: $ELF_HASH"
echo "[Builder] Metadata: $OUTPUT_DIR/run_metadata.json"
