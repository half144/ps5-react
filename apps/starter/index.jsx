// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
import {useLayoutEffect, useState} from 'react';
import {AppRegistry, View, Text, useController} from '@ps5-react/core';

const text = 'font-inter text-body text-white';

// Proof only: React owns the state; the C host sends directional actions.
function App() {
  const [state, setState] = useState({focus: 0, count: 0, detail: false});
  const {focus, count, detail} = state;
  useLayoutEffect(() => {
    globalThis.__ps5ReactTestState = {focus, count, detail: Number(detail)};
  }, [focus, count, detail]);
  useController(action => setState(previous => {
      if (action === 'back' || (previous.detail && action === 'confirm'))
        return {...previous, detail: false};
      if (!previous.detail && (action === 'next' || action === 'previous'))
        return {...previous, focus: (previous.focus + (action === 'next' ? 1 : 2)) % 3};
      if (!previous.detail && action === 'confirm')
        return {...previous, detail: true, count: previous.focus === 0
          ? previous.count + 1 : previous.focus === 1 ? previous.count + 10 : 0};
      return previous;
    }));

  // Classes are logical 1280-wide pixels, scaled to render.width at build time.
  return (
    <View className="w-screen h-screen bg-ink items-center justify-center gap-6">
      <Text className={`${text} text-title`}>PS5 React</Text>
      <Text className={text}>Count: {count}</Text>
      {detail ? (
        <View className="gap-6 items-center">
          <Text className={text}>Value updated</Text>
          <Text className={`${text} text-accent`}>Circle / Backspace: back</Text>
        </View>
      ) : (
        <View className="flex-row gap-4">
          {['Add 1', 'Add 10', 'Reset'].map((label, index) => (
            <View key={label} focused={focus === index} className="w-[220px] h-[92px] rounded-xl
              border-[3px] border-edge bg-panel justify-center items-center
              focused:border-accent focused:bg-panel-focus">
              <Text className={text}>{label}</Text>
            </View>
          ))}
        </View>
      )}
      <Text className={`${text} text-hint text-accent`}>
        D-pad: choose    X / Enter: confirm
      </Text>
    </View>
  );
}
AppRegistry.registerComponent('overdrive-proof', () => App);
