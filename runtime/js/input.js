// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
const listeners = new Set();

// ABI v2: hosts emit up/down/left/right/confirm/back on the JS/render thread.
globalThis.__ps5ReactDispatch = action => {
  for (const listener of listeners) listener(action);
};

/**
 * Internal: receive every host action until the returned function is called.
 * @param {(action: 'up' | 'down' | 'left' | 'right' | 'confirm' | 'back') => void} listener
 */
export function subscribeInput(listener) {
  listeners.add(listener);
  return () => { listeners.delete(listener); };
}
