// Runtime smoke test for the web UI's render path.
//
// check_ui_syntax.mjs only PARSES the inline <script>; it never runs it. So a
// reference to a top-level constant that was accidentally deleted (e.g. a
// `const MODE_COLOR = {...}` lookup table) parses fine but throws
// `ReferenceError: MODE_COLOR is not defined` at RUNTIME the first time the
// render path executes.
//
// That is especially nasty here because refresh() wraps the whole render in a
// try/catch whose catch path flips the "WiFi" diagnostic badge red and sets the
// subtitle to "disconnected". A pure client-side JS error therefore masquerades
// as a connectivity problem — the device is perfectly reachable, /api/status
// returns 200, yet the UI screams "disconnected". (Shipped once as v0.2.96.)
//
// This test loads the ENTIRE inline script into a vm with a permissive fake DOM
// (so it defines every function + top-level const exactly as the browser would)
// and then drives the real render functions with representative state. Crucially
// it renders with climate.power == true for every mode, because renderClimate
// only dereferences MODE_COLOR when the unit is ON — the exact condition that
// made the bug invisible to a defaults-only smoke test.
//
// No external deps: a single hand-rolled Proxy stands in for every DOM node.

import fs from 'node:fs';
import vm from 'node:vm';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

process.on('unhandledRejection', () => {}); // startup IIFE awaits a stubbed fetch

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..');
const html = fs.readFileSync(path.join(root, 'main', 'web', 'index.html'), 'utf8');

const scriptM = /<script\b(?![^>]*\bsrc=)[^>]*>([\s\S]*?)<\/script>/i.exec(html);
if (!scriptM) { console.error('FAIL: no inline <script> found'); process.exit(1); }
const script = scriptM[1];

// ── A permissive universal DOM node ───────────────────────────────────────
// UNI is callable (so el.method(...) works), chainable (any property access
// returns UNI), spread-safe, and absorbs every assignment. This lets the whole
// inline script load without us having to model the real DOM — we only care
// that the render functions don't throw on undefined *script* globals.
const UNI = new Proxy(function () {}, {
  apply() { return UNI; },
  construct() { return UNI; },
  set() { return true; },
  has() { return true; },
  get(_t, prop) {
    switch (prop) {
      case 'classList': return { add() {}, remove() {}, toggle() {}, contains() { return false; } };
      case 'style': return UNI;
      case 'dataset': return UNI;
      case 'children':
      case 'options': return [];
      case 'length': return 0;
      case 'querySelectorAll': return () => [];
      case 'querySelector':
      case 'closest': return () => UNI;
      case 'getBoundingClientRect': return () => ({ top: 0, left: 0, width: 0, height: 0 });
      case 'getPropertyValue': return () => '';
      case 'then': return undefined; // must not look thenable
      case Symbol.iterator: return function* () {};
      case Symbol.toPrimitive:
      case 'toString':
      case 'valueOf': return () => '';
      default: return UNI;
    }
  },
});

const documentStub = new Proxy(UNI, {
  get(_t, prop) {
    if (prop === 'getElementById' || prop === 'createElement' ||
        prop === 'createElementNS' || prop === 'querySelector') return () => UNI;
    if (prop === 'querySelectorAll') return () => [];
    if (prop === 'addEventListener' || prop === 'removeEventListener') return () => {};
    if (prop === 'hidden') return false;
    if (prop === 'documentElement' || prop === 'body' || prop === 'head' ||
        prop === 'activeElement') return UNI;
    if (prop === 'cookie') return '';
    return UNI[prop];
  },
});

// Any fetch during load (the startup IIFE calls loadAuth) resolves to a benign,
// field-rich object so no loader trips over a missing property.
const fetchPayload = {
  enabled: false, required: false, authenticated: true, role: 'admin',
  temp_unit: 'C', power: 'ON', mode: 'HEAT', fan: 'AUTO', vane: 'AUTO',
  wideVane: '|', connected: true, caps: { wideVane: { detected: 'unsupported', override: 'auto', detecting: false, show: false } },
};
const fetchStub = async () => ({
  ok: true, status: 200,
  json: async () => fetchPayload,
  text: async () => JSON.stringify(fetchPayload),
});

const sandbox = {
  document: documentStub,
  window: UNI,
  navigator: { userAgent: 'node', onLine: true, serviceWorker: undefined },
  location: { href: 'http://device/', pathname: '/', reload() {}, replace() {} },
  history: { pushState() {}, replaceState() {} },
  localStorage: { getItem: () => null, setItem() {}, removeItem() {}, clear() {} },
  sessionStorage: { getItem: () => null, setItem() {}, removeItem() {}, clear() {} },
  getComputedStyle: () => ({ getPropertyValue: () => '' }),
  matchMedia: () => ({ matches: false, addEventListener() {}, removeEventListener() {}, addListener() {}, removeListener() {} }),
  fetch: fetchStub,
  AbortController,
  setTimeout: () => 0, clearTimeout: () => {},
  setInterval: () => 0, clearInterval: () => {},
  requestAnimationFrame: () => 0, cancelAnimationFrame: () => {},
  console: { log() {}, warn() {}, error() {}, info() {}, debug() {} },
  URL, URLSearchParams,
};
sandbox.globalThis = sandbox;
sandbox.self = sandbox;

// Expose the render entry points + a climate setter from inside the script's
// own lexical scope (climate/renderClimate are top-level const/function, not
// attached to globalThis, so we must reach them from within the script).
const harness = script + `
;globalThis.__probe = {
  setClimate: (o) => Object.assign(climate, o),
  renderClimate,
  renderCurrent: (typeof renderCurrent === 'function') ? renderCurrent : () => {},
  applyCaps: (typeof applyCaps === 'function') ? applyCaps : () => {},
};`;

const ctx = vm.createContext(sandbox);
try {
  vm.runInContext(harness, ctx, { filename: 'index.html#inline' });
} catch (e) {
  console.error('FAIL: inline script threw at load: ' + e.message);
  console.error((e.stack || '').split('\n').slice(0, 4).join('\n'));
  process.exit(1);
}

const probe = sandbox.__probe;
if (!probe || typeof probe.renderClimate !== 'function') {
  console.error('FAIL: could not reach render functions after load');
  process.exit(1);
}

let failures = 0;
function drive(label, fn) {
  try { fn(); }
  catch (e) {
    failures++;
    console.error(`  FAIL: ${label} -> ${e.name}: ${e.message}`);
    console.error('        ' + (e.stack || '').split('\n')[1]?.trim());
  }
}

// renderClimate only touches MODE_COLOR when power is ON — cover every mode,
// both operating states, and the power-off path.
const modes = ['HEAT', 'COOL', 'AUTO', 'DRY', 'FAN'];
for (const mode of modes) {
  for (const operating of [true, false]) {
    drive(`renderClimate(power=on mode=${mode} operating=${operating})`, () => {
      probe.setClimate({ power: true, mode, temp: 22, room: 20.5, operating });
      probe.renderClimate();
    });
  }
}
drive('renderClimate(power=off)', () => {
  probe.setClimate({ power: false, mode: 'HEAT', temp: 21, room: 20, operating: false });
  probe.renderClimate();
});

drive('renderCurrent()', () => probe.renderCurrent());

for (const caps of [
  { wideVane: { detected: 'unsupported', override: 'auto', detecting: false, show: false } },
  { wideVane: { detected: 'supported', override: 'auto', detecting: false, show: true } },
  { wideVane: { detected: 'unknown', override: 'show', detecting: true, show: true } },
  null, undefined, {},
]) {
  drive(`applyCaps(${JSON.stringify(caps)})`, () => probe.applyCaps(caps));
}

if (failures > 0) {
  console.error(`render-runtime check FAILED: ${failures} problem(s)`);
  process.exit(1);
}
console.log('render-runtime check passed: render path runs clean for all modes + capability states');
