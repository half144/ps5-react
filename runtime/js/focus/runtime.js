// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
// The app's focus manager, its React contexts, and the host input that drives it.
import {createContext} from 'react';
import {subscribeInput} from '../input.js';
import {dispatchBackPress} from '../native.js';
import {FocusManager} from './manager.js';

export const manager = new FocusManager();

/** The enclosing FocusScope; the manager's root outside any. */
export const ScopeContext = createContext(manager.root);
/** @type {import('react').Context<import('./manager.js').Frame | null>} the enclosing ScrollView */
export const FrameContext = createContext(null);
/** @type {import('react').Context<import('./manager.js').Node | null>} the nearest focusable ancestor */
export const NodeContext = createContext(null);

subscribeInput(action => {
  if (action === 'confirm') manager.press();
  else if (action === 'back') {
    if (!manager.back()) dispatchBackPress();
  } else if (action === 'up' || action === 'down' || action === 'left' || action === 'right') {
    manager.move(action);
  }
});
