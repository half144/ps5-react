// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
// Interface sounds for every focus move, press and back. The WAVs come from tools/ui_sounds.mjs.
import {Sound, useNavigationEvents} from '@ps5-react/core';
import focus from './assets/sounds/focus.wav';
import confirm from './assets/sounds/confirm.wav';
import back from './assets/sounds/back.wav';
import error from './assets/sounds/error.wav';
import page from './assets/sounds/page.wav';

export {default as notify} from './assets/sounds/notify.wav';

const PAGE_ACTIONS = new Set(['l1', 'r1', 'l2', 'r2']);

export function useInterfaceSounds() {
  useNavigationEvents(event => {
    if (event.type === 'move') Sound.play(focus);
    else if (event.type === 'blocked') Sound.play(error);
    else if (event.type === 'press') Sound.play(event.handled ? confirm : error);
    else if (event.handled) Sound.play(event.type === 'back' ? back : PAGE_ACTIONS.has(event.action) ? page : confirm);
  });
}
