// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
// A ScrollView that keeps its focused descendant in view by scrolling natively through
// NativeUI.scrollTo (patches/embeddedReact-scroll-to.patch). Layout rectangles stay pre-scroll; the
// focus manager subtracts each enclosing frame's offset.
import {createElement, forwardRef, useContext, useLayoutEffect, useState} from 'react';
import {ScrollView as HostScrollView} from 'embedded-react';
import {approach, revealOffset} from './geometry.js';
import {FrameContext} from './runtime.js';

// 32 logical px of a 1280-wide layout, in screen px like the layout rectangles.
const MARGIN = 32 / 1280;
// The engine cannot animate a scroll offset, so JavaScript steps it once per frame. Each step closes a
// share of the distance (95% within ~200 ms for a single move) and never more than 40 logical px per
// 60 Hz frame: a held key keeps scrolling at that speed instead of restarting a fast ease each repeat,
// which bounds the strip the engine has to rasterize per frame.
const STEP_MS = 16;
const TAU_MS = 65;
const MAX_LOGICAL_PX_PER_FRAME = 40;

function assignRef(ref, value) {
  if (typeof ref === 'function') ref(value);
  else if (ref) ref.current = value;
}

/** @param {import('./manager.js').Frame | null} parent */
function createFrame(parent) {
  const frame = {
    parent, x: 0, y: 0, props: {}, forwardedRef: null, handle: null, viewport: null, timer: null, target: null,
    /** @param {import('./geometry.js').Rect} rect pre-scroll, like `viewport` */
    reveal(rect) {
      const {viewport, handle} = frame;
      if (!viewport || handle == null) return;
      const [fromX, fromY, maxX, maxY] = NativeUI.scrollTo(handle, NaN, NaN);
      const margin = Math.round(screen.width * MARGIN);
      const x = revealOffset(frame.x, rect.x - viewport.x, rect.width, viewport.width, maxX, margin);
      const y = revealOffset(frame.y, rect.y - viewport.y, rect.height, viewport.height, maxY, margin);
      if (x === frame.x && y === frame.y) return;
      frame.x = x;
      frame.y = y;
      frame.animate(fromX, fromY, x, y);
    },
    // A new target while scrolling only moves the target; the running steps carry on from where they are.
    animate(fromX, fromY, toX, toY) {
      frame.target = [toX, toY];
      if (frame.timer !== null) return;
      const maxPerMs = MAX_LOGICAL_PX_PER_FRAME * screen.width / 1280 / (1000 / 60);
      let x = fromX, y = fromY, last = Date.now();
      frame.timer = setInterval(() => {
        const now = Date.now();
        // A late tick advances one frame's worth, not the whole gap: catching up would expose a tall strip,
        // make that frame slower still, and snowball.
        const dt = Math.min(STEP_MS * 1.25, Math.max(1, now - last));
        last = now;
        const wantX = approach(x, frame.target[0], dt, TAU_MS, maxPerMs);
        const wantY = approach(y, frame.target[1], dt, TAU_MS, maxPerMs);
        [x, y] = NativeUI.scrollTo(frame.handle, wantX, wantY);
        // The engine stores offsets as floats. Far from the request means clamped by a content size that
        // changed since the target was chosen: settle where it stopped.
        if (Math.abs(x - wantX) > 0.5) frame.target[0] = x;
        if (Math.abs(y - wantY) > 0.5) frame.target[1] = y;
        if (Math.abs(x - frame.target[0]) > 0.5 || Math.abs(y - frame.target[1]) > 0.5) return;
        frame.stop();
        frame.x = x;
        frame.y = y;
      }, STEP_MS);
    },
    stop() {
      clearInterval(frame.timer);
      frame.timer = null;
    },
    onLayout(event) {
      frame.viewport = event.layout;
      frame.props.onLayout?.(event);
    },
    // While focus scrolling animates, x/y already hold its target; touch scrolling updates them live.
    onScroll(event) {
      if (frame.timer === null) {
        frame.x = event.scrollX;
        frame.y = event.scrollY;
      }
      frame.props.onScroll?.(event);
    },
    ref(handle) {
      frame.handle = handle;
      assignRef(frame.forwardedRef, handle);
    },
  };
  return frame;
}

/**
 * Embedded React's ScrollView, plus scrolling its focused descendant into view (minimal movement
 * with a margin, eased and speed-capped).
 */
export const ScrollView = forwardRef((props, ref) => {
  const parent = useContext(FrameContext);
  const [frame] = useState(() => createFrame(parent));
  frame.props = props;
  frame.forwardedRef = ref;
  useLayoutEffect(() => () => frame.stop(), []);
  const {children, ...rest} = props;
  return createElement(HostScrollView, {...rest, ref: frame.ref, onLayout: frame.onLayout, onScroll: frame.onScroll},
    createElement(FrameContext.Provider, {value: frame}, children));
});
ScrollView.displayName = 'ScrollView';
