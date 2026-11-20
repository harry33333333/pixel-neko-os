#!/usr/bin/env bash
set -e

# Version generator according to Apple Build Train specification
# MajorYear: 26 (2026 Darwin 64-bit baseline)
# TrainLetter: A (Alpha / Bring-up)
# BuildNumber: derived from git commit count or build counter

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_NUM_FILE="${SCRIPT_DIR}/build_number.txt"

COUNT=101
if [ -n "${PIXEL_NEKO_BUILD_NUMBER:-}" ]; then
    # Milestone releases pin the build number so every CI rebuild produces the
    # same tag and ISO filename.
    COUNT="${PIXEL_NEKO_BUILD_NUMBER}"
else
    if [ -d "${ROOT_DIR}/.git" ]; then
        GIT_COUNT=$(git -C "${ROOT_DIR}" rev-list --count HEAD 2>/dev/null || echo "")
        if [ -n "$GIT_COUNT" ] && [ "$GIT_COUNT" -gt 0 ]; then
            COUNT=$((100 + GIT_COUNT))
        fi
    fi

    if [ -f "$BUILD_NUM_FILE" ]; then
        SAVED_COUNT=$(cat "$BUILD_NUM_FILE" 2>/dev/null || echo "")
        if [ -n "$SAVED_COUNT" ] && [ "$SAVED_COUNT" -ge "$COUNT" ]; then
            COUNT=$((SAVED_COUNT + 1))
        fi
    fi
fi
echo "$COUNT" > "$BUILD_NUM_FILE"

BUILD_SUFFIX="${PIXEL_NEKO_BUILD_SUFFIX:-b}"
BUILD_ID="26A${COUNT}${BUILD_SUFFIX}"
BUILD_DATE=$(date -u +"%Y-%m-%d %H:%M:%S UTC")

cat <<EOF > "${ROOT_DIR}/include/version.h"
#ifndef _VERSION_H
#define _VERSION_H

#define OS_NAME         "Pixel Neko OS"
#define OS_CODENAME     "Snow Neko"
#define OS_TRAIN        "26A"
#define OS_BUILD        "${BUILD_ID}"
#define OS_VERSION      "2.0-alpha"
#define OS_BUILD_DATE   "${BUILD_DATE}"
#define OS_FULL_STRING  "Pixel Neko OS (Darwin ${BUILD_ID} - Codename Snow Neko)"
#define KERNEL_BUILD    OS_BUILD

#endif /* _VERSION_H */
EOF

echo "Generated include/version.h: ${BUILD_ID}"
