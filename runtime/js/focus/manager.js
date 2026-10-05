// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
// Focus state without React: focusable nodes, nested scopes, D-pad moves, press, back and recovery.
// Components keep one node or scope each and re-render only when their own node changes.
import {findClosest, findNearest, findWrap} from './geometry.js';

/**
 * @typedef {import('./geometry.js').Rect} Rect
 * @typedef {import('./geometry.js').Direction} Direction
 * @typedef {{parent: Frame | null, x: number, y: number, reveal: (rect: Rect) => void}} Frame
 *   A scrolling ancestor; `x`/`y` is how far its content is scrolled.
 * @typedef {{focusKey?: string, onPress?: () => void, onFocus?: () => void, onBlur?: () => void,
 *   autoFocus?: boolean, disabled?: boolean, nextFocusUp?: string, nextFocusDown?: string,
 *   nextFocusLeft?: string, nextFocusRight?: string}} NodeProps
 * @typedef {{focusKey?: string, autoFocus?: boolean, trap?: boolean, wrap?: boolean,
 *   restoreFocus?: boolean, onBack?: () => boolean | void}} ScopeProps
 * @typedef {{kind: 'scope', parent: Scope | null, props: ScopeProps, remembered: Node | null,
 *   returnTo: Node | null, dead: boolean}} Scope
 * @typedef {{kind: 'node', scope: Scope, frame: Frame | null, props: NodeProps, rect: Rect | null,
 *   order: number, pressed: boolean, dead: boolean, listeners: Set<() => void>,
 *   pressTimer: unknown}} Node
 */

const NEXT = {up: 'nextFocusUp', down: 'nextFocusDown', left: 'nextFocusLeft', right: 'nextFocusRight'};

/** @param {Scope | null} scope @param {Scope} ancestor */
function within(scope, ancestor) {
  for (; scope; scope = scope.parent) if (scope === ancestor) return true;
  return false;
}

export class FocusManager {
  /**
   * @param {{schedule?: (fn: () => void) => void, setTimer?: typeof setTimeout,
   *   clearTimer?: typeof clearTimeout, pressMs?: number}} [options]
   *   `schedule` runs focus recovery after the commit that unmounted the focused node.
   */
  constructor({schedule = fn => { Promise.resolve().then(fn); }, setTimer = setTimeout,
    clearTimer = clearTimeout, pressMs = 120} = {}) {
    Object.assign(this, {schedule, setTimer, clearTimer, pressMs});
    this.root = this.createScope(null, {});
    /** @type {Set<Node>} */
    this.nodes = new Set();
    /** @type {Map<string, Node | Scope>} */
    this.keys = new Map();
    /** @type {Node | null} */
    this.focused = null;
    /** @type {Set<() => void>} */
    this.listeners = new Set();
    this.order = 0;
  }

  /** @param {Scope | null} parent @param {ScopeProps} props @returns {Scope} */
  createScope(parent, props) {
    return {kind: 'scope', parent, props, remembered: null, returnTo: null, dead: false};
  }

  /**
   * Called while rendering, so `order` follows document order for elements mounted together.
   * @param {Scope} scope @param {Frame | null} frame @param {NodeProps} props @returns {Node}
   */
  createNode(scope, frame, props) {
    return {kind: 'node', scope, frame, props, rect: null, order: this.order++, pressed: false, dead: false,
      listeners: new Set(), pressTimer: null};
  }

  /** Replaces an entry's props, keeping its `focusKey` registered. @param {Node | Scope} entry @param {object} props */
  setProps(entry, props) {
    const previous = entry.props.focusKey;
    entry.props = props;
    if (entry.dead || previous === props.focusKey) return;
    if (previous != null && this.keys.get(previous) === entry) this.keys.delete(previous);
    if (props.focusKey != null) this.keys.set(props.focusKey, entry);
  }

  /** @param {Node} node */
  register(node) {
    node.dead = false;
    this.nodes.add(node);
    if (node.props.focusKey != null) this.keys.set(node.props.focusKey, node);
    if (node.props.autoFocus && !(this.focused && within(this.focused.scope, node.scope))) this.setFocus(node);
  }

  /** @param {Node} node */
  unregister(node) {
    node.dead = true;
    this.nodes.delete(node);
    if (this.keys.get(node.props.focusKey) === node) this.keys.delete(node.props.focusKey);
    this.clearTimer(node.pressTimer);
    if (this.focused !== node) return;
    this.focused = null;
    const lost = {scope: node.scope, rect: this.rectOf(node)};
    this.schedule(() => this.recover(lost));
  }

  /** @param {Scope} scope */
  mountScope(scope) {
    scope.dead = false;
    if (scope.props.focusKey != null) this.keys.set(scope.props.focusKey, scope);
    if (scope.props.autoFocus && !(this.focused && within(this.focused.scope, scope))) {
      const target = this.firstOf(scope);
      if (target) this.setFocus(target);
    }
  }

  /** @param {Scope} scope */
  unmountScope(scope) {
    scope.dead = true;
    if (this.keys.get(scope.props.focusKey) === scope) this.keys.delete(scope.props.focusKey);
  }

  /** Laid-out screen rectangle, before transforms and scrolling. @param {Node} node @param {Rect} rect */
  setRect(node, rect) {
    node.rect = rect;
  }

  /** @param {() => void} listener @returns {() => void} */
  subscribe(listener) {
    this.listeners.add(listener);
    return () => { this.listeners.delete(listener); };
  }

  /**
   * Focuses a node, a scope (its remembered or first element), or the entry registered under a key.
   * @param {Node | Scope | string} target
   * @returns {boolean} whether something took focus
   */
  focus(target) {
    const entry = typeof target === 'string' ? this.keys.get(target) : target;
    const node = entry?.kind === 'scope' ? this.firstOf(entry) : entry;
    if (!node || node.dead) return false;
    this.setFocus(node);
    return true;
  }

  blur() {
    this.setFocus(null);
  }

  /** @param {Direction} direction */
  move(direction) {
    const current = this.focused;
    if (!current) {
      this.focus(this.root);
      return;
    }
    const override = current.props[NEXT[direction]];
    if (override != null) {
      this.focus(override);
      return;
    }
    const from = this.rectOf(current);
    if (!from) return;
    for (let scope = current.scope; scope; scope = scope.parent) {
      const candidates = this.candidates(scope).filter(candidate => candidate.node !== current);
      const winner = findNearest(from, candidates, direction)
        ?? (scope.props.wrap ? findWrap(from, candidates, direction) : null);
      if (winner) {
        this.setFocus(this.entryPoint(winner.node, current));
        return;
      }
      if (scope.props.trap) return;
    }
  }

  /** Cross: press feedback for `pressMs`, then the focused element's onPress. */
  press() {
    const node = this.focused;
    if (!node) return;
    this.clearTimer(node.pressTimer);
    node.pressed = true;
    node.pressTimer = this.setTimer(() => {
      node.pressed = false;
      this.notify(node);
    }, this.pressMs);
    this.notify(node);
    if (!node.props.disabled) node.props.onPress?.();
  }

  /** Circle: each enclosing scope's `onBack`, innermost first, until one consumes it. @returns {boolean} */
  back() {
    for (let scope = this.focused?.scope ?? this.root; scope; scope = scope.parent) {
      if (scope.props.onBack?.() === true) return true;
    }
    return false;
  }

  /** @param {Node} node @returns {Rect | null} the rectangle on screen, scrolling applied */
  rectOf(node) {
    let rect = node.rect;
    if (!rect) return null;
    for (let frame = node.frame; frame; frame = frame.parent) {
      rect = {...rect, x: rect.x - frame.x, y: rect.y - frame.y};
    }
    return rect;
  }

  /** @param {Scope} scope @returns {Array<{node: Node, rect: Rect, order: number}>} */
  candidates(scope) {
    const result = [];
    for (const node of this.nodes) {
      if (!within(node.scope, scope)) continue;
      const rect = this.rectOf(node);
      if (rect && (rect.width > 0 || rect.height > 0)) result.push({node, rect, order: node.order});
    }
    return result;
  }

  /** @param {Scope} scope @returns {Node | null} */
  firstOf(scope) {
    if (scope.remembered && !scope.remembered.dead && scope.props.restoreFocus !== false) return scope.remembered;
    let first = null;
    for (const node of this.nodes) {
      if (within(node.scope, scope) && (!first || node.order < first.order)) first = node;
    }
    return first;
  }

  /** The node to focus when a move lands on `winner`: entering a scope restores its remembered element. */
  entryPoint(winner, current) {
    let entered = null;
    for (let scope = winner.scope; scope && !within(current.scope, scope); scope = scope.parent) entered = scope;
    const remembered = entered?.remembered;
    return remembered && !remembered.dead && entered.props.restoreFocus !== false ? remembered : winner;
  }

  /** @param {Node | null} next */
  setFocus(next) {
    const previous = this.focused;
    if (previous === next) return;
    this.focused = next;
    for (let scope = next?.scope; scope; scope = scope.parent) {
      if (!previous || !within(previous.scope, scope)) scope.returnTo = previous;
      scope.remembered = next;
    }
    if (previous) {
      this.notify(previous);
      previous.props.onBlur?.();
    }
    if (next) {
      this.notify(next);
      next.props.onFocus?.();
      this.reveal(next);
    }
    for (const listener of this.listeners) listener();
  }

  /** Scrolls every scrolling ancestor, innermost first, until `node` is in view. @param {Node} node */
  reveal(node) {
    let rect = node.rect;
    if (!rect) return;
    for (let frame = node.frame; frame; frame = frame.parent) {
      frame.reveal(rect);
      rect = {...rect, x: rect.x - frame.x, y: rect.y - frame.y};
    }
  }

  /** After the focused node unmounted: a dead scope's previous focus, else the nearest survivor. */
  recover({scope, rect}) {
    if (this.focused) return;
    for (; scope; scope = scope.parent) {
      if (scope.dead) {
        if (scope.returnTo && !scope.returnTo.dead) {
          this.setFocus(scope.returnTo);
          return;
        }
        continue;
      }
      const target = (rect && findClosest(rect, this.candidates(scope))?.node) ?? this.firstOf(scope);
      if (target) {
        this.setFocus(target);
        return;
      }
    }
    for (const listener of this.listeners) listener();
  }

  /** @param {Node} node */
  notify(node) {
    for (const listener of node.listeners) listener();
  }
}
