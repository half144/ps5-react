// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
import assert from 'node:assert/strict';
import test from 'node:test';
import {acquireImage, Downloads, Http} from './native.js';

test('native network tasks deliver progress, release listeners and stop polling', async () => {
  let tick;
  let clears = 0;
  let snapshots = [];
  const cancelled = [];
  const originalInterval = globalThis.setInterval;
  const originalClear = globalThis.clearInterval;
  globalThis.setInterval = callback => { tick = callback; return 1; };
  globalThis.clearInterval = () => { clears++; };
  globalThis.__ps5ReactNative = {network: {
    download: () => 1, request: () => 2,
    cancel: id => cancelled.push(id), poll: () => snapshots, version: () => 'fixture',
  }};
  try {
    const task = Downloads.enqueue({url: 'https://example.com/file', destination: '/download0/file'});
    const states = [];
    const unsubscribe = task.subscribe(progress => states.push(progress.state));
    snapshots = [{id: 1, state: 'downloading', body: 'must not reach progress', written: 12}];
    tick();
    assert.equal(task.snapshot.destination, '/download0/file');
    assert.equal('body' in task.snapshot, false);
    unsubscribe();
    snapshots = [{id: 1, state: 'completed', written: 24}];
    tick();
    assert.equal((await task.done).written, 24);
    assert.deepEqual(states, ['queued', 'downloading']);
    assert.equal(clears, 1);
    task.cancel();
    assert.deepEqual(cancelled, []);
    const aborted = Downloads.enqueue({url: 'https://example.com/file', destination: '/download0/file'});
    aborted.cancel();
    snapshots = [{id: 1, state: 'cancelled'}];
    tick();
    await assert.rejects(aborted.done, error => error.name === 'AbortError' && error.message.includes('/download0/file'));
    assert.deepEqual(cancelled, [1]);
    const response = Http.request('https://example.com/json');
    snapshots = [{id: 2, state: 'completed', status: 404, body: '{"value":42}'}];
    tick();
    const result = await response;
    assert.equal(result.ok, false);
    assert.equal(result.status, 404);
    assert.deepEqual(await result.json(), {value: 42});
    assert.equal(await result.text(), '{"value":42}');
    assert.equal(clears, 3);
  } finally {
    globalThis.setInterval = originalInterval;
    globalThis.clearInterval = originalClear;
    delete globalThis.__ps5ReactNative;
  }
});

test('remote images poll only while loads are pending and release every reference', () => {
  const released = [];
  let finished = [];
  let polls = 0;
  globalThis.__ps5ReactNative = {image: {
    load: (uri, width, height) => (width === 10
      ? {id: 7, state: 'ready', name: '@image:7', width: 10, height: 10, error: ''}
      : {id: 8, state: 'loading', name: '', width: 0, height: 0, error: ''}),
    release: id => released.push(id),
    poll: () => { polls++; const out = finished; finished = []; return out; },
  }};
  const seen = [];
  const releaseReady = acquireImage('https://example.com/a.jpg', 10, 10, 0, false, result => seen.push(result.state));
  globalThis.__ps5ReactFrame(16);
  assert.equal(polls, 0, 'a cached image needs no polling');
  const releaseLoading = acquireImage('https://example.com/b.jpg', 20, 20, 0, false, result => seen.push(result.state));
  globalThis.__ps5ReactFrame(16);
  finished = [{id: 8, state: 'ready', name: '@image:8', width: 20, height: 20, error: ''}];
  globalThis.__ps5ReactFrame(16);
  globalThis.__ps5ReactFrame(16);
  assert.equal(polls, 2, 'polling stops once nothing is pending');
  assert.deepEqual(seen, ['ready', 'loading', 'ready']);
  releaseReady();
  releaseLoading();
  assert.deepEqual(released, [7, 8]);
  globalThis.__ps5ReactFrame(16);
  globalThis.__ps5ReactFrame(16);
  assert.equal(polls, 3, 'a release polls once more so eviction can run');
  delete globalThis.__ps5ReactNative;
});
