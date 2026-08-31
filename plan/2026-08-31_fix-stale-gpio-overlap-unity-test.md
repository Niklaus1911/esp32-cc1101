# Fix Stale GPIO-Overlap Unity Test

## Summary

Update the FH4R2 Unity board-policy test to match the intentional policy that permits Generic ASK/OOK GPIOs to overlap CC1101 GPIOs.

## Changes

- Change the FH4R2 overlap assertion in `test_apps/unit/main/test_platform_board_component.cpp` from `TEST_ASSERT_FALSE` to `TEST_ASSERT_TRUE`.
- Keep production GPIO validation and runtime behavior unchanged.

## Verification

- Build both ESP32 and ESP32-S3 Unity images with the repository's isolated commands.
- Run native CTest and JavaScript source/asset contract tests.
- Run `git diff --check` and inspect the final diff.
- Do not run the production verifier, flash hardware, or erase NVS for this test-only correction.

## Assumptions

- The overlap-permitted board policy is intentional and remains the source of truth.
- Existing uncommitted changes and other plan files remain untouched.
- On-device Unity execution is not required for this correction.
