// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
// <Image>: bundled assets by name, and http(s) URLs loaded natively at the size they are drawn.
import {createElement, forwardRef, useLayoutEffect, useRef, useState} from 'react';
import {acquireImage} from '../native.js';
import {createFocusable, resolveStyle} from './focusable.js';

const HostImage = createFocusable('Image');
const FITS = {cover: 0, contain: 1, stretch: 2, center: 3, repeat: 3};

/** The URL of a remote `source`, or null for a bundled asset name. */
function remoteUri(source) {
  const uri = typeof source === 'string' ? source : source?.uri;
  return typeof uri === 'string' && /^https?:\/\//i.test(uri) ? uri : null;
}

/** Width and height when the style fixes both in pixels, so loading need not wait for layout. */
function fixedBox(style) {
  let width, height;
  for (const part of [resolveStyle(style, false, false)].flat(Infinity)) {
    if (!part) continue;
    if (part.width !== undefined) width = part.width;
    if (part.height !== undefined) height = part.height;
  }
  return typeof width === 'number' && typeof height === 'number' && width >= 1 && height >= 1
    ? {width: Math.round(width), height: Math.round(height)} : null;
}

function RemoteImage({uri, resizeMode = 'cover', onLoad, onError, onLayout, forwardedRef, ...props}) {
  const fixed = fixedBox(props.style);
  const [laidOut, setLaidOut] = useState(null);
  const [name, setName] = useState('');
  const box = fixed ?? laidOut;
  const fit = FITS[resizeMode] ?? 0;
  const events = useRef(null);
  events.current = {onLoad, onError};

  useLayoutEffect(() => {
    if (!box) return undefined;
    let release = null;
    const handle = result => {
      setName(result.name);
      const {onLoad: loaded, onError: failed} = events.current;
      if (result.state === 'ready') loaded?.({nativeEvent: {source: {uri, width: result.width, height: result.height}}});
      if (result.state === 'failed') {
        if (failed) failed({nativeEvent: {error: result.error}});
        else console.warn(result.error);
      }
    };
    try {
      release = acquireImage(uri, box.width, box.height, fit, handle);
    } catch (error) {
      handle({state: 'failed', name: '', error: error.message});
    }
    return () => release?.();
  }, [uri, box?.width, box?.height, fit]);

  const layout = event => {
    const {width, height} = event.layout;
    if (width >= 1 && height >= 1) {
      setLaidOut(previous => (previous?.width === width && previous?.height === height ? previous : {width, height}));
    }
    onLayout?.(event);
  };
  return createElement(HostImage, {...props, ref: forwardedRef, resizeMode, imageName: name, onLayout: layout});
}

/**
 * React Native's Image. `source` is a bundled asset (an imported file) or `{uri}` with an http(s)
 * URL, which is fetched and decoded off the render thread at the size the image is drawn; the
 * element draws only its own style (such as backgroundColor) until the image arrives.
 */
export const Image = forwardRef((props, ref) => {
  const uri = remoteUri(props.source);
  if (!uri) return createElement(HostImage, {...props, ref});
  const {source, ...rest} = props;
  return createElement(RemoteImage, {...rest, uri, forwardedRef: ref});
});
Image.displayName = 'Image';

/**
 * Loads a URL into the image cache ahead of time for a box of `width` × `height` render pixels,
 * the size an `<Image>` with that `resizeMode` will draw it at. Holds no reference: the image stays
 * cached until the cache needs the room. Resolves when decoded; rejects with the load error.
 * @param {string} uri @param {{width: number, height: number, resizeMode?: string}} box
 * @returns {Promise<void>}
 */
Image.prefetch = (uri, {width, height, resizeMode = 'cover'}) => new Promise((resolve, reject) => {
  let release = null, settled = false;
  release = acquireImage(uri, Math.round(width), Math.round(height), FITS[resizeMode] ?? 0, result => {
    if (result.state === 'loading') return;
    settled = true;
    release?.();
    if (result.state === 'ready') resolve();
    else reject(new Error(result.error));
  });
  if (settled) release();
});
