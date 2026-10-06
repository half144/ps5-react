// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
// Synthesizes the framework's interface sounds as 16-bit 48 kHz mono WAVs:
//   node tools/ui_sounds.mjs <directory>
// Everything is generated here (sine partials, a seeded noise burst), so the files carry no
// third-party material. Each entry is [seconds, peak dBFS, render]; peaks stay at -12 dBFS or below
// so the sounds sit under whatever else plays, and the focus tick, heard most, is the quietest.
import {mkdirSync, writeFileSync} from 'node:fs';
import {join, resolve} from 'node:path';

const RATE = 48000;

/** A struck tone: partials [ratio, gain], an attack and an exponential decay; pitch glides by `glide`. */
function note(out, {at = 0, hz, partials = [[1, 1]], attack = 0.002, decay, gain = 1, glide = 1}) {
  const start = Math.round(at * RATE);
  let phase = 0;
  for (let i = start; i < out.length; ++i) {
    const t = (i - start) / RATE;
    const envelope = Math.min(1, t / attack) * Math.exp(-t / decay);
    if (envelope < 1e-4 && t > attack) break;
    phase += hz * (1 + (glide - 1) * Math.min(1, t / (decay * 3))) / RATE;
    let sample = 0;
    for (const [ratio, level] of partials) sample += Math.sin(2 * Math.PI * phase * ratio) * level;
    out[i] += sample * envelope * gain;
  }
}

/** Band-limited noise (one-pole high- then low-pass) under a short envelope, for the page swish. */
function swish(out, {decay, low, high, gain}) {
  let seed = 0x2545f491, lp = 0, hp = 0, previous = 0;
  const a = 1 - Math.exp(-2 * Math.PI * high / RATE);
  const b = Math.exp(-2 * Math.PI * low / RATE);
  for (let i = 0; i < out.length; ++i) {
    seed = (seed * 1664525 + 1013904223) >>> 0;
    const white = seed / 2 ** 31 - 1;
    lp += a * (white - lp);
    hp = b * (hp + lp - previous);
    previous = lp;
    const t = i / RATE;
    out[i] += hp * Math.min(1, t / 0.012) * Math.exp(-t / decay) * gain;
  }
}

const bell = [[1, 1], [2, 0.28], [3, 0.08]];

const SOUNDS = {
  // The highlight moved: short and dry, so it can tick on every held-key repeat.
  focus: [0.045, -18, out => note(out, {hz: 1750, partials: [[1, 1], [2.01, 0.2]], attack: 0.001, decay: 0.009, glide: 0.88})],
  // Cross: a rising fifth.
  confirm: [0.24, -12, out => {
    note(out, {hz: 880, partials: bell, decay: 0.035, gain: 0.8});
    note(out, {at: 0.055, hz: 1318.5, partials: bell, decay: 0.05});
  }],
  // Circle: the same pair, falling and softer.
  back: [0.22, -14, out => {
    note(out, {hz: 1174.7, partials: bell, decay: 0.03, gain: 0.7});
    note(out, {at: 0.05, hz: 784, partials: bell, decay: 0.045, gain: 0.8});
  }],
  // No target in that direction, or the element is disabled: a low, muted knock.
  error: [0.14, -14, out => {
    note(out, {hz: 196, partials: [[1, 1], [2.7, 0.25]], attack: 0.003, decay: 0.03, glide: 0.8});
    note(out, {at: 0.045, hz: 165, partials: [[1, 1], [2.7, 0.2]], attack: 0.003, decay: 0.03, gain: 0.8, glide: 0.8});
  }],
  // L1/R1 changed the page or tab: an airy swish under a light tick.
  page: [0.12, -16, out => {
    swish(out, {decay: 0.03, low: 1800, high: 7000, gain: 1.6});
    note(out, {at: 0.012, hz: 2093, partials: [[1, 1]], attack: 0.001, decay: 0.014, gain: 0.5});
  }],
  // A toast or notification: three rising bell notes.
  notify: [0.6, -12, out => {
    [1046.5, 1318.5, 1568].forEach((hz, i) => note(out, {at: i * 0.07, hz, partials: bell, decay: 0.12, gain: 0.8}));
  }],
};

function wav(samples) {
  const data = Buffer.alloc(samples.length * 2);
  samples.forEach((sample, i) => data.writeInt16LE(Math.round(Math.max(-1, Math.min(1, sample)) * 32767), i * 2));
  const header = Buffer.alloc(44);
  header.write('RIFF', 0);
  header.writeUInt32LE(36 + data.length, 4);
  header.write('WAVEfmt ', 8);
  header.writeUInt32LE(16, 16);
  header.writeUInt16LE(1, 20); // PCM
  header.writeUInt16LE(1, 22); // mono
  header.writeUInt32LE(RATE, 24);
  header.writeUInt32LE(RATE * 2, 28);
  header.writeUInt16LE(2, 32);
  header.writeUInt16LE(16, 34);
  header.write('data', 36);
  header.writeUInt32LE(data.length, 40);
  return Buffer.concat([header, data]);
}

const directory = process.argv[2];
if (!directory) {
  console.error('Usage: node tools/ui_sounds.mjs <directory>');
  process.exit(1);
}
mkdirSync(directory, {recursive: true});
for (const [name, [seconds, peakDb, render]] of Object.entries(SOUNDS)) {
  const samples = new Float64Array(Math.round(seconds * RATE));
  render(samples);
  const peak = samples.reduce((max, sample) => Math.max(max, Math.abs(sample)), 0);
  const fade = Math.round(0.004 * RATE); // no click if a decay is cut by the length
  for (let i = 0; i < fade; ++i) samples[samples.length - 1 - i] *= i / fade;
  const path = join(directory, `${name}.wav`);
  writeFileSync(path, wav(Array.from(samples, sample => sample * 10 ** (peakDb / 20) / peak)));
  console.log(`${resolve(path)} ${seconds * 1000} ms`);
}
