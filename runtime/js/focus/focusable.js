// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
// Focusable elements: one manager node per element, re-rendered only when that node changes.
import {createElement, forwardRef, useContext, useLayoutEffect, useReducer, useRef} from 'react';
import {FrameContext, manager, NodeContext, ScopeContext} from './runtime.js';

/**
 * @typedef {import('./manager.js').NodeProps & {focusable?: boolean,
 *   onLayout?: (event: {layout: import('./geometry.js').Rect}) => void}} FocusableOptions
 */

/** Props that only the focus runtime reads; they never reach the host element. */
export const FOCUS_PROPS = ['focusable', 'autoFocus', 'focusKey', 'nextFocusUp', 'nextFocusDown', 'nextFocusLeft',
  'nextFocusRight', 'onFocus', 'onBlur'];

/** @param {object} props @param {boolean} [byDefault] focusable without any focus prop (Pressable) */
export function isFocusable(props, byDefault = false) {
  return props.focusable ?? (byDefault || props.onPress != null || props.autoFocus === true || props.focusKey != null);
}

/** Calls a `({focused, pressed}) => style` style; other styles pass through. */
export function resolveStyle(style, focused, pressed) {
  return typeof style === 'function' ? style({focused, pressed}) : style;
}

/** Subscribes the calling component to `node`'s focus and press changes while mounted. */
function useNodeState(node) {
  const [, rerender] = useReducer(count => count + 1, 0);
  useLayoutEffect(() => {
    if (!node) return undefined;
    node.listeners.add(rerender);
    return () => { node.listeners.delete(rerender); };
  }, [node]);
}

/**
 * The element's manager node while `enabled`, else null; the hooks run either way, so a component
 * can decide per render whether it is focusable.
 * @param {FocusableOptions} options
 * @param {boolean} [enabled]
 */
export function useFocusNode(options, enabled = true) {
  const scope = useContext(ScopeContext);
  const frame = useContext(FrameContext);
  const self = useRef(null);
  if (enabled && !self.current) {
    const node = manager.createNode(scope, frame, options);
    self.current = {node, onLayout: event => {
      manager.setRect(node, event.layout);
      node.props.onLayout?.(event);
    }};
  }
  const node = enabled ? self.current.node : null;
  useNodeState(node);
  useLayoutEffect(() => {
    if (node) manager.setProps(node, options);
  });
  useLayoutEffect(() => {
    if (!node) return undefined;
    manager.register(node);
    return () => manager.unregister(node);
  }, [node]);
  if (!node) return null;
  return {node, focused: manager.focused === node, pressed: node.pressed,
    focusProps: {onLayout: self.current.onLayout, onPress: options.onPress}};
}

/**
 * Focus for a custom component: spread `focusProps` on the host element that should be navigable.
 * `useIsFocused` below it needs one of the focusable elements from `@ps5-react/core` instead.
 * @param {FocusableOptions} [options]
 * @returns {{focused: boolean, pressed: boolean,
 *   focusProps: {onLayout: Function, onPress?: Function, focusable: false}}}
 */
export function useFocusable(options = {}) {
  const {focused, pressed, focusProps} = useFocusNode(options);
  // The hook owns the node, so the core View it is spread on must not register a second one.
  return {focused, pressed, focusProps: {...focusProps, focusable: false}};
}

/** Whether the nearest focusable ancestor element has focus. @returns {boolean} */
export function useIsFocused() {
  const node = useContext(NodeContext);
  useNodeState(node);
  return node !== null && manager.focused === node;
}

function Focusable({host, props, forwardedRef}) {
  const {node, focused, pressed, focusProps} = useFocusNode(props);
  const rest = {ref: forwardedRef};
  for (const key in props) if (!FOCUS_PROPS.includes(key)) rest[key] = props[key];
  rest.style = resolveStyle(props.style, focused, pressed);
  rest.onLayout = focusProps.onLayout;
  return createElement(NodeContext.Provider, {value: node}, createElement(host, rest));
}

/**
 * A host component that joins D-pad navigation when it is focusable; otherwise it renders the host
 * element directly, so plain elements carry no focus state.
 * @param {string} host Embedded React host tag
 * @param {boolean} [byDefault] focusable without `focusable`/`onPress`/`autoFocus`/`focusKey`
 */
export function createFocusable(host, byDefault = false) {
  const Component = forwardRef((props, ref) => {
    if (isFocusable(props, byDefault)) return createElement(Focusable, {host, props, forwardedRef: ref});
    if (ref === null && typeof props.style !== 'function' && !('focusable' in props)) return createElement(host, props);
    const rest = {ref};
    for (const key in props) if (!FOCUS_PROPS.includes(key)) rest[key] = props[key];
    rest.style = resolveStyle(props.style, false, false);
    return createElement(host, rest);
  });
  Component.displayName = host;
  return Component;
}
