// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
import assert from 'node:assert/strict';
import {test} from 'node:test';
import {approach, findClosest, findNearest, findWrap, revealOffset} from './geometry.js';

const rect = (x, y, width = 100, height = 100) => ({x, y, width, height});
const grid = () => {
  const cells = [];
  for (let row = 0; row < 3; row++) {
    for (let column = 0; column < 3; column++) {
      cells.push({id: `${row}${column}`, rect: rect(column * 120, row * 120), order: cells.length});
    }
  }
  return cells;
};
const move = (cells, id, direction) => {
  const from = cells.find(cell => cell.id === id);
  return findNearest(from.rect, cells.filter(cell => cell !== from), direction)?.id ?? null;
};

test('a grid moves one cell per direction and stops at the edges', () => {
  const cells = grid();
  assert.equal(move(cells, '11', 'up'), '01');
  assert.equal(move(cells, '11', 'down'), '21');
  assert.equal(move(cells, '11', 'left'), '10');
  assert.equal(move(cells, '11', 'right'), '12');
  assert.equal(move(cells, '00', 'up'), null);
  assert.equal(move(cells, '00', 'left'), null);
  assert.equal(move(cells, '22', 'right'), null);
});

test('orthogonal offset weighs double the primary distance', () => {
  const from = rect(0, 0);
  const aligned = {rect: rect(400, 0), order: 0};
  const diagonal = {rect: rect(150, 200), order: 1};
  // aligned: 300 + 0; diagonal: 50 + 2 × 100.
  assert.equal(findNearest(from, [aligned, diagonal], 'right'), diagonal);
  const far = {rect: rect(150, 300), order: 1};
  assert.equal(findNearest(from, [aligned, far], 'right'), aligned);
});

test('ties prefer more orthogonal overlap, then document order', () => {
  const from = rect(0, 0, 100, 100);
  const partial = {rect: rect(0, 120, 100, 100), order: 0};
  const shifted = {rect: rect(50, 120, 100, 100), order: 1};
  assert.equal(findNearest(from, [shifted, partial], 'down'), partial);
  const first = {rect: rect(0, 120), order: 0};
  const second = {rect: rect(0, 120), order: 1};
  assert.equal(findNearest(from, [second, first], 'down'), first);
});

test('an element containing the origin or behind it is not a candidate', () => {
  const from = rect(100, 100);
  assert.equal(findNearest(from, [{rect: rect(0, 0, 400, 400), order: 0}], 'right'), null);
  assert.equal(findNearest(from, [{rect: rect(50, 100), order: 0}], 'right'), null);
  assert.ok(findNearest(from, [{rect: rect(150, 100), order: 0}], 'right'));
});

test('wrap picks the far end of the same row or column', () => {
  const cells = grid();
  const at = id => cells.find(cell => cell.id === id);
  const others = id => cells.filter(cell => cell.id !== id);
  assert.equal(findWrap(at('12').rect, others('12'), 'right').id, '10');
  assert.equal(findWrap(at('10').rect, others('10'), 'left').id, '12');
  assert.equal(findWrap(at('21').rect, others('21'), 'down').id, '01');
  assert.equal(findWrap(at('00').rect, [], 'right'), null);
});

test('closest by centre distance', () => {
  const cells = grid();
  assert.equal(findClosest(rect(250, 250, 20, 20), cells).id, '22');
});

test('reveal scrolls the least, keeps a margin, and clamps', () => {
  assert.equal(revealOffset(0, 100, 50, 300, 1000, 10), 0);
  assert.equal(revealOffset(0, 400, 50, 300, 1000, 10), 160);
  assert.equal(revealOffset(500, 400, 50, 300, 1000, 10), 390);
  assert.equal(revealOffset(0, 5, 50, 300, 1000, 10), 0);
  assert.equal(revealOffset(0, 1400, 50, 300, 1000, 10), 1000);
  assert.equal(revealOffset(0, 400, 500, 300, 1000, 10), 390);
});

test('a scroll step approaches its target, capped per frame, and lands exactly', () => {
  assert.equal(approach(0, 100, 16, 70, 10), 100 * (1 - Math.exp(-16 / 70)));
  assert.equal(approach(0, 2000, 16, 70, 4), 64);
  assert.equal(approach(1000, 0, 16, 70, 4), 936);
  assert.equal(approach(99.6, 100, 16, 70, 4), 100);
  let y = 0;
  for (let frame = 0; frame < 120 && y !== 600; frame++) y = approach(y, 600, 16, 70, 4);
  assert.equal(y, 600);
});
