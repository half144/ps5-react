// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
// A ScrollView that keeps its focused descendant in view by scrolling natively through
// NativeUI.scrollTo (patches/embeddedReact-scroll-to.patch). Layout rectangles stay pre-scroll; the
// focus manager subtracts each enclosing frame's offset.
import {createElement, forwardRef, useContext, useLayoutEffect, useState} from 'react';
import {ScrollView as HostScrollView} from 'embedded-react';
import {revealOffset} from './geometry.js';
import {FrameContext} from './runtime.js';

// 32 logical px of a 1280-wide layout, in screen px like the layout rectangles.
const MARGIN = 32 / 1280;
// The engine cannot animate a scroll offset, so JavaScript steps it once per frame, briefly.
const DURATION_MS = 250;
const STEP_MS = 16;
const easeOut = t => 1 - (1 - t) ** 3;

function assignRef(ref, value) {
  if (typeof ref === 'function') ref(value);
  else if (ref) ref.current = value;
}

/** @param {import('./manager.js').Frame | null} parent */
function createFrame(parent) {
  const frame = {
    parent, x: 0, y: 0, props: {}, forwardedRef: null, handle: null, viewport: null, timer: null,
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
    animate(fromX, fromY, toX, toY) {
      clearInterval(frame.timer);
      const start = Date.now();
      frame.timer = setInterval(() => {
        const t = Math.min(1, (Date.now() - start) / DURATION_MS);
        const k = easeOut(t);
        const [x, y] = NativeUI.scrollTo(frame.handle, fromX + (toX - fromX) * k, fromY + (toY - fromY) * k);
        if (t < 1) return;
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
 * with a margin, eased over 250 ms).
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
