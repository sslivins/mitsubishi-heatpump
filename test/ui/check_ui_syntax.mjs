// Basic web-UI smoke test.
//
// The device serves main/web/index.html as a single embedded blob. A bad edit
// that drops a function header or an unbalanced brace still lets the server
// return HTTP 200 with the full page, but the browser aborts on the first
// SyntaxError and renders NOTHING - exactly the failure that shipped in the
// first cut of the group CRDT UI (a lost `async function pollOnce(){` header).
//
// This test compiles (does not run) every inline <script> block so that class
// of breakage fails CI instead of the dashboard. No external deps: it uses the
// built-in `vm` module, which parses with the same engine the browser uses.

import fs from 'node:fs';
import vm from 'node:vm';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..');
const htmlPath = path.join(root, 'main', 'web', 'index.html');

const html = fs.readFileSync(htmlPath, 'utf8');

const re = /<script\b([^>]*)>([\s\S]*?)<\/script>/gi;
let m;
let checked = 0;
let failed = 0;
while ((m = re.exec(html)) !== null) {
  const attrs = m[1] || '';
  if (/\bsrc\s*=/i.test(attrs)) continue; // external script: nothing inline to parse
  const code = m[2];
  if (code.trim() === '') continue;
  checked++;
  try {
    // Compile only. Undefined browser globals (document, window, fetch) are
    // irrelevant because we never execute the code.
    new vm.Script(code, { filename: `index.html#script${checked}` });
  } catch (e) {
    failed++;
    console.error(`SYNTAX ERROR in inline <script> #${checked}: ${e.message}`);
  }
}

if (checked === 0) {
  console.error('FAIL: no inline <script> block found in main/web/index.html');
  process.exit(1);
}
if (failed > 0) {
  console.error(`UI syntax check FAILED: ${failed} of ${checked} inline script block(s) did not parse`);
  process.exit(1);
}
console.log(`UI syntax check passed: ${checked} inline script block(s) parse cleanly`);
