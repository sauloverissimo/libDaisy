#!/usr/bin/env bash
# sync-midi2.sh - re-vendor the midi2 C99 sources into src/hid/midi2/.
#
# midi2 lives upstream at https://github.com/sauloverissimo/midi2 and is
# vendored in-tree so libDaisy users build against a pinned snapshot
# without an extra fetch step. Run this script whenever a new midi2
# release is tagged and the fork should track it.
#
# Default: pulls from ../midi2/src/ (sibling checkout at the desired tag).
# Override with: MIDI2_SRC=/path/to/midi2/src ./scripts/sync-midi2.sh

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="${REPO_ROOT}/src/hid/midi2"
MIDI2_SRC="${MIDI2_SRC:-${REPO_ROOT}/../midi2/src}"

if [[ ! -d "${MIDI2_SRC}" ]]; then
    echo "[sync-midi2] midi2 source dir not found at ${MIDI2_SRC}"
    echo "[sync-midi2] either clone midi2 next to libdaisy, or"
    echo "[sync-midi2] set MIDI2_SRC=/path/to/midi2/src and re-run"
    exit 1
fi

echo "[sync-midi2] copying ${MIDI2_SRC} -> ${DEST}"
cp "${MIDI2_SRC}"/midi2_ci.c \
   "${MIDI2_SRC}"/midi2_ci.h \
   "${MIDI2_SRC}"/midi2_ci_dispatch.c \
   "${MIDI2_SRC}"/midi2_ci_dispatch.h \
   "${MIDI2_SRC}"/midi2_ci_msg.h \
   "${MIDI2_SRC}"/midi2_conv.c \
   "${MIDI2_SRC}"/midi2_conv.h \
   "${MIDI2_SRC}"/midi2_dispatch.c \
   "${MIDI2_SRC}"/midi2_dispatch.h \
   "${MIDI2_SRC}"/midi2_msg.h \
   "${MIDI2_SRC}"/midi2_proc.c \
   "${MIDI2_SRC}"/midi2_proc.h \
   "${DEST}/"

MIDI2_VERSION=$(git -C "${MIDI2_SRC}/.." describe --tags 2>/dev/null || echo "unknown")
MIDI2_SHA=$(git -C "${MIDI2_SRC}/.." rev-parse HEAD 2>/dev/null || echo "unknown")

cat > "${DEST}/MIDI2-VERSION" <<EOF
midi2 sources vendored from https://github.com/sauloverissimo/midi2
version: ${MIDI2_VERSION}
sha: ${MIDI2_SHA}
synced: $(date -u +%Y-%m-%dT%H:%M:%SZ)
EOF

echo "[sync-midi2] vendored ${MIDI2_VERSION} (${MIDI2_SHA:0:8})"
