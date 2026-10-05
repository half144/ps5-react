// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
import {useState} from 'react';
import {View, Text, FileSystem} from '@ps5-react/core';
import {Panel, Field, Button, Status, attempt, useMenu, body, mono} from '../ui.jsx';
import {bytes} from '../format.js';

const ROOTS = [FileSystem.dataDir, FileSystem.appDir, FileSystem.tempDir, '/'];
const TEST_FILE = `${FileSystem.dataDir}/system-explorer.txt`;
const SHORTCUTS = ROOTS.length + 1;
const ROWS = 11;
const PREVIEW_BYTES = 4096;

const join = (dir, name) => (dir === '/' ? `/${name}` : `${dir}/${name}`);
const parent = dir => dir.slice(0, dir.lastIndexOf('/')) || '/';
const printable = text => text.slice(0, 240).replace(/[^\x20-\x7e\n]/g, '?');

function list(dir) {
  return FileSystem.readDir(dir).sort((a, b) =>
    a.isDirectory !== b.isDirectory ? (a.isDirectory ? -1 : 1) : a.name < b.name ? -1 : 1);
}

function writeTestFile() {
  FileSystem.writeFile(TEST_FILE, `Written by System Explorer at ${Date.now()} ms\n`);
  FileSystem.appendFile(TEST_FILE, 'Appended a second line.\n');
  return `Read back: ${FileSystem.readFile(TEST_FILE)}`;
}

function describe(path, entry) {
  if (entry.size > PREVIEW_BYTES) return `${path}: ${bytes(entry.size)}, too large to preview`;
  return `${path}: ${bytes(entry.size)}\n${printable(FileSystem.readFile(path))}`;
}

function Entry({entry, focused}) {
  return (
    <View focused={focused} className="flex-row justify-between px-3 py-1 rounded-md focused:bg-panel-focus">
      <Text focused={focused} className="font-inter text-sm text-slate-300 focused:text-white">
        {entry.isDirectory ? `${entry.name}/` : entry.name}
      </Text>
      <Text style={mono}>{entry.isDirectory ? 'dir' : bytes(entry.size)}</Text>
    </View>
  );
}

export function FilesPage({active, onBack}) {
  const [dir, setDir] = useState(FileSystem.dataDir);
  const [listing, setListing] = useState(() => attempt(() => list(FileSystem.dataDir)));
  const [status, setStatus] = useState(null);
  const entries = listing.value ?? [];

  const open = path => {
    setDir(path);
    setListing(attempt(() => list(path)));
    setFocus(0);
    setStatus(null);
  };
  const confirm = index => {
    if (index < ROOTS.length) return open(ROOTS[index]);
    if (index === ROOTS.length) {
      setStatus(attempt(writeTestFile));
      return setListing(attempt(() => list(dir)));
    }
    const entry = entries[index - SHORTCUTS];
    const path = join(dir, entry.name);
    if (entry.isDirectory) return open(path);
    setStatus(attempt(() => describe(path, entry)));
  };
  const [focus, setFocus] = useMenu(active, SHORTCUTS + entries.length, {
    onConfirm: confirm,
    onBack: () => (dir === '/' ? onBack() : open(parent(dir))),
  });

  const row = focus - SHORTCUTS;
  const start = Math.max(0, Math.min(row - 4, entries.length - ROWS));
  const usage = attempt(() => FileSystem.diskUsage(dir));
  const mounts = attempt(FileSystem.mounts);

  return (
    <View className="flex-1 flex-row gap-6">
      <Panel title={`${dir} · ${entries.length} entries`} grow>
        <View className="flex-row flex-wrap gap-2">
          {ROOTS.map((root, index) => <Button key={root} label={root} focused={active && focus === index} />)}
          <Button label="Write test file" focused={active && focus === ROOTS.length} />
        </View>
        <Status status={listing.error ? listing : null} />
        {entries.length === 0 && !listing.error && <Text style={body}>Empty, or not listable here.</Text>}
        <View className="gap-1">
          {entries.slice(start, start + ROWS).map((entry, index) => (
            <Entry key={entry.name} entry={entry} focused={active && row === start + index} />
          ))}
        </View>
      </Panel>
      <View className="w-96 gap-5">
        <Panel title="DISK USAGE">
          {usage.error ? <Status status={usage} /> : (
            <View className="gap-3">
              <Field name="Free" value={bytes(usage.value.free)} />
              <Field name="Total" value={bytes(usage.value.total)} />
            </View>
          )}
        </Panel>
        <Panel title="MOUNTS">
          {mounts.error ? <Status status={mounts} /> : mounts.value.slice(0, 6).map(mount => (
            <Text key={mount.path} className="font-inter text-xs text-accent truncate">{mount.path} · {mount.type}</Text>
          ))}
        </Panel>
        <Panel title="RESULT"><Status status={status} /></Panel>
      </View>
    </View>
  );
}
