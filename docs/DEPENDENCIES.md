# Dependencies and licensing

Framework-owned code uses GPL-3.0-or-later with the attribution-preservation
term in `LICENSE-ATTRIBUTION`. Third-party code retains its original license.
No proprietary Sony modules are redistributed.

| Dependency | Purpose and license |
| --- | --- |
| Embedded React | C engine, QuickJS bridge, reconciler, software compositor. Apache-2.0; revision `cf5dfe4fae966fe21a265acc3dd3b054d245f981`. |
| ps5-homebrew-ui | EGL, controller, system, InputTracker, heap, CRT, shims, native tools. GPL-3.0-or-later; revision `4bd942579dd981b3df9c740438489ca6a614ddc0`. |
| ps5-opengl | SDK 1.0.0; GPL and per-component/Mesa licenses. The release includes sources and notices. |
| React / QuickJS-ng | React 18.3.1, reconciler 0.29.2, QuickJS-ng 0.15.0; MIT. npm lockfile and QuickJS commit are pinned. |
| Inter / LLVM / payload SDK | Inter: SIL OFL. LLVM: Apache-2.0 with exceptions. The public SDK contains separately licensed components. |

Sources: [Embedded React](https://github.com/TheMasterCoder007/embedded-react),
[ps5-homebrew-ui](https://github.com/blackbearreloaded/ps5-homebrew-ui),
[ps5-opengl 1.0.0](https://github.com/blackbearreloaded/ps5-opengl/releases/tag/v1.0.0),
[PS5 Payload SDK](https://github.com/ps5-payload-dev/sdk),
[QuickJS-ng](https://github.com/quickjs-ng/quickjs).

Exact revisions, archive hashes, and compiler-rt package identity are recorded
in `dependencies.lock.json`; each local build also creates a receipt.

`tools/bundle.mjs` adapts upstream Apache-2.0 tooling and preserves its notice.
This file is not covered by the framework's additional attribution term.
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
