# Scroll layer

Scroll by copy (see [ANIMATION.md](ANIMATION.md)) repaints only what scrolls into
view, but it still rasterizes that strip on the CPU in the scroll frame, along
with whatever is animating at the same time. On the PS5 this leaves some
held-key scroll frames above 20 ms.

The scroll layer moves that work out of the scroll frame:

- the page's content is rasterized once into a tall buffer;
- the GPU shows the visible window of that buffer;
- scrolling only changes which window is shown.

## Model

The engine stays the source of truth for every pixel.

- **Layer space.** One ScrollView's content is laid out unscrolled. A
  descendant's layer position is its screen position with that ScrollView's
  offset taken out. The layer is as wide as the ScrollView and as tall as its
  content or its viewport, whichever is taller.
- **GPU-resident.** The layer exists only as a GPU texture. The engine paints
  it region by region, each at most 128 rows tall: a region starts with the
  color under the viewport, then the children are drawn over it, all inside the
  region's clip. Nothing reads pixels outside the region, so shadows, gradients,
  transforms and opacity across a region edge come out the same as in one
  pass. The software backend stages one region (`layer_begin`), and the host
  uploads it into the texture (`layer_end`). The staging buffer is at most
  1920 × 128 × 4 bytes (1 MiB).
- **Two passes.** A commit paints the screen as before, except that the layered
  ScrollView paints its own background but not its children. A second pass
  paints the layer's regions. Both passes use the same software rasterizer.
- **Damage.** The damage pre-pass runs once per space: nodes in the layer damage
  layer rects, and every other node damages screen rects. Changing the scroll
  offset damages nothing, because the content did not change.
- **Bands.** The layer is tracked in bands of 128 rows, each either current or
  stale:
  - Damage on the viewport's bands, or within two bands of them, is painted in
    the commit.
  - Damage farther away only marks its bands stale, so an off-screen animation
    or layout shift costs no raster.
  - A commit paints every stale band the viewport shows.
  - The host paints the other stale bands during spare frame time
    (`er_scroll_layer_prefetch`), nearest to the viewport first, starting in the
    direction of the last scroll. A band that keeps changing is skipped until it
    has been quiet for three commits.
- **Composite.** The presenter keeps a texture for the screen and one for the
  layer. It draws the screen, then draws the layer's visible window over the
  viewport. `er_scroll_layer_get` reports the viewport rect and which layer
  pixel lands at its top-left corner, as of the last commit.
- **Uploads.** Both textures are filled through a pixel buffer. On the PS5,
  `glTexSubImage2D` from client memory costs about 17 µs per thousand pixels.
  Writing the same rows into a buffer takes well under a microsecond per
  thousand pixels. Sampling a buffer texture directly was tried once and not
  measured: the console entered rest mode during the run ([HARDWARE.md](HARDWARE.md)).

## When a ScrollView is layered

The engine layers at most one ScrollView: the largest one that scrolls
vertically and meets every condition below. Every other ScrollView, and any
page that fails a condition, keeps scroll by copy, the existing path.

- The backend implements `layer_begin` (the PS5 and desktop hosts do), with a
  single display buffer, no banded backend, and the on-screen keyboard inactive.
- The ScrollView is visible, does not scroll horizontally, is not inside another
  ScrollView, and its viewport covers at least a quarter of the screen.
- Neither the ScrollView nor its ancestors have a transform or opacity.
- Behind the viewport, the ScrollView and its ancestors paint one opaque color,
  with no border, radius, gradient or shadow there. That color is the layer's
  underlay.
- Nothing outside the ScrollView's content paints into the viewport, such as
  an overlay, a menu or a toast. While anything does, the ScrollView is not
  layered.
- Its content contains no Modal and fits in the engine's 16,383-pixel paint range.

Entering the layer paints the visible bands at once, which costs about as much
as a full viewport repaint. Leaving the layer repaints the viewport on the
screen. A ScrollView nested inside the layered one repaints its own viewport
when it scrolls, instead of copying.

## Memory

The process heap holds only the staging buffer: at most 1 MiB, allocated on
first use. The layer costs its width × height × 4 bytes of GPU texture. The
texture grows in steps of 1,024 rows, so a change in content height rarely
reallocates it. Reallocating loses its pixels, so the engine repaints the
visible bands and prefetches the rest. The hosts refuse a layer taller than
8,192 rows, which is 60 MiB at 1,920 px wide; a taller page keeps scroll by
copy.

In the Overdrive example, the browse page's layer is 1920 × 3603: 30 MiB of
texture, allocated as 4,096 rows. The game page's layer is 1920 × 2962. The
hosts log `scroll layer #n: …` each time a ScrollView enters the layer.

## Validation

- `PS5_REACT_SCROLL_LAYER=0` turns the layer off on the desktop. On the PS5, a
  `dev/scroll-layer-off` file in a test deploy does the same.
- `PS5_REACT_LAYER_CHECK=<frames>` (desktop) reads back what GL drew once per
  that many frames. It compares the readback with the viewport repainted on the
  framebuffer without the layer, then puts the layer back. It needs the
  default window, where one framebuffer pixel is one drawable pixel.
  - Held-key scripts on the Overdrive browse page report no differences.
  - On the game page, during its opacity entrance, the check can show transient
    differences. In those same frames the engine without the layer already
    differs from a full repaint.
- The engine test `test_scroll_layer` stages each region in a buffer filled
  with garbage, so any pixel the engine skips would show. It composites the
  layer after:
  - scrolls;
  - a shadow, gradient, scale and opacity across a region edge;
  - content changes and layout shifts;
  - removed and added rows;
  - a nested horizontal scroll;
  - an overlay;
  - a refused layer.

  Each time it compares the result with a full repaint without the layer.
