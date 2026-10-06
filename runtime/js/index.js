// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
import {useLayoutEffect, useRef} from 'react';
import {subscribeInput} from './input.js';
export {AppRegistry, Text, Animated, useAnimatedValue, Easing, LayoutAnimation, Svg, Path, Circle, Rect, Line, G}
  from 'embedded-react';
export {View, Image, Pressable, ScrollView, VirtualList, FocusScope, useFocusable, useFocus, useIsFocused} from './focus/index.js';
export {motion, AnimatePresence, transitions} from './motion/index.js';
export {DownloadFormats} from './download-formats.js';
export {Platform, DeviceInfo, FileSystem, Notifications, Users, Controller, useGamepad, Linking, Power, BackHandler,
  Http, Downloads} from './native.js';

// ABI v1 compatibility: each direction is followed by its legacy action.
const legacyActions = {up: 'previous', left: 'previous', down: 'next', right: 'next'};

/**
 * Subscribe while mounted; Options is reserved for the host's exit action.
 * @param {(action: import('./input.js').Action | 'previous' | 'next') => void} handler
 */
export function useController(handler) {
  const current = useRef(handler);
  useLayoutEffect(() => { current.current = handler; });
  useLayoutEffect(() => subscribeInput(action => {
    current.current(action);
    if (legacyActions[action]) current.current(legacyActions[action]);
  }), []);
}

/**
 * Tailwind-style classes → a style object, compiled by the build (see docs/TAILWIND.md).
 * @param {TemplateStringsArray} classes
 * @returns {object}
 */
export function tw(classes) {
  throw new Error(`tw\`${classes.join('')}\` was not compiled; use it in an app source file`);
}
