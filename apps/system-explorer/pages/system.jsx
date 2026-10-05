// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
import {useState} from 'react';
import {View, Text, Notifications, Linking, BackHandler} from '@ps5-react/core';
import {Panel, Button, Status, attempt, body} from '../ui.jsx';

const REPO = 'https://github.com/half144/ps5-react';

const ACTIONS = [
  ['Send notification', () => Notifications.show('Hello from System Explorer', 'Sent with PS5 React')
    ? 'Notification sent' : 'The host could not show the notification'],
  ['Open GitHub', () => (Linking.openURL(REPO) ? `Opened ${REPO}` : 'The host could not open the browser')],
  ['Exit app', () => { BackHandler.exitApp(); return 'Exiting…'; }],
];

export function SystemPage() {
  const [status, setStatus] = useState(null);

  return (
    <View className="flex-1 gap-5">
      <Panel title="SYSTEM SERVICES">
        <Text style={body}>Notifications.show, Linking.openURL, and BackHandler.exitApp.</Text>
        <View className="flex-row gap-3">
          {ACTIONS.map(([label, run]) => (
            <Button key={label} label={label} onPress={() => setStatus(attempt(run))} />
          ))}
        </View>
        <Status status={status} />
      </Panel>
    </View>
  );
}
