# Add New-Session Tooling Rules

## Summary

Update `AGENTS.md` so every new session reads `docs/development-tooling-setup.md` completely before using ESP-IDF, MCP, Playwright, or connected hardware.

## Changes

- Add a concise session-start section under Environment and Commands.
- Declare `tools/build-board.sh <profile> <build|size|flash|monitor>` the required interface for profile-specific operations.
- Prohibit bypassing its profile, image-descriptor, and approved by-id checks with direct `esptool`, generic `idf.py flash`/`monitor`, port auto-detection, or MCP flashing.
- Retain the existing requirement for explicit approval and a confirmed board/port.
- Keep exact board-to-port mappings authoritative in the tooling guide rather than duplicating them.

## Verification

- Inspect the final `AGENTS.md` diff for clarity and consistency with the tooling guide.
- Run `git diff --check`.
- Skip firmware builds and the production verifier because this is documentation-only.

## Assumptions

- Do not modify the tooling guide, wrapper script, or hardware permissions.
