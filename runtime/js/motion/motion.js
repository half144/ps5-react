// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
// motion.* components: props describe targets, the engine animates them, React renders once per
// target change rather than per frame.
import {createContext, createElement, forwardRef, useContext, useEffect, useLayoutEffect, useReducer, useRef,
  useState} from 'react';
import {View} from 'embedded-react';
import {FOCUS_PROPS, isFocusable, resolveStyle, useFocusNode} from '../focus/focusable.js';
import {NodeContext} from '../focus/runtime.js';
import {engine} from './engine.js';
import {PresenceContext} from './presence.js';
import {collectKeys, labelsOf, pick, resolve} from './targets.js';
import {bindStyle, baseFromStyle, flatten} from './style.js';
import {childDelay, transitionFor} from './transitions.js';
import {MotionValues} from './values.js';

// Logical px are 1280-wide, the Tailwind default baseWidth.
const BASE_WIDTH = 1280;

/**
 * Variant labels a motion element passes down, and the registry its children stagger against. The
 * value is one object for the element's lifetime, updated as it renders; only descendants that use
 * its labels subscribe to them, so a label change re-renders those and not every motion element
 * below (a screen of focusable cards held about 150).
 * @typedef {{labels: string[], labelKey: string, initial: string[] | false | undefined,
 *   registry: {order: object[], transition?: object}, listeners: Set<() => void>}} Channel
 * @type {import('react').Context<Channel | null>}
 */
const MotionContext = createContext(null);

const isLabel = definition => typeof definition === 'string' || Array.isArray(definition);

/** Re-renders the caller when `channel`'s labels change, while `follow`. @param {Channel | null} channel */
function useLabels(channel, follow) {
  const [, rerender] = useReducer(count => count + 1, 0);
  const seen = useRef(undefined);
  seen.current = channel?.labelKey;
  useLayoutEffect(() => {
    if (!channel || !follow) return undefined;
    const listener = () => {
      if (channel.labelKey !== seen.current) rerender();
    };
    channel.listeners.add(listener);
    listener();
    return () => { channel.listeners.delete(listener); };
  }, [channel, follow]);
}

function useMotion(props) {
  const {initial, animate, exit, transition, variants, whileFocus, whileSelect, whilePress,
    onAnimationComplete, style, focused, selected, pressed} = props;
  const parent = useContext(MotionContext);
  const presence = useContext(PresenceContext);
  const state = useRef(null);
  if (!state.current) state.current = {values: null, mounted: false, animateKey: undefined, animatePending: null};
  const self = state.current;

  const inherited = animate === undefined;
  const controlling = [initial, animate, exit, whileFocus, whileSelect, whilePress].some(isLabel);
  // Inherited labels matter to an element that resolves them against its variants, or passes them on.
  useLabels(parent, inherited && (variants !== undefined || controlling));
  const exiting = presence ? !presence.isPresent : false;
  const keys = collectKeys([initial, animate, exit, whileFocus, whileSelect, whilePress], variants);
  const layers = [
    inherited
      ? {targets: resolve(parent?.labels, variants, 'animate', true), inherited: true}
      : {targets: resolve(animate, variants, 'animate'), inherited: false},
    focused && {targets: resolve(whileFocus, variants, 'whileFocus'), inherited: false},
    selected && {targets: resolve(whileSelect, variants, 'whileSelect'), inherited: false},
    pressed && {targets: resolve(whilePress, variants, 'whilePress'), inherited: false},
    exiting && {targets: resolve(exit, variants, 'exit'), inherited: false},
  ].filter(Boolean);
  const flat = flatten(style);

  let bound = null;
  let resolved = null;
  if (keys.size) {
    self.values ??= new MotionValues(engine, screen.width / BASE_WIDTH);
    const base = baseFromStyle(flat, self.values.scale);
    resolved = pick(layers, keys, base);
    if (!self.mounted) {
      const initialDefinition = presence?.initial === false ? false
        : initial !== undefined ? initial : parent?.initial;
      const from = initialDefinition === false || initialDefinition === undefined ? resolved
        : pick([{targets: resolve(initialDefinition, variants, 'initial', initial === undefined)}], keys, base);
      for (const key of keys) self.values.ensure(key, from[key].value);
    }
    for (const key of keys) self.values.ensure(key, resolved[key].value);
    bound = self.values.bound();
  }

  const labels = [
    ...(inherited ? parent?.labels ?? [] : labelsOf(animate)),
    ...(focused ? labelsOf(whileFocus) : []),
    ...(selected ? labelsOf(whileSelect) : []),
    ...(pressed ? labelsOf(whilePress) : []),
    ...(exiting ? labelsOf(exit) : []),
  ];
  const registry = useRef({order: []}).current;
  registry.transition = [...layers].reverse().flatMap(layer => layer.targets)
    .find(target => target.transition)?.transition ?? transition;
  const labelKey = labels.join('\0');
  const ownInitial = initial === false || isLabel(initial) ? labelsOf(initial) : undefined;
  const childInitial = initial === undefined ? parent?.initial : initial === false ? false : ownInitial;
  const [channel] = useState(() => ({labels, labelKey, initial: childInitial, registry, listeners: new Set()}));
  channel.labels = labels;
  channel.labelKey = labelKey;
  channel.initial = childInitial;
  useLayoutEffect(() => {
    for (const listener of [...channel.listeners]) listener();
  }, [labelKey]);

  const complete = useRef(onAnimationComplete);
  complete.current = onAnimationComplete;
  const parentRegistry = parent?.registry;
  useLayoutEffect(() => {
    if (!parentRegistry) return undefined;
    parentRegistry.order.push(self);
    return () => { parentRegistry.order.splice(parentRegistry.order.indexOf(self), 1); };
  }, [parentRegistry]);
  useLayoutEffect(() => presence?.register(self), [presence]);
  useEffect(() => () => {
    self.disposed = true;
    self.values?.dispose();
  }, []);

  const animateDefinition = inherited ? parent?.labels : animate;
  const animateKey = JSON.stringify(animateDefinition ?? null);
  useEffect(() => {
    self.mounted = true;
    self.exiting = exiting;
    self.presence = presence;
    if (animateKey !== self.animateKey) self.animatePending = {definition: animateDefinition};
    self.animateKey = animateKey;
    const finish = finished => {
      if (self.disposed || !finished) return;
      if (self.exiting) {
        self.presence.complete(self);
        return;
      }
      const pending = self.animatePending;
      self.animatePending = null;
      if (pending) complete.current?.(pending.definition);
    };
    if (!resolved) {
      finish(true);
      return;
    }
    const order = parentRegistry?.order ?? [];
    const delay = parentRegistry
      ? childDelay(parentRegistry.transition, order.indexOf(self), order.length) : 0;
    // An exit waits only for the keys it names, not for a repeating animation it leaves running.
    const exitKeys = exiting && new Set(layers[layers.length - 1].targets.flatMap(target => Object.keys(target)));
    const targets = {};
    for (const [key, {value, transition: own, inherited: fromParent}] of Object.entries(resolved)) {
      if (exitKeys && !exitKeys.has(key)) continue;
      targets[key] = {value, transition: transitionFor(own ?? transition, key), delay: fromParent ? delay : 0};
    }
    self.values.apply(targets, finish);
  });

  return {bound, flat, context: controlling ? channel : parent, consumedPresence: presence !== null};
}

const MOTION_PROPS = ['initial', 'animate', 'exit', 'transition', 'variants', 'whileFocus', 'whileSelect',
  'whilePress', 'onAnimationComplete', 'style'];

/**
 * Wraps a component so it accepts motion props; it must pass `style` to an engine node.
 * `wrapOpacity` fades a wrapper View instead (for Text and Image, which cannot fade themselves).
 * @template P
 * @param {import('react').ComponentType<P> | string} Component
 * @param {{wrapOpacity?: boolean}} [options]
 */
export function createMotion(Component, {wrapOpacity = false} = {}) {
  const MotionComponent = forwardRef((props, ref) => {
    // A focusable element drives whileFocus/whilePress and a style function from its own focus state.
    const focus = useFocusNode(props, isFocusable(props));
    const focused = props.focused ?? focus?.focused;
    const pressed = props.pressed ?? focus?.pressed;
    const ownStyle = resolveStyle(props.style, !!focused, !!pressed);
    const {bound, flat, context, consumedPresence} = useMotion({...props, focused, pressed, style: ownStyle});
    const rest = {ref};
    for (const key in props) if (!MOTION_PROPS.includes(key) && !FOCUS_PROPS.includes(key)) rest[key] = props[key];
    if (focus) rest.onLayout = focus.focusProps.onLayout;
    let element;
    if (bound) {
      const {style, outer} = bindStyle(flat, bound, wrapOpacity);
      element = createElement(Component, {...rest, style});
      if (outer) element = createElement(View, {style: outer}, element);
    } else {
      element = createElement(Component, {...rest, style: ownStyle});
    }
    if (focus) element = createElement(NodeContext.Provider, {value: focus.node}, element);
    element = createElement(MotionContext.Provider, {value: context}, element);
    return consumedPresence ? createElement(PresenceContext.Provider, {value: null}, element) : element;
  });
  MotionComponent.displayName = `motion(${typeof Component === 'string' ? Component
    : Component.displayName || Component.name || 'Component'})`;
  return MotionComponent;
}
