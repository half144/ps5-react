# Changelog

## Unreleased

- Wrap bold text that is slightly wider than its box: lines broke at the
  regular glyph width, so a bold title a few pixels too wide drew as one line
  cut at the edge instead of continuing on its second line. Engine patch.

- Clip an `Image` to its `borderRadius`: covers drew square corners whatever
  `rounded-*` said. The bitmap now follows the same anti-aliased rounded shape
  as a background; square images draw as before. Fixed by an engine patch.

- Make a remote `<Image>` lighter: one component with one state object, one
  hook to re-render with and one effect, stable layout and load callbacks, and
  the host element rendered directly unless the image is focusable. Mounting
  and loading one takes about a quarter less JavaScript heap.

- Collect JavaScript garbage in idle frames: hosts count the QuickJS heap and
  run its collector half a second after the last input once a third of the
  room before its threshold is garbage, so a collection (40–70 ms on the PS5)
  lands in a pause rather than mid-scroll; the threshold is never raised. Each
  collection is logged as `gc: idle` or `gc: automatic`. `VirtualList` fill
  steps re-render the filling row alone and keep its existing cards, cutting
  the garbage a scroll through a grid leaves by about a third, and focusable elements
  use one effect fewer per render.

- Size a `Text` by the lines it actually wraps into: an auto-height text was one
  line tall (cutting wrapped text to its first line) or exactly `numberOfLines`
  lines whether or not it needed them. It now wraps at the width it gets, at
  most `numberOfLines`, also as a `flex-1` child of a row, which grows to hold
  it. Layouts that relied on `line-clamp-N` reserving N lines need a height.
  Fixed by an engine patch.

- Fix pieces of a sliding element left on screen, such as a header's section
  pill sliding while the screen under it changes: when an opaque element
  covered a repaint elsewhere, a translated element inside a clipping
  container recorded its untranslated position, so its next move erased the
  wrong area. Fixed by an engine patch.

- Add `Power.keepAwake(enabled)` (native API ABI v4) to keep the console out
  of rest mode during long downloads: the PS5 host ticks the system power
  timer every 30 s while it is on, the desktop preview toggles the screen
  saver. Not yet validated on hardware. See docs/NATIVE-API.md.

- Fix a translated element leaving its children behind: a `y` or `x`
  translate without scale or rotation moved only the element's own paint, so a
  header slid past the top edge stayed frozen on screen as its items, and a
  partial lift left a ghost strip. An engine patch moves the subtree and
  repaints the area it leaves. Sliding a bar off screen no longer needs a fade.

- Entering a `ScrollView` from outside it with the D-pad reaches only elements
  it shows at least in part: Right from the last header item no longer jumps
  to a card scrolled out of a rail. Moves inside a scroll view are unchanged.

- Add `scrollAnchor` on `View` and `Pressable`: a vertical `ScrollView` aligns
  the top of the focused element's nearest anchor (a section with its title,
  the hero) past the margin instead of revealing only the element, falling back
  to minimal movement when that would hide it. Add `ScrollView`
  `onScrollTarget({x, y})`, called with each new focus-scroll target. See
  docs/NAVIGATION.md.

- Deliver L1, R1, L2, R2, Triangle and Square as the `l1`, `r1`, `l2`, `r2`,
  `triangle` and `square` actions (host input ABI v3): to `useController`, to
  the new `FocusScope.onAction` handler (innermost first, until one returns
  `true`), and to input scripts. Desktop: Q, E, Z, C, T, F and the
  controller's shoulders, triggers, Y and X. See docs/NAVIGATION.md.

- Add `VirtualList` for long lists and grids inside a `ScrollView`: it mounts
  the rows on screen, around focus and a little ahead of the scroll, filling
  rows ahead an item per frame, with spacers for the rest, `onEndReached` for
  incremental loading and optional row recycling. Lists taller than the engine's
  16-bit layout range re-centre the rows the content represents without moving
  anything on screen. `ScrollView` frames gain change listeners and `shift`.
  See [Long lists](docs/LISTS.md).

- Add the `shot:NAME` script step: test deploys save the frame on screen as
  `dev/NAME.bmp` (preview: the sandbox's `temp0/`), and `tools/ps5_shots.py`
  fetches them through PS5Upload as PNG. See docs/ANIMATION.md.

- Load `<Image source={{uri}}>` from http(s) URLs: native fetch and stb_image
  decode on two workers, resampled to the drawn size (cover/contain/stretch,
  never enlarged) as premultiplied ARGB, a 32 MiB LRU decoded cache, an 8 MiB
  encoded cache shared across sizes, cancellation on unmount and
  `Image.prefetch`. Engine patches add image unloading and size a flow child's
  main axis from a definite cross size and `aspectRatio`. See docs/IMAGES.md.

- Add opt-in receipt-backed recovery of published downloads after app interruption.
  Verify the full SHA-256 and original request/storage/file identity without
  contacting the provider; preserve unowned or damaged existing destinations.
  Expose filesystem device/inode IDs as lossless strings for app storage guards.

- Add split-manifest downloads with source-relative HTTP ranges and direct
  output-offset writes, SHA-1 piece and SHA-256 file verification in one read,
  exact expected sizes, storage-root identity guards, and exclusive publication.
  Export `DownloadFormats` for image, package, archive and binary destinations.
  Package installation and archive extraction remain external operations.

- Add native `Http.request` and `Downloads.enqueue` with validated parallel
  ranges, bounded 8 MiB file buffers, an independent disk writer, cancellation,
  durable range checkpoints and optional SHA-256 verification. PS5 networking
  is opt-in with pinned PacBrew ports and native-title compatibility; hardware
  operation and throughput remain unverified. Add localhost native and JS
  lifecycle coverage to `npm test`.

- Focus scrolling steps once per presented frame, through a new host frame
  callback (`globalThis.__ps5ReactFrame`), in whole pixels: it speeds up over
  four frames, cruises at 40 logical px per frame and brakes to the target.
  It used to step from a 16 ms timer with an exponential ease: on the PS5,
  frames with no timer tick presented no motion (about 45 per 330 scrolling
  frames in five Overdrive runs) and every scroll ended in a crawl of 1-3 px
  steps (about 110 frames). Now there are none of either, and a held key
  scrolls at a constant 60 px per frame.
- The PS5 frame log's engine split (`ERUI_PERF_STATS`) reads the time-stamp
  counter (`sceKernelReadTsc`, about 11 ns) instead of `clock_gettime` (about
  0.88 µs). The engine reads that clock around every blit and bridge call, so a
  full-screen frame paid 10-15 ms for its own statistics: opening an Overdrive
  game page went from a 51 ms to a 36 ms frame and going back from 39 to 20 ms.
  Raster figures logged before this change include that overhead.
- On AVX2 targets (the PS5) the software backend blends eight pixels per step,
  bit-identical to the scalar loops.
- When the surface matches the render size, the presenter keeps the frame in
  two buffer textures (`texelFetch`) and uploads changed rows with
  `glBufferSubData`. On the PS5 that costs about 0.46 ms for 1920×540 rows
  against 14.9 ms through `glTexSubImage2D`; opening an Overdrive game page went
  from an 80 ms to a 53 ms frame, and the gallery's 28 ms image-change frame
  fits in 16.7 ms. Scroll moves need no GPU copy there. Other sizes keep the
  scaled texture.
- A horizontal ScrollView beside a transparent container, such as a section
  header whose box overlaps the rail through a negative margin, scrolls by
  copy again instead of repainting its whole viewport each frame. On the PS5
  the Overdrive genre rail went from 23 ms frames (about 43 fps) for the whole
  scroll to 60 fps.
- The PS5 host writes klog lines from a writer thread. A synchronous
  `sceKernelDebugOutText` on the render thread took 0.1-1 ms per line and
  stalled a frame by about 9 ms one to two times a minute (once by 1.45 s).

- `--app-dir <path>` previews, watches, and builds an app outside `apps/`;
  `create --dir <parent>` scaffolds one. Build receipts record app sources as
  `app/<path>` instead of `apps/<name>/<path>`.

- Build the optional filesystem helper with the app payload SDK v0.42 instead
  of the upstream builder's v0.40, which lacks firmware 13.60 initialization.
  Preserve the full 32-byte credential attributes required by the new SDK API.
  The previous helper SIGILL/EOF is documented; the user confirmed the replacement
  filesystem integration on firmware 13.60 with kstuff and ShadowMount.

- Initialize libSceNet and allocate a network pool before the optional Lapy
  helper request. Added stage-specific native transport errors after hardware
  logs showed the previous request failed with status 9 before elevation.
- Optional `filesystemAccess: "console"` packages the pinned upstream Lapy
  exact-title helper and requests filesystem access once before starting React.
  System Explorer enables it. Root aliases are preserved after successful
  elevation. A resident Lapy service or local ELF loader on port 9021 is required;
  the exact tested application/helper hashes are recorded in hardware evidence.
- PS5 storage queries use the native-title `_fstatfs` export, following the
  ProsperoStore reference, with bounded discovery of accessible mount records.
  Removed direct syscalls after hardware logs confirmed a fatal
  `SYSTEM_ILLEGAL_FUNCTION_CALL`. The replacement was user-confirmed on firmware 13.60.
- PS5 directory listing validates record lengths and filename boundaries before
  invoking callbacks; malformed buffers return an I/O error.
- System Explorer queries storage on directory navigation or explicit Refresh,
  rather than on every React render. Desktop tests cover native storage bindings
  and error propagation.

- Build-time `className` styling: a subset of Tailwind CSS v3 utilities compiles
  to literal style objects on any JSX element, scaled to the render width.
- State variants (`focused:`, `selected:`, `disabled:`, `active:`, `checked:`,
  `pressed:`) driven by props of the same name; `hover:`, `focus:`, and
  `focus-visible:` are aliases of `focused:`.
- Pinned dependencies can carry reviewed patches from `patches/`. Embedded React
  is patched so auto-height `flex-wrap` rows grow to hold every wrapped line.
- `tw` tagged templates exported from `@ps5-react/core`.
- Optional per-app `tailwind.config.js` for `baseWidth` and theme overrides,
  including font files that are baked automatically.
- Unsupported classes and runtime-computed class names fail the build with a
  source location.
- The starter uses `className`; its snapshots match the explicit-style version.
- React Native-style native modules in `@ps5-react/core`: `Platform`,
  `DeviceInfo`, `FileSystem`, `Notifications`, `Users`, `Controller` with the
  `useGamepad` hook, `Linking`, and `BackHandler`, implemented by both hosts through the
  `__ps5ReactNative` contract. The desktop preview uses a per-app sandbox. The
  PS5 implementation is not yet hardware-validated. See `docs/NATIVE-API.md`.
- `apps/system-explorer` (`PPSA99056`) demonstrates every native module.
- Motion-style animation in `@ps5-react/core`: `motion.*` components,
  `AnimatePresence`, variants with stagger, and `transitions` presets, driven by
  the engine without per-frame JavaScript. `Animated`, `useAnimatedValue`,
  `Easing`, `LayoutAnimation`, and `Pressable` are re-exported. See
  `docs/ANIMATION.md` for the API, performance rules, and design guidelines.
- Tailwind animation classes (`animate-in`, `fade-in`, `slide-in-from-*`,
  `zoom-in`, `animate-out`, `duration-*`, `delay-*`, `ease-*`, `transition`,
  `animate-spin`/`pulse`/`bounce`/`ping`) compile to motion props at build time.
- Both hosts log a frame-time summary every two seconds (`[PS5-REACT] frame:
  fps=… total avg=… p95=… max=…` with input, update, present, and swap phases);
  on the PS5 it goes to the kernel log.
- `apps/motion-lab` (`PPSA99057`) demonstrates staggered entrances, focus lift
  with a gliding highlight, page transitions, Tailwind animation classes, a
  toast, and idle motion.
- Spatial D-pad navigation (`docs/NAVIGATION.md`): `View`, `Image`, and
  `Pressable` take `focusable`, `onPress`, `onFocus`/`onBlur`, `autoFocus`,
  `focusKey`, and `nextFocus*`; `FocusScope` (with `trap`, `wrap`,
  `restoreFocus`, `onBack`), `useFocusable`, `useFocus`, `useIsFocused`,
  `BackHandler.addEventListener`, and a `ScrollView` that follows focus. Hosts
  send `up`/`down`/`left`/`right`; `useController` still receives
  `previous`/`next` after them.
- The starter, the `npm run create` template, `apps/tailwind-gallery`,
  `apps/system-explorer`, and `apps/motion-lab` navigate with focusable elements
  and scopes instead of manual focus indices. The starter's snapshots are
  unchanged.
- `Svg`, `Path`, `Circle`, `Rect`, `Line`, and `G` are re-exported from
  `@ps5-react/core`.
- The engine profile scales images bilinearly, across the full 2560-pixel
  render width, registers up to 256 images, and holds 64 `<Svg>` nodes with 32
  cached (about 2.9 MB more static memory). The starter's snapshots are
  unchanged.
- `ScrollView` scrolls its focused descendant into view with the engine's
  native scroll offset (Embedded React is patched with
  `NativeUI.scrollTo`) instead of translating a content wrapper, so layout
  props stay on the ScrollView and nested rails scroll innermost first.
- `FocusScope` takes `inert`: its subtree leaves D-pad navigation, autoFocus,
  and focus restoration, and focus leaves it when it becomes inert. Use it for
  mounted but hidden screens. When no scope has a candidate, focus stays put.
- The engine profile raises the node pool and bridge handles to 2048, so two
  full screens fit during a transition; the bridge warns once when node
  creation fails instead of dropping nodes silently.
- Views take a `backgroundGradient` style (CSS linear and radial gradients,
  up to four stops), and Tailwind compiles `bg-gradient-to-*`, `from-*`,
  `via-*`, and `to-*` with stop positions to it. The engine profile enables
  `ERUI_GRADIENT` (about 23 KB more static memory). The starter's snapshots are
  unchanged.
- The frame-time summary adds the engine's own split (`js` with dispatch,
  React render, and marshal; `layout`; `raster` with pre-pass, render, and
  blit) and the repainted and written pixels per frame. Frames over 33 ms print
  a `slow frame:` line with that split and the repaint bounds. The engine
  profile enables `ERUI_PERF_STATS` for this; no overlay is drawn.
- The desktop preview replays `PS5_REACT_INPUT_SCRIPT` (for example
  `wait:3000,right*3,confirm,wait:2000,back,quit`) through the normal input
  path for reproducible profiling; `PS5_REACT_SLOW_FRAME_MS` sets the
  slow-frame threshold. See `docs/ANIMATION.md`. The PS5 host reads the same
  script from `/app0/dev/input-script.txt` when a test deploy adds that file,
  and live commands from `/app0/dev/commands.txt` while it runs.
- Embedded React is patched so a repaint over an image converts and scales only
  the repainted part instead of the whole image, bilinear scaling uses integer
  weights, and a scale animation resting at 1 no longer renders its subtree
  through the transform buffer. The starter's snapshots are unchanged.
- Controller actions reach JavaScript inside React's batch, from the frame's
  microtask drain: a handler that sets several pieces of state renders and
  commits once, and its render counts as `react` in the frame log.
- Scrolling moves the pixels already painted: Embedded React is patched so a
  ScrollView whose offset changes moves its viewport through a new backend
  `move_rect` and repaints only the exposed strip, falling back to a full
  viewport repaint when anything but a solid background shares those pixels.
  Both hosts replay the move on the GPU texture instead of uploading the
  viewport again.

## 0.1.0 — 2026-10-05

Initial experimental source release.

- React/JSX runtime backed by QuickJS and Embedded React software rendering.
- Shared OpenGL framebuffer presenter and independent PS5 application host.
- Native macOS preview with rebuild/restart on save.
- Controller hook, counter starter, app manifests, and app scaffolding.
- Pinned dependency bootstrap, folder/ZIP builds, integrity checks, and receipts.

The baseline proof of concept was user-tested on PS5 firmware 13.60. The
reorganized framework starter is not yet independently hardware-tested.
