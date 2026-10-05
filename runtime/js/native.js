// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
// React Native-style modules over the host contract in native/shared/host_api.hpp.
// Every call is synchronous and runs on the render thread; see docs/NATIVE-API.md.
import {useEffect, useState} from 'react';

function host() {
  const api = globalThis.__ps5ReactNative;
  if (!api) {
    throw new Error('Native modules need a PS5 React host (globalThis.__ps5ReactNative is missing); '
      + 'run the app with npm run dev, npm run preview, or a PS5 build');
  }
  return api;
}

/**
 * @typedef {{name: string, isDirectory: boolean, isFile: boolean, size: number, modified: number}} FileStat
 *   `modified` is milliseconds since the Unix epoch.
 * @typedef {{device: string, path: string, type: string}} Mount
 * @typedef {{id: number, name: string}} User
 * @typedef {{model: string | null, firmware: string | null, cpuTemperature: number | null,
 *   socTemperature: number | null, cpuFrequency: number | null, freeMemory: number | null,
 *   processTime: number | null}} Device
 *   Temperatures in °C, frequency in Hz, memory in bytes, process time in µs.
 * @typedef {'up' | 'down' | 'left' | 'right' | 'cross' | 'circle' | 'triangle' | 'square' | 'l1' | 'r1'
 *   | 'l2' | 'r2' | 'l3' | 'r3' | 'options' | 'touchpad'} Button
 * @typedef {{connected: boolean, leftX: number, leftY: number, rightX: number, rightY: number,
 *   l2: number, r2: number, buttons: Button[]}} GamepadState
 *   Sticks are -1..1 with the deadzone applied; triggers are 0..1.
 */

export const Platform = {
  /** @returns {'ps5' | 'desktop'} */
  get OS() { return host().platform; },
  /**
   * @template T
   * @param {{ps5?: T, desktop?: T, default?: T}} spec
   * @returns {T | undefined}
   */
  select(spec) {
    const os = host().platform;
    return os in spec ? spec[os] : spec.default;
  },
};

export const DeviceInfo = {
  /** @returns {Device} */
  get: () => host().device.info(),
};

// The title is sandboxed: paths are app paths such as /app0 (read-only) and /download0.
export const FileSystem = {
  appDir: '/app0',
  dataDir: '/download0',
  tempDir: '/temp0',
  /** @param {string} path @returns {FileStat[]} */
  readDir: path => host().fs.readDir(path),
  /** @param {string} path @returns {FileStat | null} */
  stat: path => host().fs.stat(path),
  /** @param {string} path @returns {boolean} */
  exists: path => host().fs.stat(path) !== null,
  /** UTF-8, at most 8 MiB. @param {string} path @returns {string} */
  readFile: path => host().fs.readFile(path),
  /** @param {string} path @param {string} text */
  writeFile: (path, text) => host().fs.writeFile(path, text, false),
  /** @param {string} path @param {string} text */
  appendFile: (path, text) => host().fs.writeFile(path, text, true),
  /** @param {string} path @param {{recursive?: boolean}} [options] */
  mkdir: (path, {recursive = false} = {}) => host().fs.mkdir(path, recursive),
  /** Removes a file or an empty directory. @param {string} path */
  remove: path => host().fs.remove(path),
  /** @param {string} from @param {string} to */
  rename: (from, to) => host().fs.rename(from, to),
  /** @returns {Mount[]} */
  mounts: () => host().fs.mounts(),
  /** @param {string} path @returns {{total: number, free: number}} bytes */
  diskUsage: path => host().fs.diskUsage(path),
};

export const Notifications = {
  /** @param {string} message @param {string} [subMessage] @returns {boolean} */
  show: (message, subMessage = '') => host().notify(message, subMessage),
};

export const Users = {
  /** @returns {User | null} */
  getForeground: () => host().users.foreground(),
  /** @returns {User[]} */
  getLoggedIn: () => host().users.loggedIn(),
};

function rgb(color) {
  if (typeof color !== 'string') return color;
  const match = /^#([0-9a-f]{2})([0-9a-f]{2})([0-9a-f]{2})$/i.exec(color);
  if (!match) throw new Error(`Controller.setLightBar: expected '#rrggbb' or {r, g, b}, got '${color}'`);
  const [r, g, b] = match.slice(1).map(hex => parseInt(hex, 16));
  return {r, g, b};
}

export const Controller = {
  /** @param {string | {r: number, g: number, b: number}} color '#rrggbb' or 0..255 channels */
  setLightBar(color) {
    const {r, g, b} = rgb(color);
    host().pad.setLightBar(r, g, b);
  },
  resetLightBar: () => host().pad.resetLightBar(),
  /** @param {number} [strength] 0..1 @param {number} [ms] */
  vibrate: (strength = 1, ms = 200) => host().pad.vibrate(strength, ms / 1000),
  /** @returns {GamepadState} */
  getState: () => host().pad.state(),
};

function sameState(a, b) {
  return a.connected === b.connected && a.leftX === b.leftX && a.leftY === b.leftY
    && a.rightX === b.rightX && a.rightY === b.rightY && a.l2 === b.l2 && a.r2 === b.r2
    && a.buttons.join() === b.buttons.join();
}

/**
 * Polls the controller while mounted and re-renders only when the state changes.
 * @param {number} [intervalMs]
 * @returns {GamepadState}
 */
export function useGamepad(intervalMs = 16) {
  const [state, setState] = useState(Controller.getState);
  useEffect(() => {
    const id = setInterval(() => {
      const next = Controller.getState();
      setState(previous => (sameState(previous, next) ? previous : next));
    }, intervalMs);
    return () => clearInterval(id);
  }, [intervalMs]);
  return state;
}

export const Linking = {
  /** Opens an http(s) URL in the system browser. @param {string} url @returns {boolean} */
  openURL(url) {
    if (!/^https?:\/\//i.test(url)) throw new Error(`Linking.openURL: only http and https URLs are supported, got '${url}'`);
    return host().openURL(url);
  },
};

const backListeners = [];

export const BackHandler = {
  /** Closes the app after the current frame, like React Native's BackHandler.exitApp. */
  exitApp: () => host().exit(),
  /**
   * Circle, after the innermost FocusScope.onBack; the last listener added runs first.
   * @param {'hardwareBackPress'} event
   * @param {() => boolean | void} listener returns true to consume Circle
   * @returns {{remove: () => void}}
   */
  addEventListener(event, listener) {
    if (event !== 'hardwareBackPress') {
      throw new Error(`BackHandler.addEventListener: only 'hardwareBackPress' is supported, got '${event}'`);
    }
    backListeners.push(listener);
    return {remove() {
      const index = backListeners.indexOf(listener);
      if (index !== -1) backListeners.splice(index, 1);
    }};
  },
};

/** Internal: offers Circle to the listeners, last added first. @returns {boolean} consumed */
export function dispatchBackPress() {
  for (const listener of [...backListeners].reverse()) if (listener() === true) return true;
  return false;
}
