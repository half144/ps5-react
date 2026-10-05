// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
import assert from 'node:assert/strict';
import {createRequire} from 'node:module';
import {resolve} from 'node:path';
import {test} from 'node:test';
import {fileURLToPath} from 'node:url';
import {createCompiler} from './compile.mjs';
import {transform} from './esbuild-plugin.mjs';

const root = resolve(fileURLToPath(import.meta.url), '../../..');
const er = process.env.PS5_REACT_ER ?? resolve(root, '.deps/embeddedReact');
const require = createRequire(resolve(er, 'bridges/quickjs/js/package.json'));
const babel = {core: require('@babel/core'), syntaxJsx: require('@babel/plugin-syntax-jsx').default};
const compiler = createCompiler({renderWidth: 1280, renderHeight: 720, appDir: '/app',
  config: {theme: {extend: {fontFamily: {inter: './Inter.ttf'}}}}});

const compile = source => transform(babel, compiler, source, '/app/index.jsx').replace(/\s+/g, ' ').trim();
const fails = (source, pattern) => assert.throws(() => compile(source), pattern);

test('static className becomes a style object', () => {
  assert.equal(compile('<View className="p-4 bg-white" />;'),
    '<View style={{ padding: 16, backgroundColor: "#fff" }} />;');
});

test('explicit style keeps precedence', () => {
  assert.equal(compile('<View style={s} className="p-4" />;'), '<View style={[{ padding: 16 }, s]} />;');
});

test('conditional and logical classes stay conditional', () => {
  assert.equal(compile('<View className={`p-2 ${on ? "bg-white" : "bg-black"}`} />;'),
    '<View style={[{ padding: 8 }, on ? { backgroundColor: "#fff" } : { backgroundColor: "#000" }]} />;');
  assert.equal(compile('<View className={on && "p-1"} />;'), '<View style={on ? { padding: 4 } : null} />;');
});

test('const identifiers resolve at build time', () => {
  assert.match(compile('const card = "p-1"; <View className={card} />;'), /style=\{\{ padding: 4 \}\}/);
});

test('variants read the same-named prop and compose transforms', () => {
  assert.equal(compile('<View focused={i === f} className="scale-100 translate-x-1 focused:scale-110" />;'),
    '<View focused={i === f} style={[{ transform: [{ translateX: 4 }, { scale: 1 }] }, ' +
    'i === f ? { transform: [{ translateX: 4 }, { scale: 1.1 }] } : null]} />;');
  fails('<View className="focused:p-1" />;', /needs a focused=\{\.\.\.\} prop/);
  assert.equal(compile('<View focused={on} className="hover:p-1 focus:m-1" />;'),
    '<View focused={on} style={on ? { padding: 4, margin: 4 } : null} />;');
  fails('<View focused={check()} className="focused:p-1" />;', /read twice/);
});

test('relative leading in a branch uses the static font size', () => {
  assert.match(compile('<Text className={`text-base ${a ? "leading-tight" : ""}`} />;'),
    /a \? \{ lineHeight: 20 \} : null/);
});

test('tw templates from @ps5-react/core compile to objects', () => {
  assert.match(compile('import {tw} from "@ps5-react/core"; const s = tw`m-1`;'), /const s = \{ margin: 4 \};/);
  assert.match(compile('const tw = x => x; const s = tw`m-1`;'), /tw`m-1`/);
});

test('font paths are imported for baking', () => {
  assert.match(compile('<Text className="font-inter" />;'), /^import "\/app\/Inter\.ttf";.*fontFamily: "Inter"/);
});

test('dynamic or partial classes are rejected with a location', () => {
  fails('<View className={cls} />;', /known at build time/);
  fails('<View className={`bg-${c}`} />;', /whole words/);
  assert.throws(() => compile('<View\n  className="p-4 shadow-lg" />;'),
    error => assert.deepEqual(error.loc, {line: 2, column: 17}) ?? true);
});
