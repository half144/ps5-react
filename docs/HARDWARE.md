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

## System Explorer storage crash — 2026-10-05

The user reported a crash after the storage-query change. PS5Upload's read-only
syslog and kernel-log endpoints captured two `PPSA99056` executions (PIDs 194
and 197). Both initialized graphics, QuickJS, the bundle, and presented a frame.
Both then logged `native fs.readDir`, `native fs.diskUsage`, followed by:

```text
PPRBUG-22859: the process pid=197 directly issued a syscall 396 at 0x400b37
exception: 0xa002030a (SYSTEM_ILLEGAL_FUNCTION_CALL)
```

This establishes that the direct `statfs` syscall caused this app termination.
The raw `getfsstat` call was not reached; it has also been removed rather than
repeating the same unsupported route. No system-UI modification was involved.

The replacement calls the native-title `_fstatfs` export through an opened
descriptor, following ProsperoStore's storage probe. Mount discovery uses bounded
queries of accessible paths. Directory parsing now rejects malformed records.
These changes compile and pass local tests but have not been hardware-validated.
Existing rendering evidence does not establish storage support.

The user subsequently reported `Operation not permitted` from the native-wrapper
build (`df14dd51f9fcc03c3da2e97c9839d93d8448978bd0e302c5fae1a1118ea366a5`).
The new System Explorer packages an opt-in upstream Lapy exact-title helper and
requests filesystem access once at startup. Localhost ELF loader port 9021 was
reachable during investigation. This is a transport check, not an elevation or
storage-success test. The new integration remains hardware-unvalidated.

The user tested the helper build and still reported `EPERM`. Captured syslog
shows `filesystem access status=9 path=helper`: a transport failure, not a
successful elevation followed by a storage error. The bootstrap was missing
libSceNet initialization and a network pool. The replacement initializes them
before the client request and logs socket creation, socket options, connection,
and send/receive failures with libSceNet's own error value. It still needs a
console test; network initialization is a source-confirmed missing step, not a
hardware-confirmed explanation of every transport failure.

The network-initialized artifact (`eboot.bin` SHA-256
`21581994b1143175288abe71838554480c8a7f8d0535ca185a0291500a85d9b8`)
connected to the ELF loader successfully. Its helper (SHA-256
`0117526a829bc55ead25a5c6633775e7aee4798aca738370d07dd2abbf08e313`)
crashed with SIGILL at SDK v0.40 startup/termination address `0x200006a38`.
The client received EOF and access status 9; the user still reported EPERM.
This confirms transport worked but filesystem elevation did not complete.
The SDK v0.42 helper replacement is pending hardware validation.

The replacement helper SHA-256 is
`2df1a7bcee26b183aa082081f47fd39a91e7039b9f420bd0eedc1ffaeda0d56b`.
The application executable is unchanged; identify this test by the helper hash.
Local compilation, exact-title protocol tests and ELF/SELF/package checks pass.
A native mock test using the actual patched credential functions confirms full
32-byte save/restore, retention of bytes 1–31, detection of high-byte mismatch
and rejection of failed attribute reads. These do not establish console access.

## System Explorer filesystem success — 2026-10-05

The user confirmed the replacement worked on the same PS5 Slim Digital,
firmware 13.60, with kstuff and ShadowMount, without etaHEN. This confirmation
applies to the native filesystem integration being tested: directory listing,
mount discovery and disk usage after the exact-title helper startup.

Tested title: `PPSA99056`, uploaded as a complete extracted application folder.
Application SHA-256:
`21581994b1143175288abe71838554480c8a7f8d0535ca185a0291500a85d9b8`.
Helper SHA-256:
`2df1a7bcee26b183aa082081f47fd39a91e7039b9f420bd0eedc1ffaeda0d56b`.
Both application and helper use payload SDK v0.42. The helper includes the
pinned 32-byte credential-attributes compatibility patch.

Evidence is the user's direct confirmation, not a captured successful syscall
trace or a measurement of every mount. It does not qualify unrelated native
modules, every filesystem operation, other firmware, or later rebuilt hashes.
Generated build receipts continue to default to `hardware_tested: false`; use
this record to identify the qualified artifacts.

## Remote images — 2026-10-06

Overdrive (`PPSA99058`, game-store `65a3e58`) on the same PS5 Slim Digital,
firmware 13.60, kstuff, console filesystem elevation through the bundled
helper. A development build of ps5-react `b6047eb` with an uncommitted
per-frame motion log, screenshot command and heap log loaded every cover,
hero, logo and screenshot from the Steam and Spectrum CDNs; console
screenshots show them on browse and game pages.

- Held-key scroll on browse (14 down, 14 up) while covers streamed in: 225
  motion frames, none motionless, longest frame 17.7 ms, none over 20 ms.
- One-second windows: 59.9–60.0 fps, p95 17.3 ms. Game page mount 25 ms
  (cached art); user-driven mounts of uncached pages 43–88 ms, all JavaScript.
- 128 MiB heap: live 64–71 MiB on browse, about 80 MiB with a game page, peak
  98 MiB, no allocation failures.

The release artifact without diagnostics (eboot SHA-256
`4592332f53eaa597116faee4ecf98d1edb3d512a896b9cd426c4c35a7d0c7606`) was
installed and launched: first frame and 60 fps windows, not re-measured in
detail. Downloads were not exercised.
