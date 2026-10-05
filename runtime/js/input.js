// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
const listeners = new Set();

const pending = [];

function deliver() {
  for (const action of pending.splice(0)) for (const listener of listeners) listener(action);
}

// ABI v2: hosts emit up/down/left/right/confirm/back on the JS/render thread, before the frame's
// pump. Actions are delivered from the pump's microtask drain, which runs inside React's batch like
// the bridge's own events: a direct host call is outside it, so each setState of a handler would
// render and commit on its own, and the frame log would count that render outside `react`.
globalThis.__ps5ReactDispatch = action => {
  if (pending.push(action) === 1) Promise.resolve().then(deliver);
};

/**
 * Internal: receive every host action until the returned function is called.
 * @param {(action: 'up' | 'down' | 'left' | 'right' | 'confirm' | 'back') => void} listener
 */
export function subscribeInput(listener) {
  listeners.add(listener);
  return () => { listeners.delete(listener); };
}
