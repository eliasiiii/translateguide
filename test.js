const fs = require('fs'), vm = require('vm'), assert = require('assert');
const ctx = {}; vm.createContext(ctx);
vm.runInContext(fs.readFileSync(__dirname + '/universal_translate.js', 'utf8'), ctx);
const rt = ctx.__ptRuntime();

// 1. segment split/join round trip, codes never touched
{
  const src = "\\N[1] said: \\C[2]Hello\\C[0] world!\\!";
  const segs = rt.splitSegments(src);
  assert.strictEqual(rt.joinSegments(segs), src, 'joining untouched segments must reproduce the original exactly');
  const codeSegs = segs.filter(s => s.code).map(s => s.value);
  assert.deepStrictEqual(JSON.parse(JSON.stringify(codeSegs)), ['\\N[1]', '\\C[2]', '\\C[0]', '\\!']);
  const textSegs = segs.filter(s => !s.code).map(s => s.value);
  assert.deepStrictEqual(JSON.parse(JSON.stringify(textSegs)), [' said: ', 'Hello', ' world!']);
  console.log('segment split/join OK:', JSON.stringify(segs));
}

// 2. isTranslatable
assert.strictEqual(rt.isTranslatable('   '), false);
assert.strictEqual(rt.isTranslatable('123'), false);
assert.strictEqual(rt.isTranslatable('Hi'), true);
console.log('isTranslatable checks OK');

// 3. scan() on a fake Map*.json (standard RPG Maker MV/MZ shape)
const fakeMap = {
  events: [
    null,
    { id: 1, pages: [{ list: [
      { code: 401, parameters: ["Hello traveler."] },
      { code: 401, parameters: ["\\N[1] welcomes you."] },
      { code: 102, parameters: [["Yes", "No"], 0, 0, 2, 0] },
      { code: 108, parameters: ["this is a comment, not shown"] }, // must be ignored
      { code: 122, parameters: [1, 1, 0, 0, 5] } // control variables, must be ignored
    ] }] }
  ]
};
const slots = rt.scan(fakeMap);
assert.strictEqual(slots.length, 4, 'expected 4 translatable slots, got ' + slots.length);
console.log('scan() found', slots.length, 'slots (expected 4)');

// 4. process() end-to-end: the native bridge must receive ONLY clean prose
//    segments, never a code or a placeholder marker mixed into them.
let capturedTexts = null, capturedLang = null;
ctx.__ptNative = {
  translateBatch: function (textsJson, lang, cb) {
    capturedTexts = JSON.parse(textsJson); capturedLang = lang;
    cb(JSON.stringify(capturedTexts.map(t => '[AR]' + t)));
  }
};
const fakeMap2 = JSON.parse(JSON.stringify(fakeMap));
const result = rt.process(fakeMap2, 'ar');
assert.strictEqual(result.scanned, 4);
assert.strictEqual(result.pending, 4);
assert.strictEqual(capturedLang, 'ar');
for (const t of capturedTexts) assert(!/[\x01\x02\\]/.test(t), 'native bridge must never see raw control chars or codes: ' + JSON.stringify(t));
assert.deepStrictEqual(JSON.parse(JSON.stringify(capturedTexts)), ["Hello traveler.", " welcomes you.", "Yes", "No"]);
const evList = fakeMap2.events[1].pages[0].list;
assert.strictEqual(evList[0].parameters[0], '[AR]Hello traveler.');
assert.strictEqual(evList[1].parameters[0], '\\N[1][AR] welcomes you.', 'the \\N[1] code must survive in its original position');
assert.strictEqual(evList[2].parameters[0][0], '[AR]Yes');
assert.strictEqual(evList[3].parameters[0], 'this is a comment, not shown', 'non-text command codes must be left untouched');
console.log('process() end-to-end OK, codes preserved in place:', JSON.stringify(evList[1].parameters[0]));

// 5. cache hit on second pass: native bridge must NOT be called again
let secondCallHappened = false;
ctx.__ptNative.translateBatch = function () { secondCallHappened = true; };
const fakeMap3 = JSON.parse(JSON.stringify(fakeMap));
const result2 = rt.process(fakeMap3, 'ar');
assert.strictEqual(result2.pending, 0, 'everything should be served from cache');
assert.strictEqual(secondCallHappened, false);
assert.strictEqual(fakeMap3.events[1].pages[0].list[0].parameters[0], '[AR]Hello traveler.');
console.log('cache hit on repeat scan OK, no native call made');

// 6. JSON.parse hook installs transparently, translates through its own
//    (independent) runtime, and refuses to install a second time.
ctx.__ptNative.translateBatch = function (textsJson, lang, cb) {
  cb(JSON.stringify(JSON.parse(textsJson).map(t => '[AR]' + t)));
};
const install = ctx.pgInstallTranslateHook('ar');
assert.strictEqual(install.ok, true);
ctx.__fakeMapJsonText = JSON.stringify(fakeMap);
const parsed = vm.runInContext('JSON.parse(__fakeMapJsonText)', ctx);
assert.strictEqual(parsed.events[1].pages[0].list[0].parameters[0], '[AR]Hello traveler.', 'translation must happen transparently through the hooked JSON.parse');
const reinstall = ctx.pgInstallTranslateHook('ar');
assert.strictEqual(reinstall.ok, false, 'installing twice must be refused');
console.log('JSON.parse hook install + idempotency OK');

console.log('ALL TESTS PASSED');
