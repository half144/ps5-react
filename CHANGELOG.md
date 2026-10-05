# Changelog

## Unreleased

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

## 0.1.0 — 2026-10-05

Initial experimental source release.

- React/JSX runtime backed by QuickJS and Embedded React software rendering.
- Shared OpenGL framebuffer presenter and independent PS5 application host.
- Native macOS preview with rebuild/restart on save.
- Controller hook, counter starter, app manifests, and app scaffolding.
- Pinned dependency bootstrap, folder/ZIP builds, integrity checks, and receipts.

The baseline proof of concept was user-tested on PS5 firmware 13.60. The
reorganized framework starter is not yet independently hardware-tested.
