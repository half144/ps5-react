// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
import {createElement, useCallback, useContext, useLayoutEffect, useState} from 'react';
import {manager, ScopeContext} from './runtime.js';

/**
 * Groups focusable elements: navigation searches the innermost scope first, then its parents.
 * @param {import('./manager.js').ScopeProps & {children?: import('react').ReactNode}} props
 *   `restoreFocus` defaults to true.
 */
export function FocusScope(props) {
  const parent = useContext(ScopeContext);
  const [scope] = useState(() => manager.createScope(parent, props));
  useLayoutEffect(() => { manager.setProps(scope, props); });
  useLayoutEffect(() => {
    manager.mountScope(scope);
    return () => manager.unmountScope(scope);
  }, []);
  return createElement(ScopeContext.Provider, {value: scope}, props.children);
}

const focusedKey = () => manager.focused?.props.focusKey ?? null;

/**
 * The focused element's key, and imperative focus by `focusKey` (an element or a FocusScope).
 * Re-renders the caller when the focused key changes.
 * @returns {{focusedKey: string | null, focus: (key: string) => boolean, blur: () => void}}
 */
export function useFocus() {
  const [key, setKey] = useState(focusedKey);
  useLayoutEffect(() => {
    setKey(focusedKey());
    return manager.subscribe(() => setKey(focusedKey()));
  }, []);
  const focus = useCallback(target => manager.focus(target), []);
  const blur = useCallback(() => manager.blur(), []);
  return {focusedKey: key, focus, blur};
}
