// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
// One Tailwind utility → one style fragment. Fragments may carry `$transform`, `$leading`, and
// `$tracking`, which compile.mjs resolves once a whole class list is merged.
import {STATIC, REJECTED} from './static.mjs';
import {arbitrary, fraction, isColor, paletteColor, parseLength, parseNumber, withAlpha} from './values.mjs';

export class UtilityError extends Error {}

const unsupported = message => { throw new UtilityError(message); };

/** A length from a theme scale, a fraction, `full`, `screen`, or an arbitrary value. */
function length(value, ctx, scale, {percent = false, screen} = {}) {
  if (value in scale && scale[value] !== undefined) return ctx.px(scale[value]);
  if (value === 'screen' && screen) return screen === 'width' ? ctx.renderWidth : ctx.renderHeight;
  const pct = fraction(value);
  if (pct) return percent ? pct
    : unsupported(`the engine accepts percentages only for width, height, insets, and basis`);
  const parsed = parseLength(arbitrary(value) ?? '');
  if (parsed?.pct) return percent ? parsed.pct
    : unsupported(`the engine accepts percentages only for width, height, insets, and basis`);
  return parsed ? ctx.px(parsed.px) : null;
}

function negate(value) {
  return typeof value === 'number' ? -value : value.startsWith('-') ? value.slice(1) : `-${value}`;
}

/** Splits `color/alpha` without touching a slash inside brackets. */
function splitModifier(value) {
  const match = /^(.+?)\/([^/\]]+|\[[^\]]+\])$/.exec(value);
  return match && !/\[[^\]]*$/.test(match[1]) ? [match[1], match[2]] : [value, null];
}

function color(value, ctx) {
  const [name, modifier] = splitModifier(value);
  const raw = arbitrary(name);
  const base = raw !== null ? (isColor(raw) ? raw : null) : paletteColor(ctx.theme.colors, name);
  if (base === null) return null;
  if (modifier === null) return base;
  const inner = arbitrary(modifier);
  const alpha = inner === null ? ctx.theme.opacity[modifier]
    : inner.endsWith('%') ? parseFloat(inner) / 100 : parseNumber(inner);
  if (alpha === undefined || alpha === null || alpha < 0 || alpha > 1) return null;
  try {
    return withAlpha(base, alpha);
  } catch (error) {
    unsupported(error.message);
  }
}

const sides = (keys, value) => value === null ? null
  : Object.fromEntries(keys.map(key => [key, value]));

const box = {
  '': [''], x: ['Horizontal'], y: ['Vertical'], t: ['Top'], r: ['Right'], b: ['Bottom'],
  l: ['Left'], s: ['Left'], e: ['Right'],
};

function spacingUtility(property, negatable) {
  return Object.entries(box).map(([side, suffixes]) => [`${property[0]}${side}`, (value, ctx, negative) => {
    if (value === 'auto') unsupported('auto margins are not supported by the layout engine; ' +
      'use self-center, or items-center/justify-center on the parent');
    if (negative && !negatable) return null;
    const v = length(value, ctx, ctx.theme.spacing);
    return v === null ? null
      : sides(suffixes.map(s => property + s), negative ? negate(v) : v);
  }]);
}

const insets = {
  inset: ['top', 'right', 'bottom', 'left'], 'inset-x': ['left', 'right'],
  'inset-y': ['top', 'bottom'], top: ['top'], right: ['right'], bottom: ['bottom'],
  left: ['left'], start: ['left'], end: ['right'],
};

const radii = {
  rounded: ['borderRadius'],
  'rounded-t': ['borderTopLeftRadius', 'borderTopRightRadius'],
  'rounded-r': ['borderTopRightRadius', 'borderBottomRightRadius'],
  'rounded-b': ['borderBottomLeftRadius', 'borderBottomRightRadius'],
  'rounded-l': ['borderTopLeftRadius', 'borderBottomLeftRadius'],
  'rounded-tl': ['borderTopLeftRadius'], 'rounded-tr': ['borderTopRightRadius'],
  'rounded-br': ['borderBottomRightRadius'], 'rounded-bl': ['borderBottomLeftRadius'],
};

const borders = {
  border: ['', ''], 'border-x': ['Left', 'Right'], 'border-y': ['Top', 'Bottom'],
  'border-t': ['Top'], 'border-r': ['Right'], 'border-b': ['Bottom'], 'border-l': ['Left'],
};

function borderUtility([name, edges]) {
  return [name, (value, ctx) => {
    const width = value === '' ? ctx.theme.borderWidth.DEFAULT
      : value in ctx.theme.borderWidth ? ctx.theme.borderWidth[value] : null;
    if (width !== null) return sides(edges.map(e => `border${e}Width`), ctx.px(width));
    const c = color(value, ctx);
    if (c !== null) return sides(edges.map(e => `border${e}Color`), c);
    const parsed = parseLength(arbitrary(value) ?? '');
    return parsed?.px !== undefined ? sides(edges.map(e => `border${e}Width`), ctx.px(parsed.px)) : null;
  }];
}

function fontSize(value, ctx) {
  const [name, modifier] = splitModifier(value);
  let entry = ctx.theme.fontSize[name];
  if (entry === undefined) {
    const parsed = parseLength(arbitrary(name) ?? '');
    if (parsed?.px === undefined) return null;
    entry = [parsed.px];
  }
  const [size, extra] = Array.isArray(entry) ? entry : [entry];
  const px = parseLength(size)?.px;
  if (px === undefined) return null;
  const style = {fontSize: ctx.px(px)};
  const leading = modifier === null ? (typeof extra === 'object' ? extra.lineHeight : extra)
    : lineHeight(modifier, ctx);
  if (leading === null) return null;
  return leading === undefined ? style : {...style, $textLeading: leadingValue(leading, ctx)};
}

function lineHeight(value, ctx) {
  if (value in ctx.theme.lineHeight) return ctx.theme.lineHeight[value];
  const inner = arbitrary(value);
  return inner !== null && (parseNumber(inner) !== null || parseLength(inner)?.px !== undefined) ? inner : null;
}

/**
 * `{px}` for a fixed line height or `{em}` for a multiple of the font size: a unitless string
 * (as in CSS) or a theme number below 4.
 */
function leadingValue(value, ctx) {
  if (typeof value === 'string' && parseNumber(value) !== null) return {em: Number(value)};
  return value < 4 ? {em: value} : {px: ctx.px(value)};
}

function fontWeight(value, ctx) {
  if (value in ctx.theme.fontWeight) return {fontWeight: Number(ctx.theme.fontWeight[value])};
  const inner = arbitrary(value);
  if (inner !== null && parseNumber(inner) !== null) return {fontWeight: Number(inner)};
  return null;
}

function fontFamily(value, ctx) {
  const family = ctx.theme.fontFamily[value] ?? arbitrary(value);
  if (family === undefined || family === null) {
    if (['sans', 'serif', 'mono'].includes(value)) unsupported(`font-${value} needs a font: ` +
      `import a .ttf/.otf and map it in tailwind.config.js theme.fontFamily`);
    return null;
  }
  return ctx.font(Array.isArray(family) ? family[0] : family);
}

const transform = (key, value) => value === null ? null : {$transform: {[key]: value}};

function scaleValue(value, ctx) {
  if (value in ctx.theme.scale) return Number(ctx.theme.scale[value]);
  return parseNumber(arbitrary(value) ?? '');
}

function angle(value, ctx) {
  if (value in ctx.theme.rotate) return `${parseFloat(ctx.theme.rotate[value])}deg`;
  const inner = arbitrary(value);
  return inner && /^-?[\d.]+(deg|rad)$/.test(inner) ? inner : null;
}

function translate(value, ctx) {
  if (fraction(value) || arbitrary(value)?.endsWith('%'))
    unsupported('the engine translates by pixels only; percentage translations are not supported');
  return length(value, ctx, ctx.theme.spacing);
}

function themeNumber(scale, integer = false) {
  return (value, ctx, negative) => {
    let v = value in ctx.theme[scale] ? Number(ctx.theme[scale][value]) : parseNumber(arbitrary(value) ?? '');
    if (v === null || (integer && !Number.isInteger(v))) return null;
    return negative ? -v : v;
  };
}

const sized = (keys, options) => (value, ctx) =>
  sides(keys, length(value, ctx, ctx.theme.spacing, options));

const zIndex = themeNumber('zIndex', true);
const opacity = themeNumber('opacity');
const lineClamp = themeNumber('lineClamp', true);

// [prefix, handler(value, ctx, negative)]; a handler returns null when the value is not its kind.
const NEGATABLE = new Set(['m', 'mx', 'my', 'mt', 'mr', 'mb', 'ml', 'ms', 'me', ...Object.keys(insets),
  'z', 'tracking', 'translate-x', 'translate-y', 'rotate', 'scale', 'scale-x', 'scale-y']);

const FUNCTIONAL = [
  ...spacingUtility('padding', false),
  ...spacingUtility('margin', true),
  ['gap', sized(['gap'])],
  ['gap-x', sized(['columnGap'])],
  ['gap-y', sized(['rowGap'])],
  ['w', sized(['width'], {percent: true, screen: 'width'})],
  ['h', sized(['height'], {percent: true, screen: 'height'})],
  ['size', sized(['width', 'height'], {percent: true})],
  ['min-w', sized(['minWidth'], {screen: 'width'})],
  ['min-h', sized(['minHeight'], {screen: 'height'})],
  ['max-w', (value, ctx) => value in ctx.theme.maxWidth && ctx.theme.maxWidth[value] !== undefined
    ? {maxWidth: ctx.px(ctx.theme.maxWidth[value])}
    : sides(['maxWidth'], length(value, ctx, ctx.theme.spacing, {screen: 'width'}))],
  ['max-h', sized(['maxHeight'], {screen: 'height'})],
  ['basis', sized(['flexBasis'], {percent: true})],
  ...Object.entries(insets).map(([name, keys]) => [name, (value, ctx, negative) => {
    const v = length(value, ctx, ctx.theme.spacing, {percent: true});
    return sides(keys, v !== null && negative ? negate(v) : v);
  }]),
  ['z', (value, ctx, negative) => sides(['zIndex'], zIndex(value, ctx, negative))],
  ['opacity', (value, ctx) => sides(['opacity'], opacity(value, ctx))],
  ['flex', (value, ctx) => {
    const v = parseNumber(arbitrary(value) ?? value);
    return v === null ? null : {flex: v};
  }],
  ...['grow', 'flex-grow'].map(name => [name, value =>
    sides(['flexGrow'], value === '' ? 1 : parseNumber(arbitrary(value) ?? value))]),
  ...['shrink', 'flex-shrink'].map(name => [name, value =>
    sides(['flexShrink'], value === '' ? 1 : parseNumber(arbitrary(value) ?? value))]),
  ['aspect', (value, ctx) => {
    if (value in ctx.theme.aspectRatio) return {aspectRatio: ctx.theme.aspectRatio[value]};
    const inner = arbitrary(value)?.replaceAll(' ', '');
    const [w, h = 1] = inner?.split('/').map(Number) ?? [];
    return w > 0 && h > 0 ? {aspectRatio: w / h} : null;
  }],
  ['bg', (value, ctx) => sides(['backgroundColor'], color(value, ctx))],
  ['text', (value, ctx) => fontSize(value, ctx) ?? sides(['color'], color(value, ctx))],
  ['font', (value, ctx) => fontWeight(value, ctx) ?? fontFamily(value, ctx)],
  ['leading', (value, ctx) => {
    const v = lineHeight(value, ctx);
    return v === null ? null : {$leading: leadingValue(v, ctx)};
  }],
  ['tracking', (value, ctx, negative) => {
    const raw = value in ctx.theme.letterSpacing ? ctx.theme.letterSpacing[value] : arbitrary(value) ?? '';
    const sign = negative ? -1 : 1;
    if (typeof raw === 'number' || /^-?(\d+\.?\d*|\.\d+)em$/.test(raw)) return {$tracking: {em: sign * parseFloat(raw)}};
    const parsed = parseLength(raw);
    return parsed?.px === undefined ? null
      : {$tracking: {px: Number((sign * parsed.px * ctx.scale).toFixed(2))}};
  }],
  ['line-clamp', (value, ctx) => {
    const lines = lineClamp(value, ctx);
    return lines === null ? null : {numberOfLines: lines, ellipsizeMode: 'tail'};
  }],
  ...Object.entries(borders).map(borderUtility),
  ...Object.entries(radii).map(([name, keys]) => [name, (value, ctx) => {
    const key = value === '' ? 'DEFAULT' : value;
    const v = key in ctx.theme.borderRadius ? ctx.px(ctx.theme.borderRadius[key])
      : length(value, ctx, {});
    return sides(keys, v);
  }]),
  ['scale', (value, ctx, negative) => {
    const v = scaleValue(value, ctx);
    return v === null ? null : {$transform: {scaleX: negative ? -v : v, scaleY: negative ? -v : v}};
  }],
  ...['x', 'y'].map(axis => [`scale-${axis}`, (value, ctx, negative) => {
    const v = scaleValue(value, ctx);
    return transform(`scale${axis.toUpperCase()}`, v !== null && negative ? -v : v);
  }]),
  ['rotate', (value, ctx, negative) => {
    const a = angle(value, ctx);
    return transform('rotate', a && negative ? negate(a) : a);
  }],
  ['translate-x', (value, ctx, negative) => {
    const v = translate(value, ctx);
    return transform('translateX', v !== null && negative ? -v : v);
  }],
  ['translate-y', (value, ctx, negative) => {
    const v = translate(value, ctx);
    return transform('translateY', v !== null && negative ? -v : v);
  }],
  ['tint', (value, ctx) => sides(['tintColor'], color(value, ctx))],
  ['caret', (value, ctx) => sides(['cursorColor'], color(value, ctx))],
].sort((a, b) => b[0].length - a[0].length);

/** Resolves one utility (no variant) to a style fragment, or throws UtilityError. */
export function resolveUtility(utility, ctx) {
  if (utility in STATIC) return STATIC[utility];
  const negative = utility.startsWith('-');
  const name = negative ? utility.slice(1) : utility;
  for (const [prefix, handler] of FUNCTIONAL) {
    if (name !== prefix && !name.startsWith(prefix + '-')) continue;
    if (negative && !NEGATABLE.has(prefix)) continue;
    const style = handler(name.slice(prefix.length + 1), ctx, negative);
    if (style !== null && Object.values(style).every(v => v !== null && !Number.isNaN(v))) return style;
  }
  for (const [prefix, reason] of REJECTED) {
    if (name === prefix || name.startsWith(prefix + '-')) unsupported(reason);
  }
  unsupported('unknown utility');
}
