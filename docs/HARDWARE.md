# Hardware evidence

## User-confirmed baseline — 2026-10-05

PS5 Slim Digital, firmware 13.60, kstuff and ShadowMount active, without etaHEN.
Title `PPSA99052`, uploaded as an extracted folder under `/data/homebrew/`.

The user confirmed responsive rendering, D-pad focus, Cross confirmation,
counter state updates, and Options returning to the system home screen.
The proof of concept intentionally exits after 30 seconds. FPS was not measured.

Confirmed executable SHA-256:
`b4d154838f1df886288ffd938e57336caf07158bdcd82d0f1e2622d098db3339`.

The debug PKG `PPSA99051` failed to launch with CE-100022-5. This alone does
not identify the cause; the same executable worked through the folder workflow.

## Framework starter

`PPSA99053` is a new identity and build with extracted configuration and APIs.
It does not automatically inherit the proof of concept's hardware status.
The build receipt sets `hardware_tested: false` pending a test of that artifact.

Validation procedure: launch PS5 React Starter, move focus, update the counter,
return, exit with Options, and verify the configured timeout. Changes to the
host or SDK need new validation. Desktop tests and ELF checks do not establish
PS5 execution or support for other firmware/loader combinations.
