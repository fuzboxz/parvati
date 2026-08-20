#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# Parvati — Developer-ID signing + notarization + stapling for macOS releases.
#
# Signs every plugin artefact in an artefacts dir (VST3 / AU / CLAP /
# Standalone) with a "Developer ID Application" certificate and the hardened
# runtime, verifies the signatures, submits each bundle to Apple's notary
# service, staples the tickets, and finally builds a distribution .zip
# (via ditto) that includes README.md + NOTICES.md (license compliance).
#
# This script contains NO credentials. Everything is supplied via environment
# variables (or a stored notarytool keychain profile — see tools/release/README.md).
#
# Usage:
#   tools/release/sign_and_notarize.sh [ARTEFACTS_DIR] [OUT_DIR]
#     ARTEFACTS_DIR  default: build/Parvati_artefacts/Release
#     OUT_DIR        default: dist/  (created)
#
# Required environment:
#   DEV_ID                      Developer ID Application identity, e.g.
#                               "Developer ID Application: ACME Inc (ABCD123456)"
#                               (see `security find-identity -v -p codesigning`)
#   and ONE of the notarytool credential sets:
#     PARVATI_NOTARY_PROFILE    name of a stored profile
#                               (`xcrun notarytool store-credentials PROFILE
#                               --apple-id ... --team-id ... --password ...`)
#     or APPLE_ID + APPLE_ID_PASSWORD (app-specific password) + TEAM_ID
#     or ASC_KEY_ID + ASC_ISSUER_ID + ASC_AUTH_KEY_PATH (.p8 App Store
#                               Connect API key — the RECOMMENDED form for CI)
#
# Optional:
#   SKIP_NOTARIZE=1             sign + verify + package only (no Apple upload)
#   STAPLE_README_NOTICES=0     omit README/NOTICES from the dist zip
# ---------------------------------------------------------------------------
set -euo pipefail

die() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }
log() { printf '\n==> %s\n' "$*"; }

ARTEFACTS_DIR="${1:-build/Parvati_artefacts/Release}"
OUT_DIR="${2:-dist}"
ENTITLEMENTS="$(cd "$(dirname "$0")" && pwd)/macos_release.entitlements"

[ -f "$ENTITLEMENTS" ]     || die "entitlements missing: $ENTITLEMENTS"
[ -d "$ARTEFACTS_DIR" ]    || die "artefacts dir not found: $ARTEFACTS_DIR (build first)"

# --- credentials -------------------------------------------------------------
if [ -z "${DEV_ID:-}" ]; then
    die "DEV_ID is not set. List identities with: security find-identity -v -p codesigning"
fi
security find-identity -v -p codesigning | grep -F "$DEV_ID" >/dev/null \
    || die "identity '$DEV_ID' not found in the keychain (security find-identity -v -p codesigning)"

NOTARY_ARGS=()
if [ "${SKIP_NOTARIZE:-0}" != "1" ]; then
    if [ -n "${PARVATI_NOTARY_PROFILE:-}" ]; then
        NOTARY_ARGS=(--keychain-profile "$PARVATI_NOTARY_PROFILE")
    elif [ -n "${ASC_KEY_ID:-}" ] && [ -n "${ASC_ISSUER_ID:-}" ] && [ -n "${ASC_AUTH_KEY_PATH:-}" ]; then
        [ -f "$ASC_AUTH_KEY_PATH" ] || die "ASC_AUTH_KEY_PATH does not exist: $ASC_AUTH_KEY_PATH"
        NOTARY_ARGS=(--key-id "$ASC_KEY_ID" --issuer "$ASC_ISSUER_ID" --key "$ASC_AUTH_KEY_PATH")
    elif [ -n "${APPLE_ID:-}" ] && [ -n "${APPLE_ID_PASSWORD:-}" ] && [ -n "${TEAM_ID:-}" ]; then
        NOTARY_ARGS=(--apple-id "$APPLE_ID" --password "$APPLE_ID_PASSWORD" --team-id "$TEAM_ID")
    else
        die "no notarytool credentials: set PARVATI_NOTARY_PROFILE, or ASC_KEY_ID+ASC_ISSUER_ID+ASC_AUTH_KEY_PATH, or APPLE_ID+APPLE_ID_PASSWORD+TEAM_ID (or SKIP_NOTARIZE=1)"
    fi
fi

# --- collect bundles ---------------------------------------------------------
BUNDLES=()
for rel in "VST3/Parvati.vst3" "AU/Parvati.component" "CLAP/Parvati.clap" "Standalone/Parvati.app"; do
    if [ -d "$ARTEFACTS_DIR/$rel" ]; then
        BUNDLES+=("$ARTEFACTS_DIR/$rel")
    else
        printf 'note: %s not present, skipping\n' "$rel"
    fi
done
[ "${#BUNDLES[@]}" -ge 1 ] || die "no Parvati bundles found under $ARTEFACTS_DIR"

mkdir -p "$OUT_DIR"

# --- sign (inner binaries first, then the bundle) -----------------------------
# codesign --deep is unreliable for correct entitlement propagation, so every
# Mach-O inside the bundle is signed explicitly, then the bundle itself.
sign_bundle() {
    local bundle="$1"
    log "signing $bundle"
    # sign every executable binary inside the bundle (arm64 single-arch today)
    while IFS= read -r -d '' bin; do
        codesign --force --timestamp --options runtime \
                 --entitlements "$ENTITLEMENTS" \
                 --sign "$DEV_ID" "$bin"
    done < <(find "$bundle" -type f -perm -111 -exec file {} + \
             | awk -F: '/Mach-O/ {sub(/:.*/, "", $1); print $1}' \
             | while IFS= read -r f; do printf '%s\0' "$f"; done)
    codesign --force --timestamp --options runtime \
             --entitlements "$ENTITLEMENTS" \
             --sign "$DEV_ID" "$bundle"
    codesign --verify --deep --strict --verbose=2 "$bundle"
}

notarize_bundle() {
    local bundle="$1"
    local name; name="$(basename "$bundle")"
    local zip="$OUT_DIR/${name%.zip}.notary.zip"
    log "notarizing $name"
    rm -f "$zip"
    ditto -c -k --keepParent "$bundle" "$zip"
    xcrun notarytool submit "$zip" --wait "${NOTARY_ARGS[@]}"
    xcrun stapler staple "$bundle"
    # verify the staple actually landed
    xcrun stapler validate "$bundle"
    rm -f "$zip"
}

for b in "${BUNDLES[@]}"; do
    sign_bundle "$b"
    if [ "${SKIP_NOTARIZE:-0}" != "1" ]; then
        notarize_bundle "$b"
    fi
done

# --- distribution zip ---------------------------------------------------------
log "building distribution archive"
STAGE="$OUT_DIR/Parvati-macOS"
rm -rf "$STAGE"
mkdir -p "$STAGE"
for b in "${BUNDLES[@]}"; do
    ditto "$b" "$STAGE/$(basename "$b")"      # ditto preserves signing + metadata
done
if [ "${STAPLE_README_NOTICES:-1}" = "1" ]; then
    cp README.md NOTICES.md "$STAGE/"
fi
DIST_ZIP="$OUT_DIR/Parvati-macOS.zip"
rm -f "$DIST_ZIP"
ditto -c -k --keepParent --sequesterRsrc "$STAGE" "$DIST_ZIP"
rm -rf "$STAGE"

log "assessing Gatekeeper verdict (unsigned-network sim)"
if command -v spctl >/dev/null 2>&1; then
    spctl --assess --type execute -vv "$DIST_ZIP" || true   # zips are not directly assessable; per-bundle check:
    for b in "${BUNDLES[@]}"; do
        spctl --assess -vv "$b" || die "spctl rejected $b — notarization/stapling incomplete"
    done
fi

log "DONE"
printf '  bundles signed%s and distribution written to: %s\n' \
       "$([ "${SKIP_NOTARIZE:-0}" = "1" ] && echo ' (NOT notarized)' || echo ' + notarized + stapled')" \
       "$DIST_ZIP"
