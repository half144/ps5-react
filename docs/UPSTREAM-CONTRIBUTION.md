# Potential contributions to Embedded React

## First proposal: software framebuffer presentation with OpenGL

This framework rasterizes the UI through Embedded React's `software` backend
and presents the framebuffer as a fullscreen OpenGL texture. It does not
implement the engine's OpenGL backend; layout and rasterization remain on CPU.

A focused upstream contribution could be a desktop integration example covering
framebuffer upload, channel conversion, orientation, resizing, linear filtering,
and GL resource cleanup. Existing preview tests verify channels, orientation,
and React state-driven updates.

Compare with existing upstream hosts before proposing this example to avoid
duplication. Follow upstream documentation, formatting, and CI requirements,
with one concern per pull request.

## Second proposal: independent-host porting guide

Document the host contract: graphics context, single-threaded QuickJS execution,
heap/stack budgets, NUL-terminated bundles, input dispatch, and lifecycle.
Use PS5 as a case study and link to this external framework.

Hardware evidence is the user-confirmed PPSA99052 proof on firmware 13.60.
The reorganized PPSA99053 starter still needs hardware validation. Do not claim
measured performance, universal firmware support, or historical priority.

## License boundary

The PS5 integration uses GPL infrastructure. Keep that platform code external
to the Apache-2.0 upstream project. Any generic example proposed upstream must
isolate original presenter code and receive an explicitly compatible license
from its copyright holder; third-party GPL code cannot simply be relicensed.

Relevant files: `native/shared/gl_presenter.cpp`, `native/desktop/host.cpp`,
and `runtime/js/index.js`.

Upstream rules reviewed at the pinned revision:
[CONTRIBUTING.md](https://github.com/TheMasterCoder007/embedded-react/blob/cf5dfe4fae966fe21a265acc3dd3b054d245f981/CONTRIBUTING.md).
No upstream issue or pull request has been submitted by this project.
