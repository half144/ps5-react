# Styling with className

PS5 React compiles a **subset of Tailwind CSS v3 utilities** into style objects
at build time, similar in spirit to NativeWind but with no runtime. The
renderer still receives only JavaScript style objects; no CSS is parsed on the
PS5. This is not full Tailwind compatibility: every supported utility is listed
below, and anything else fails the build with a source location and a reason.

## Quick example

```jsx
import {useState} from 'react';
import {AppRegistry, View, Text, tw, useController} from '@ps5-react/core';

const title = tw`text-4xl font-bold text-white`;

function Menu() {
  const [focus, setFocus] = useState(0);
  useController(action => {
    if (action === 'next') setFocus(i => (i + 1) % 3);
    if (action === 'previous') setFocus(i => (i + 2) % 3);
  });
  return (
    <View className="flex-1 items-center justify-center gap-4 bg-slate-950">
      <Text style={title}>Library</Text>
      {['Games', 'Media', 'Settings'].map((label, i) => (
        <View key={label} focused={focus === i}
          className="w-80 rounded-xl border-2 border-transparent bg-slate-800 px-6 py-4
            focused:border-sky-400 focused:bg-sky-800 focused:scale-105">
          <Text className="text-xl text-slate-200 focused:text-white" focused={focus === i}>
            {label}
          </Text>
        </View>
      ))}
    </View>
  );
}

AppRegistry.registerComponent('menu', () => Menu);
```

## How it works

```text
index.jsx ── esbuild ── Babel plugin (tools/tailwind) ── bundle with literal styles
                              ↑
       tailwind.config.js + app.json render size
```

1. During bundling, a Babel plugin visits every `className` attribute and every
   `tw` template imported from `@ps5-react/core`.
2. Each class is resolved against the theme (`tools/tailwind/theme.mjs`, merged
   with the app's `tailwind.config.js`) and converted to engine style keys.
3. Lengths are scaled to the render resolution and rounded to integers.
4. The attribute is replaced by `style={...}` containing literal objects.
   Conditions and variants become conditional entries in a style array.
5. `className` and the class strings do not reach the bundle or the PS5 runtime.

`className` works on any JSX element: host primitives (`View`, `Text`, `Image`,
`ScrollView`) and your own components. A custom component receives
a `style` prop, possibly an array, and must forward it to a primitive.

### Precedence

- Later classes win over earlier ones in the same string.
- Conditional parts (`cond ? 'a' : 'b'`, `cond && 'a'`) apply after the static
  classes of the same expression.
- Variant classes (`focused:...`) apply after base and conditional classes.
- An explicit `style` prop always wins: `className` plus `style` compiles to
  `style={[compiled, style]}`.
- The `!` important modifier is rejected; precedence above covers its uses.

## Scaling

Utilities are written in **logical pixels** at a base width of 1280: `1rem` is
16px and spacing unit `1` is 4px, as in Tailwind. The build multiplies every
length by `render.width / baseWidth` and rounds it:

```text
physical px = round(logical px × render.width / baseWidth)
```

`render.width` comes from `app.json`; `baseWidth` from `tailwind.config.js`
(default 1280). The starter renders at 2560×1440, so `p-4` (16px) becomes 32px.
`w-screen`, `h-screen`, `min-w-screen`, `max-w-screen`, `min-h-screen`, and
`max-h-screen` use the exact render width or height. Unitless values (opacity,
z-index, flex, scale, rotation, aspect ratio, relative line height and tracking)
are not scaled. Pixel `tracking-[…]` values are scaled but kept to two decimals.

Every font size is a literal in the bundle, so the font baker finds `text-*`
sizes automatically; they no longer need to be listed in `assets.config.js`.

## Value forms

| Form | Example | Notes |
| --- | --- | --- |
| Theme key | `p-4`, `rounded-lg`, `text-xl` | From the scales below or `tailwind.config.js` |
| Fraction | `w-1/2`, `basis-2/3`, `-left-1/4` | Percent; only where marked **%** |
| `full` | `w-full`, `inset-full` | `100%`; only where marked **%** |
| `screen` | `w-screen`, `max-h-screen` | Render width/height; only where marked **screen** |
| Arbitrary length | `p-[18px]`, `w-[12.5rem]`, `top-[10%]` | `px`, `rem`, or `0`; `%` only where marked **%**. No `em`, `vw`, `vh` |
| Arbitrary number | `opacity-[.35]`, `z-[5]`, `scale-[1.2]` | Unitless |
| Negative | `-mt-2`, `-translate-x-4`, `-rotate-6` | Only where marked **neg** |
| Color | `bg-sky-500`, `bg-[#0af]`, `bg-black/50` | See [Colors](#colors) |

In arbitrary values `_` stands for a space: `bg-[rgb(14_165_233)]`.

### Spacing scale

`0`=0, `px`=1, `0.5`=2, `1`=4, `1.5`=6, `2`=8, `2.5`=10, `3`=12, `3.5`=14,
`4`=16, `5`=20, `6`=24, `7`=28, `8`=32, `9`=36, `10`=40, `11`=44, `12`=48,
`14`=56, `16`=64, `20`=80, `24`=96, `28`=112, `32`=128, `36`=144, `40`=160,
`44`=176, `48`=192, `52`=208, `56`=224, `60`=240, `64`=256, `72`=288, `80`=320,
`96`=384 (logical px).

## Utility reference

### Layout and flexbox

| Class | Style |
| --- | --- |
| `flex` | `display: 'flex'` |
| `hidden` | `display: 'none'` |
| `relative`, `absolute` | `position` |
| `overflow-hidden`, `overflow-visible`, `overflow-scroll` | `overflow` |
| `flex-row`, `flex-row-reverse`, `flex-col`, `flex-col-reverse` | `flexDirection` |
| `flex-wrap`, `flex-wrap-reverse`, `flex-nowrap` | `flexWrap` |
| `flex-<n>`, `flex-[<n>]` | `flex: n` (for example `flex-1`) |
| `flex-auto` | `flexGrow: 1, flexShrink: 1` |
| `flex-initial` | `flexGrow: 0, flexShrink: 1` |
| `flex-none` | `flexGrow: 0, flexShrink: 0` |
| `grow`, `grow-<n>`, `grow-[<n>]` (also `flex-grow-*`) | `flexGrow` (bare = 1) |
| `shrink`, `shrink-<n>`, `shrink-[<n>]` (also `flex-shrink-*`) | `flexShrink` (bare = 1) |
| `basis-*` | `flexBasis`; spacing, **%**, arbitrary |
| `justify-start`, `-end`, `-center`, `-between`, `-around`, `-evenly` | `justifyContent` |
| `items-start`, `-end`, `-center`, `-stretch` | `alignItems` |
| `self-auto`, `-start`, `-end`, `-center`, `-stretch` | `alignSelf` |
| `content-start`, `-end`, `-center`, `-stretch`, `-between`, `-around` | `alignContent` |
| `gap-*`, `gap-x-*`, `gap-y-*` | `gap`, `columnGap`, `rowGap`; spacing, arbitrary px/rem |
| `aspect-square`, `aspect-video`, `aspect-[4/3]` | `aspectRatio` (1, 16/9, w/h) |
| `z-0`, `z-10` … `z-50`, `z-[<int>]` | `zIndex`; **neg** |
| `pointer-events-none`, `-auto`, `-box-none`, `-box-only` | `pointerEvents` (`box-*` are extensions) |

### Spacing

| Prefix | Style keys | Values |
| --- | --- | --- |
| `p`, `px`, `py`, `pt`, `pr`, `pb`, `pl`, `ps`, `pe` | `padding`, `paddingHorizontal`, `paddingVertical`, `paddingTop`, `paddingRight`, `paddingBottom`, `paddingLeft`, `paddingLeft`, `paddingRight` | spacing, arbitrary px/rem |
| `m`, `mx`, `my`, `mt`, `mr`, `mb`, `ml`, `ms`, `me` | `margin…` equivalents | spacing, arbitrary px/rem; **neg** |

`ps`/`ms` map to left and `pe`/`me` to right (LTR only). Percentages and
`m-auto` are rejected.

### Sizing

| Prefix | Style keys | Values |
| --- | --- | --- |
| `w` | `width` | spacing, **%**, **screen**, arbitrary |
| `h` | `height` | spacing, **%**, **screen**, arbitrary |
| `size` | `width` and `height` | spacing, **%**, arbitrary |
| `min-w`, `min-h` | `minWidth`, `minHeight` | spacing, **screen**, arbitrary px/rem |
| `max-w` | `maxWidth` | `xs`=320, `sm`=384, `md`=448, `lg`=512, `xl`=576, `2xl`=672, `3xl`=768, `4xl`=896, `5xl`=1024, `6xl`=1152, `7xl`=1280; spacing, **screen**, arbitrary px/rem |
| `max-h` | `maxHeight` | spacing, **screen**, arbitrary px/rem |

### Position

| Prefix | Style keys | Values |
| --- | --- | --- |
| `inset` | `top`, `right`, `bottom`, `left` | spacing, **%**, arbitrary; **neg** |
| `inset-x`, `inset-y` | `left`+`right`, `top`+`bottom` | same |
| `top`, `right`, `bottom`, `left` | one edge | same |
| `start`, `end` | `left`, `right` (LTR only) | same |

### Colors

Color utilities: `bg-*` (`backgroundColor`), `text-*` (`color`),
`border-*`, `border-{x,y,t,r,b,l}-*` (border colors), `tint-*` (`tintColor`,
for `Image`), `caret-*` (`cursorColor`, the text input cursor).

Accepted colors:

- The Tailwind v3.4.17 palette: `transparent`, `black`, `white`, and `slate`,
  `gray`, `zinc`, `neutral`, `stone`, `red`, `orange`, `amber`, `yellow`,
  `lime`, `green`, `emerald`, `teal`, `cyan`, `sky`, `blue`, `indigo`, `violet`,
  `purple`, `fuchsia`, `pink`, `rose`, each with shades `50`, `100`–`900`, `950`.
  `current` and `inherit` are not available.
- Theme colors from `tailwind.config.js`, including nested `DEFAULT` entries.
- Arbitrary `[#rgb]`, `[#rgba]`, `[#rrggbb]`, `[#rrggbbaa]`, `[rgb(…)]`,
  `[rgba(…)]`, or an engine color name (`transparent`, `black`, `white`, `red`,
  `green`, `blue`, `gray`, `grey`, `yellow`, `cyan`, `magenta`, `orange`).
  `hsl()` and other CSS color syntaxes are rejected. Engine color names other
  than `transparent` do not accept an opacity modifier.

An opacity modifier is accepted on every color utility: `/0`–`/100` in steps of
5, `/[0.35]`, or `/[35%]`. The result is emitted as `#rrggbbaa`, multiplied
with any alpha the color already has. `bg-opacity-*` and similar are rejected.

### Typography

| Class | Style |
| --- | --- |
| `text-xs` … `text-9xl` | `fontSize` and `lineHeight` from the scale below |
| `text-<size>/<leading>` | Size with a line height from the `leading` scale or `[…]`, e.g. `text-lg/7`, `text-xl/[1.2]` |
| `text-[<px or rem>]` | `fontSize` only |
| `font-thin`, `-extralight`, `-light`, `-normal`, `-medium`, `-semibold`, `-bold`, `-extrabold`, `-black`, `font-[<n>]` | `fontWeight` 100–900 |
| `font-<family>`, `font-[<family>]` | `fontFamily` from `theme.fontFamily` |
| `leading-*` | `lineHeight` |
| `tracking-*` | `letterSpacing` |
| `text-left`, `-center`, `-right`, `-start`, `-end` | `textAlign` (`start`/`end` map to left/right) |
| `italic`, `not-italic` | `fontStyle` |
| `underline`, `line-through`, `no-underline` | `textDecorationLine` |
| `truncate` | `numberOfLines: 1, ellipsizeMode: 'tail'` |
| `line-clamp-1` … `line-clamp-6`, `line-clamp-[<n>]` | `numberOfLines: n, ellipsizeMode: 'tail'` |
| `line-clamp-none` | `numberOfLines: 0` |
| `text-ellipsis`, `text-clip` | `ellipsizeMode` `tail` / `clip` |

Font sizes (size / line height, logical px): `xs` 12/16, `sm` 14/20, `base`
16/24, `lg` 18/28, `xl` 20/28, `2xl` 24/32, `3xl` 30/36, `4xl` 36/40, and
`5xl` 48, `6xl` 60, `7xl` 72, `8xl` 96, `9xl` 128 with line height 1×.

Line heights: `leading-3` … `leading-10` are fixed (12–40px); `none` 1,
`tight` 1.25, `snug` 1.375, `normal` 1.5, `relaxed` 1.625, `loose` 2 are
multiples of the font size. In `leading-[…]`, `[1.3]` is relative and `[30px]`
fixed: unitless values are multiples, values with a unit are fixed, as in CSS.
A later `leading-*` overrides the line height that `text-*` sets.

Letter spacing: `tracking-tighter` -0.05em, `tight` -0.025em, `normal` 0,
`wide` 0.025em, `wider` 0.05em, `widest` 0.1em, `tracking-[0.1em]` (relative),
`tracking-[2px]` or `[0.1rem]` (fixed).

Relative line heights and letter spacing need a `text-*` size on the same
element (in the same `className`, or the static part of it for conditional and
variant classes); otherwise the build fails. The engine renders regular and bold
only: `fontWeight` 600 and above is bold, everything else regular.
`font-sans`, `font-serif`, and `font-mono` fail until mapped in the config.

### Borders and radius

| Class | Style |
| --- | --- |
| `border`, `border-0`, `border-2`, `border-4`, `border-8`, `border-[<px>]` | `borderWidth` (bare = 1) |
| `border-x-*`, `border-y-*`, `border-t-*`, `border-r-*`, `border-b-*`, `border-l-*` | Per-edge width or color, same values |
| `border-<color>` | `borderColor` |
| `border-solid`, `border-dashed`, `border-dotted` | `borderStyle` |
| `rounded`, `rounded-{none,sm,md,lg,xl,2xl,3xl,full}`, `rounded-[<px>]` | `borderRadius` (none 0, sm 2, bare 4, md 6, lg 8, xl 12, 2xl 16, 3xl 24, full 9999) |
| `rounded-t-*`, `-r-*`, `-b-*`, `-l-*`, `-tl-*`, `-tr-*`, `-br-*`, `-bl-*` | Corner radii |

### Effects, images, and transforms

| Class | Style |
| --- | --- |
| `opacity-0` … `opacity-100` (steps of 5), `opacity-[<0–1>]` | `opacity` |
| `object-contain`, `-cover`, `-fill`, `-none` | `resizeMode` `contain`, `cover`, `stretch`, `center` |
| `tint-<color>` | `tintColor` (extension) |
| `scale-{0,50,75,90,95,100,105,110,125,150}`, `scale-[<n>]` | `transform: [{scale}]`; **neg** |
| `scale-x-*`, `scale-y-*` | `scaleX`, `scaleY`; **neg** |
| `rotate-{0,1,2,3,6,12,45,90,180}`, `rotate-[<n>deg]`, `rotate-[<n>rad]` | `rotate`; **neg** |
| `translate-x-*`, `translate-y-*` | `translateX`, `translateY`; spacing, arbitrary px/rem; **neg** |
| `origin-center`, `-top`, `-top-right`, `-right`, `-bottom-right`, `-bottom`, `-bottom-left`, `-left`, `-top-left` | `transformOrigin` |
| `transform-none` | `transform: []` |

Transform classes compose into one `transform` array in the order translate,
rotate, scale. A variant such as `focused:scale-105` keeps the element's base
`rotate-*` or `translate-*`. Percentage translations are rejected.

## State variants

| Variant | Reads prop |
| --- | --- |
| `focused:`, `hover:`, `focus:`, `focus-visible:` | `focused` |
| `selected:` | `selected` |
| `disabled:` | `disabled` |
| `active:` | `active` |
| `checked:` | `checked` |
| `pressed:` | `pressed` |

Controller focus plays the role of the pointer hover on the console, so
`hover:`, `focus:`, and `focus-visible:` are aliases of `focused:`.

A variant reads the JSX prop of the same name on the **same element**; the prop
itself is left untouched and still reaches the component. The build fails if
the prop is missing. A bare prop (`<View focused className="…">`) counts as
`true`. Stacked variants such as `focused:selected:bg-sky-600` apply only when
both props are truthy.

The prop expression is evaluated twice (once by the component, once for the
style), so it must be side-effect free: identifiers, member access, literals,
and unary, binary, logical, or conditional expressions. Function calls are
rejected; compute the value into a variable first.

Text styles are not inherited, so a `Text` inside a focused `View` needs its own
`focused` prop to use `focused:text-white`.

## Dynamic classes

`className` must be resolvable at build time. Accepted forms, freely nested:

```jsx
<View className="p-4 bg-slate-800" />
<View className={`p-4 ${active ? 'bg-sky-800' : 'bg-slate-800'}`} />
<View className={done && 'opacity-50'} />
<View className={size === 'lg' ? 'p-6' : size === 'md' ? 'p-4' : 'p-2'} />

const card = 'rounded-xl bg-slate-800 p-4';      // module or function const
<View className={card} />
```

- String and template literals; `cond ? a : b`; `cond && a`; `null`,
  `undefined`, and `false` contribute nothing.
- Template `${}` parts must themselves be one of these forms.
- An identifier must refer to a `const` in the same file whose initializer is one of these forms.
  To share classes across files, export a `tw` style object instead.
- Class names must be whole words: `` `bg-${color}-500` `` is rejected.
- Function calls, props, `let` variables, and state values are rejected. Pick
  between complete class strings with a condition instead.

## tw templates

```jsx
import {tw, Text} from '@ps5-react/core';

const heading = tw`text-3xl font-semibold text-white`;
// <Text style={heading}>Now playing</Text>

function Badge({live}) {
  const badge = tw`rounded-full px-3 py-1 text-sm ${live ? 'bg-red-600' : 'bg-zinc-700'}`;
  return <Text style={badge}>{live ? 'Live' : 'Offline'}</Text>;
}
```

`tw` compiles to a style object (or a style array when it contains conditions)
at build time and accepts the same dynamic forms as `className`. Variants are
not allowed because there is no element to read props from. `tw` must be
imported from `@ps5-react/core`; calling it at runtime without the build step
throws.

## Configuration

An optional `apps/<app>/tailwind.config.js` default-exports the configuration:

```js
export default {
  baseWidth: 1280,
  theme: {
    // Replaces a whole scale.
    borderRadius: {none: 0, DEFAULT: 6, lg: 12, full: 9999},
    extend: {
      // Adds to a scale.
      colors: {brand: {DEFAULT: '#0070d1', dark: '#00439c'}},
      spacing: {18: 72, 'card': '22rem'},
      fontSize: {hero: ['4.5rem', 1.1], label: 15},
      fontFamily: {sans: 'Inter-Regular', display: './fonts/Display-Bold.ttf'},
    },
  },
};
```

- `baseWidth`: logical width that maps to `render.width` (default 1280).
- `theme.<key>` replaces the default scale; `theme.extend.<key>` merges into it.
- Supported keys: `colors`, `spacing`, `fontSize`, `fontFamily`, `fontWeight`,
  `lineHeight`, `letterSpacing`, `borderRadius`, `borderWidth`, `opacity`,
  `zIndex`, `scale`, `rotate`, `aspectRatio`, `maxWidth`, `lineClamp`. Any other
  key is a configuration error.
- Lengths may be numbers (logical px) or `'Npx'` / `'Nrem'` strings.
- `fontSize` entries may be a number, a string, or `[size, lineHeight]`; a line
  height that is a unitless string or a number below 4 is a multiple of the size.
- `fontFamily` entries are a baked font name or a `.ttf`/`.otf` path relative to
  the app directory. A path is imported automatically, so the font is baked
  without editing `index.jsx`. An array uses its first entry.
- Colors must be hex or `rgb()`/`rgba()` to accept an opacity modifier.

## Unsupported utilities

These fail the build with the reason shown. Any other unknown class fails with
`unknown utility`.

| Classes | Reason |
| --- | --- |
| `shadow-*`, `drop-shadow-*` | Shadows are disabled in this engine profile (`ERUI_SHADOWS=OFF`) |
| `space-x-*`, `space-y-*`, `divide-*` | No child selectors; use `gap-*` or borders on children |
| `grid`, `grid-cols-*`, `grid-rows-*`, `col-*`, `row-*`, `col-span-*`, `row-span-*`, `place-*` | Flexbox-only layout; use `flex-row flex-wrap` with `gap-*` |
| `block`, `inline`, `inline-block`, `inline-flex`, `table`, `contents`, `flow-root` | Only `flex` (default) and `hidden` |
| `fixed`, `sticky`, `static` | Only `relative` and `absolute` |
| `uppercase`, `lowercase`, `capitalize`, `normal-case` | No text-transform; transform the string in JavaScript |
| `ring-*`, `outline-*` | Use `border-*` |
| `transition-*`, `duration-*`, `ease-*`, `delay-*`, `animate-*` | No CSS animation; use the Animated API |
| `bg-gradient-*`, `from-*`, `via-*`, `to-*`, `bg-none` | No View gradients |
| `blur`, `brightness`, `contrast`, `grayscale`, `hue-rotate`, `invert`, `saturate`, `sepia`, `backdrop-*`, `filter`, `mix-blend-*`, `bg-blend-*` | No filters or blending |
| `bg-opacity-*`, `text-opacity-*`, `border-opacity-*` | Use a color opacity modifier such as `bg-black/50` |
| `skew-*` | No skew transform |
| `invisible`, `visible`, `collapse` | Use `opacity-0` or `hidden` |
| `overflow-auto`, `overflow-clip`, `overflow-x-*`, `overflow-y-*` | Only hidden, visible, and scroll |
| `items-baseline`, `self-baseline`, `content-evenly`, `justify-normal`, `justify-stretch` | Alignment value not supported by the layout engine |
| `order-*`, `float-*`, `clear-*`, `isolate`, `container`, `sr-only`, `not-sr-only`, `columns-*`, `box-*` | Not supported by the layout engine |
| `cursor-*`, `select-*`, `resize`, `appearance-*`, `scroll-*`, `snap-*`, `touch-*`, `will-change-*`, `accent-*`, `list-*`, `whitespace-*`, `break-*`, `hyphens-*`, `indent-*`, `align-*`, `decoration-*`, `underline-offset-*`, `text-wrap`, `antialiased`, `subpixel-antialiased`, `tabular-nums`, `ordinal`, `object-scale-down`, `object-{left,right,top,bottom}`, `bg-fixed`, `bg-cover`, `bg-contain`, `bg-center`, `bg-no-repeat`, `bg-repeat`, `bg-clip-*`, `bg-origin-*` | Browser-only CSS |
| `w-auto`, `h-auto`, `size-auto`, `basis-auto`, `z-auto`, `max-w-none`, `aspect-auto`, `inset-auto`, `top/right/bottom/left-auto` | `auto` is the default; remove the class |
| `w-min/max/fit`, `h-min/max/fit`, `w-dvw/svw/lvw`, `h-dvh/svh/lvh`, `max-w-prose`, `max-w-min/max/fit`, `max-w-screen-*` | Intrinsic and viewport-unit sizes; use a fixed size |
| Percent or `full` on `min-*`, `max-*`, padding, margin, gap, `translate-*` | Percentages only for width, height, insets, and basis |
| `m-auto`, `mx-auto`, … | No auto margins; use `self-center` or parent alignment |
| `font-sans`, `font-serif`, `font-mono` | No default font stacks; map a font in `theme.fontFamily` |
| `!p-4` (important) | Not needed; later classes and `style` already win |
| Relative `leading-*`/`tracking-*` without `text-*` | Needs a font size on the same element |

Rejected variants:

| Variant | Reason |
| --- | --- |
| `focus-within:` | Use `focused:` with a `focused` prop |
| `dark:` | No color-scheme query; choose colors from app state |
| `first:`, `last:`, `odd:`, `even:` | Need selectors; choose the class from the index |
| `sm:`, `md:`, `lg:`, `xl:`, `2xl:`, `max-*:`, `portrait:`, `landscape:`, `print:`, `motion-safe:`, `motion-reduce:` | The render size is fixed per app |
| Any other prefix | Unknown variant |

## Differences from Tailwind and NativeWind

- Compilation happens entirely at build time; there is no stylesheet, runtime
  class lookup, or CSS interop. Class strings must be static or chosen from
  static alternatives.
- Styles do not cascade. As in React Native, text styles on a `View` do not
  reach its `Text` children.
- Interaction states are explicit props (`focused`, `selected`, …), not
  pseudo-classes; `hover:`/`focus:` read the `focused` prop. There are no
  dark-mode, responsive, or platform variants.
- Lengths are scaled to the app's render width and rounded at build time.
- Percentages work only for width, height, insets, and basis.
- `start`/`end`, `ps`/`pe`, `ms`/`me`, and `text-start`/`text-end` assume LTR.
- Colors with opacity become `#rrggbbaa`; there are no CSS variables.
- Font weights collapse to regular or bold (600 and above).
- Extensions not in Tailwind: `tint-*`, `pointer-events-box-none`,
  `pointer-events-box-only`, and `caret-*` mapped to `cursorColor`.
  `truncate` and `line-clamp-*` map to `numberOfLines` and `ellipsizeMode`.

## Validation

`npm test` runs the compiler unit tests (`tools/tailwind/*.test.mjs` under `node --test`) and the
starter's desktop test. The starter uses `className`, and its framebuffer
snapshots are pixel-identical to the previous explicit-style version.

The palette and default scales are copied from Tailwind CSS v3.4.17 under the
MIT License ([licenses/tailwindcss-LICENSE](../licenses/tailwindcss-LICENSE)).
The `tailwindcss` package is not installed or used.
