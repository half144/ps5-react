// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
// Spatial navigation over screen rectangles; see docs/NAVIGATION.md.

/**
 * @typedef {{x: number, y: number, width: number, height: number}} Rect
 * @typedef {'up' | 'down' | 'left' | 'right'} Direction
 * @typedef {{rect: Rect, order: number}} Candidate order is document order
 */

const HORIZONTAL = {left: -1, right: 1};

/** Edges along the pressed axis and across it, so every direction reads like `right`. */
function axes(rect, direction) {
  const horizontal = direction in HORIZONTAL;
  const sign = horizontal ? HORIZONTAL[direction] : direction === 'down' ? 1 : -1;
  const [start, size, crossStart, crossSize] = horizontal
    ? [rect.x, rect.width, rect.y, rect.height] : [rect.y, rect.height, rect.x, rect.width];
  const [near, far] = sign > 0 ? [start, start + size] : [-(start + size), -start];
  return {near, far, crossStart, crossEnd: crossStart + crossSize};
}

/** Length shared on the orthogonal axis; negative is the gap between the rectangles. */
function overlap(a, b) {
  return Math.min(a.crossEnd, b.crossEnd) - Math.max(a.crossStart, b.crossStart);
}

/** Whether candidate `a` (score `sa`, overlap `oa`) beats `b`: lower score, more overlap, earlier. */
function better(a, sa, oa, b, sb, ob) {
  if (sa !== sb) return sa < sb;
  if (oa !== ob) return oa > ob;
  return a.order < b.order;
}

/**
 * The candidate the D-pad moves to from `from`: among rectangles lying in `direction`, the one
 * minimising `primary + 2 × orthogonal` distance between nearest edges; ties prefer more orthogonal
 * overlap, then document order.
 * @template {Candidate} T
 * @param {Rect} from
 * @param {T[]} candidates
 * @param {Direction} direction
 * @returns {T | null}
 */
export function findNearest(from, candidates, direction) {
  const origin = axes(from, direction);
  let best = null;
  let bestScore = Infinity;
  let bestOverlap = -Infinity;
  for (const candidate of candidates) {
    const target = axes(candidate.rect, direction);
    if (!(target.far > origin.far && target.near > origin.near)) continue;
    const shared = overlap(origin, target);
    const score = Math.max(0, target.near - origin.far) + 2 * Math.max(0, -shared);
    if (!best || better(candidate, score, shared, best, bestScore, bestOverlap)) {
      best = candidate;
      bestScore = score;
      bestOverlap = shared;
    }
  }
  return best;
}

/**
 * Wrapping past the last element: the candidate in the same row (or column) farthest in the
 * opposite direction, or null when `from` is already the only one.
 * @template {Candidate} T
 * @param {Rect} from
 * @param {T[]} candidates
 * @param {Direction} direction
 * @returns {T | null}
 */
export function findWrap(from, candidates, direction) {
  const origin = axes(from, direction);
  let best = null;
  let bestNear = Infinity;
  let bestOverlap = -Infinity;
  for (const candidate of candidates) {
    const target = axes(candidate.rect, direction);
    const shared = overlap(origin, target);
    if (shared <= 0 || target.near >= origin.near) continue;
    if (!best || better(candidate, target.near, shared, best, bestNear, bestOverlap)) {
      best = candidate;
      bestNear = target.near;
      bestOverlap = shared;
    }
  }
  return best;
}

/**
 * The candidate whose centre is closest to `rect`'s, ties in document order.
 * @template {Candidate} T
 * @param {Rect} rect
 * @param {T[]} candidates
 * @returns {T | null}
 */
export function findClosest(rect, candidates) {
  const cx = rect.x + rect.width / 2;
  const cy = rect.y + rect.height / 2;
  let best = null;
  let bestDistance = Infinity;
  for (const candidate of candidates) {
    const {x, y, width, height} = candidate.rect;
    const distance = Math.hypot(x + width / 2 - cx, y + height / 2 - cy);
    if (distance < bestDistance || (distance === bestDistance && candidate.order < best.order)) {
      best = candidate;
      bestDistance = distance;
    }
  }
  return best;
}

/**
 * The scroll offset on one axis that brings an item into view with the least movement, keeping
 * `margin` around it; an item larger than the viewport aligns its start.
 * @param {number} offset current offset
 * @param {number} start item start in content coordinates
 * @param {number} size item size
 * @param {number} viewport viewport size
 * @param {number} max largest valid offset
 * @param {number} margin
 */
export function revealOffset(offset, start, size, viewport, max, margin) {
  let next = offset;
  if (start + size + margin > next + viewport) next = start + size + margin - viewport;
  if (start - margin < next) next = start - margin;
  return Math.min(Math.max(next, 0), max);
}

/**
 * One step of a scroll toward `target`: an exponential approach (time constant `tauMs`), so a new target
 * mid-scroll continues from the current position without restarting, capped at `maxPerMs` px per ms so a
 * held key never exposes more than a small strip per frame. Lands exactly on the target.
 * @param {number} current @param {number} target @param {number} dtMs @param {number} tauMs
 * @param {number} maxPerMs
 */
export function approach(current, target, dtMs, tauMs, maxPerMs) {
  const remaining = target - current;
  const cap = maxPerMs * dtMs;
  const step = remaining * (1 - Math.exp(-dtMs / tauMs));
  const bounded = Math.max(-cap, Math.min(cap, step));
  // The last pixel of an exponential tail would take many frames: finish it at once.
  return Math.abs(remaining - bounded) < 1 ? target : current + bounded;
}
