// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
// A ScrollView that keeps its focused descendant in view by scrolling natively through
// NativeUI.scrollTo (patches/embeddedReact-scroll-to.patch). Layout rectangles stay pre-scroll; the
// focus manager subtracts each enclosing frame's offset.
import {createElement, forwardRef, useContext, useLayoutEffect, useState} from 'react';
import {ScrollView as HostScrollView} from 'embedded-react';
import {onFrame} from '../frame.js';
import {revealOffset, scrollStep} from './geometry.js';
import {FrameContext} from './runtime.js';

// 32 logical px of a 1280-wide layout, in screen px like the layout rectangles.
const MARGIN = 32 / 1280;
// The engine cannot animate a scroll offset, so JavaScript steps it on every presented frame (the
// host's frame callback, not a timer that drifts against the display), in whole pixels: up to 40
// logical px per 60 Hz frame, reached in four frames, then braking to stop on the target. A held
// key scrolls at that one steady speed, which also bounds the strip the engine rasterizes per frame.
const MAX_LOGICAL_PX_PER_FRAME = 40;
const ACCEL_LOGICAL_PX = 12;
const BRAKE_LOGICAL_PX = 4;
const FRAME_MS = 1000 / 60;

function assignRef(ref, value) {
  if (typeof ref === 'function') ref(value);
  else if (ref) ref.current = value;
}

/** @param {import('./manager.js').Frame | null} parent */
function createFrame(parent) {
  const frame = {
    parent, x: 0, y: 0, props: {}, forwardedRef: null, handle: null, viewport: null, timer: null, target: null,
    speed: [0, 0],
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
    // A new target while scrolling only moves the target; the steps carry on at the current speed.
    animate(fromX, fromY, toX, toY) {
      frame.target = [Math.round(toX), Math.round(toY)];
      if (frame.timer !== null) return;
      const scale = screen.width / 1280;
      const [maxSpeed, accel, brake] = [MAX_LOGICAL_PX_PER_FRAME, ACCEL_LOGICAL_PX, BRAKE_LOGICAL_PX]
        .map(logical => Math.max(1, Math.round(logical * scale)));
      let x = Math.round(fromX), y = Math.round(fromY);
      frame.speed = [0, 0];
      frame.timer = onFrame(elapsedMs => {
        // A late frame advances the vblanks it covered, at most two, so a hitch is not followed by a jump.
        for (let n = Math.min(2, Math.max(1, Math.round(elapsedMs / FRAME_MS))); n > 0; n--) {
          let speedX, speedY;
          [x, speedX] = scrollStep(x, frame.speed[0], frame.target[0], maxSpeed, accel, brake);
          [y, speedY] = scrollStep(y, frame.speed[1], frame.target[1], maxSpeed, accel, brake);
          frame.speed = [speedX, speedY];
        }
        const [atX, atY] = NativeUI.scrollTo(frame.handle, x, y);
        // The engine stores offsets as floats. Far from the request means clamped by a content size that
        // changed since the target was chosen: settle where it stopped.
        if (Math.abs(atX - x) > 0.5) frame.target[0] = x = Math.round(atX);
        if (Math.abs(atY - y) > 0.5) frame.target[1] = y = Math.round(atY);
        if (x !== frame.target[0] || y !== frame.target[1]) return;
        frame.stop();
        frame.x = x;
        frame.y = y;
      });
    },
    stop() {
      frame.timer?.();
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
