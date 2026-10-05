// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
// A ScrollView that keeps its focused descendant in view. The bridge has no imperative scrollTo, so
// focus scrolling translates a content View with engine-driven values; touch scrolling stays native.
import {createElement, forwardRef, useContext, useLayoutEffect, useState} from 'react';
import {ScrollView as HostScrollView, View} from 'embedded-react';
import {engine} from '../motion/engine.js';
import {transitions} from '../motion/transitions.js';
import {MotionValues} from '../motion/values.js';
import {revealOffset} from './geometry.js';
import {FrameContext} from './runtime.js';

// Layout props that arrange the children, so they move to the content View.
const CONTENT = ['flexWrap', 'alignItems', 'justifyContent', 'alignContent', 'gap', 'rowGap', 'columnGap'];
// 32 logical px of a 1280-wide layout, in screen px like the layout rectangles.
const MARGIN = 32 / 1280;

function flatten(style, out = {}) {
  if (Array.isArray(style)) for (const part of style) flatten(part, out);
  else if (style) Object.assign(out, style);
  return out;
}

/** Offset that brings `[start, start + size)` into the viewport on one axis, all in screen px. */
function axisOffset(offset, start, size, viewStart, viewSize, contentStart, contentSize, margin) {
  // The content View sits inside the ScrollView's padding; assume the trailing padding matches the leading one.
  const padding = contentStart - viewStart;
  const max = Math.max(0, padding + contentSize + padding - viewSize);
  return revealOffset(offset, start - viewStart, size, viewSize, max, margin);
}

/** @param {import('./manager.js').Frame | null} parent */
function createFrame(parent) {
  const values = new MotionValues(engine, 1);
  values.ensure('x', 0);
  values.ensure('y', 0);
  const frame = {
    parent, x: 0, y: 0, values, props: {},
    native: {x: 0, y: 0},
    translate: {x: 0, y: 0},
    viewport: null,
    content: null,
    /** @param {import('./geometry.js').Rect} rect */
    reveal(rect) {
      const {viewport, content} = frame;
      if (!viewport || !content) return;
      const margin = Math.round(screen.width * MARGIN);
      const x = axisOffset(frame.x, rect.x, rect.width, viewport.x, viewport.width, content.x, content.width, margin);
      const y = axisOffset(frame.y, rect.y, rect.height, viewport.y, viewport.height, content.y, content.height,
        margin);
      if (x === frame.x && y === frame.y) return;
      frame.translate = {x: x - frame.native.x, y: y - frame.native.y};
      frame.x = x;
      frame.y = y;
      values.apply({x: {value: -frame.translate.x, transition: transitions.focus},
        y: {value: -frame.translate.y, transition: transitions.focus}}, () => {});
    },
    onViewportLayout(event) {
      frame.viewport = event.layout;
      frame.props.onLayout?.(event);
    },
    onContentLayout(event) {
      frame.content = event.layout;
    },
    onScroll(event) {
      frame.native = {x: event.scrollX, y: event.scrollY};
      frame.x = event.scrollX + frame.translate.x;
      frame.y = event.scrollY + frame.translate.y;
      frame.props.onScroll?.(event);
    },
  };
  return frame;
}

/**
 * Embedded React's ScrollView, plus scrolling its focused descendant into view (minimal movement
 * with a margin, animated with `transitions.focus`).
 */
export const ScrollView = forwardRef((props, ref) => {
  const parent = useContext(FrameContext);
  const [frame] = useState(() => createFrame(parent));
  frame.props = props;
  useLayoutEffect(() => () => frame.values.dispose(), []);
  const {children, style, onLayout, onScroll, ...rest} = props;
  const outer = flatten(style);
  const inner = {flexDirection: outer.flexDirection ?? 'column',
    transform: [...frame.values.bound()].map(([key, value]) => ({[key === 'x' ? 'translateX' : 'translateY']: value}))};
  for (const key of CONTENT) {
    if (key in outer) {
      inner[key] = outer[key];
      delete outer[key];
    }
  }
  const content = createElement(View, {style: inner, onLayout: frame.onContentLayout}, children);
  return createElement(HostScrollView, {...rest, ref, style: outer, onLayout: frame.onViewportLayout,
    onScroll: frame.onScroll}, createElement(FrameContext.Provider, {value: frame}, content));
});
ScrollView.displayName = 'ScrollView';
