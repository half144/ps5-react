import {useState} from 'react';
import {AppRegistry, View, Text, useController} from '@ps5-react/core';

const items = ['Play', 'Settings', 'About'];
const label = 'font-inter text-body text-white';

function App() {
  const [focus, setFocus] = useState(0);
  const [selected, setSelected] = useState(null);
  useController(action => {
    if (action === 'next') setFocus(index => (index + 1) % items.length);
    if (action === 'previous') setFocus(index => (index + items.length - 1) % items.length);
    if (action === 'confirm') setSelected(items[focus]);
    if (action === 'back') setSelected(null);
  });

  return (
    <View className="w-screen h-screen bg-ink items-center justify-center gap-8">
      <Text className="font-inter text-title text-white">__TITLE__</Text>
      <View className="flex-row gap-4">
        {items.map((item, index) => (
          <View key={item} focused={focus === index} className="w-[220px] h-[92px] rounded-xl
            border-[3px] border-edge bg-panel items-center justify-center
            focused:border-accent focused:bg-panel-focus focused:scale-105">
            <Text className={label}>{item}</Text>
          </View>
        ))}
      </View>
      <Text className="font-inter text-hint text-accent">
        {selected ? `${selected} selected - Circle / Backspace: back` : 'D-pad: move    X / Enter: select'}
      </Text>
    </View>
  );
}

AppRegistry.registerComponent('__NAME__', () => App);
