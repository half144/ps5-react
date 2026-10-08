# International text

Text is drawn by Embedded React's software renderer. Glyphs come from two
places:

- **Baked fonts.** An imported TTF/OTF is rasterized at build time, at the
  sizes the app uses, into bitmap glyphs compiled into the executable. This is
  the fast path, and the only one for text the baked font fully covers.
- **Runtime fonts.** Characters the baked font lacks are drawn from subset Noto
  fonts the app packages with `textFonts`, shaped by HarfBuzz and rasterized on
  demand (`native/shared/text_shaper.cpp`). A string goes to this path only
  when it contains such a character; its baked characters keep their baked
  glyphs, so Latin text inside a Japanese sentence still uses the app's font.

## Baked glyph sets

A baked font covers printable ASCII plus the codepoints its `assets.config.js`
entry names. Text known at build time is checked by the font baker, which warns
about missing glyphs. Text that arrives at runtime (catalog titles, user names)
cannot be checked, so pick a named set that covers the languages you show:

| `glyphs` | Adds |
| --- | --- |
| `'common'` | Typographic quotes, dashes, bullets, currency and math signs |
| `'latin-ext'` | `common`, Latin-1 Supplement, Latin Extended-A, Ș Ț ẞ, `„ ‚ ‹ ›`, `€ ₹ ₺ ₽ №` |
| `'cyrillic'` | `common`, U+0400–U+045F, Ґ ґ, № |
| `'european'` | `latin-ext`, `cyrillic` and modern Greek (tonos, final sigma) |

`'latin-ext'` covers Spanish, French, German, Portuguese, Indonesian and the
other Latin-alphabet European languages; Vietnamese is not included.
`'european'` adds about 470 glyphs to each baked size (about 65 KB at 24 px with
the default 4 bits per pixel).

```js
export default {
  fonts: {'Inter-Regular': {glyphs: 'european', extraGlyphs: '…'}},
};
```

A codepoint the baked font lacks draws as `?`, unless a runtime font has it.

## Runtime fonts

List the scripts an app shows in `app.json`:

```json
"textFonts": ["chinese", "japanese"]
```

| Script | Font (subset) | Characters | Package |
| --- | --- | --- | --- |
| `chinese` | Noto Sans SC | GB 2312: 6,763 hanzi, CJK punctuation, full-width forms | 1.6 MB |
| `japanese` | Noto Sans JP | JIS X 0208: kana, 6,355 kanji (levels 1 and 2), punctuation | 1.7 MB |

The build pins each font in `dependencies.lock.json`, subsets it with HarfBuzz
(`tools/font_subset.cpp`, cached in `.deps/fonts/subset/`) and packages it in
`fonts/` with its OFL notice in `notices/Noto-fonts-LICENSE.txt`. The desktop
preview reads the same files from `.build/<app>/generated/fonts/`. Han
characters outside one font fall back to the other when both are listed, so a
Japanese UI that lists both also draws kanji beyond JIS X 0208.

Kana always uses the Japanese font. Han characters shared by both use the
Chinese forms unless the language is Japanese: the shaper starts with the
system language (`DeviceInfo.get().language`) and `Fonts.setLanguage(tag)`
overrides it (see [Native API](NATIVE-API.md)).

### Memory

Nothing is loaded until a string needs a runtime font. Then the font file is
read whole and kept (1.6 MB for Chinese, 1.7 MB for Japanese). Rasterized
glyphs live in one least-recently-used cache capped at 4 MiB, about 7,000 CJK
glyphs at 24 px; the 32 most recent shaped strings are kept as well (a few KB
each). An app showing both CJK scripts therefore holds at most about 7.5 MB
for text, from the title heap, never the JavaScript heap. HarfBuzz adds about
0.7 MB of code to the executable.

### Line breaking

Runtime text breaks where the bitmap path does (newlines, spaces, inside a
word only when nothing else fits) and also between any two CJK characters,
following the basic kinsoku rules: closing punctuation (`。、，」』）`), small
kana, `ー` and iteration marks never start a line, and opening brackets
(`「『（`) never end one. `numberOfLines` truncates with `…` (from the baked
font, or `...` when it lacks one).
