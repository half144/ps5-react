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
each host installs before the bundle runs. Its exact shape (ABI v1) is
documented in [`native/shared/host_api.hpp`](../native/shared/host_api.hpp).
`native/shared/host_api.cpp` implements the filesystem with POSIX and the object
itself; `native/ps5/` and `native/desktop/` implement the platform functions.

- **Synchronous.** Every call blocks and returns its result directly; there are
  no promises.
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

The title runs sandboxed: paths are app paths, not host paths.

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
| `mounts()` | `{device, path, type}[]` | Mounted filesystems, as host paths; see [Hardware status](#hardware-status) |
| `diskUsage(path)` | `{total, free}` | Bytes for the filesystem holding `path` |

`FileStat` is `{name, isDirectory, isFile, size, modified}`, where `size` is in
bytes and `modified` is milliseconds since the Unix epoch.

Files are text only; there is no binary read or write.

## Notifications

`Notifications.show(message, subMessage?)` shows a system notification and
returns `true` when the host accepted it.

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

`useController` remains the way to receive `previous`, `next`, `confirm`, and
`back` actions; `useGamepad` is for raw analog and button state. Options is
still reserved for the host's exit action.

## Linking

`Linking.openURL(url)` opens an `http://` or `https://` URL in the system
browser and returns `true` when the host launched it. Other schemes throw.

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
| `FileSystem.mounts()` | `libkernel_sys.sprx` | Throws `fs.mounts: Function not implemented` |
| `DeviceInfo.get().model` | `libkernel_sys.sprx` | `null` |
| `Notifications.show()` | `libSceNotification.sprx` | Returns `false` |

## Not yet available

- Networking (`fetch`, sockets, WebSocket).
- Audio playback.
- The system on-screen keyboard (IME) for text input.
- System save data; use `FileSystem.dataDir` for now.
