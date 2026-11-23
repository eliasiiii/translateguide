static const char* kRuntimeSrc = R"PTJS(
// universal_translate.js
// Engine-generic text extraction/reinsertion for RPG Maker MV/MZ games.
// Runs INSIDE the game's JSContext. Depends only on documented, version-agnostic
// RPG Maker data shapes (event command {code, parameters} lists), never on any
// single game's custom code. Talks to native translation via __ptNative (injected
// by the host app) and caches results so repeated lines translate only once.

function __ptRuntime() {
  // Event command codes whose parameters are player-facing prose, per the public
  // RPG Maker MV/MZ event-command reference. Anything else is left untouched,
  // because treating a non-text parameter as text could corrupt game logic
  // (variable ids, switch ids, numeric amounts, etc.).
  var TEXT_CODES = {
    401: [0],       // "Show Text" line
    405: [0],       // "Show Scrolling Text" line (MV/MZ)
    102: ['choices'] // "Show Choices": parameters[0] is an array of option strings
  };

  // RPG Maker escape codes. Captured and replaced with placeholders before
  // translation, then restored by position afterward, so a translator never
  // sees or reorders them in a way that corrupts them.
  var ESCAPE_RE = /\\[A-Za-z][\[\{<>\]]?\d*\]?|\\[.\\|!><^{}$]/g;

  // Splits a line into alternating text/code segments instead of inlining
  // placeholder markers into the string that gets sent to translation: a
  // translator is never asked to pass an unfamiliar control character
  // through unchanged, it only ever sees plain prose. Codes keep their
  // exact position because every segment, text or code, is kept in order.
  function splitSegments(text) {
    var segs = [], last = 0, m;
    ESCAPE_RE.lastIndex = 0;
    while ((m = ESCAPE_RE.exec(text)) !== null) {
      if (m.index > last) segs.push({ code: false, value: text.slice(last, m.index) });
      segs.push({ code: true, value: m[0] });
      last = m.index + m[0].length;
    }
    if (last < text.length) segs.push({ code: false, value: text.slice(last) });
    return segs;
  }

  function joinSegments(segs) {
    return segs.map(function (s) { return s.value; }).join('');
  }

  // A line is worth sending to the translator only if it has real letters
  // left after codes are stripped (skips blank lines, pure-code lines, numbers).
  function isTranslatable(stripped) {
    return /\p{L}/u.test(stripped);
  }

  // Walk one event command list (Map*.json / CommonEvents.json / Troops.json
  // all share this exact {code, parameters} shape), collecting translatable
  // strings and a setter for each.
  function collectFromList(list, out) {
    if (!Array.isArray(list)) return;
    for (var i = 0; i < list.length; i++) {
      var cmd = list[i];
      if (!cmd || typeof cmd.code !== 'number') continue;
      var idxs = TEXT_CODES[cmd.code];
      if (!idxs || !Array.isArray(cmd.parameters)) continue;
      for (var j = 0; j < idxs.length; j++) {
        var key = idxs[j];
        if (key === 'choices') {
          var arr = cmd.parameters[0];
          if (!Array.isArray(arr)) continue;
          (function (arr) {
            for (var k = 0; k < arr.length; k++) {
              (function (k) {
                if (typeof arr[k] !== 'string') return;
                out.push({ get: function () { return arr[k]; }, set: function (v) { arr[k] = v; } });
              })(k);
            }
          })(arr);
        } else {
          (function (cmd, key) {
            if (typeof cmd.parameters[key] !== 'string') return;
            out.push({ get: function () { return cmd.parameters[key]; }, set: function (v) { cmd.parameters[key] = v; } });
          })(cmd, key);
        }
      }
    }
  }

  // Scan one already-parsed data object (a Map*.json root, or CommonEvents.json /
  // Troops.json array) for every translatable slot. Works the same for any
  // RPG Maker MV/MZ game because it only relies on the public data shape.
  function scan(data) {
    var out = [];
    if (Array.isArray(data)) {
      for (var i = 0; i < data.length; i++) {
        var entry = data[i];
        if (!entry) continue;
        if (Array.isArray(entry.list)) collectFromList(entry.list, out);          // CommonEvents.json
        if (Array.isArray(entry.pages)) for (var p = 0; p < entry.pages.length; p++) collectFromList(entry.pages[p].list, out); // Troops.json
      }
    } else if (data && Array.isArray(data.events)) {
      for (var e = 0; e < data.events.length; e++) {
        var ev = data.events[e];
        if (!ev || !Array.isArray(ev.pages)) continue;
        for (var p2 = 0; p2 < ev.pages.length; p2++) collectFromList(ev.pages[p2].list, out); // Map*.json
      }
    }
    return out;
  }

  // Cache keyed by exact source line, persisted by the native side so a line
  // translated once is never re-sent, even across app launches.
  var cache = {};
  function loadCache(json) { try { cache = JSON.parse(json) || {}; } catch (e) { cache = {}; } }
  function dumpCache() { return JSON.stringify(cache); }

  // Entry point called after a data file is parsed. Finds translatable slots,
  // fills in anything already cached immediately, and asynchronously requests
  // the rest from the native bridge. Safe to call on any parsed JSON value:
  // non-RPG-Maker-shaped data (anything without .events/.list/.pages) scans to
  // zero slots and is a no-op.
  function process(data, targetLang) {
    var slots = scan(data);
    var pending = [];      // one entry per whole line still needing translation
    var queue = [];        // flat list of {pendingIndex, segIndex} for every text segment to send
    for (var i = 0; i < slots.length; i++) {
      var src = slots[i].get();
      if (typeof src !== 'string' || !src) continue;
      var cacheKey = targetLang + '\u0003' + src;
      if (cache[cacheKey] !== undefined) {
        slots[i].set(cache[cacheKey]);
        continue;
      }
      var segs = splitSegments(src);
      var anyText = segs.some(function (s) { return !s.code && isTranslatable(s.value); });
      if (!anyText) continue; // code-only or non-letter line: nothing to translate, leave as-is
      var pIdx = pending.length;
      pending.push({ slot: slots[i], src: src, segs: segs, cacheKey: cacheKey });
      for (var s = 0; s < segs.length; s++) {
        if (!segs[s].code && isTranslatable(segs[s].value)) queue.push({ p: pIdx, s: s });
      }
    }
    if (pending.length && typeof __ptNative !== 'undefined' && __ptNative.translateBatch) {
      var texts = queue.map(function (q) { return pending[q.p].segs[q.s].value; });
      __ptNative.translateBatch(JSON.stringify(texts), targetLang, function (resultJson) {
        var results;
        try { results = JSON.parse(resultJson); } catch (e) { return; }
        for (var k = 0; k < queue.length && k < results.length; k++) {
          var t = results[k];
          if (typeof t === 'string' && t) pending[queue[k].p].segs[queue[k].s].value = t;
        }
        for (var pi = 0; pi < pending.length; pi++) {
          var joined = joinSegments(pending[pi].segs);
          cache[pending[pi].cacheKey] = joined;
          pending[pi].slot.set(joined);
        }
      });
    }
    return { scanned: slots.length, cached: slots.length - pending.length, pending: pending.length };
  }

  return { scan: scan, process: process, splitSegments: splitSegments, joinSegments: joinSegments,
           isTranslatable: isTranslatable, loadCache: loadCache, dumpCache: dumpCache, _cache: function () { return cache; } };
}

// Hook JSON.parse once, globally, so every RPG Maker data file that passes
// through it (which is all of them, in both MV's XHR loader and MZ's fetch
// loader) gets scanned automatically, with zero per-game, per-version code.
function pgInstallTranslateHook(targetLang) {
  if (typeof JSON.__ptOriginalParse === 'function') return { ok: false, reason: 'already installed' };
  var rt = __ptRuntime();
  JSON.__ptOriginalParse = JSON.parse;
  JSON.__ptRuntime = rt;
  JSON.parse = function (text, reviver) {
    var data = JSON.__ptOriginalParse(text, reviver);
    try { rt.process(data, targetLang); } catch (e) { /* never break data loading */ }
    return data;
  };
  return { ok: true, reason: 'JSON.parse hooked' };
}
)PTJS";
