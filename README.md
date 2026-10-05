# PS5 React

**Build standalone, fullscreen PS5 homebrew interfaces with React and JSX.**

Write your UI in React, preview it natively on macOS, and build an extracted
PS5 application folder for a compatible homebrew loader. No browser, Sony RNPS
hooks, or system UI replacement is involved.

> **Experimental v0.1.0.** The original proof of concept ran on a PS5 Slim Digital
> with firmware 13.60, kstuff, and ShadowMount. This reorganized framework starter
> needs its own hardware validation. Desktop tests do not prove PS5 execution.

## How it works

```text
React / JSX → QuickJS → Embedded React software renderer
                                      ↓ ARGB framebuffer
                              OpenGL texture presenter
                                      ↓
                      macOS preview / fullscreen PS5 title
```

Embedded React owns layout, text, and CPU rasterization. OpenGL presents the
resulting framebuffer. This is not the complete official React Native runtime
and does not use the ps5-homebrew-ui widget library.

## Quick start

The initial development environment supports **macOS with Apple Silicon**.
Install Python 3.12+, Node.js/npm, Git, CMake, SDL2, and a C++ compiler.
LLVM 18 and Pillow are also required to build the PS5 application.

```sh
brew install python node cmake sdl2 llvm@18
git clone https://github.com/half144/ps5-react.git
cd ps5-react
python3 -m venv .venv
source .venv/bin/activate
python3 -m pip install -r requirements.txt
npm ci
npm run doctor
npm run dev
```

Edit **`apps/starter/index.jsx`**. Saving JSX, assets, or configuration rebuilds
and reopens the preview. State resets on restart; this is not Fast Refresh.
Use `npm run preview` for a single preview run.

| Action | Desktop | PS5 |
| --- | --- | --- |
| Move focus | Arrow keys | D-pad |
| Confirm | Enter | Cross (X) |
| Back | Backspace | Circle |
| Exit | Escape | Options |

Escape closes the preview. Ctrl+C stops the watcher. Development commands do
not connect to the console.

## Build for PS5

```sh
npm run build
```

The first build downloads pinned public dependencies and checks their hashes.
Subsequent builds use the local cache.

Outputs:

- `dist/PPSA99053/`: complete application folder.
- `dist/PPSA99053.zip`: ZIP containing that folder.
- `dist/PPSA99053.receipt.json`: dependency, toolchain, and output hashes.

Extract the ZIP and upload the **complete folder** to
`/data/homebrew/PPSA99053/` using PS5Upload or your loader's supported workflow.
Use kstuff and a compatible title-registering loader such as ShadowMount.
Follow your loader's registration procedure before launching **PS5 React Starter**.
The initial supported distribution workflow is an extracted folder, not a PKG.

The starter intentionally closes **30 seconds after the first displayed frame**.
Set `timeoutSeconds` to `0` in `apps/starter/app.json` to disable automatic exit.
Options remains the explicit exit action.

To use an existing public SDK:

```sh
npm run build -- --sdk /path/to/ps5-payload-sdk
```

Changing the SDK changes build provenance and requires a new hardware check.

Apps that need console filesystem access can set `"filesystemAccess": "console"`
in `app.json`. This packages the pinned Lapy exact-title helper and requests
access once at startup; a resident Lapy service or local ELF loader on port 9021
is required. The default is `"sandbox"`. System Explorer opts in. See
[native API access requirements](docs/NATIVE-API.md#hardware-status) and
[storage investigation](docs/STORAGE.md) for limits and hardware status.

## Create an app

```sh
npm run create -- --name my-store --title-id PPSA99054
npm run dev -- --app my-store
npm run build -- --app my-store
```

The new app starts from a small focusable menu written with `className`, the
starter's theme and font, and no automatic exit (`timeoutSeconds: 0`). Pass
`--title "My Store"` to set the name shown on the console (default: from
`--name`). Each app contains JSX, `app.json`, assets, and font configuration.
No C++ edits are needed. Choose a title ID unused on your console; the CLI also rejects IDs
already used by another local app. The manifest configures title, version,
render resolution, display surface, and timeout.

## Minimal API

```jsx
import {useState} from 'react';
import {AppRegistry, View, Text, FocusScope} from '@ps5-react/core';

function Counter() {
  const [count, setCount] = useState(0);
  return (
    <FocusScope autoFocus>
      <View onPress={() => setCount(value => value + 1)}>
        <Text>Count: {count}</Text>
      </View>
    </FocusScope>
  );
}

AppRegistry.registerComponent('my-app', () => Counter);
```

`@ps5-react/core` exports Embedded React's `AppRegistry`, `View`, `Text`, `Image`,
and `ScrollView` (with D-pad focus on `View`, `Image`, and `Pressable`), the
[navigation](#navigation) API, the `useController` hook for raw input, the `tw`
style template, and the [native modules](#native-modules).
`useController` receives `up`, `down`, `left`, `right`, `confirm`, and `back`
(plus `previous`/`next` for compatibility) and unsubscribes on unmount.

## Navigation

```jsx
<FocusScope autoFocus onBack={closeLibrary}>
  {games.map(game => (
    <View key={game.id} onPress={() => open(game)}
      className="rounded-xl bg-slate-800 focused:bg-sky-700 focused:scale-105" />
  ))}
</FocusScope>
```

Focus moves spatially: the D-pad picks the nearest focusable element in the
pressed direction from the laid-out positions, Cross calls `onPress`, and Circle
goes to the enclosing `FocusScope.onBack` handlers, then `BackHandler`
listeners. Apps mark elements focusable instead of tracking focus indices.
`FocusScope` groups regions (tabs, page, modal) with `trap`, `wrap`, and focus
memory; `ScrollView` follows focus. See [Navigation](docs/NAVIGATION.md).

Images and fonts are baked during the build. Font sizes written in `className`
or as literal styles are found automatically; declare only sizes chosen at
runtime and extra glyphs in `assets.config.js`; see the starter.
The initial rendering profile is 2560×1440 downsampled to a 1920×1080 surface.

## Styling with className

```jsx
<View onPress={() => open(label)}
  className="rounded-xl bg-slate-800 px-6 py-4 focused:bg-sky-800 focused:scale-105">
  <Text className="text-xl text-white">{label}</Text>
</View>
```

A subset of Tailwind CSS v3 utilities compiles to literal style objects at
build time; nothing is parsed on the PS5. Sizes are scaled to the app's render
width. On a focusable element, `focused:` and `pressed:` follow its own focus;
`selected:` and other state variants read the element's props, and explicit
`style` always wins. Unsupported classes fail the build with
a source location. See [Styling with className](docs/TAILWIND.md).

## Animation

```jsx
import {motion, AnimatePresence, transitions} from '@ps5-react/core';

<motion.View onPress={play} whileFocus={{y: -6, scale: 1.06}} transition={transitions.focus}
  className="w-48 h-28 rounded-2xl bg-slate-800" />
<View className="animate-in fade-in slide-in-from-bottom-4 duration-300" />
```

`motion.View`, `AnimatePresence`, variants with stagger, and transition presets
follow the Motion API; Tailwind animation classes compile to the same props.
Animations run in the engine without per-frame JavaScript. On the CPU renderer,
translation is cheap, scale and rotation are limited to small elements, and
full-screen fades are the most expensive. See [Animation](docs/ANIMATION.md)
and `apps/motion-lab`.

## Native modules

```jsx
import {DeviceInfo, FileSystem, Controller, useGamepad} from '@ps5-react/core';

const {firmware} = DeviceInfo.get();
FileSystem.writeFile(`${FileSystem.dataDir}/notes.txt`, 'hello');
Controller.setLightBar('#38bdf8');
const pad = useGamepad(); // sticks, triggers, held buttons
```

React Native-style modules reach platform services through the host:
`Platform`, `DeviceInfo`, `FileSystem`, `Notifications`, `Users`, `Controller`
with `useGamepad`, `Linking`, and `BackHandler`. Calls are synchronous and run on the
render thread. The macOS preview implements the same API against a sandbox in
`.build/<app>/sandbox/`. The filesystem integration was user-confirmed on
firmware 13.60 with kstuff and ShadowMount; other native modules still require
validation.
See [Native modules](docs/NATIVE-API.md) and `apps/system-explorer`.

## Validation and limitations

```sh
npm test
```

The starter's desktop test checks framebuffer channels, orientation, React state,
focus, simulated controller actions, and fullscreen presentation. The PS5 build
checks ELF/SELF structure, metadata, integrity, hashes, and ZIP contents.
Logs and test screenshots are under `.build/<app>/`.

- The PS5 framework starter has not yet been independently hardware-tested.
- Rasterization runs on the CPU; there is no direct GPU UI backend or measured FPS claim.
- DOM, browser APIs, Node.js APIs, networking, audio, and text input are not implemented.
- Spatial focus is not yet tested on hardware; there are no pointer, touch, or text-input focus modes.
- Dependencies are pinned, but cross-machine bit-identical builds are not guaranteed.

## Documentation

- [Architecture](docs/ARCHITECTURE.md)
- [Navigation](docs/NAVIGATION.md)
- [Styling with className](docs/TAILWIND.md)
- [Native modules](docs/NATIVE-API.md)
- [Animation](docs/ANIMATION.md)
- [Hardware evidence](docs/HARDWARE.md)
- [Dependencies and licenses](docs/DEPENDENCIES.md)
- [Contribution guide](CONTRIBUTING.md)
- [Potential Embedded React contributions](docs/UPSTREAM-CONTRIBUTION.md)

## Credits and license

Created by **[half144](https://github.com/half144)** and PS5 React contributors.
Built on [Embedded React](https://github.com/TheMasterCoder007/embedded-react),
[ps5-homebrew-ui](https://github.com/blackbearreloaded/ps5-homebrew-ui),
[ps5-opengl](https://github.com/blackbearreloaded/ps5-opengl),
[PS5 Payload SDK](https://github.com/ps5-payload-dev/sdk),
[QuickJS-ng](https://github.com/quickjs-ng/quickjs), and React.
The utility palette and scales come from [Tailwind CSS](https://github.com/tailwindlabs/tailwindcss) (MIT).

Framework-owned code is licensed under **GPL-3.0-or-later**, with the reasonable
attribution-preservation term in [LICENSE-ATTRIBUTION](LICENSE-ATTRIBUTION)
under GPLv3 section 7(b). Preserve the [NOTICE](NOTICE) when redistributing it.
Third-party material retains its own license and credits. No proprietary Sony
modules are redistributed. See [dependency licensing](docs/DEPENDENCIES.md).
