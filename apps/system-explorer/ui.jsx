// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
import {useState} from 'react';
import {View, Text, tw, useController} from '@ps5-react/core';

export const caption = tw`font-inter text-xs text-slate-400 tracking-wider`;
export const body = tw`font-inter text-sm text-slate-100`;
export const mono = tw`font-inter text-xs text-accent`;

/** Runs a native call, capturing its error message for display instead of throwing. */
export function attempt(call) {
  try {
    return {value: call()};
  } catch (error) {
    return {error: error.message};
  }
}

/**
 * One-dimensional focus over `count` items while `active`; `back` is left to the page.
 * @returns {[number, (index: number) => void]}
 */
export function useMenu(active, count, {onConfirm, onBack}) {
  const [focus, setFocus] = useState(0);
  const index = Math.min(focus, count - 1);
  useController(action => {
    if (!active) return;
    if (action === 'next') setFocus((index + 1) % count);
    else if (action === 'previous') setFocus((index + count - 1) % count);
    else if (action === 'confirm') onConfirm(index);
    else onBack();
  });
  return [index, setFocus];
}

export function Panel({title, children, grow}) {
  return (
    <View className={`p-5 gap-3 rounded-2xl border-2 border-slate-700 bg-panel ${grow ? 'flex-1' : ''}`}>
      <Text style={caption}>{title}</Text>
      {children}
    </View>
  );
}

export function Field({name, value}) {
  return (
    <View className="flex-row justify-between gap-4">
      <Text className="font-inter text-sm text-slate-400">{name}</Text>
      <Text style={body}>{value}</Text>
    </View>
  );
}

export function Button({label, focused}) {
  return (
    <View focused={focused}
      className="px-5 py-2 rounded-full border-2 border-slate-600 bg-slate-800
        focused:border-accent focused:bg-panel-focus focused:scale-105">
      <Text focused={focused} className="font-inter text-sm text-slate-300 focused:text-white">{label}</Text>
    </View>
  );
}

/** Shows the last action's result: a string value, or an error in red. */
export function Status({status}) {
  if (!status) return null;
  return (
    <Text className={`font-inter text-sm ${status.error ? 'text-rose-400' : 'text-emerald-300'}`}>
      {status.error ?? status.value}
    </Text>
  );
}
