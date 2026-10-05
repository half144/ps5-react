# Dependencies and licensing

Framework-owned code uses GPL-3.0-or-later with the attribution-preservation
term in `LICENSE-ATTRIBUTION`. Third-party code retains its original license.
No proprietary Sony modules are redistributed.

| Dependency | Purpose and license |
| --- | --- |
| Embedded React | C engine, QuickJS bridge, reconciler, software compositor. Apache-2.0; revision `cf5dfe4fae966fe21a265acc3dd3b054d245f981`. |
| ps5-homebrew-ui | EGL, controller, system, InputTracker, heap, CRT, shims, native tools. GPL-3.0-or-later; revision `4bd942579dd981b3df9c740438489ca6a614ddc0`. |
| ProsperoStore elevation client | Optional cooperative filesystem-access client and pinned helper build. GPL-3.0-or-later; revision `22dca63607cf5f1f95006eda6ffb802607bed699`. |
| Lapy helper | Optional exact-title one-shot helper: mpereiraesaa/PS5-Lapy-JB-Daemon `54a095c0f19161825e845daa760a03b446e654fa`, MIT; protocol LGPL-2.1-or-later, ps5log GPL-3.0-or-later. The helper uses the app SDK v0.42 with `patches/lapy-sdk-042-attributes.patch`; source revision and ps5log hash are pinned in the lock file. |
| ps5-opengl | SDK 1.0.0; GPL and per-component/Mesa licenses. The release includes sources and notices. |
| React / QuickJS-ng | React 18.3.1, reconciler 0.29.2, QuickJS-ng 0.15.0; MIT. npm lockfile and QuickJS commit are pinned. |
| Tailwind CSS palette and scales | v3.4.17 default colors and theme values copied into `tools/tailwind/`; MIT. Build-time data only; the `tailwindcss` package is not installed or used. |
| Inter / LLVM / payload SDK | Inter: SIL OFL. LLVM: Apache-2.0 with exceptions. The public SDK contains separately licensed components. |

Sources: [Embedded React](https://github.com/TheMasterCoder007/embedded-react),
[ps5-homebrew-ui](https://github.com/blackbearreloaded/ps5-homebrew-ui),
[ps5-opengl 1.0.0](https://github.com/blackbearreloaded/ps5-opengl/releases/tag/v1.0.0),
[PS5 Payload SDK](https://github.com/ps5-payload-dev/sdk),
[QuickJS-ng](https://github.com/quickjs-ng/quickjs),
[Tailwind CSS v3.4.17](https://github.com/tailwindlabs/tailwindcss/tree/v3.4.17).

Exact revisions, archive hashes, and compiler-rt package identity are recorded
in `dependencies.lock.json`; each local build also creates a receipt.

`patches/` holds modifications applied to pinned dependencies after checkout;
each patch states what it changes, and the build rejects any other local change.
`embeddedReact-flex-wrap-auto-height.patch` modifies Apache-2.0 engine files and
has been proposed upstream; drop it once the pinned revision contains the fix.
`embeddedReact-scroll-offset.patch` and `embeddedReact-scroll-to.patch`
(Apache-2.0 engine and bridge files, applied to build-tree copies by
`native/ps5/CMakeLists.txt`) expose ScrollView offsets to JavaScript as
`NativeUI.scrollTo` for focus scrolling, and warn when the node pool is full.
`embeddedReact-view-gradient.patch` (Apache-2.0 engine and bridge files) adds
the `backgroundGradient` View style with dithered, damage-clipped rasterization.

`tools/bundle.mjs` adapts upstream Apache-2.0 tooling and preserves its notice.
This file is not covered by the framework's additional attribution term.
`tools/tailwind/colors.mjs` reproduces the Tailwind CSS palette unchanged under
its MIT notice in `licenses/tailwindcss-LICENSE`.
The Inter font and bundled third-party license texts retain their own terms.

`LICENSE`, `LICENSE-ATTRIBUTION`, `NOTICE`, `licenses/`, and generated
`dist/<TITLE_ID>/notices/` preserve credits and licensing information.

## Initial public release

Version 0.1.0 is a **source release**. It does not distribute prebuilt PS5
executables or SDK libraries. The source archive and pinned build instructions
are available from the repository and GitHub release.

Before redistributing compiled applications, assemble the complete corresponding
source and required notices for all linked GPL components, plus notices for
LLVM and other statically linked libraries. The application's generated notices
are useful build output, not a substitute for this source-distribution step.

The Lapy SDK compatibility patch should be proposed upstream once hardware
qualification confirms this integration. It preserves all 32 attribute bytes
required by the current SDK. No upstream contribution has been submitted.
