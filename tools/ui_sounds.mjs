// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
// Synthesizes the framework's interface sounds as 16-bit 48 kHz mono WAVs:
//   node tools/ui_sounds.mjs <directory>
// Everything is generated here (FM and additive voices, filtered seeded noise), so the files carry
// no third-party material. Each sound is [seconds, peak dBFS, render]: the focus tick, heard most,
// sits near -22 dBFS and the rest near -14, under whatever else plays.
import {mkdirSync, writeFileSync} from 'node:fs';
import {join, resolve} from 'node:path';

const RATE = 48000;
const TAU = 2 * Math.PI;

/** RBJ biquad, run in place; `frequency` may be a function of time for sweeps. */
function biquad(samples, type, frequency, q = 0.707) {
  let x1 = 0, x2 = 0, y1 = 0, y2 = 0;
  for (let i = 0; i < samples.length; ++i) {
    const f = typeof frequency === 'function' ? frequency(i / RATE) : frequency;
    const w = TAU * f / RATE, cos = Math.cos(w), alpha = Math.sin(w) / (2 * q);
    const [b0, b1, b2] = type === 'lowpass' ? [(1 - cos) / 2, 1 - cos, (1 - cos) / 2]
      : type === 'highpass' ? [(1 + cos) / 2, -(1 + cos), (1 + cos) / 2]
      : [alpha, 0, -alpha]; // band-pass, 0 dB at the centre
    const a0 = 1 + alpha, a1 = -2 * cos, a2 = 1 - alpha;
    const x = samples[i];
    const y = (b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2) / a0;
    x2 = x1; x1 = x; y2 = y1; y1 = y;
    samples[i] = y;
  }
  return samples;
}

/** Seeded white noise under an attack and an exponential decay, starting at `at` seconds. */
function noise(length, {at = 0, attack = 0.0005, decay, seed = 0x2545f491}) {
  const out = new Float64Array(length);
  for (let i = Math.round(at * RATE); i < length; ++i) {
    seed = (seed * 1664525 + 1013904223) >>> 0;
    const t = i / RATE - at;
    out[i] = (seed / 2 ** 31 - 1) * Math.min(1, t / attack) * Math.exp(-t / decay);
  }
  return out;
}

/**
 * A struck voice: additive partials [ratio, gain] (higher partials die faster), optionally
 * phase-modulated by a sine whose index decays (a bright strike that settles into a round tone),
 * and a pitch that settles from `bend` times the note.
 */
function voice(length, {at = 0, hz, partials = [[1, 1]], fm = null, attack = 0.002, decay, bend = 1, gain = 1}) {
  const out = new Float64Array(length);
  const phases = partials.map(() => 0);
  let modPhase = 0;
  for (let i = Math.round(at * RATE); i < length; ++i) {
    const t = i / RATE - at;
    const pitch = hz * (1 + (bend - 1) * Math.exp(-t / 0.012));
    modPhase += pitch * (fm?.ratio ?? 0) / RATE;
    const modulation = fm ? fm.index * Math.exp(-t / fm.decay) * Math.sin(TAU * modPhase) : 0;
    const shape = 0.5 - 0.5 * Math.cos(Math.PI * Math.min(1, t / attack));
    let sample = 0;
    partials.forEach(([ratio, level], p) => {
      phases[p] += pitch * ratio / RATE;
      sample += Math.sin(TAU * phases[p] + modulation) * level * Math.exp(-t * (ratio - 1) / (decay * 4));
    });
    out[i] = sample * shape * Math.exp(-t / decay) * gain;
  }
  return out;
}

const mix = (...layers) => layers[0].map((_, i) => layers.reduce((sum, layer) => sum + layer[i], 0));
const scale = (samples, gain) => samples.map(sample => sample * gain);

const MALLET = [[1, 1], [3.93, 0.18]];

const SOUNDS = {
  // The highlight moved: a filtered click, mostly air, gone in about 20 ms.
  focus: [0.028, -22, n => mix(
    biquad(biquad(noise(n, {attack: 0.0003, decay: 0.0025}), 'bandpass', 4200, 1.8), 'highpass', 1800),
    voice(n, {hz: 2600, attack: 0.0005, decay: 0.005, gain: 0.35}),
  )],
  // Cross: a soft mallet dyad (D5 and A5, strummed) with a warm FM strike, a marimba-like overtone
  // that dies first (so it reads as a struck object, not a beep) and a low body.
  confirm: [0.26, -14, n => biquad(mix(
    voice(n, {hz: 587.33, partials: MALLET, fm: {ratio: 1, index: 1.6, decay: 0.025}, attack: 0.003, decay: 0.06,
      bend: 1.02}),
    voice(n, {at: 0.018, hz: 880, partials: MALLET, fm: {ratio: 1, index: 1.2, decay: 0.02}, attack: 0.003,
      decay: 0.05, gain: 0.55}),
    voice(n, {hz: 293.66, attack: 0.004, decay: 0.05, gain: 0.25}),
    scale(biquad(noise(n, {decay: 0.0015}), 'bandpass', 2500, 1), 0.15),
  ), 'lowpass', 4500)],
  // Circle: lower, shorter and falling (A4 then E4).
  back: [0.17, -15, n => biquad(mix(
    voice(n, {hz: 440, partials: MALLET, fm: {ratio: 1, index: 1.2, decay: 0.02}, attack: 0.003, decay: 0.035,
      gain: 0.7}),
    voice(n, {at: 0.02, hz: 329.63, partials: MALLET, fm: {ratio: 1, index: 1.2, decay: 0.02}, attack: 0.003,
      decay: 0.04}),
    scale(biquad(noise(n, {decay: 0.0012}), 'bandpass', 1800, 1), 0.1),
  ), 'lowpass', 3000)],
  // Refused (an edge, a disabled element): two muffled taps, felt more than heard.
  error: [0.15, -16, n => biquad(mix(
    ...[0, 0.055].map((at, hit) => voice(n, {at, hz: 155 - hit * 12, partials: [[1, 1], [2, 0.3]],
      attack: 0.0015, decay: 0.022, bend: 1.3, gain: 1 - hit * 0.25})),
    ...[0, 0.055].map(at => scale(biquad(noise(n, {at, decay: 0.008}), 'lowpass', 500), 0.3)),
  ), 'lowpass', 900)],
  // L1/R1 changed the page: a light whoosh, band-passed noise sweeping upward.
  page: [0.17, -17, n => biquad(biquad(noise(n, {attack: 0.045, decay: 0.04}), 'bandpass',
    t => 700 * 5 ** Math.min(1, t / 0.14), 0.9), 'highpass', 300)],
  // A toast or notification: an elegant two-note bell (G5 then D6).
  notify: [1.0, -14, n => biquad(mix(
    ...[[783.99, 0, 1], [1174.66, 0.11, 0.85]].map(([hz, at, gain]) => voice(n, {at, hz, gain,
      partials: [[1, 1], [2, 0.22], [3.01, 0.07]], fm: {ratio: 2, index: 0.6, decay: 0.06},
      attack: 0.002, decay: 0.24})),
  ), 'lowpass', 7000)],
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
  const samples = biquad(render(Math.round(seconds * RATE)), 'highpass', 40); // no DC or rumble
  const fade = Math.round(0.004 * RATE); // no click where the length cuts a decay
  for (let i = 0; i < fade; ++i) samples[samples.length - 1 - i] *= i / fade;
  const peak = samples.reduce((max, sample) => Math.max(max, Math.abs(sample)), 0);
  const path = join(directory, `${name}.wav`);
  writeFileSync(path, wav(Array.from(samples, sample => sample * 10 ** (peakDb / 20) / peak)));
  console.log(`${resolve(path)} ${seconds * 1000} ms`);
}
