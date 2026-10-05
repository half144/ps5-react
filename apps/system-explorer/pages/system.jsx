// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
import {useState} from 'react';
import {View, Text, Notifications, Linking, BackHandler} from '@ps5-react/core';
import {Panel, Button, Status, attempt, useMenu, body} from '../ui.jsx';

const REPO = 'https://github.com/half144/ps5-react';

const ACTIONS = [
  ['Send notification', () => Notifications.show('Hello from System Explorer', 'Sent with PS5 React')
    ? 'Notification sent' : 'The host could not show the notification'],
  ['Open GitHub', () => (Linking.openURL(REPO) ? `Opened ${REPO}` : 'The host could not open the browser')],
  ['Exit app', () => { BackHandler.exitApp(); return 'Exiting…'; }],
];

export function SystemPage({active, onBack}) {
  const [status, setStatus] = useState(null);
  const [focus] = useMenu(active, ACTIONS.length, {
    onConfirm: index => setStatus(attempt(ACTIONS[index][1])),
    onBack,
  });

  return (
    <View className="flex-1 gap-5">
      <Panel title="SYSTEM SERVICES">
        <Text style={body}>Notifications.show, Linking.openURL, and BackHandler.exitApp.</Text>
        <View className="flex-row gap-3">
          {ACTIONS.map(([label], index) => (
            <Button key={label} label={label} focused={active && focus === index} />
          ))}
        </View>
        <Status status={status} />
      </Panel>
    </View>
  );
}
