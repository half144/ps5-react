# Native modules

`@ps5-react/core` exposes platform services as React Native-style modules.
Apps import them like any other export; they never touch the SDK, libkernel,
or SDL directly.

```jsx
import {Platform, DeviceInfo, FileSystem, Controller, useGamepad} from '@ps5-react/core';

const {model, firmware} = DeviceInfo.get();
FileSystem.writeFile(`${FileSystem.dataDir}/save.json`, JSON.stringify(state));
Controller.setLightBar('#38bdf8');
```

[`apps/system-explorer`](../apps/system-explorer) exercises every module.

## Contract

The modules in `runtime/js/native.js` wrap `globalThis.__ps5ReactNative`, which
each host installs before the bundle runs. Its exact shape (ABI v5) is
documented in [`native/shared/host_api.hpp`](../native/shared/host_api.hpp).
`native/shared/host_api.cpp` implements the filesystem with POSIX and the object
itself; `native/ps5/` and `native/desktop/` implement the platform functions.

- **Synchronous bridge.** Each native call returns directly. Networking calls
  only submit/cancel/poll bounded tasks; `Http.request` and `task.done` expose
  promises in the JavaScript wrapper while native workers perform I/O.
- **Render thread.** Calls run on the same thread as React and the rasterizer.
  Keep them cheap: avoid listing large directories or reading files every
  frame. Call them from event handlers, effects, or state initializers rather
  than repeatedly during render.
- **Errors.** A failed call throws an `Error` whose message names the call, the
  path, and the system error, for example
  `fs.readDir /download0/missing: No such file or directory`. Wrap calls in
  `try`/`catch` and show the message.
- **Missing host.** If `__ps5ReactNative` does not exist (for example when a
  module is evaluated outside a PS5 React host), every call throws an error
  explaining that a PS5 React host is required.
- **QuickJS runtime.** The runtime has no `Date` objects (`Date.now()` works),
  typed arrays, or `Proxy`; values cross the boundary as strings, numbers,
  booleans, plain objects, arrays, and `null`.

## Platform

| Member | Returns | Notes |
| --- | --- | --- |
| `Platform.OS` | `'ps5' \| 'desktop'` | The running host |
| `Platform.select(spec)` | `spec[OS]`, else `spec.default` | Keys: `ps5`, `desktop`, `default` |

```js
const label = Platform.select({ps5: 'Console', desktop: 'Preview', default: 'Unknown'});
```

## DeviceInfo

`DeviceInfo.get()` returns:

| Field | Unit | Notes |
| --- | --- | --- |
| `model` | string | Console model, or `Desktop preview (<Mac model>)` |
| `firmware` | string | System software version, or `macOS <version>` |
| `cpuTemperature` | °C | `null` on desktop |
| `socTemperature` | °C | `null` on desktop |
| `cpuFrequency` | Hz | `null` when unknown (always on Apple silicon) |
| `freeMemory` | bytes | Memory available to the app |
| `processTime` | µs | CPU time used by this process |

Any value the host cannot read is `null`.

## FileSystem

By default, the title runs sandboxed. App paths are logical roots; explicit
console paths become accessible only after opt-in filesystem access succeeds.

| Constant | Path | Access |
| --- | --- | --- |
| `FileSystem.appDir` | `/app0` | The installed application folder; read-only |
| `FileSystem.dataDir` | `/download0` | Writable, persists between launches |
| `FileSystem.tempDir` | `/temp0` | Writable scratch space |

On the console, listing `/app0` may return an empty array even though files
inside it can be read by path. Paths containing `..` components are rejected.

| Function | Returns | Notes |
| --- | --- | --- |
| `readDir(path)` | `FileStat[]` | Entries of a directory, without `.` and `..` |
| `stat(path)` | `FileStat \| null` | `null` when the path does not exist |
| `exists(path)` | `boolean` | `stat(path) !== null` |
| `readFile(path)` | `string` | UTF-8; throws above 8 MiB |
| `writeFile(path, text)` | — | Creates or truncates |
| `appendFile(path, text)` | — | Creates or appends |
| `mkdir(path, {recursive})` | — | `recursive` defaults to `false` |
| `remove(path)` | — | A file or an empty directory |
| `rename(from, to)` | — | Both are app paths |
| `mounts()` | `{device, path, type}[]` | Up to 64 mounted filesystems visible to this process, as host paths; see [Hardware status](#hardware-status) |
| `diskUsage(path)` | `{total, free}` | Bytes for the filesystem holding `path`; `free` is available to the app, clamped to `0..total` |

`FileStat` is `{name, isDirectory, isFile, size, modified, device, inode}`, where
`size` is in bytes and `modified` is milliseconds since the Unix epoch.
`device` and `inode` are decimal strings that preserve native 64-bit identities.
Use both to detect replacement of a directory or file, including another drive
mounted at the same path; these identities are filesystem-local, not universal
drive serial numbers.

Files are text only; there is no binary read or write.

For console-wide filesystem access, add this to the app manifest:

```json
{ "filesystemAccess": "console" }
```

Then query storage from an event handler or effect:

```js
const entries = FileSystem.readDir('/data');
const mounts = FileSystem.mounts();
const {total, free} = FileSystem.diskUsage('/data');
```

Keep `filesystemAccess` set to `"sandbox"` (the default) for apps that only
need their own files. The opt-in helper requires resident Lapy or a local ELF
loader; see [access requirements](#hardware-status). On failure, filesystem
calls retain their normal permission errors rather than returning fake values.


## Http and Downloads

`Http.request(url, options)` provides bounded text/JSON HTTP responses, effective
URLs and allowlisted lowercase response headers. Set `followRedirects: false`
to inspect redirect metadata; cookies and authentication headers stay private.
`Downloads.enqueue({url, destination, ...options})` returns a cancellable task
with progress subscriptions and a completion promise. Binary downloads stay
native, use parallel validated ranges when eligible, and resume durable ranges.
`Downloads.enqueueManifest({manifest, destination, ...options})` supports split
byte manifests with per-piece SHA-1 and whole-file SHA-256 verification.
`DownloadFormats` validates manifests and routes image, package, archive and
binary filenames to their appropriate destinations. Native download options
include opt-in `rejectHtml`, exact `expectedBytes`, `storageRoot` identity checks, `recoverCompleted`
for receipt-backed SHA-256 verification of published files, and normalized
`pieces: [{url, offset, size, sha1}]`. These options share ABI v2 on both hosts;
there are no additional global native functions.
PS5 builds require `networking: true` and `filesystemAccess: "console"`; desktop
networking is available by default. See [NETWORKING.md](NETWORKING.md) for
options, resource limits, errors, lifecycle and the hardware-validation gap.

## Images

`<Image source={{uri}}>` loads `http://` and `https://` URLs natively: fetched
and decoded by two workers off the render thread, at the size the element is
drawn, into a bounded LRU cache. `Image.prefetch(uri, {width, height,
resizeMode})` warms that cache. `Image.getColor(uri)` resolves to the art's most
prominent vivid colour as `'#rrggbb'`, or `null` for grey or dark art, computed
while decoding a small copy. The bridge (`image.load`, `image.release`,
`image.poll`, ABI v3; results carry that `color`) is internal to the `Image` component. See
[IMAGES.md](IMAGES.md) for sizing, formats, memory budgets and errors.

## Notifications

`Notifications.show(message, subMessage?)` shows a system notification and
returns `true` when the host accepted it.
On PS5 it uses libkernel's `sceKernelSendNotificationRequest`: a plain toast whose
subMessage follows the message on a second line.

## Users

| Function | Returns |
| --- | --- |
| `Users.getForeground()` | `{id, name} \| null` — the user who launched the app |
| `Users.getLoggedIn()` | `{id, name}[]` |

## Controller

| Function | Notes |
| --- | --- |
| `setLightBar(color)` | `'#rrggbb'` or `{r, g, b}` with 0–255 channels |
| `resetLightBar()` | Restores the system color |
| `vibrate(strength = 1, ms = 200)` | `strength` 0–1 |
| `getState()` | `GamepadState`, the latest polled state |

`GamepadState` is:

```ts
{
  connected: boolean;
  leftX: number; leftY: number;    // -1..1, deadzone applied
  rightX: number; rightY: number;  // -1..1, deadzone applied
  l2: number; r2: number;          // 0..1
  buttons: string[];               // held: up down left right cross circle triangle
                                   // square l1 r1 l2 r2 l3 r3 options touchpad
}
```

The host updates the state once per frame, before JavaScript timers and
controller actions run.

`useGamepad(intervalMs = 16)` polls `getState()` while the component is mounted
and re-renders only when the state changes:

```jsx
function Sticks() {
  const pad = useGamepad();
  return <Text>{pad.leftX} {pad.leftY} {pad.buttons.join(' ')}</Text>;
}
```

Menus use focus navigation ([NAVIGATION.md](NAVIGATION.md)). `useController`
receives the raw `up`, `down`, `left`, `right`, `confirm`, `back`, `l1`, `r1`,
`l2`, `r2`, `triangle`, and `square` actions (plus `previous`/`next` for
compatibility); `useGamepad` is for raw analog and
button state. Options is still reserved for the host's exit action.

## Sound

Short interface sounds: focus ticks, confirm, back, errors, notifications.
Import a WAV like an image; the build bakes it into the app and the import is
the sound's name:

```jsx
import {Sound, useNavigationEvents} from '@ps5-react/core';
import tick from './assets/sounds/focus.wav';

useNavigationEvents(event => { if (event.type === 'move') Sound.play(tick); });
```

| Function | Notes |
| --- | --- |
| `Sound.play(name, {volume = 1, pan = 0})` | Returns `true` when the sound started; `volume` 0–1, `pan` -1 (left) to 1 (right) |
| `Sound.setVolume(volume)` | Master volume 0–1 for every sound (0 mutes) |

- **Format.** 16-bit PCM WAV, mono or stereo, 8–96 kHz, at most 10 seconds;
  other files fail the build with the file name. Keep sounds short: they stay
  decoded in memory (8 bytes per frame). An app that imports no WAV opens no
  audio output.
- **One voice per sound.** While a sound still plays, playing it again is
  skipped and returns `false`, so a held D-pad keeps ticking without stacking.
  Different sounds overlap.
- **Cheap and non-blocking.** `play` posts a command to a lock-free queue and
  returns; mixing runs on an audio thread (the ps5-homebrew-ui mixer: 32 voices,
  48 kHz stereo, a soft limiter). It returns `false` when there is no audio
  output. An unknown name throws `sound.play <name>: no imported .wav has this
  name`.
- **Feedback for navigation.** `useNavigationEvents` ([NAVIGATION.md](NAVIGATION.md#navigation-events))
  reports each move, blocked direction, press, back and button action, so one
  hook at the root gives every screen its sounds.

`apps/starter/sounds.js` wires the framework's sound set (`focus`, `confirm`,
`back`, `error`, `page`, `notify`) this way, and `npm run create` copies it. The
set is derived from Google's Material Design sound resources (CC BY 4.0): keep
`licenses/material-sounds-NOTICE.txt` with an app that ships it. `node
tools/ui_sounds.mjs <material wav directory> <output directory>` rebuilds it from
the pack's WAV files.

## Linking

`Linking.openURL(url)` opens an `http://` or `https://` URL in the system
browser and returns `true` when the host launched it. Other schemes throw.

## Power

`Power.keepAwake(enabled)` keeps the console out of rest mode for inactivity
while `true`, for work that runs with nobody touching the controller, such as a
download of several hours. Turn it on when the work starts and off when it
ends; the last call wins, so an app with several jobs keeps one count of its
own.

The PS5 host calls `sceSystemServicePowerTick` every 30 seconds while it is on,
which restarts the inactivity timer as input does (the method FTP and download
homebrew use). The desktop preview disables or enables the SDL screen saver and
prints `[power] keep awake on|off`. **Not yet validated on hardware**: no test
has shown a console staying awake through its rest-mode timeout.

## BackHandler

`BackHandler.exitApp()` asks the host to close the app after the current frame,
exactly like pressing Options (React Native's name for the same call).

## Desktop preview behavior

`npm run dev` and `npm run preview` implement the same API on macOS:

- Files live in `.build/<app>/sandbox/`: `download0/` and `temp0/` are
  directories, and `app0` is a link to `apps/<app>/`. `/` lists the sandbox
  root. Unlike the console, `/app0` is writable on desktop; do not rely on it.
- `mounts()` lists the Mac's mounted filesystems.
- `Notifications.show` prints `[notify] message: subMessage` to the terminal.
- `Users` reports the macOS user (`$USER`) with id `1`.
- Temperatures are `null`; `cpuFrequency` is `null` on Apple silicon.
- `openURL` opens the URL in the default macOS browser.
- With an SDL game controller connected, the light bar and rumble use it;
  otherwise the calls print a `[pad]` line. Without a controller, `getState()`
  reports `connected: false` and neutral values.
- Sounds play through SDL's default output. `SDL_AUDIODRIVER=dummy` keeps the
  preview silent; the self-test sets it.

## Hardware status

The PS5 host implements this API against the public PS5 Payload SDK.
It has **not yet been validated on hardware** except where
[HARDWARE.md](HARDWARE.md) records a test for a specific artifact, firmware,
and loader. Desktop behavior and successful PS5 builds do not establish console
behavior; individual calls may fail or return `null` on a given firmware.

Some PS5 calls load a system module at runtime. It is unverified whether a
title can load these modules; when one cannot, only the dependent call degrades:

| Call | Module | Without it |
| --- | --- | --- |
| `DeviceInfo.get().model` | `libkernel_sys.sprx` | `null` |
| `Notifications.show()` | `libSceNotification.sprx` | Returns `false` |

PS5 `diskUsage()` uses the native-title `_fstatfs` export on a descriptor opened
with `sceKernelOpen`. `mounts()` discovers kernel-provided records through
accessible descriptors, deduplicates them, and returns up to 64 records. Discovery
checks sandbox roots, common storage paths, and immediate children of `/` and
`/mnt`, with at most 96 metadata queries. It is not the privileged global mount
table returned by a payload's `getmntinfo`; inaccessible or deeper mounts can be
absent. No mount records or capacities are invented.

Hardware logs confirmed that our removed direct-syscall prototype terminated the
title with `SYSTEM_ILLEGAL_FUNCTION_CALL`. The replacement uses libkernel's
native wrapper, following a community title reference. The replacement
filesystem integration was confirmed by the user on firmware 13.60 with kstuff
and ShadowMount; exact application/helper hashes are recorded in
[Hardware evidence](HARDWARE.md#system-explorer-filesystem-success--2026-10-05). Directory records from `sceKernelGetdents` are bounds-checked before
names reach JavaScript. See [Storage investigation](STORAGE.md).

Standalone titles can still receive `EPERM` even with kstuff running. For a
console filesystem explorer, set `"filesystemAccess": "console"` in `app.json`
(default: `"sandbox"`). The build packages a pinned upstream Lapy one-shot
helper restricted to that title. Startup requests access before creating the
React thread and accepts success only after a write/read/remove proof in `/data`.
The console must provide resident Lapy or an ELF loader on localhost port 9021.
Startup can take several seconds; no access requests run during React renders.

After success, `/app0`, `/download0`, and `/temp0` remain logical app paths mapped
to their accessible sandbox mounts. Data and temporary roots must pass an
exclusive file creation, write, read and removal proof; opening a directory
for reading alone does not establish write access. If the original roots fail
that proof after the root change, data falls back to
`/data/ps5-react/<TITLE_ID>` and temporary files to its `tmp` directory. Directory
creation and fallback proofs are checked and failures log the path and errno.
That fallback persists until explicitly removed.
Failure leaves console path mapping disabled and logs the upstream status.
Optional native API modules are resolved before requesting the root change.
Desktop behavior is unchanged. The opt-in integration was user-confirmed on
firmware 13.60 with kstuff and ShadowMount. Other firmware/loader combinations
require their own validation.

`diskUsage` measures a filesystem, not recursive directory size or the console
Settings storage categories. On desktop it reports the real Mac filesystem
backing the per-app sandbox.

System Explorer caches these queries when its Files page opens. Navigate to a
directory or select **Refresh** to query again; focus changes do not query storage.

## Not yet available

- Global `fetch`, raw sockets and WebSocket.
- Music, streamed or long audio, and decoding compressed formats at run time.
- The system on-screen keyboard (IME) for text input.
- System save data; use `FileSystem.dataDir` for now.
