// Regression test for member-row rendering with hostile display names.
//
// The Zones panel builds each member row's markup as an HTML string in
// memberRow(). A head's display name is user-controlled ("Eric's Room",
// "Kids' Room", …) and can contain a single quote, double quote, backslash or
// angle brackets. An earlier cut built the remove/leave buttons as an inline
//     onclick="groupRemoveMember('<uid>','<name>')"
// with a name-escaper that did NOT escape the single quote. For a name like
// "Eric's Room" the apostrophe closed the JS string early, so the button's
// handler was a SyntaxError and clicking it did NOTHING (no confirm, no remove)
// — with zero console noise the user could see.
//
// check_ui_syntax.mjs cannot catch this: the broken JS only exists inside a
// template string that becomes an attribute at runtime, not in the inline
// <script> it parses. This test renders memberRow() with adversarial names and
// asserts the result is safe: (1) any inline on*= handler value still parses as
// JS, and (2) the buttons carry the delegated data-* hooks (not interpolated
// inline handlers), so a quote in a name can never break the click path again.
//
// No external deps: memberRow depends only on gEsc(), so both functions are
// sliced out of the inline <script> by brace-matching and evaluated in a vm.

import fs from 'node:fs';
import vm from 'node:vm';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..');
const html = fs.readFileSync(path.join(root, 'main', 'web', 'index.html'), 'utf8');

// Grab the (single) inline script body.
const scriptM = /<script\b(?![^>]*\bsrc=)[^>]*>([\s\S]*?)<\/script>/i.exec(html);
if (!scriptM) { console.error('FAIL: no inline <script> found'); process.exit(1); }
const script = scriptM[1];

// Extract a top-level `function NAME(...) { ... }` by brace matching.
function extractFn(src, name) {
  const start = src.indexOf('function ' + name);
  if (start < 0) throw new Error(`function ${name} not found`);
  const braceOpen = src.indexOf('{', start);
  if (braceOpen < 0) throw new Error(`no body for ${name}`);
  let depth = 0;
  for (let i = braceOpen; i < src.length; i++) {
    const c = src[i];
    if (c === '{') depth++;
    else if (c === '}') { depth--; if (depth === 0) return src.slice(start, i + 1); }
  }
  throw new Error(`unbalanced braces in ${name}`);
}

let memberRow;
try {
  const ctx = vm.createContext({});
  vm.runInContext(extractFn(script, 'gEsc') + '\n' + extractFn(script, 'memberRow'), ctx);
  // Expose for calling.
  memberRow = vm.runInContext('memberRow', ctx);
} catch (e) {
  console.error('FAIL: could not load render helpers: ' + e.message);
  process.exit(1);
}

// Names crafted to break naive string interpolation.
const HOSTILE = [
  "Eric's Room",
  "Kids' Room",
  'A"B quote',
  "'); alert(1);//",
  'back\\slash',
  '<script>x</script>',
  'both\'"kinds',
];

let failures = 0;
function fail(msg) { console.error('  FAIL: ' + msg); failures++; }

for (const name of HOSTILE) {
  // isPeer=true renders the ✕ remove button; isPeer=false renders Leave.
  for (const isPeer of [true, false]) {
    let out;
    try {
      out = memberRow(name, 'HEAT', isPeer ? 'known' : 'selfdot',
                      isPeer ? '' : 'you', false, 'abcd', isPeer);
    } catch (e) {
      fail(`memberRow threw for name=${JSON.stringify(name)} isPeer=${isPeer}: ${e.message}`);
      continue;
    }

    // (1) Every inline event-handler attribute value must be valid JS. This is
    //     the exact property the apostrophe bug violated.
    const handlerRe = /\son[a-z]+\s*=\s*("([^"]*)"|'([^']*)')/gi;
    let h;
    while ((h = handlerRe.exec(out)) !== null) {
      const code = h[2] !== undefined ? h[2] : h[3];
      try { new vm.Script(code); }
      catch (e) {
        fail(`inline handler is broken JS for name=${JSON.stringify(name)}: <${code}> (${e.message})`);
      }
    }

    // (2) The buttons must use the delegated data-* hooks, not interpolated
    //     inline handlers — so a quote in a name can never reach a JS context.
    if (/\sonclick\s*=/i.test(out)) {
      fail(`member button uses inline onclick (data can break it) for name=${JSON.stringify(name)}`);
    }
    const wantAct = isPeer ? 'data-gact="remove"' : 'data-gact="leave"';
    if (!out.includes(wantAct)) {
      fail(`missing ${wantAct} for name=${JSON.stringify(name)} isPeer=${isPeer}`);
    }
  }
}

if (failures > 0) {
  console.error(`member-row render check FAILED: ${failures} problem(s)`);
  process.exit(1);
}
console.log(`member-row render check passed: ${HOSTILE.length} hostile name(s) render safely`);
