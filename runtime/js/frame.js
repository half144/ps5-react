// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
const callbacks = new Set();
let batch = run => run();

// Hosts call this once per frame, after the pump and before the commit, with the time the frame
// advances: on the PS5 a whole number of vblanks. Motion stepped here moves once per presented frame;
// a timer drifts against the display and skips or doubles frames.
// All of them in one React batch (setFrameBatcher): outside a batch every setState renders on its own,
// so a frame that delivered ten images and stepped a list rendered eleven times.
globalThis.__ps5ReactFrame = elapsedMs => batch(() => {
  for (const callback of callbacks) callback(elapsedMs);
});

/** Internal: runs each frame's callbacks inside `batcher(run)`; index.js installs React's. */
export function setFrameBatcher(batcher) {
  batch = batcher;
}

/**
 * Internal: call `callback(elapsedMs)` every frame until the returned function is called.
 * @param {(elapsedMs: number) => void} callback
 */
export function onFrame(callback) {
  callbacks.add(callback);
  return () => { callbacks.delete(callback); };
}
