# Remote images

`<Image>` from `@ps5-react/core` draws bundled assets (imported files, baked
into the executable) and images from `http://` or `https://` URLs:

```jsx
import {Image} from '@ps5-react/core';

<Image source={{uri: game.coverUrl}} resizeMode="cover"
  className="w-[150px] h-[200px] rounded-md bg-surface" />
```

URL images are fetched and decoded natively, off the render thread, at the size
they are drawn, so remote art adds nothing to the build. On the PS5 they need
`networking: true` (see [NETWORKING.md](NETWORKING.md)); desktop preview loads
them with no opt-in.

## Drawing

- **Size.** The image is decoded for the element's box in render pixels. When
  the style fixes both `width` and `height` in pixels, loading starts on mount;
  otherwise it starts after the first layout (one frame later). A box that
  changes size loads again for the new size.
- **Fit.** `resizeMode` `cover` (the default) crops to the box's aspect ratio,
  `contain` keeps the whole image inside it, `stretch` fills it, and `center` or
  `repeat` keep the source size. The image is shrunk to the box with an area
  (box) filter but never enlarged; a source smaller than its box is scaled up by
  the engine when drawn.
- **Format.** Pixels are premultiplied ARGB8888, registered with their known
  opacity. An opaque image drawn at its decoded size takes the engine's plain
  copy path, the cheapest way the software backend draws a bitmap; RGB565
  would halve memory but this backend expands it row by row on every repaint.
- **Placeholder.** Until the image arrives the element draws only its own style:
  give it a `backgroundColor` (as above) for a tile of that color. The image
  replaces it in one frame, without a fade.
- **Events.** `onLoad({nativeEvent: {source: {uri, width, height}}})` reports
  the decoded size, `onError({nativeEvent: {error}})` the failure. Without
  `onError`, failures are logged with `console.warn`. Messages name the call and
  the URL, for example `Image.load https://example.com/a.jpg: HTTP 404`.
- **Lifetime.** Unmounting the element or changing its `source` releases the
  image; a fetch or decode that nothing else is waiting for is cancelled.

`Image.prefetch(uri, {width, height, resizeMode})` loads a URL ahead of time for
a box (pass the same style size the `<Image>` will have) and resolves when it is
decoded. It holds no reference: the image waits in the cache until it is drawn
or the cache needs the room. Use it for the next slide of a carousel or the
other shots of a gallery.

## Formats and limits

| Limit | Value |
| --- | --- |
| Formats | JPEG (baseline and progressive) and PNG, decoded by stb_image 2.30; others fail with an actionable error |
| Encoded size | 8 MiB per response |
| Source size | 5 megapixels (for example 3840 × 1300) |
| Request | GET, 30-second total timeout, the shared transport policy of [NETWORKING.md](NETWORKING.md) (verified TLS, at most five redirects, identity encoding) |
| Retries | Two more attempts after connection failures, timeouts and HTTP 429/502/503/504 |

## Memory and threads

Two native workers own the work: one fetches over a dedicated libcurl multi
handle (up to four connections, separate from the download queue, so images
never wait behind a large file), one decodes. Workers never touch JavaScript or
the engine. The render thread only submits, releases and polls once per frame
while loads are pending, then registers finished pixels with the engine.

| Budget | Value |
| --- | --- |
| Decoded cache | 32 MiB and at most 128 images. Images in use are never evicted, so a screen that draws more than that exceeds it; unused ones are evicted least recently used first |
| Encoded cache | 8 MiB of recently fetched bytes by URL, so the same image at another size decodes without a second fetch. Loads of a URL already being fetched join that request |
| Decode | One image at a time; a decode holds about 4.5 bytes per source pixel for JPEG and 8 for PNG (up to 40 MiB at the source limit) until it finishes |
| Threads | Two, with 1 MiB stacks |

All of it comes from the 128 MiB process heap shared with QuickJS, the
framebuffer and the download buffers. Hosts stop the image workers and free
every image before shutting the runtime down.

Fetched bytes are also kept on disk in the app's data directory
(`FileSystem.dataDir` + `/.cache/images`, at most 64 MiB, least recently used
first), so a relaunch decodes art without the network. Entries are fetched
again after a week; responses over 16 MiB are not stored. Deleting the
directory clears the cache.

## Hardware status

See [HARDWARE.md](HARDWARE.md) for what has been measured on a console. Desktop
tests (`npm test` runs `tools/test_images.py` against a local HTTP server) do
not establish PS5 behavior.
