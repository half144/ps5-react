// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
import {useState} from 'react';
import {AppRegistry, View, Text, useController} from '@ps5-react/core';
import {SpacingPage, FlexPage} from './pages/layout.jsx';
import {ColorPage, BorderPage} from './pages/paint.jsx';
import {TypePage} from './pages/type.jsx';
import {TransformPage} from './pages/motion.jsx';

const PAGES = [
  ['Spacing', SpacingPage], ['Flex', FlexPage], ['Color', ColorPage],
  ['Borders', BorderPage], ['Type', TypePage], ['Transform', TransformPage],
];
const CARDS = 3;
const hint = 'font-inter text-xs text-slate-400';

const wrap = (value, length) => (value + length) % length;

// card === -1 means the tab row has focus; picked holds "page:card" keys toggled with confirm.
function step(nav, action) {
  const {page, card, picked} = nav;
  const delta = action === 'next' ? 1 : action === 'previous' ? -1 : 0;
  if (card < 0) {
    if (action === 'confirm') return {...nav, card: 0};
    return delta ? {...nav, page: wrap(page + delta, PAGES.length)} : nav;
  }
  if (action === 'back') return {...nav, card: -1};
  if (action === 'confirm') {
    const key = `${page}:${card}`;
    return {...nav, picked: picked.includes(key) ? picked.filter(k => k !== key) : [...picked, key]};
  }
  return delta ? {...nav, card: wrap(card + delta, CARDS)} : nav;
}

function Tab({label, active, focused}) {
  return (
    <View focused={focused} selected={active}
      className="px-5 py-2 rounded-full border-2 border-transparent
        selected:bg-slate-700 focused:border-accent focused:scale-105">
      <Text className={`font-inter text-sm ${active ? 'text-white' : 'text-slate-400'}`}>{label}</Text>
    </View>
  );
}

function App() {
  const [nav, setNav] = useState({page: 0, card: -1, picked: []});
  const {page, card, picked} = nav;
  useController(action => setNav(previous => step(previous, action)));

  const [, Current] = PAGES[page];
  const at = index => ({focused: card === index, selected: picked.includes(`${page}:${index}`)});

  return (
    <View className="w-screen h-screen bg-ink px-10 py-8 gap-6">
      <View className="flex-row items-end justify-between">
        <View className="gap-1">
          <Text className="font-inter text-xs text-accent tracking-widest">PS5 REACT · CLASSNAME</Text>
          <Text className="font-inter text-xl text-white leading-none">Tailwind Gallery</Text>
        </View>
        <Text className="font-inter text-sm text-slate-500">{page + 1} / {PAGES.length}</Text>
      </View>
      <View className={`self-start flex-row gap-2 p-1 rounded-full bg-panel border-2
        ${card < 0 ? 'border-slate-600' : 'border-transparent'}`}>
        {PAGES.map(([label], index) => (
          <Tab key={label} label={label} active={index === page} focused={card < 0 && index === page} />
        ))}
      </View>
      <Current at={at} />
      <View className="flex-row justify-between border-t border-slate-800 pt-3">
        <Text className={hint}>
          {card < 0 ? 'D-pad: switch page    X / Enter: explore cards'
            : 'D-pad: move focus    X / Enter: toggle selected    Circle: back to tabs'}
        </Text>
        <Text className={`${hint} italic`}>{picked.length} selected</Text>
      </View>
    </View>
  );
}
AppRegistry.registerComponent('tailwind-gallery', () => App);
