// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
// Babel plugin: `className` on any JSX element and `tw` templates from '@ps5-react/core' become
// literal style objects. Conditional classes become conditional style entries; state variants read
// the element's prop of the same name. Nothing class-related survives into the bundle.
import {ClassError} from './compile.mjs';

export class TailwindError extends Error {
  constructor(message, node, token) {
    super(message);
    this.reason = message;
    this.loc = locate(node, token);
  }
}

/** Line/column of `token` inside a single-line string, else of the node itself. */
function locate(node, token) {
  if (!node?.loc) return null;
  const {line, column} = node.loc.start;
  const text = node.type === 'TemplateElement' ? node.value.raw : node.value;
  const offset = typeof text === 'string' && token ? text.indexOf(token) : -1;
  const sameLine = offset >= 0 && !text.slice(0, offset).includes('\n');
  return {line, column: sameLine ? column + (node.type === 'StringLiteral' ? 1 : 0) + offset : column};
}

const PURE = {
  Identifier: () => true,
  StringLiteral: () => true, NumericLiteral: () => true, BooleanLiteral: () => true, NullLiteral: () => true,
  MemberExpression: (n, pure) => pure(n.object) && (!n.computed || pure(n.property)),
  UnaryExpression: (n, pure) => n.operator !== 'delete' && pure(n.argument),
  BinaryExpression: (n, pure) => pure(n.left) && pure(n.right),
  LogicalExpression: (n, pure) => pure(n.left) && pure(n.right),
  ConditionalExpression: (n, pure) => pure(n.test) && pure(n.consequent) && pure(n.alternate),
  TemplateLiteral: (n, pure) => n.expressions.every(pure),
};
const isPure = node => PURE[node.type]?.(node, isPure) ?? false;

export default function tailwindPlugin({types: t}, {compiler}) {
  /** Class expression → segments: strings (with their node) and `{test, consequent, alternate}`. */
  function collect(path, seen = new Set()) {
    const node = path.node;
    if (path.isStringLiteral()) return [{classes: node.value, node}];
    if (path.isNullLiteral() || path.isIdentifier({name: 'undefined'}) ||
      path.isBooleanLiteral({value: false})) return [];
    if (path.isTemplateLiteral()) {
      const quasis = path.get('quasis');
      return quasis.flatMap((quasi, i) => {
        const text = quasi.node.value.cooked;
        const expression = path.get('expressions')[i];
        if (expression && ((text && !/\s$/.test(text)) ||
          /^\S/.test(quasis[i + 1].node.value.cooked))) {
          throw new TailwindError('class names must be whole words; `bg-${...}` cannot be compiled. ' +
            'Write complete classes, e.g. ${active ? "bg-sky-800" : "bg-slate-800"}', quasi.node);
        }
        return [{classes: text, node: quasi.node}, ...(expression ? collect(expression, seen) : [])];
      });
    }
    if (path.isConditionalExpression()) return [{test: node.test,
      consequent: collect(path.get('consequent'), seen), alternate: collect(path.get('alternate'), seen)}];
    if (path.isLogicalExpression({operator: '&&'}))
      return [{test: node.left, consequent: collect(path.get('right'), seen), alternate: []}];
    if (path.isIdentifier()) {
      const binding = path.scope.getBinding(node.name);
      const init = binding?.path.isVariableDeclarator() && binding.kind === 'const' && binding.path.get('init');
      if (init?.node && !seen.has(binding)) return collect(init, new Set(seen).add(binding));
    }
    throw new TailwindError('className must be known at build time: a string, a template literal, ' +
      '`cond ? "a" : "b"`, `cond && "a"`, or a const holding one of these', node);
  }

  function parse(segments) {
    const statics = segments.filter(s => 'classes' in s);
    for (const {classes, node} of statics) {
      try {
        compiler.parse(classes);
      } catch (error) {
        if (error instanceof ClassError) throw new TailwindError(error.message, node, error.token);
        throw error;
      }
    }
    return compiler.parse(statics.map(s => s.classes).join(' '));
  }

  function finalize(fragment, context, node) {
    try {
      return compiler.finalize(fragment, context);
    } catch (error) {
      if (error instanceof ClassError) throw new TailwindError(error.message, node);
      throw error;
    }
  }

  /** Builds the style expression for segments; `context` is the enclosing static fragment. */
  function build(segments, element, imports, node, context = {}) {
    const {base, variants, imports: fonts} = parse(segments);
    fonts.forEach(path => imports.add(path));
    const scope = {...context, ...base, $transform: {...context.$transform, ...base.$transform}};
    const entries = [];
    const style = finalize(base, scope, node);
    if (Object.keys(style).length) entries.push(t.valueToNode(style));
    for (const segment of segments.filter(s => 'test' in s)) {
      const branch = s => s.length ? build(s, element, imports, node, scope) : t.nullLiteral();
      entries.push(t.conditionalExpression(segment.test,
        branch(segment.consequent), branch(segment.alternate)));
    }
    for (const {props, fragment} of variants) {
      if (!element) throw new TailwindError(`variants such as ${props[0]}: need a JSX element prop; ` +
        'tw templates take base classes only', node);
      const tests = props.map(prop => variantTest(element, prop, node));
      entries.push(t.conditionalExpression(tests.reduce((a, b) => t.logicalExpression('&&', a, b)),
        t.valueToNode(finalize(fragment, scope, node)), t.nullLiteral()));
    }
    if (!entries.length) return t.nullLiteral();
    return entries.length === 1 ? entries[0] : t.arrayExpression(entries);
  }

  function variantTest(element, prop, node) {
    const attribute = element.node.attributes.find(a => t.isJSXAttribute(a) && a.name.name === prop);
    if (!attribute) throw new TailwindError(`${prop}: variant needs a ${prop}={...} prop on this element`, node);
    if (attribute.value === null) return t.booleanLiteral(true);
    const value = t.isJSXExpressionContainer(attribute.value) ? attribute.value.expression : attribute.value;
    if (!isPure(value)) throw new TailwindError(`the ${prop} prop is read twice by ${prop}: classes; ` +
      'move the expression into a variable first', value);
    return t.cloneNode(value, true);
  }

  return {
    name: 'ps5-react-tailwind',
    visitor: {
      Program: {
        enter(_, state) { state.fontImports = new Set(); },
        exit(path, state) {
          for (const font of state.fontImports)
            path.unshiftContainer('body', t.importDeclaration([], t.stringLiteral(font)));
        },
      },
      JSXAttribute(path, state) {
        if (path.node.name.name !== 'className') return;
        const element = path.parentPath;
        const value = path.get('value');
        const expression = value.isJSXExpressionContainer() ? value.get('expression') : value;
        const style = build(collect(expression), element, state.fontImports, path.node);
        const existing = element.get('attributes').find(a => a.isJSXAttribute() && a.node.name.name === 'style');
        if (existing) {
          const current = existing.node.value.expression ?? existing.node.value;
          existing.node.value = t.jsxExpressionContainer(t.arrayExpression([style, current]));
          path.remove();
        } else {
          path.replaceWith(t.jsxAttribute(t.jsxIdentifier('style'), t.jsxExpressionContainer(style)));
        }
      },
      TaggedTemplateExpression(path, state) {
        const tag = path.get('tag');
        if (!tag.isIdentifier()) return;
        const binding = path.scope.getBinding(tag.node.name);
        if (binding?.kind !== 'module' || binding.path.parent.source.value !== '@ps5-react/core' ||
          binding.path.node.imported?.name !== 'tw') return;
        const style = build(collect(path.get('quasi')), null, state.fontImports, path.node);
        path.replaceWith(t.isNullLiteral(style) ? t.objectExpression([]) : style);
      },
    },
  };
}
