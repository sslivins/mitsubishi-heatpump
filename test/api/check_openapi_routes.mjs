// Zero-dependency drift check: every HTTP route registered in the firmware's
// web server must be documented in openapi.yaml (and vice-versa). Runs under
// bare `node` in CI — no npm install, no YAML library.
//
// It parses two sources with narrow regexes:
//   1. The httpd route table in main/web_ui.cpp:
//        {"/api/status", HTTP_GET, handle_status, nullptr},
//   2. The `paths:` section of openapi.yaml (path keys + their HTTP methods).
//
// The CORS preflight wildcard {"/api/*", HTTP_OPTIONS, ...} is intentionally
// excluded — it is a catch-all, not a documentable endpoint.

import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const root = join(dirname(fileURLToPath(import.meta.url)), '..', '..');
const cpp = readFileSync(join(root, 'main', 'web_ui.cpp'), 'utf8');
const spec = readFileSync(join(root, 'openapi.yaml'), 'utf8');

// ── 1. Routes from the C++ route table ────────────────────────────────────
// Match:  {"/api/foo", HTTP_GET, handle_foo, ...},
const routeRe = /\{\s*"(\/[^"]+)"\s*,\s*HTTP_(GET|POST|PUT|DELETE|PATCH|OPTIONS)\b/g;
const routes = new Set();
for (const m of cpp.matchAll(routeRe)) {
  const path = m[1];
  const method = m[2].toLowerCase();
  if (!path.startsWith('/api/')) continue; // static assets (icons, manifest) aren't API
  if (method === 'options' && path.endsWith('*')) continue; // CORS wildcard
  routes.add(`${method} ${path}`);
}

// ── 2. Documented path+method from openapi.yaml `paths:` section ───────────
// Slice out just the paths block so schema/component keys can't leak in.
const lines = spec.split(/\r?\n/);
let inPaths = false;
let curPath = null;
const documented = new Set();
const methodRe = /^(get|post|put|delete|patch|options):$/i;
for (const raw of lines) {
  if (/^paths:\s*$/.test(raw)) { inPaths = true; continue; }
  if (inPaths && /^\S/.test(raw)) break; // next top-level key (components:) ends paths
  if (!inPaths) continue;
  // Path key: two-space indent, starts with a slash, ends with a colon.
  const pm = raw.match(/^ {2}(\/\S+):\s*$/);
  if (pm) { curPath = pm[1]; continue; }
  // Method key: four-space indent under a path.
  const mm = raw.match(/^ {4}([a-z]+):\s*$/);
  if (mm && curPath && methodRe.test(mm[1] + ':')) {
    documented.add(`${mm[1].toLowerCase()} ${curPath}`);
  }
}

// ── 3. Compare ────────────────────────────────────────────────────────────
const missing = [...routes].filter((r) => !documented.has(r)).sort();
const extra = [...documented].filter((d) => !routes.has(d)).sort();

let failed = false;
if (missing.length) {
  failed = true;
  console.error('\u2717 Routes in web_ui.cpp NOT documented in openapi.yaml:');
  for (const r of missing) console.error(`    ${r}`);
}
if (extra.length) {
  failed = true;
  console.error('\u2717 Paths in openapi.yaml with no matching route in web_ui.cpp:');
  for (const e of extra) console.error(`    ${e}`);
}

if (failed) {
  console.error(`\nRoutes: ${routes.size}, documented: ${documented.size}. Update openapi.yaml to match the API.`);
  process.exit(1);
}

console.log(`\u2713 openapi.yaml is in sync with web_ui.cpp (${routes.size} routes documented).`);
