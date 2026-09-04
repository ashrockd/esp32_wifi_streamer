#!/usr/bin/env bash
#
# Runs INSIDE the espressif/idf:v5.5.5 container (see ../workflows/build.yml,
# which invokes this with `--entrypoint bash`, bypassing that image's own
# entrypoint.sh so this script has full, explicit control over environment
# setup instead of relying on unverified entrypoint behavior - see
# build.yml's comment on that decision).
#
# Reproduces, in order, what a human runs locally via
# esp32_wifi_streamer/build.ps1, minus the interactive/flash-specific parts:
#   1. activate ESP-IDF (build.ps1: ". $IdfProfile")
#   2. activate ESP-ADF (build.ps1: ". $AdfExport")
#   3. force a clean sdkconfig regen from sdkconfig.defaults
#   4. idf.py set-target esp32s3
#   5. idf.py build
#
# The one deliberate departure from build.ps1's literal steps: this script
# applies esp32_wifi_streamer/.github/ci-patches/ to the freshly-checked-out
# esp-adf/esp-idf trees BEFORE step 1/2 run, because those checkouts start
# clean (no local edits) every single CI run, unlike the dev machine's
# checkouts, which already carry the edits permanently. See
# .github/ci-patches/README.md for exactly what each patch does and why.

set -euo pipefail

WORKSPACE="/workspace"
PROJECT_DIR="$WORKSPACE/esp32_wifi_streamer"
ADF_DIR="$WORKSPACE/esp-adf"
PATCH_DIR="$PROJECT_DIR/.github/ci-patches"

echo "::group::Versions"
echo "Container IDF_PATH (baked-in, image-provided): ${IDF_PATH:-<unset>}"
git -C "$IDF_PATH" describe --tags --always 2>&1 || echo "(esp-idf tree inside the image has no .git metadata - expected for some image variants; version is still pinned by the image tag itself, espressif/idf:v5.5.5)"
git -C "$ADF_DIR" rev-parse HEAD
git -C "$ADF_DIR" describe --tags --always
echo "::endgroup::"

# --- Step 0: apply this project's captured local patches -------------------
#
# WHY THIS RUNS FIRST, before anything else touches these trees: every patch
# below is either (a) required for esp-adf/esp-idf to even COMPILE against
# this exact IDF version (the ADC channel enum fix, the audio_pipeline
# -Wno-error=return-type fix, the FreeRTOS xTaskCreateRestrictedPinnedToCore
# addition ESP-ADF's own audio_thread.c calls), or (b) required for the
# firmware to behave CORRECTLY rather than just compile (most critically:
# the M5Stack AtomS3R I2S pin override - see below - and the esp_audio_codec
# exposure this project's AAC decode path needs). An "it built successfully"
# green checkmark on an UNPATCHED esp-adf checkout would be actively
# misleading for this project - see .github/ci-patches/README.md's patch #1
# for the specific, hardware-confirmed failure mode (wrong I2S GPIOs driven,
# no compile error, no runtime error).
echo "::group::Applying esp-adf local patches"
for p in "$PATCH_DIR"/esp-adf/*.patch; do
    echo "Applying $(basename "$p")"
    git -C "$ADF_DIR" apply --whitespace=nowarn "$p"
done
echo "::endgroup::"

echo "::group::Applying esp-adf-libs (submodule) local patch"
git -C "$ADF_DIR/components/esp-adf-libs" apply --whitespace=nowarn \
    "$PATCH_DIR/esp-adf-libs/0001-cmakelists-expose-esp_audio_codec.patch"
echo "::endgroup::"

# The freertos patch targets the CONTAINER'S OWN baked-in /opt/esp/idf
# (= $IDF_PATH), not a separately-checked-out esp-idf tree - see
# ci-patches/README.md "Which ESP-IDF actually matters" for why no separate
# esp-idf checkout exists in this workflow at all: the real local build
# activates a standalone v5.5.5 IDF install BEFORE esp-adf/export.ps1 runs,
# so esp-adf's own bundled (older, v5.5.3) esp-idf submodule is never used,
# and this image's baked-in IDF is exactly v5.5.5 - the same version, so a
# diff captured against the real dev-machine's v5.5.5 checkout applies
# cleanly here without ever needing to fetch a second copy of esp-idf.
#
# Applied directly (NOT via esp-adf's own tools/adf_install_patches.py
# apply-patch, even though esp-adf/export.sh will also try that automatically
# a few lines down) because that script's git-apply call has no error
# checking at all - see ci-patches/esp-idf/0001-*.patch's own header comment
# for the full story (this project independently hit exactly that silent
# failure on the dev machine, and there's no reason to expect the identical
# IDF version + identical patch file to behave differently here). The grep
# immediately after is this workflow's replacement for that missing check.
echo "::group::Applying esp-idf (container-baked v5.5.5) FreeRTOS patch"
git -C "$IDF_PATH" apply --whitespace=nowarn \
    "$PATCH_DIR/esp-idf/0001-freertos-xTaskCreateRestrictedPinnedToCore.patch"

if ! grep -q "xTaskCreateRestrictedPinnedToCore" \
        "$IDF_PATH/components/freertos/esp_additions/include/freertos/idf_additions.h"; then
    echo "::error::xTaskCreateRestrictedPinnedToCore is missing from esp-idf's" \
         "idf_additions.h after applying the FreeRTOS patch. ESP-ADF's" \
         "audio_sal/audio_thread.c calls this function - continuing would" \
         "produce a confusing 'undefined reference' LINK error far from this" \
         "root cause, so failing fast here instead. See" \
         ".github/ci-patches/esp-idf/0001-freertos-xTaskCreateRestrictedPinnedToCore.patch's" \
         "header comment for the full story."
    exit 1
fi
echo "FreeRTOS patch verified present."
echo "::endgroup::"

# --- Step 1+2: activate ESP-IDF, then ESP-ADF (build.ps1's own order) ------
#
# MINIMAL_BUILD=1, set BEFORE either activation step, exactly where build.ps1
# sets it (before its ". $IdfProfile" line) - esp32_wifi_streamer's top-level
# CMakeLists.txt reads this env var and calls
# idf_build_set_property(MINIMAL_BUILD ON) at the correct point (after
# project.cmake is included, before project() triggers component resolution -
# see that file's own comment, and the project memory on MINIMAL_BUILD
# previously being dead code before that wiring was fixed). Restricts CMake's
# component discovery to main's actual REQUIRES closure instead of scanning
# esp-adf's entire components/ tree (esp-sr, dueros_service, clouds, esp_coze,
# every OTHER board's driver, ...) - main/CMakeLists.txt already lists a
# complete explicit REQUIRES, so this does not change what gets built, only
# how much irrelevant tree CMake's configure step has to look at first. Purely
# a speed optimization in principle, but skipping it here would still be a
# real, unnecessary deviation from what build.ps1 actually does - and a slower
# configure step is a real CI-minutes cost against a ~4.7GB image pull already
# eating into the job.
export MINIMAL_BUILD=1
echo "MINIMAL_BUILD=$MINIMAL_BUILD"

# IDF_PATH is already set by the image (espressif/idf:v5.5.5 sets
# IDF_PATH=/opt/esp/idf) - sourcing its own export.sh here is what build.ps1's
# ". $IdfProfile" step accomplishes locally (activating the python venv,
# putting idf.py/xtensa-esp32s3-elf-gcc/etc. on PATH).
echo "::group::Activating ESP-IDF"
# shellcheck disable=SC1091
source "$IDF_PATH/export.sh"
echo "::endgroup::"

# ADF_PATH must be set before esp-adf's export.sh AND before the project's own
# top-level CMakeLists.txt (which FATAL_ERRORs if ADF_PATH is unset - see
# esp32_wifi_streamer/CMakeLists.txt).
export ADF_PATH="$ADF_DIR"
echo "::group::Activating ESP-ADF"
# shellcheck disable=SC1091
#
# This ALSO runs tools/adf_install_patches.py apply-patch internally (the
# last lines of esp-adf/export.sh) - harmless here: it will try to git-apply
# esp-adf's own idf_v5.5_freertos.patch on top of a tree that already has our
# equivalent hand-verified patch applied above, which will simply fail to
# match (nothing left to patch) and get silently ignored by that script's own
# (unchecked) subprocess.run() call - see the patch-application step above,
# which already verified success before this ever runs.
source "$ADF_DIR/export.sh"
echo "::endgroup::"

if ! command -v idf.py >/dev/null 2>&1; then
    echo "::error::idf.py not on PATH after activation - environment setup failed."
    exit 1
fi

# --- Step 3: force a clean sdkconfig regen, exactly like build.ps1 ---------
#
# sdkconfig IS committed to this repo (not in .gitignore - verified), but
# sdkconfig.defaults changes are only picked up by idf.py when sdkconfig does
# not already exist yet, so build.ps1 always deletes it first to force a
# fresh regeneration. Skipping this in CI would silently build against
# whatever sdkconfig happened to be committed rather than proving
# sdkconfig.defaults on its own actually produces a working config - the
# same reasoning build.ps1's own comment gives.
cd "$PROJECT_DIR"
echo "::group::Regenerating sdkconfig from sdkconfig.defaults"
rm -f sdkconfig
echo "::endgroup::"

echo "::group::idf.py set-target esp32s3"
idf.py set-target esp32s3
echo "::endgroup::"

echo "::group::idf.py build"
idf.py build
echo "::endgroup::"

echo "Build succeeded."
