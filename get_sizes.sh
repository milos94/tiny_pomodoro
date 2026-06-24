#!/usr/bin/env bash
# get_sizes.sh — print file sizes and disk usage for all pomodoro binaries
# Run from the project root: ./get_sizes.sh

set -euo pipefail

BUILDS=(debug release debugasan debugubsan tiny)
BINS=(tiny_pomodoro tiny_pomodoro_pro tiny_pomodoro_pro_plus tiny_pomodoro_pro_plus_ultra tiny_pomodoro_2)
BUILD_DIR="$(dirname "$0")/build"

# Column widths
COL_BUILD=12
COL_SIZE=12
COL_DISK=12

pad_right() {
    local s="$1" w="$2"
    printf "%-${w}s" "$s"
}

print_sep() {
    printf '+%s+%s+%s+\n' \
        "$(printf -- '-%.0s' $(seq 1 $((COL_BUILD + 2))))" \
        "$(printf -- '-%.0s' $(seq 1 $((COL_SIZE + 2))))" \
        "$(printf -- '-%.0s' $(seq 1 $((COL_DISK + 2))))"
}

for bin in "${BINS[@]}"; do
    echo
    echo "=== $bin ==="
    print_sep
    printf '| %-*s | %-*s | %-*s |\n' \
        "$COL_BUILD" "Build" \
        "$COL_SIZE"  "File size" \
        "$COL_DISK"  "Disk usage"
    print_sep

    for build in "${BUILDS[@]}"; do
        path="$BUILD_DIR/$build/$bin"
        if [[ -f "$path" ]]; then
            # ls -lsh: column 1 = disk blocks (human), column 5 = file size (bytes), use stat for human-readable
            file_size=$(stat --printf="%s" "$path")
            disk_usage=$(du -sh "$path" | cut -f1)

            # Human-readable file size
            if   (( file_size >= 1073741824 )); then
                hr=$(awk "BEGIN{printf \"%.1fG\", $file_size/1073741824}")
            elif (( file_size >= 1048576 )); then
                hr=$(awk "BEGIN{printf \"%.1fM\", $file_size/1048576}")
            elif (( file_size >= 1024 )); then
                hr=$(awk "BEGIN{printf \"%.1fK\", $file_size/1024}")
            else
                hr="${file_size}B"
            fi

            printf '| %-*s | %-*s | %-*s |\n' \
                "$COL_BUILD" "$build" \
                "$COL_SIZE"  "$hr ($file_size B)" \
                "$COL_DISK"  "$disk_usage"
        else
            printf '| %-*s | %-*s | %-*s |\n' \
                "$COL_BUILD" "$build" \
                "$COL_SIZE"  "(not found)" \
                "$COL_DISK"  "(not found)"
        fi
    done
    print_sep
done
echo
