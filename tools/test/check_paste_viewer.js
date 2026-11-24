#!/usr/bin/env node
'use strict';

// Offline host tests for PasteViewer (Host -> Target text paste over USB HID).
// Verifies pure keymap planning (US & JIS 95 printable ASCII + \n + \t), DOM / form /
// clipboard / IME-safe submit / pacing / cancel / disconnect / logout isolation, static
// HTML/CSS/CSP checks, and mutation killing (--mutations).

const fs = require('fs');
const path = require('path');
const vm = require('vm');

const ROOT = path.resolve(__dirname, '../..');
const WEB_DIR = path.join(ROOT, 'components/web_server/web');
const INDEX_HTML = path.join(WEB_DIR, 'index.html');
const APP_CSS = path.join(WEB_DIR, 'app.css');
const APP_JS = path.join(WEB_DIR, 'app.js');

const PRINTABLE_ASCII = [];
for (let c = 0x20; c <= 0x7E; c++) PRINTABLE_ASCII.push(String.fromCharCode(c));

function extractPasteViewer(source) {
    const start = source.indexOf('// ==== Paste viewer (begin) ====');
    const end = source.indexOf('// ==== Paste viewer (end) ====');
    if (start < 0 || end <= start) throw new Error('Paste viewer block not found in app.js');
    return new Function(source.slice(start, end) + '\nreturn PasteViewer;')();
}

// Independent expected HID tables (Keyboard/Keypad Page 0x07, Shift = 0xE1).
const EXPECTED_COMMON = {
    ' ': [0x2C, false], '\n': [0x28, false], '\t': [0x2B, false],
    '!': [0x1E, true], '#': [0x20, true], '$': [0x21, true], '%': [0x22, true],
    ',': [0x36, false], '<': [0x36, true], '-': [0x2D, false],
    '.': [0x37, false], '>': [0x37, true], '/': [0x38, false], '?': [0x38, true],
    '0': [0x27, false], ';': [0x33, false],
};
for (let i = 0; i < 26; i++) {
    EXPECTED_COMMON[String.fromCharCode(97 + i)] = [0x04 + i, false];
    EXPECTED_COMMON[String.fromCharCode(65 + i)] = [0x04 + i, true];
}
for (let i = 0; i < 9; i++) EXPECTED_COMMON[String.fromCharCode(49 + i)] = [0x1E + i, false];

const EXPECTED_US = Object.assign({}, EXPECTED_COMMON, {
    '"': [0x34, true], '&': [0x24, true], "'": [0x34, false], '(': [0x26, true], ')': [0x27, true],
    '*': [0x25, true], '+': [0x2E, true], ':': [0x33, true], '=': [0x2E, false], '@': [0x1F, true],
    '[': [0x2F, false], '\\': [0x31, false], ']': [0x30, false], '^': [0x23, true], '_': [0x2D, true],
    '`': [0x35, false], '{': [0x2F, true], '|': [0x31, true], '}': [0x30, true], '~': [0x35, true],
});

const EXPECTED_JIS = Object.assign({}, EXPECTED_COMMON, {
    '"': [0x1F, true], '&': [0x23, true], "'": [0x24, true], '(': [0x25, true], ')': [0x26, true],
    '*': [0x34, true], '+': [0x33, true], ':': [0x34, false], '=': [0x2D, true], '@': [0x2F, false],
    '[': [0x30, false], '\\': [0x87, false], ']': [0x32, false], '^': [0x2E, false], '_': [0x87, true],
    '`': [0x2F, true], '{': [0x30, true], '|': [0x89, true], '}': [0x32, true], '~': [0x2E, true],
});

function checkPure(source) {
    const problems = [];
    let pv;
    try { pv = extractPasteViewer(source); } catch (e) { return [String(e.message)]; }

    const eq = (name, got, want) => {
        if (JSON.stringify(got) !== JSON.stringify(want)) problems.push(name + ': expected ' + JSON.stringify(want) + ', got ' + JSON.stringify(got));
    };

    eq('constants', [pv.MAX_CHARS, pv.STEP_MS, pv.SHIFT_USAGE, pv.LAYOUT_KEY], [4096, 12, 0xE1, 'quadkvm.pasteLayout']);

    for (const [inp, want] of [['jis', 'jis'], ['us', 'us'], ['', 'jis'], ['jp', 'jis'], ['US', 'jis'], [null, 'jis'], [undefined, 'jis'], [{}, 'jis']]) {
        eq('normalizeLayout(' + JSON.stringify(inp) + ')', pv.normalizeLayout(inp), want);
    }

    eq('normalizeNewlines', pv.normalizeNewlines('a\r\nb\rc\n\r\n'), 'a\nb\nc\n\n');
    eq('normalizeNewlines non-string', pv.normalizeNewlines(null), '');

    const allChars = PRINTABLE_ASCII.concat(['\n', '\t']);
    for (const [layout, table] of [['us', EXPECTED_US], ['jis', EXPECTED_JIS]]) {
        if (Object.keys(table).length !== 97) problems.push('oracle table size for ' + layout);
        for (const ch of allChars) {
            const p = pv.planText(ch, layout);
            if (!p.ok || p.steps.length !== 1) {
                problems.push('planText ' + layout + ' failed for code ' + ch.charCodeAt(0));
                continue;
            }
            const want = table[ch];
            eq('map ' + layout + ' 0x' + ch.charCodeAt(0).toString(16), [p.steps[0].usage, p.steps[0].shift], want);
            const evs = pv.eventsForStep(p.steps[0]);
            const wantEvs = want[1]
                ? [[1, 0xE1], [1, want[0]], [0, want[0]], [0, 0xE1]]
                : [[1, want[0]], [0, want[0]]];
            eq('events ' + layout + ' 0x' + ch.charCodeAt(0).toString(16), evs, wantEvs);
        }
    }

    // \r\n counts as 1 char after normalization; 4096 passes, 4097 fails.
    eq('empty rejected', pv.planText('', 'jis'), { ok: false, error: 'empty' });
    eq('4096 CRLF normalized ok', pv.planText('\r\n'.repeat(4096), 'us').ok, true);
    eq('4097 rejected', pv.planText('a'.repeat(4097), 'us'), { ok: false, error: 'too_long', length: 4097 });

    // Non-ASCII / control rejections (excluding \n and \t).
    for (const [label, txt, cnt, idx] of [
        ['Japanese', 'abcあdeい', 2, 3],
        ['Emoji surrogate pair', 'x😀y', 1, 1],
        ['NUL', '\x00', 1, 0],
        ['BEL', 'ok\x07', 1, 2],
        ['DEL', 'a\x7f', 1, 1],
        ['Non-breaking space', '\u00a0', 1, 0],
    ]) {
        eq('unsupported ' + label, pv.planText(txt, 'jis'), { ok: false, error: 'unsupported', unsupportedCount: cnt, firstIndex: idx });
    }

    return problems;
}

function parseIndexHtml(html) {
    const out = {};
    const re = /<(\w+)((?:\s+[\w-]+(?:="[^"]*")?)*)\s*>/g;
    let m;
    while ((m = re.exec(html)) !== null) {
        const attrs = m[2];
        const id = /\sid="([^"]*)"/.exec(attrs);
        if (!id) continue;
        const cls = /\sclass="([^"]*)"/.exec(attrs);
        out[id[1]] = { tag: m[1], attrs, classes: cls ? cls[1].split(/\s+/).filter(Boolean) : [], checked: /\schecked(\s|$)/.test(attrs) };
    }
    return out;
}

function checkFiles(source) {
    const problems = [];
    const html = fs.readFileSync(INDEX_HTML, 'utf8');
    const css = fs.readFileSync(APP_CSS, 'utf8');
    const infos = parseIndexHtml(html);

    for (const id of ['btn-paste', 'paste-panel', 'btn-paste-close', 'paste-form', 'paste-fields', 'paste-layout-jis', 'paste-layout-us', 'paste-hint', 'paste-text', 'paste-clear-after', 'btn-paste-clipboard', 'btn-paste-clear', 'paste-progress-wrap', 'paste-progress', 'btn-paste-cancel', 'paste-status', 'paste-error']) {
        if (!infos[id]) problems.push('index.html lacks #' + id);
    }
    if (problems.length) return problems;

    for (const id of ['btn-paste', 'btn-paste-close', 'btn-paste-clipboard', 'btn-paste-clear', 'btn-paste-cancel']) {
        if (infos[id].tag !== 'button') problems.push('#' + id + ' must be a button');
        if (!/\btype="button"/.test(infos[id].attrs)) problems.push('#' + id + ' must have type="button"');
    }
    if (!/<button type="submit" class="paste-submit">/.test(html)) problems.push('paste form needs explicit <button type="submit" class="paste-submit">');
    if (infos['paste-layout-jis'].checked !== true || infos['paste-layout-us'].checked !== false) problems.push('default radio must be JIS');
    if (infos['paste-clear-after'].checked !== true) problems.push('#paste-clear-after must start checked');
    if (!/for="paste-clear-after"/.test(html)) problems.push('#paste-clear-after needs explicit label');

    // Ordering & accessibility checks.
    if (html.indexOf('id="paste-hint"') > html.indexOf('id="paste-text"')) problems.push('#paste-hint must be placed above #paste-text');
    if (html.indexOf('id="btn-paste"') > html.indexOf('id="btn-iso"')) problems.push('#btn-paste belongs before #btn-iso');
    if (!/for="paste-text"/.test(html)) problems.push('#paste-text needs explicit <label for="paste-text">');
    if (!/aria-describedby="paste-hint"/.test(infos['paste-text'].attrs)) problems.push('#paste-text needs aria-describedby="paste-hint"');
    for (const attr of ['autocomplete="off"', 'autocorrect="off"', 'autocapitalize="none"', 'spellcheck="false"']) {
        if (!infos['paste-text'].attrs.includes(attr)) problems.push('#paste-text missing ' + attr);
    }
    if (html.indexOf('id="btn-paste-cancel"') < html.indexOf('</fieldset>', html.indexOf('id="paste-fields"'))) problems.push('#btn-paste-cancel must sit outside #paste-fields');
    if (!/id="paste-panel"[^>]*aria-hidden="true"/.test(html)) problems.push('#paste-panel must start aria-hidden="true"');
    if (!infos['paste-panel'].classes.includes('paste-hidden')) problems.push('#paste-panel must start with .paste-hidden');
    if (!infos['paste-error'].classes.includes('paste-hidden')) problems.push('#paste-error must start with .paste-hidden');
    if (!/role="alert"/.test(infos['paste-error'].attrs)) problems.push('#paste-error must have role="alert"');
    if (!/role="status"[^>]*aria-live="polite"/.test(infos['paste-status'].attrs)) problems.push('#paste-status must have role="status" aria-live="polite"');

    // CSS & CSP invariants.
    if (/console\.(log|info|debug|warn|error)/.test(source)) problems.push('app.js must not call console.*');
    if (/innerHTML|outerHTML|insertAdjacentHTML|eval\(|Function\(/.test(source)) problems.push('app.js contains forbidden dynamic HTML/eval API');
    if (/https?:\/\/|ws:\/\//.test(html + '\n' + css + '\n' + source)) problems.push('static web files must not hardcode http(s)/ws URLs');
    const pasteHiddenIdx = css.indexOf('.paste-hidden');
    const isoHiddenIdx = css.indexOf('.iso-hidden');
    if (pasteHiddenIdx < 0 || isoHiddenIdx < 0 || pasteHiddenIdx > isoHiddenIdx) problems.push('.paste-hidden must appear before .iso-hidden in app.css');
    if (!/\.paste-panel button \{[^}]*min-inline-size:\s*24px[^}]*min-block-size:\s*24px/.test(css)) problems.push('.paste-panel buttons need >=24x24px minimum size');
    if (!/#view-dashboard\.is-fullscreen \.paste-panel \{\s*display:\s*none;\s*\}/.test(css)) problems.push('.paste-panel must be hidden in fullscreen');

    return problems;
}

async function flush() {
    for (let i = 0; i < 6; i++) await Promise.resolve();
}

async function bootApp(source, opts) {
    opts = opts || {};
    const html = fs.readFileSync(INDEX_HTML, 'utf8');
    const infos = parseIndexHtml(html);
    const elements = {};
    const violations = [];
    const docListeners = {};
    const winListeners = {};
    const timers = [];
    const sockets = [];
    let seq = 0;

    function mkEl(id, info) {
        const classes = new Set(info ? info.classes : []);
        const attrs = {};
        const listeners = {};
        const el = {
            id,
            tag: info ? info.tag : 'div',
            disabled: false,
            checked: info ? info.checked : false,
            _value: '',
            _text: '',
            _classes: classes,
            _attrs: attrs,
            _listeners: listeners,
            width: 1280, height: 720,
            clientWidth: 1000, clientHeight: 400, offsetWidth: 1000, offsetHeight: 400, clientLeft: 0, clientTop: 0,
            classList: {
                add(...ns) { ns.forEach((n) => classes.add(n)); },
                remove(...ns) { ns.forEach((n) => classes.delete(n)); },
                contains(n) { return classes.has(n); },
            },
            setAttribute(k, v) { if (k === 'style') violations.push('setAttribute(style)'); attrs[k] = String(v); },
            getAttribute(k) { return Object.prototype.hasOwnProperty.call(attrs, k) ? attrs[k] : null; },
            addEventListener(t, fn) { (listeners[t] = listeners[t] || []).push(fn); },
            getContext() { return { drawImage() {} }; },
            getBoundingClientRect() { return { left: 0, top: 0, width: 1000, height: 400 }; },
            requestPointerLock() {},
            focus() { el._focused = (el._focused || 0) + 1; },
            appendChild(c) { return c; },
            removeAttribute() {},
            get value() { return el._value; },
            set value(v) { el._value = String(v); },
            get textContent() { return el._text; },
            set textContent(v) { el._text = String(v); },
            get innerHTML() { return ''; },
            set innerHTML(_) { violations.push('innerHTML on ' + id); },
            get style() {
                if (id === 'video') {
                    return { set width(_) {}, set height(_) {} };
                }
                violations.push('style on ' + id);
                return {};
            },
        };
        return el;
    }

    for (const id of Object.keys(infos)) elements[id] = mkEl(id, infos[id]);
    const document = {
        fullscreenEnabled: false,
        fullscreenElement: null,
        pointerLockElement: null,
        hidden: false,
        addEventListener(t, fn) { docListeners[t] = fn; },
        getElementById(id) { return elements[id] || null; },
        createElement(tag) { return mkEl('created-' + tag, { tag, classes: [] }); },
        exitPointerLock() {},
        exitFullscreen() { return Promise.resolve(); },
    };

    class FakeWS {
        constructor(url) {
            this.url = url;
            this.readyState = 0;
            this.sent = [];
            sockets.push(this);
        }
        send(data) { this.sent.push(new Uint8Array(data)); }
        close() { this.readyState = 3; }
    }
    FakeWS.OPEN = 1;

    const status = { value: { state: 'ready', authenticated: true } };
    const ctx = {
        document,
        window: {
            addEventListener(t, fn) { (winListeners[t] = winListeners[t] || []).push(fn); },
            confirm: () => true,
        },
        location: { host: 'kvm.local', hostname: 'kvm.local', port: '' },
        WebSocket: FakeWS,
        TextEncoder,
        fetch: async (url) => {
            if (url === '/api/v1/auth/status') return { ok: true, status: 200, json: async () => status.value, headers: { get: () => null } };
            if (url === '/api/v1/iso') return { ok: true, status: 200, json: async () => ({ sd: { state: 'unmounted', total_bytes: null, free_bytes: null }, images: [], mounted: null, target: { locked: false, ejected_by_target: false }, chunk_size: 131072, hidden_images: 0, upload: null }), headers: { get: () => null } };
            return { ok: true, status: 200, json: async () => ({}), headers: { get: () => null } };
        },
        setTimeout(fn, ms) { const id = ++seq; timers.push({ id, fn, ms }); return id; },
        clearTimeout(id) { const idx = timers.findIndex((t) => t.id === id); if (idx >= 0) timers.splice(idx, 1); },
        setInterval() { return ++seq; },
        clearInterval() {},
        requestAnimationFrame() { return 1; },
        createImageBitmap: async () => ({ width: 1, height: 1, close() {} }),
    };

    if (opts.storage !== undefined) {
        if (opts.storage !== null) ctx.localStorage = opts.storage;
    }
    if (opts.navigator !== undefined) ctx.navigator = opts.navigator;

    vm.runInContext(source, vm.createContext(ctx), { filename: 'app.js' });
    docListeners.DOMContentLoaded();
    await flush();
    await flush();

    const wsInput = sockets.find((s) => /\/ws$/.test(s.url));
    if (wsInput && opts.grantInput !== false) {
        wsInput.readyState = 1;
        wsInput.onopen();
        wsInput.onmessage({ data: Uint8Array.from([0x80]).buffer });
        wsInput.sent.length = 0; // clear initial query/state messages if any
    }

    const emitClick = (id) => (elements[id]._listeners.click || []).forEach((fn) => fn({ preventDefault() {} }));
    const emitChange = (id) => (elements[id]._listeners.change || []).forEach((fn) => fn({ preventDefault() {} }));
    const emitSubmit = () => (elements['paste-form']._listeners.submit || []).forEach((fn) => fn({ preventDefault() {} }));
    const stepTimer = () => {
        if (!timers.length) return null;
        const t = timers.shift();
        t.fn();
        return t.ms;
    };
    const keyReports = () => wsInput.sent.map((u) => Array.from(u));

    return { document, docListeners, elements, winListeners, timers, sockets, wsInput, status, violations, emitClick, emitChange, emitSubmit, stepTimer, keyReports };
}

function makeStorage(init, opts) {
    opts = opts || {};
    const s = { data: Object.assign({}, init), reads: [], writes: [] };
    s.getItem = (k) => {
        s.reads.push(k);
        if (opts.getThrows) throw new Error('SecurityError');
        return Object.prototype.hasOwnProperty.call(s.data, k) ? s.data[k] : null;
    };
    s.setItem = (k, v) => {
        if (opts.setThrows) throw new Error('QuotaError');
        s.writes.push([k, String(v)]);
        s.data[k] = String(v);
    };
    return s;
}

async function checkBrowser(source) {
    const problems = [];
    const eq = (name, got, want) => {
        if (JSON.stringify(got) !== JSON.stringify(want)) problems.push(name + ': expected ' + JSON.stringify(want) + ', got ' + JSON.stringify(got));
    };
    const guard = async (name, fn) => {
        try { await fn(); } catch (e) { problems.push(name + ': threw ' + (e && e.message)); }
    };

    // 1. Open/close, no startup storage read, lazy layout restore
    await guard('browser/open close and layout persistence', async () => {
        const st = makeStorage({ 'quadkvm.pasteLayout': 'us' });
        const h = await bootApp(source, { storage: st });
        eq('1 no storage read at startup', st.reads.includes('quadkvm.pasteLayout'), false);
        eq('1 initially hidden', [h.elements['paste-panel']._classes.has('paste-hidden'), h.elements['btn-paste'].getAttribute('aria-expanded')], [true, null]);

        h.emitClick('btn-paste');
        eq('1 opened panel', [
            h.elements['paste-panel']._classes.has('paste-hidden'),
            h.elements['paste-panel'].getAttribute('aria-hidden'),
            h.elements['btn-paste'].getAttribute('aria-expanded'),
            h.elements['paste-layout-us'].checked,
            h.elements['paste-layout-jis'].checked,
        ], [false, 'false', 'true', true, false]);
        eq('1 lazy storage read once', st.reads.filter((k) => k === 'quadkvm.pasteLayout').length, 1);

        h.elements['paste-layout-jis'].checked = true;
        h.elements['paste-layout-us'].checked = false;
        h.emitChange('paste-layout-jis');
        eq('1 saved jis', st.writes, [['quadkvm.pasteLayout', 'jis']]);

        h.emitClick('btn-paste-close');
        eq('1 closed panel', [h.elements['paste-panel']._classes.has('paste-hidden'), h.elements['paste-panel'].getAttribute('aria-hidden'), h.elements['btn-paste'].getAttribute('aria-expanded')], [true, 'true', 'false']);
        eq('1 no DOM security violations', h.violations, []);
    });

    // 2. Throwing / missing localStorage still defaults cleanly to JIS
    await guard('browser/storage errors handled', async () => {
        const st = makeStorage({ 'quadkvm.pasteLayout': 'us' }, { getThrows: true, setThrows: true });
        const h = await bootApp(source, { storage: st });
        h.emitClick('btn-paste');
        eq('2 throwing getItem defaults jis', h.elements['paste-layout-jis'].checked, true);
        h.elements['paste-layout-us'].checked = true;
        h.elements['paste-layout-jis'].checked = false;
        h.emitChange('paste-layout-us');
        eq('2 throwing setItem does not throw', h.elements['paste-panel']._classes.has('paste-hidden'), false);
    });

    // 3. Clipboard read: success, empty, denied, unavailable
    await guard('browser/clipboard', async () => {
        let clipVal = 'echo "hi"';
        let clipMode = 'ok';
        const h = await bootApp(source, {
            navigator: { clipboard: { readText: async () => { if (clipMode === 'deny') throw new Error('NotAllowedError'); return clipVal; } } },
        });
        h.emitClick('btn-paste');
        h.emitClick('btn-paste-clipboard');
        await flush();
        eq('3 clipboard filled textarea', h.elements['paste-text'].value, 'echo "hi"');
        eq('3 status omits clipboard text', h.elements['paste-status'].textContent.includes('echo'), false);
        clipVal = '';
        h.emitClick('btn-paste-clipboard');
        await flush();
        eq('3 empty clipboard keeps status', h.elements['paste-status'].textContent, 'クリップボードは空です');
        clipMode = 'deny';
        h.emitClick('btn-paste-clipboard');
        await flush();
        eq('3 denied shows error', h.elements['paste-error']._classes.has('paste-hidden'), false);

        const hNo = await bootApp(source, {}); // missing navigator
        hNo.emitClick('btn-paste');
        hNo.emitClick('btn-paste-clipboard');
        await flush();
        eq('3 missing navigator shows error', hNo.elements['paste-error']._classes.has('paste-hidden'), false);
    });

    // 4. Validation errors send zero HID reports
    await guard('browser/validation rejections', async () => {
        const h = await bootApp(source);
        h.emitClick('btn-paste');
        h.wsInput.sent.length = 0;
        h.emitSubmit();
        eq('4 empty rejected without send', [h.elements['paste-error']._classes.has('paste-hidden'), h.keyReports()], [false, []]);

        h.elements['paste-text'].value = 'secret日本語';
        h.emitSubmit();
        eq('4 non-ascii rejected without send', [h.elements['paste-error']._classes.has('paste-hidden'), h.keyReports()], [false, []]);
        eq('4 error does not leak text', h.elements['paste-error'].textContent.includes('secret') || h.elements['paste-error'].textContent.includes('日本'), false);

        h.elements['paste-text'].value = 'a'.repeat(4097);
        h.emitSubmit();
        eq('4 too long rejected without send', [h.elements['paste-error']._classes.has('paste-hidden'), h.keyReports()], [false, []]);

        const hDisconnected = await bootApp(source, { grantInput: false });
        hDisconnected.emitClick('btn-paste');
        hDisconnected.elements['paste-text'].value = 'ok';
        hDisconnected.emitSubmit();
        eq('4 ungranted rejected without send', [hDisconnected.elements['paste-error']._classes.has('paste-hidden'), hDisconnected.wsInput.sent.length], [false, 0]);
    });

    // 5. Paced US & JIS send + onBeforeSend releaseAll + live window keydown/keyup guard + IME-safe Ctrl+Enter
    await guard('browser/paced send, live key isolation, and IME guard', async () => {
        const h = await bootApp(source, { storage: makeStorage({ 'quadkvm.pasteLayout': 'jis' }) });
        h.emitClick('btn-input-toggle'); // enable live input first
        // While paste panel is closed, live key forwards
        const ev = (code, extra) => Object.assign({ code, key: code, repeat: false, preventDefault() {} }, extra || {});
        h.winListeners.keydown[0](ev('KeyA'));
        eq('5 live key sent when closed', h.keyReports(), [[0x01, 0x04]]);

        h.emitClick('btn-paste');
        // Opening panel releases any held live keys immediately, then isolates keydown/keyup while open
        eq('5 open emits releaseAll for held key', h.keyReports(), [[0x01, 0x04], [0x03]]);
        h.winListeners.keydown[0](ev('KeyB'));
        h.winListeners.keyup[0](ev('KeyA'));
        eq('5 live keys isolated while open', h.keyReports().length, 2);

        // Esc long press state still resets on keyup even while open
        h.winListeners.keyup[0](ev('Escape'));

        // IME-safe Ctrl+Enter ignored when composing or keyCode 229
        h.elements['paste-text'].value = 'A]';
        const kd = h.elements['paste-text']._listeners.keydown[0];
        let def = 0;
        kd({ key: 'Enter', ctrlKey: true, isComposing: true, keyCode: 13, preventDefault() { def++; } });
        kd({ key: 'Enter', ctrlKey: true, isComposing: false, keyCode: 229, preventDefault() { def++; } });
        eq('5 IME composing Enter ignored', [def, h.timers.length], [0, 0]);

        // Confirmed Ctrl+Enter submits
        h.wsInput.sent.length = 0;
        kd({ key: 'Enter', ctrlKey: true, isComposing: false, keyCode: 13, preventDefault() { def++; } });
        eq('5 Ctrl+Enter submitted', [def, h.elements['paste-fields'].disabled, h.elements['paste-progress-wrap']._classes.has('paste-hidden')], [1, true, false]);
        // releaseAll (0x03) + first HID event (Shift 0xE1 down for 'A') fired immediately
        eq('5 initial releaseAll + first down', h.keyReports(), [[0x03], [0x01, 0xE1]]);
        const delays = [];
        for (let i = 0; i < 10; i++) {
            const d = h.stepTimer();
            if (d === null) break;
            delays.push(d);
        }
        eq('5 all steps spaced 12ms', delays, [12, 12, 12, 12, 12, 12]);
        // 'A' = [down E1, down 04, up 04, up E1], ']' (JIS) = [down 32, up 32]
        eq('5 exact JIS HID sequence', h.keyReports(), [
            [0x03],
            [0x01, 0xE1], [0x01, 0x04], [0x02, 0x04], [0x02, 0xE1],
            [0x01, 0x32], [0x02, 0x32],
        ]);
        eq('5 fields re-enabled and text cleared after done', [h.elements['paste-fields'].disabled, h.elements['paste-progress-wrap']._classes.has('paste-hidden'), h.elements['paste-status'].textContent, h.elements['paste-text'].value], [false, true, '送信完了: 2 文字', '']);

        // Unchecked paste-clear-after preserves text after completion; Mac Cmd+Enter also works in US layout
        h.elements['paste-clear-after'].checked = false;
        h.elements['paste-layout-us'].checked = true;
        h.elements['paste-layout-jis'].checked = false;
        h.emitChange('paste-layout-us');
        h.wsInput.sent.length = 0;
        h.elements['paste-text'].value = ']';
        kd({ key: 'Enter', metaKey: true, isComposing: false, keyCode: 13, preventDefault() {} });
        while (h.stepTimer() !== null) {}
        eq('5 US ] uses 0x30', h.keyReports(), [[0x03], [0x01, 0x30], [0x02, 0x30]]);
        eq('5 unchecked keeps text after done', h.elements['paste-text'].value, ']');
    });

    // 6. Cancel, Escape, Background visibilitychange, pagehide, Disconnect, and Logout cleanup
    await guard('browser/cancel visibilitychange pagehide disconnect and logout', async () => {
        const h = await bootApp(source);
        h.emitClick('btn-paste');
        h.elements['paste-text'].value = 'abcdef';
        h.emitSubmit();
        h.stepTimer();
        h.emitClick('btn-paste-cancel');
        eq('6 cancel emits releaseAll, clears timers and text', [h.keyReports().pop(), h.timers.length, h.elements['paste-fields'].disabled, h.elements['paste-progress-wrap']._classes.has('paste-hidden'), h.elements['paste-text'].value], [[0x03], 0, false, true, '']);

        // visibilitychange (hidden = true) mid-send even with inputEnabled = false must abort + releaseAll + clear text
        h.wsInput.sent.length = 0;
        h.elements['paste-text'].value = 'hide123';
        h.emitSubmit();
        h.stepTimer();
        h.document.hidden = true;
        h.docListeners.visibilitychange();
        eq('6 visibilitychange aborts send and emits releaseAll', [h.keyReports().pop(), h.timers.length, h.elements['paste-fields'].disabled, h.elements['paste-text'].value], [[0x03], 0, false, '']);
        h.document.hidden = false;

        // pagehide mid-send must abort + releaseAll (and preserve text only when unchecked)
        h.wsInput.sent.length = 0;
        h.elements['paste-clear-after'].checked = false;
        h.elements['paste-text'].value = 'hidepage';
        h.emitSubmit();
        h.stepTimer();
        (h.winListeners.pagehide || []).forEach((fn) => fn());
        eq('6 pagehide aborts send and emits releaseAll (unchecked keeps text)', [h.keyReports().pop(), h.timers.length, h.elements['paste-fields'].disabled, h.elements['paste-text'].value], [[0x03], 0, false, 'hidepage']);

        // Escape cancels while sending, closes when idle
        h.elements['paste-clear-after'].checked = true;
        h.wsInput.sent.length = 0;
        h.emitSubmit();
        let escDef = 0;
        h.elements['paste-panel']._listeners.keydown[0]({ key: 'Escape', isComposing: false, keyCode: 27, preventDefault() { escDef++; } });
        eq('6 Esc cancels mid-send without closing', [escDef, h.keyReports().pop(), h.elements['paste-panel']._classes.has('paste-hidden'), h.elements['paste-text'].value], [1, [0x03], false, '']);
        h.elements['paste-panel']._listeners.keydown[0]({ key: 'Escape', isComposing: false, keyCode: 27, preventDefault() { escDef++; } });
        eq('6 Esc closes when idle', h.elements['paste-panel']._classes.has('paste-hidden'), true);

        // Disconnect aborts mid-send
        h.emitClick('btn-paste');
        h.elements['paste-text'].value = 'xyz';
        h.emitSubmit();
        h.wsInput.readyState = 3;
        h.wsInput.onclose();
        eq('6 disconnect stops send, clears text, and shows error', [h.elements['paste-fields'].disabled, h.elements['paste-error']._classes.has('paste-hidden'), h.elements['paste-text'].value], [false, false, '']);

        // Logout always clears textarea even when paste-clear-after is unchecked
        const h2 = await bootApp(source);
        h2.emitClick('btn-paste');
        h2.elements['paste-clear-after'].checked = false;
        h2.elements['paste-text'].value = 'topsecret';
        h2.emitSubmit();
        h2.status.value = { state: 'ready', authenticated: false };
        h2.elements['btn-logout']._listeners.click[0]();
        await flush();
        await flush();
        eq('6 logout stops, clears textarea, and closes panel', [h2.elements['paste-text'].value, h2.elements['paste-panel']._classes.has('paste-hidden'), h2.timers.length], ['', true, 0]);

        // Pending clipboard reads resolved or rejected after stop() must not overwrite textarea/error
        let resolveClip = null;
        const hClipRes = await bootApp(source, {
            navigator: { clipboard: { readText: () => new Promise((r) => { resolveClip = r; }) } },
        });
        hClipRes.emitClick('btn-paste');
        hClipRes.emitClick('btn-paste-clipboard');
        hClipRes.status.value = { state: 'ready', authenticated: false };
        hClipRes.elements['btn-logout']._listeners.click[0]();
        await flush();
        await flush();
        resolveClip('stale-secret');
        await flush();
        eq('6 stale clipboard resolve ignored after logout', [hClipRes.elements['paste-text'].value, hClipRes.elements['paste-status'].textContent], ['', '']);

        let rejectClip = null;
        const hClipRej = await bootApp(source, {
            navigator: { clipboard: { readText: () => new Promise((_, rej) => { rejectClip = rej; }) } },
        });
        hClipRej.emitClick('btn-paste');
        hClipRej.emitClick('btn-paste-clipboard');
        hClipRej.status.value = { state: 'ready', authenticated: false };
        hClipRej.elements['btn-logout']._listeners.click[0]();
        await flush();
        await flush();
        rejectClip(new Error('NotAllowedError'));
        await flush();
        eq('6 stale clipboard reject ignored after logout', hClipRej.elements['paste-error']._classes.has('paste-hidden'), true);

        // Pending clipboard read started before startSend() must be invalidated on valid send start,
        // while validation failure must NOT invalidate the pending read
        let resolveSendClip = null;
        const hClipSend = await bootApp(source, {
            navigator: { clipboard: { readText: () => new Promise((r) => { resolveSendClip = r; }) } },
        });
        hClipSend.emitClick('btn-paste');
        hClipSend.emitClick('btn-paste-clipboard');
        hClipSend.elements['paste-text'].value = 'go';
        hClipSend.emitSubmit();
        while (hClipSend.stepTimer() !== null) {}
        eq('6 cleared text right after send completion', hClipSend.elements['paste-text'].value, '');
        resolveSendClip('late-secret');
        await flush();
        eq('6 stale clipboard resolve ignored after send', [hClipSend.elements['paste-text'].value, hClipSend.elements['paste-status'].textContent], ['', '送信完了: 2 文字']);

        let resolveValErrClip = null;
        const hClipVal = await bootApp(source, {
            navigator: { clipboard: { readText: () => new Promise((r) => { resolveValErrClip = r; }) } },
        });
        hClipVal.emitClick('btn-paste');
        hClipVal.emitClick('btn-paste-clipboard');
        hClipVal.emitSubmit(); // validation failure (empty) must NOT invalidate clipboard read
        eq('6 validation error shown', hClipVal.elements['paste-error']._classes.has('paste-hidden'), false);
        resolveValErrClip('after-val-err');
        await flush();
        eq('6 clipboard read still applies after validation error', hClipVal.elements['paste-text'].value, 'after-val-err');
    });

    return problems;
}

async function checkAll(source) {
    const p1 = checkPure(source);
    const p2 = checkFiles(source);
    let p3;
    try { p3 = await checkBrowser(source); } catch (e) { p3 = ['browser threw: ' + (e && e.message)]; }
    return p1.concat(p2, p3);
}

function mutantList() {
    const M = (id, desc, from, to) => ({ id, desc, from, to });
    return [
        M('P01', 'MAX_CHARS changed', 'const MAX_CHARS = 4096;', 'const MAX_CHARS = 4095;'),
        M('P02', 'STEP_MS changed', 'const STEP_MS = 12;', 'const STEP_MS = 10;'),
        M('P03', 'SHIFT_USAGE changed to Right Shift or wrong usage', 'const SHIFT_USAGE = 0xE1;', 'const SHIFT_USAGE = 0xE0;'),
        M('P04', 'LAYOUT_KEY wrong', "const LAYOUT_KEY = 'quadkvm.pasteLayout';", "const LAYOUT_KEY = 'quadkvm.layout';"),
        M('P05', 'JIS ] mapped to US [0x30]', "']': [0x32, false],", "']': [0x30, false],"),
        M('P06', 'JIS \\ mapped to US [0x31]', "'\\\\': [0x87, false],", "'\\\\': [0x31, false],"),
        M('P07', 'JIS | unshifted instead of shifted', "'|': [0x89, true],", "'|': [0x89, false],"),
        M('P08', 'normalizeNewlines skips CRLF/CR', "text.replace(/\\r\\n|\\r/g, '\\n')", 'text'),
        M('P09', 'planText allows empty string', "if (norm.length === 0) return { ok: false, error: 'empty' };", ''),
        M('P10', 'planText allows >MAX_CHARS', "if (norm.length > MAX_CHARS) return { ok: false, error: 'too_long', length: norm.length };", ''),
        M('P11', 'eventsForStep forgets Shift release', '                [0, SHIFT_USAGE],\n            ];', '            ];'),
        M('P12', 'IME composing check removed', 'if (e.isComposing || e.keyCode === 229) return;\n            if ((e.ctrlKey', 'if ((e.ctrlKey'),
        M('P13', 'Safari keyCode 229 check removed', 'if (e.isComposing || e.keyCode === 229) return;\n            if ((e.ctrlKey', 'if (e.isComposing) return;\n            if ((e.ctrlKey'),
        M('P14', 'startSend does not call onBeforeSend', '            deps.onBeforeSend();\n            sending = true;', '            sending = true;'),
        M('P15', 'abortSend omits sendReleaseAll', '            deps.sendReleaseAll();\n            finishUi(', '            finishUi('),
        M('P16', 'window keydown leaks keys while paste open', 'if (!inputEnabled || paste.isOpen()) return;\n\n        if (e.code', 'if (!inputEnabled) return;\n\n        if (e.code'),
        M('P17', 'window keyup leaks keys while paste open', 'if (!inputEnabled || paste.isOpen()) return;\n\n        if (Object.prototype', 'if (!inputEnabled) return;\n\n        if (Object.prototype'),
        M('P18', 'stop does not clear textarea on logout', "            if (sending) abortSend('終了したため送信を中止しました', false);\n            elText.value = '';\n            elStatus.textContent = '';", "            if (sending) abortSend('終了したため送信を中止しました', false);\n            elStatus.textContent = '';"),
        M('P19', 'layout loaded eagerly before panel open', "            if (panelOpen) {\n                ensureLayoutLoaded();", "            if (panelOpen) {"),
        M('P20', 'clipboard error still allows empty silent ok', "showError('クリップボードの読込が拒否されました。下の入力欄に手動で貼り付けてください');", ''),
        M('P21', 'visibilitychange does not abort HID paste', "        if (document.hidden) {\n            paste.onBackground();", "        if (document.hidden) {"),
        M('P22', 'pagehide does not abort HID paste', "    window.addEventListener('pagehide', () => {\n        paste.onBackground();", "    window.addEventListener('pagehide', () => {"),
        M('P23', 'completion does not clear textarea when checked', "finishUi('送信完了: ' + total + ' 文字', true);", "finishUi('送信完了: ' + total + ' 文字', false);"),
        M('P24', 'abort does not clear textarea when checked', "+ ' 文字送信済み）', true);", "+ ' 文字送信済み）', false);"),
        M('P25', 'setPanel open does not call onOpen', "                if (!wasOpen && typeof deps.onOpen === 'function') deps.onOpen();\n", ''),
        M('P26', 'stop does not bump clipboardGen', "        function stop() {\n            clipboardGen++;\n", "        function stop() {\n"),
        M('P27', 'readClipboard ignores clipboardGen after resolve', "                const txt = await read();\n                if (gen !== clipboardGen) return;\n", "                const txt = await read();\n"),
        M('P28', 'readClipboard ignores clipboardGen after reject', "            } catch (err) {\n                if (gen !== clipboardGen) return;\n", "            } catch (err) {\n"),
        M('P29', 'startSend does not bump clipboardGen', "            clipboardGen++;\n            deps.onBeforeSend();", "            deps.onBeforeSend();"),
    ];
}

(async () => {
    const source = fs.readFileSync(APP_JS, 'utf8');
    let status = 0;
    const problems = await checkAll(source);
    problems.forEach((p) => console.log('FAIL:', p));
    if (problems.length) status = 1;
    else console.log('paste viewer ok (' + (PRINTABLE_ASCII.length + 2) + 'x2 keymap entries, browser & CSP scenarios)');

    if (process.argv.includes('--mutations')) {
        const list = mutantList();
        let killed = 0, survived = 0, nomatch = 0;
        for (const m of list) {
            const count = source.split(m.from).length - 1;
            if (count !== 1) {
                console.log('NOMATCH ' + m.id + ' (' + count + ' matches) ' + m.desc);
                nomatch++;
                continue;
            }
            const mutated = source.replace(m.from, () => m.to);
            const found = (await checkAll(mutated)).length > 0;
            if (found) { killed++; console.log('KILLED   ' + m.id + ' ' + m.desc); }
            else { survived++; console.log('SURVIVED ' + m.id + ' ' + m.desc); }
        }
        console.log(list.length + ' mutants: ' + killed + ' killed, ' + survived + ' survived, ' + nomatch + ' no-match');
        if (survived || nomatch) status = 1;
    }
    process.exit(status);
})();
