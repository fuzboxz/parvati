#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# tools/check_reference_integrity.sh — firmware-oracle integrity pin.
# ---------------------------------------------------------------------------
# ambika_reference/ is the READ-ONLY vendored Ambika firmware tree
# (GPL-3.0, deliberately untracked; see NOTICES.md). firmware_parity_test
# compiles nine of its sources and asserts byte-for-byte parity. The tree
# is the ORACLE: its value ends at the first unverified edit.
#
# This script turns "do not edit" into an enforced invariant. It hashes
# every CONTENT file under ambika_reference/ and compares the result with
# the pinned manifest tools/reference_integrity.sha256.
#
# Excluded from the pin (never firmware content):
#   .git/       VCS metadata; any git command inside the tree rewrites it.
#   .DS_Store   macOS Finder noise.
#   ._*         macOS AppleDouble noise.
#
# Usage:
#   tools/check_reference_integrity.sh              # verify (default)
#   PARVATI_PIN=1 tools/check_reference_integrity.sh
#          # re-pin the manifest from the current tree. The manifest must
#          # already exist (first-time pinning is deliberate):
#          #   PARVATI_PIN_FORCE=1 tools/check_reference_integrity.sh
#
# Re-pin ONLY for an intended upstream sync. Re-pinning after an accidental
# edit destroys the parity oracle: the edit becomes the new "truth".
#
# Exit codes: 0 = verified · 1 = mismatch · 2 = environment error.
# ---------------------------------------------------------------------------
set -euo pipefail

# Deterministic ordering and hash letter case on every platform.
export LC_ALL=C

SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REF="$SRC/ambika_reference"
MANIFEST="$SRC/tools/reference_integrity.sha256"

# --- hash tool: shasum (macOS) or sha256sum (Linux) ------------------------
if command -v shasum > /dev/null 2>&1; then
    hash_of() { shasum -a 256 "$1" | cut -d' ' -f 1; }
elif command -v sha256sum > /dev/null 2>&1; then
    hash_of() { sha256sum "$1" | cut -d' ' -f 1; }
else
    echo "check_reference_integrity: no sha256 tool found." >&2
    echo "  Install one: shasum (macOS) or sha256sum (Linux coreutils)." >&2
    exit 2
fi

if [ ! -d "$REF" ]; then
    echo "check_reference_integrity: $REF does not exist." >&2
    echo "  The configure step hard-fails without it. See NOTICES.md." >&2
    exit 2
fi

# --- temp files (removed on every exit path) --------------------------------
TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/parvati_ref_integrity.XXXXXX")"
cleanup() { rm -rf "$TMP_DIR"; }
trap cleanup EXIT

# --- stable content list: NUL-delimited, path-sorted ------------------------
# find prints the tree in directory order; sort -z gives the stable order
# both pin and verify modes share. The same find runs in both modes, so the
# exclusion set can never drift between them.
TMP_LIST="$TMP_DIR/list"
find "$REF" -type f \
    ! -path "$REF/.git/*" \
    ! -name '.DS_Store' \
    ! -name '._*' \
    -print0 | sort -z > "$TMP_LIST"

# --- current state: one "<sha256>  <repo-relative path>" line per file ------
TMP_CURRENT="$TMP_DIR/current"
count=0
total_bytes=0
: > "$TMP_CURRENT"
while IFS= read -r -d '' f; do
    h="$(hash_of "$f")"
    bytes="$(wc -c < "$f" | tr -d ' ')"
    rel="${f#"$SRC"/}"
    printf '%s  %s\n' "$h" "$rel" >> "$TMP_CURRENT"
    count=$((count + 1))
    total_bytes=$((total_bytes + bytes))
done < "$TMP_LIST"

# ===========================================================================
# PIN MODE — regenerate the manifest from the current tree.
# ===========================================================================
if [ "${PARVATI_PIN:-0}" = "1" ]; then
    if [ ! -f "$MANIFEST" ]; then
        if [ "${PARVATI_PIN_FORCE:-0}" != "1" ]; then
            echo "check_reference_integrity: pin refused." >&2
            echo "  $MANIFEST does not exist. First-time pinning is a" >&2
            echo "  deliberate act. Create it, then re-run pin mode:" >&2
            echo "    PARVATI_PIN_FORCE=1 tools/check_reference_integrity.sh" >&2
            exit 2
        fi
    fi
    # Provenance line: the upstream commit the checkout pins (empty for a
    # tarball copy without .git).
    upstream="$(git -C "$REF" rev-parse --short HEAD 2> /dev/null || true)"
    provenance="Pinned: $(date +%Y-%m-%d), $count files, $total_bytes bytes."
    if [ -n "$upstream" ]; then
        provenance="$provenance Upstream tree at $upstream."
    fi
    {
        echo "# Parvati firmware-oracle integrity manifest."
        echo "# Pins every content file under ambika_reference/ (upstream"
        echo "# Mutable Instruments Ambika AVR firmware, GPL-3.0; NOTICES.md)."
        echo "# The tree must stay verbatim: firmware_parity_test uses it as"
        echo "# the oracle. Excluded: .git/ (VCS metadata), .DS_Store, ._*"
        echo "# (macOS noise)."
        echo "# $provenance"
        echo "# Re-pin ONLY for an intended upstream sync:"
        echo "#   PARVATI_PIN=1 tools/check_reference_integrity.sh"
        echo "# Re-pinning after an accidental edit destroys the parity oracle."
        cat "$TMP_CURRENT"
    } > "$MANIFEST"
    echo "check_reference_integrity: pinned $count files ($total_bytes bytes)"
    echo "  -> $MANIFEST"
    echo "  Verify the change is an intended upstream sync, not a local edit."
    exit 0
fi

# ===========================================================================
# VERIFY MODE (default).
# ===========================================================================
if [ ! -f "$MANIFEST" ]; then
    echo "check_reference_integrity: $MANIFEST does not exist." >&2
    echo "  First-time pinning is a deliberate act. Run:" >&2
    echo "    PARVATI_PIN_FORCE=1 tools/check_reference_integrity.sh" >&2
    exit 2
fi

# The pinned expectation: manifest minus its comment/blank header lines.
TMP_EXPECTED="$TMP_DIR/expected"
grep -v -e '^#' -e '^[[:space:]]*$' "$MANIFEST" > "$TMP_EXPECTED" || true

if cmp -s "$TMP_EXPECTED" "$TMP_CURRENT"; then
    echo "check_reference_integrity: OK ($count files match the pin)"
    exit 0
fi

# --- classify every difference ---------------------------------------------
# "path<TAB>hash" forms, sorted by path, so join finds equal paths.
to_pathkey() { awk '{ print substr($0, 67) "\t" substr($0, 1, 64) }' | sort; }
to_pathkey < "$TMP_EXPECTED" > "$TMP_DIR/exp.key"
to_pathkey < "$TMP_CURRENT"   > "$TMP_DIR/cur.key"

TAB="$(printf '\t')"
cut -f 1 "$TMP_DIR/exp.key" > "$TMP_DIR/exp.paths"
cut -f 1 "$TMP_DIR/cur.key" > "$TMP_DIR/cur.paths"

mismatched="$(join -t "$TAB" "$TMP_DIR/exp.key" "$TMP_DIR/cur.key" \
    | awk -F '\t' '$2 != $3 { print "    " $1 }')"
missing="$(comm -23 "$TMP_DIR/exp.paths" "$TMP_DIR/cur.paths" \
    | sed 's/^/    /')"
extra="$(comm -13 "$TMP_DIR/exp.paths" "$TMP_DIR/cur.paths" \
    | sed 's/^/    /')"

echo "check_reference_integrity: FAILED — the oracle tree differs from the pin." >&2
[ -n "$mismatched" ] && { echo "  MISMATCH (content changed):" >&2; printf '%s\n' "$mismatched" >&2; }
[ -n "$missing" ]   && { echo "  MISSING (pinned, absent on disk):" >&2; printf '%s\n' "$missing" >&2; }
[ -n "$extra" ]     && { echo "  EXTRA (on disk, absent from the pin):" >&2; printf '%s\n' "$extra" >&2; }
echo "" >&2
echo "The oracle tree must stay verbatim. Find the cause of every change first." >&2
echo "An accidental edit means the firmware parity oracle is compromised." >&2
echo "To re-pin after an INTENDED upstream sync only:" >&2
echo "  PARVATI_PIN=1 tools/check_reference_integrity.sh" >&2
exit 1
