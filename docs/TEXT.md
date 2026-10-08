# International text

Text is drawn by Embedded React's software renderer from baked fonts: an
imported TTF/OTF is rasterized at build time, at the sizes the app uses, into
bitmap glyphs compiled into the executable.

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

A codepoint the baked font lacks draws as `?`.
