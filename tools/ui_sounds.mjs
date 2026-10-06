// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
// Prepares the framework's interface sounds from Google's Material Design sound resources
// (CC BY 4.0, https://m2.material.io/design/sound/sound-resources.html; see docs/DEPENDENCIES.md):
//   node tools/ui_sounds.mjs <material wav directory> <output directory>
// The input directory holds the pack's `wav` files (any subfolders). Each chosen file is mixed to
// mono, trimmed to where it is audible, faded and normalized, then written as 16-bit 48 kHz WAV.
import {mkdirSync, readdirSync, readFileSync, writeFileSync} from 'node:fs';
import {basename, join, resolve} from 'node:path';

const RATE = 48000;

// name: [Material source, peak dBFS, longest seconds]. The focus tick, heard most, is the quietest.
const SOUNDS = {
  focus: ['navigation_hover-tap', -24, 0.07], // shorter than the 110 ms D-pad repeat
  confirm: ['navigation_forward-selection-minimal', -16, 0.3],
  back: ['navigation_backward-selection-minimal', -16, 0.3],
  error: ['navigation_unavailable-selection', -18, 0.25],
  page: ['navigation_transition-right', -18, 0.45],
  notify: ['notification_simple-01', -16, 0.9],
};

/** 16- or 24-bit PCM WAV at 48 kHz → mono samples in -1..1. */
function read(path) {
  const file = readFileSync(path);
  let format = null;
  for (let at = 12; at + 8 <= file.length; at += 8 + file.readUInt32LE(at + 4) + (file.readUInt32LE(at + 4) & 1)) {
    const id = file.toString('ascii', at, at + 4);
    if (id === 'fmt ') {
      format = {channels: file.readUInt16LE(at + 10), rate: file.readUInt32LE(at + 12), bits: file.readUInt16LE(at + 22)};
    } else if (id === 'data') {
      const {channels, rate, bits} = format;
      if (rate !== RATE || (bits !== 16 && bits !== 24)) throw new Error(`${path}: expected 16/24-bit ${RATE} Hz PCM`);
      const bytes = bits / 8, frames = Math.floor(file.readUInt32LE(at + 4) / (bytes * channels));
      const sample = offset => (bits === 16 ? file.readInt16LE(offset) / 32768 : file.readIntLE(offset, 3) / 8388608);
      return Float64Array.from({length: frames}, (_, frame) => {
        let sum = 0;
        for (let channel = 0; channel < channels; ++channel) sum += sample(at + 8 + (frame * channels + channel) * bytes);
        return sum / channels;
      });
    }
  }
  throw new Error(`${path}: no fmt and data chunks`);
}

/** Trims silence (below -50 dB of the peak) at both ends, caps the length, fades and normalizes. */
function prepare(samples, peakDb, seconds) {
  let peak = 0;
  for (const sample of samples) peak = Math.max(peak, Math.abs(sample));
  const floor = peak * 10 ** (-50 / 20);
  const first = Math.max(0, samples.findIndex(sample => Math.abs(sample) > floor) - RATE / 2000);
  let last = samples.length - 1;
  while (last > first && Math.abs(samples[last]) <= floor) --last;
  const end = Math.min(last + 1, first + Math.round(seconds * RATE));
  const out = samples.slice(first, end);
  // A length cap cuts into the decay, so it fades longer than a natural end.
  const fadeIn = RATE / 2000, fadeOut = Math.min(Math.round((end <= last ? 0.06 : 0.015) * RATE), out.length >> 2);
  for (let i = 0; i < fadeIn; ++i) out[i] *= i / fadeIn;
  for (let i = 0; i < fadeOut; ++i) out[out.length - 1 - i] *= i / fadeOut;
  return out.map(sample => sample * 10 ** (peakDb / 20) / peak);
}

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

const [source, directory] = process.argv.slice(2);
if (!source || !directory) {
  console.error('Usage: node tools/ui_sounds.mjs <material wav directory> <output directory>');
  process.exit(1);
}
const files = new Map(readdirSync(source, {recursive: true}).filter(file => file.endsWith('.wav'))
  .map(file => [basename(file, '.wav'), join(source, file)]));
mkdirSync(directory, {recursive: true});
for (const [name, [material, peakDb, seconds]] of Object.entries(SOUNDS)) {
  if (!files.has(material)) throw new Error(`${material}.wav is missing from ${source}`);
  const samples = prepare(read(files.get(material)), peakDb, seconds);
  const path = join(directory, `${name}.wav`);
  writeFileSync(path, wav(samples));
  console.log(`${resolve(path)} ← ${material} ${Math.round(samples.length / 48)} ms`);
}
