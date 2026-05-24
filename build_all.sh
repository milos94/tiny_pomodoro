#!/usr/bin/env bash
set -euo pipefail

BUILD_TYPES=(Release Debug DebugASAN DebugUBSAN Tiny)
ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
JOBS=$(nproc)

build_type() {
    local type="$1"
    local lower
    lower=$(echo "$type" | tr '[:upper:]' '[:lower:]')
    local build_dir="${ROOT_DIR}/build/${lower}"

    echo "[${type}] Configuring..."
    cmake -S "${ROOT_DIR}" -B "${build_dir}" -DCMAKE_BUILD_TYPE="${type}" > "${build_dir}/cmake.log" 2>&1

    echo "[${type}] Building..."
    cmake --build "${build_dir}" --parallel "${JOBS}" > "${build_dir}/build.log" 2>&1

    echo "[${type}] Done -> build/${lower}/"
}

export -f build_type

# Create build dirs upfront to avoid races
for type in "${BUILD_TYPES[@]}"; do
    lower=$(echo "$type" | tr '[:upper:]' '[:lower:]')
    mkdir -p "${ROOT_DIR}/build/${lower}"
done

# Run all builds in parallel
pids=()
for type in "${BUILD_TYPES[@]}"; do
    build_type "$type" &
    pids+=($!)
done

# Wait and collect exit codes
failed=0
for i in "${!pids[@]}"; do
    if ! wait "${pids[$i]}"; then
        echo "[${BUILD_TYPES[$i]}] FAILED (see build/${BUILD_TYPES[$i],,}/build.log)"
        failed=1
    fi
done

# Copy beep.wav into each build/<type>/sounds/ directory
if [[ -f "${ROOT_DIR}/assets/beep.wav" ]]; then
    for type in "${BUILD_TYPES[@]}"; do
        lower=$(echo "$type" | tr '[:upper:]' '[:lower:]')
        sounds_dir="${ROOT_DIR}/build/${lower}/sounds"
        mkdir -p "${sounds_dir}"
        cp "${ROOT_DIR}/assets/beep.wav" "${sounds_dir}/beep.wav"
    done
    echo "Copied beep.wav to all build/<type>/sounds/ directories."
else
    echo "Warning: assets/beep.wav not found in ${ROOT_DIR}, skipping sound copy."
fi

exit $failed
