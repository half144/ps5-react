# Architecture

## Responsibilities

```text
apps/<app>/index.jsx                 application, state, components
            ↓
runtime/js (@ps5-react/core)         focus navigation, controller API, native modules, primitives
            ↓ React reconciler / NativeUI / QuickJS
Embedded React C                    tree, layout, text, rasterization
            ↓ opaque ARGB8888 framebuffer
native/shared/GlPresenter           changed rows into buffer textures (1:1) or a texture, presentation
            ↓
native/desktop | native/ps5          window/EGL, input, clock, lifecycle,
                                     native modules via native/shared/host_api
```

Applications do not access scePad, EGL, libkernel, or the SDK directly. They
reach platform services only through the `@ps5-react/core` native modules
(`runtime/js/native.js`), which wrap `globalThis.__ps5ReactNative`. Its shape is
fixed by `native/shared/host_api.hpp`: `native/shared/host_api.cpp` installs it
and implements the filesystem with POSIX, while `native/ps5/` and
`native/desktop/` implement the platform functions. Calls are synchronous on the
render thread. Networking calls submit/cancel/poll bounded native tasks; two
workers perform network and disk I/O without touching QuickJS or the engine.
Hosts cancel and join those workers before runtime shutdown. See
[NETWORKING.md](NETWORKING.md) for the buffer, heap, stack and queue contract,
and [NATIVE-API.md](NATIVE-API.md) for the bridge. When the surface matches the
render size (the PS5), the presenter copies changed framebuffer rows into two
buffer textures (the PS5 allows 1048576 texels per buffer texture) that its
shader reads with `texelFetch`: there `glBufferSubData` of 1920×540 takes about
0.46 ms against 14.9 ms for `glTexSubImage2D`. Other sizes scale a 2D texture.
The presenter
has no knowledge of React state, fonts, or widgets. The engine and JavaScript
run on the same render thread. The PS5 host keeps the heap, shims, CRT, SDK
pair, and FSELF path used by the hardware-tested proof of concept.

`className` and `tw` styling is compiled by `tools/tailwind` during bundling into
literal style objects; class names and CSS never reach QuickJS or the engine.
See [TAILWIND.md](TAILWIND.md).

Hosts send directional actions (`up`, `down`, `left`, `right`, `confirm`,
`back`) through `globalThis.__ps5ReactDispatch`. `runtime/js/focus/` moves focus
between focusable elements from their laid-out rectangles, in JavaScript on the
render thread; hosts know nothing about focus. See [NAVIGATION.md](NAVIGATION.md).

Only execution infrastructure and InputTracker are used from ps5-homebrew-ui.
Its UI components, themes, and UI renderer are not used.

| Directory | Responsibility |
| --- | --- |
| `apps/` | Independent JSX entry points, manifests, and assets |
| `runtime/js/` | Public primitives, focus navigation, native modules, and host input contract |
| `native/` | Shared presenter and desktop/PS5 hosts |
| `tools/` | Bootstrap, bundle, `className` compiler, assets, builds, verification, CLI |
| `docs/` | Decisions, evidence, limitations, provenance |

Generated files live in `.deps/`, `.build/<app>/`, and `dist/<TITLE_ID>/`.
A process lock serializes builds because dependency tooling is shared.

## Reproducibility

1. Pin Embedded React, platform, and QuickJS revisions; use `npm ci`.
2. Verify SDK, OpenGL, compiler-rt archive, and graphics manifest SHA-256 hashes.
3. Use the same engine profile for desktop and PS5, propagating C definitions.
4. Record source, bundle, executable, and toolchain hashes; verify ELF/SELF/ZIP.
5. Validate each output on hardware and record the firmware/loader combination.

The default SDK is v0.42 from a pinned URL and hash. Cross-compilation uses
Clang/LLD 18. Host utilities use the macOS compiler and system zlib. This is
functional reproducibility with recorded dependencies, not a guarantee of
identical output across operating systems. A future release pipeline should
also pin the CI image, Python, Node, CMake, and host compiler.

## Developer experience

`npm run dev` watches JSX, assets, and configuration, rebuilding and reopening
the macOS preview. State resets; this is not Fast Refresh. Build errors remain
visible and another save retries. The watcher never connects to the console.

`npm run build` produces an application folder and ZIP. JSON controls identity,
version, resolution, and timeout. `npm run create` adds an app without copying
C++. Configuration errors are reported before dependency downloads, and each
build stage retains its full log.

## Future work

- Hardware validation of focus navigation, including modal, reconnect, and repeat tests.
- Restart only the JS runtime while preserving the desktop graphics context.
- Measure PS5 frame time and memory, then consider damaged-region uploads.
- Extend native networking as measured on hardware; add audio, text input, save data,
  caching, and virtualization behind explicit APIs.
- Ship an installable CLI and a pinned CI matrix.

The software renderer still owns the pixels. A future GPU backend can preserve
the application API but is not implemented here. No part depends on RNPS,
Sony bundle signature bypasses, or changing the system home screen.
