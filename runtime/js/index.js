// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
import {useLayoutEffect, useRef} from 'react';
export {AppRegistry, View, Text, Image, ScrollView} from 'embedded-react';

const handlers = new Set();

// ABI v1: hosts emit previous/next/confirm/back on the JS/render thread.
globalThis.__ps5ReactDispatch = action => {
  for (const handler of handlers) handler(action);
};

/**
 * Subscribe while mounted; Options is reserved for the host's exit action.
 * @param {(action: 'previous' | 'next' | 'confirm' | 'back') => void} handler
 */
export function useController(handler) {
  const current = useRef(handler);
  useLayoutEffect(() => { current.current = handler; });
  useLayoutEffect(() => {
    const listener = action => current.current(action);
    handlers.add(listener);
    return () => handlers.delete(listener);
  }, []);
}
