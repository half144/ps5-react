// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
import {useLayoutEffect, useState} from 'react';
import {AppRegistry, View, Text, useController} from '@ps5-react/core';
import Inter from './assets/Inter-Regular.ttf';

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

  // Same layout at 720p and 1440p; fonts are baked at both physical sizes.
  const px = value => value * screen.width / 1280;
  const text = {fontFamily: Inter, fontSize: px(24), color: '#ffffff'};
  return (
    <View style={{width: screen.width, height: screen.height, backgroundColor: '#101820',
      alignItems: 'center', justifyContent: 'center', gap: px(24)}}>
      <Text style={{...text, fontSize: px(40)}}>PS5 React</Text>
      <Text style={text}>Count: {count}</Text>
      {detail ? (
        <View style={{gap: px(24), alignItems: 'center'}}>
          <Text style={text}>Value updated</Text>
          <Text style={{...text, color: '#a0c4ff'}}>Circle / Backspace: back</Text>
        </View>
      ) : (
        <View style={{flexDirection: 'row', gap: px(16)}}>
          {['Add 1', 'Add 10', 'Reset'].map((label, index) => (
            <View key={label} style={{width: px(220), height: px(92), borderRadius: px(12),
              borderWidth: px(3), borderColor: focus === index ? '#a0c4ff' : '#314451',
              backgroundColor: focus === index ? '#294559' : '#182630',
              justifyContent: 'center', alignItems: 'center'}}>
              <Text style={text}>{label}</Text>
            </View>
          ))}
        </View>
      )}
      <Text style={{...text, fontSize: px(18), color: '#a0c4ff'}}>
        D-pad: choose    X / Enter: confirm
      </Text>
    </View>
  );
}
AppRegistry.registerComponent('overdrive-proof', () => App);
