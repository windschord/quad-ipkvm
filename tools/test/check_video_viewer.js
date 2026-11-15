#!/usr/bin/env node
// ビューア(components/web_server/web/app.js)の、映像の接続先と「証明書の案内」の検査(docs/VIDEO_TRANSPORT.md 5 章)。
//  (A) 純関数 VideoTarget(resolve / needsTrustHint / trustUrl)の表のテスト。期待値は手書き(実装から計算しない)。
//  (B) app.js 全体を、偽の DOM / WebSocket / fetch / タイマーの中で動かし、状態の応答 -> 接続の URL -> 失敗 ->
//      案内の表示・消去、再接続のバックオフ(既存の挙動)を観察する。index.html の id と class も、実際のファイルから読む。
// `--mutations` は、app.js の追加分を 1 か所ずつ変えたコピーに対して (A)(B) を動かし、落ちることを確認する。
// 使い方: node tools/test/check_video_viewer.js [--mutations]      (依存なし)
'use strict';
const fs = require('fs');
const path = require('path');
const vm = require('vm');

const WEB_DIR = path.join(__dirname, '..', '..', 'components', 'web_server', 'web');
const APP_JS = path.join(WEB_DIR, 'app.js');
const INDEX_HTML = path.join(WEB_DIR, 'index.html');

// ---------- (A) 純関数 ----------
function loadVideoTarget(source) {
    const m = /const VideoTarget = (\{[\s\S]*?\n    \});/.exec(source);
    if (!m) throw new Error('VideoTarget not found in app.js');
    return new Function('return ' + m[1])();
}

const L = (host, hostname, port) => ({ host, hostname, port });
const LEGACY = (host) => ({ url: 'wss://' + host + '/video', separate: false });

// [名前, loc, video_port(JSON から読んだ値), 期待する url, 期待する separate]
const RESOLVE_CASES = [
    ['port 8444, default HTTPS page', L('kvm.local', 'kvm.local', ''), 8444, 'wss://kvm.local:8444/video', true],
    ['port 8444, host by IPv4', L('192.168.1.20', '192.168.1.20', ''), 8444, 'wss://192.168.1.20:8444/video', true],
    ['port 8444, page on 8443 (host has a port)', L('kvm.local:8443', 'kvm.local', '8443'), 8444, 'wss://kvm.local:8444/video', true],
    ['host name keeps - _ . and case', L('My-KVM_1.Local', 'My-KVM_1.Local', ''), 9000, 'wss://My-KVM_1.Local:9000/video', true],
    ['lowest port 1', L('kvm.local', 'kvm.local', ''), 1, 'wss://kvm.local:1/video', true],
    ['highest port 65535', L('kvm.local', 'kvm.local', ''), 65535, 'wss://kvm.local:65535/video', true],
    ['port 443 = page on the default port (same server)', L('kvm.local', 'kvm.local', ''), 443, null, false],
    ['port 8443 = page on 8443 (same server)', L('kvm.local:8443', 'kvm.local', '8443'), 8443, null, false],
    ['port 443 with page on 8443 is a separate server', L('kvm.local:8443', 'kvm.local', '8443'), 443, 'wss://kvm.local:443/video', true],
    ['page port explicitly 443', L('kvm.local:443', 'kvm.local', '443'), 8444, 'wss://kvm.local:8444/video', true],
    ['no video_port (undefined)', L('kvm.local', 'kvm.local', ''), undefined, null, false],
    ['no video_port, page on 8443', L('kvm.local:8443', 'kvm.local', '8443'), undefined, null, false],
    ['null', L('kvm.local', 'kvm.local', ''), null, null, false],
    ['0', L('kvm.local', 'kvm.local', ''), 0, null, false],
    ['-0', L('kvm.local', 'kvm.local', ''), -0, null, false],
    ['-1', L('kvm.local', 'kvm.local', ''), -1, null, false],
    ['65536', L('kvm.local', 'kvm.local', ''), 65536, null, false],
    ['70000', L('kvm.local', 'kvm.local', ''), 70000, null, false],
    ['huge 1e300', L('kvm.local', 'kvm.local', ''), 1e300, null, false],
    ['2^53', L('kvm.local', 'kvm.local', ''), 9007199254740992, null, false],
    ['Infinity', L('kvm.local', 'kvm.local', ''), Infinity, null, false],
    ['NaN', L('kvm.local', 'kvm.local', ''), NaN, null, false],
    ['fraction 8444.5', L('kvm.local', 'kvm.local', ''), 8444.5, null, false],
    ['fraction 0.5', L('kvm.local', 'kvm.local', ''), 0.5, null, false],
    ['string "8444"', L('kvm.local', 'kvm.local', ''), '8444', null, false],
    ['string "8444/x"', L('kvm.local', 'kvm.local', ''), '8444/x', null, false],
    ['string "evil.example:1/"', L('kvm.local', 'kvm.local', ''), 'evil.example:1/', null, false],
    ['empty string', L('kvm.local', 'kvm.local', ''), '', null, false],
    ['true', L('kvm.local', 'kvm.local', ''), true, null, false],
    ['false', L('kvm.local', 'kvm.local', ''), false, null, false],
    ['array [8444]', L('kvm.local', 'kvm.local', ''), [8444], null, false],
    ['object {}', L('kvm.local', 'kvm.local', ''), {}, null, false],
    ['object with valueOf', L('kvm.local', 'kvm.local', ''), { valueOf() { return 8444; } }, null, false],
    // ホスト名の扱い(location.hostname を検査する。文字列の連結でホストを作らない)
    ['IPv6 literal (bracketed, as location.hostname)', L('[::1]', '[::1]', ''), 8444, 'wss://[::1]:8444/video', true],
    ['IPv6 literal with page port', L('[fe80::1]:8443', '[fe80::1]', '8443'), 8444, 'wss://[fe80::1]:8444/video', true],
    ['IPv6 unbracketed is not a valid host name', L('::1', '::1', ''), 8444, null, false],
    ['host name with a slash', L('a/b', 'a/b', ''), 8444, null, false],
    ['host name with @', L('u@evil.example', 'u@evil.example', ''), 8444, null, false],
    ['host name with a colon', L('a:1', 'a:1', ''), 8444, null, false],
    ['host name with a space', L('a b', 'a b', ''), 8444, null, false],
    ['host name with ?', L('a?b', 'a?b', ''), 8444, null, false],
    ['host name with #', L('a#b', 'a#b', ''), 8444, null, false],
    ['host name with a backslash', L('a\\b', 'a\\b', ''), 8444, null, false],
    ['host name with a newline', L('a\nb', 'a\nb', ''), 8444, null, false],
    ['host name non-ASCII', L('kvm.ローカル', 'kvm.ローカル', ''), 8444, null, false],
    ['host name empty', L('', '', ''), 8444, null, false],
    ['host name undefined', L('kvm.local', undefined, ''), 8444, null, false],
    ['host name not a string', L('kvm.local', 5, ''), 8444, null, false],
    ['IPv6 bracket with junk', L('[::1]x', '[::1]x', ''), 8444, null, false],
    ['IPv6 bracket with a slash', L('[::1/x]', '[::1/x]', ''), 8444, null, false],
    ['IPv6 empty brackets', L('[]', '[]', ''), 8444, null, false],
    ['host name 255 characters', L('a'.repeat(255), 'a'.repeat(255), ''), 8444, 'wss://' + 'a'.repeat(255) + ':8444/video', true],
    ['host name 256 characters', L('a'.repeat(256), 'a'.repeat(256), ''), 8444, null, false],
];

const HINT_CASES = [
    // [名前, target, wasOpen, 期待]
    ['separate, failed before open', { separate: true, port: 8444, hostname: 'kvm.local' }, false, true],
    ['separate, was open', { separate: true, port: 8444, hostname: 'kvm.local' }, true, false],
    ['same server, failed before open', { separate: false, port: 0, hostname: '' }, false, false],
    ['same server, was open', { separate: false, port: 0, hostname: '' }, true, false],
    ['no target', null, false, false],
    ['no target (undefined)', undefined, false, false],
    ['separate is truthy but not true', { separate: 1, port: 8444, hostname: 'kvm.local' }, false, false],
    ['wasOpen undefined is not "failed before open"', { separate: true, port: 8444, hostname: 'kvm.local' }, undefined, false],
];

const TRUST_URL_CASES = [
    ['normal', { separate: true, port: 8444, hostname: 'kvm.local' }, 'https://kvm.local:8444/'],
    ['IPv4', { separate: true, port: 1, hostname: '10.0.0.5' }, 'https://10.0.0.5:1/'],
    ['IPv6', { separate: true, port: 65535, hostname: '[::1]' }, 'https://[::1]:65535/'],
    ['same server: no link', { separate: false, port: 0, hostname: '' }, null],
    ['no target: no link', null, null],
];

function checkPure(source) {
    const problems = [];
    let vt;
    try { vt = loadVideoTarget(source); } catch (e) { return [String(e.message)]; }
    for (const [name, loc, port, wantUrl, wantSep] of RESOLVE_CASES) {
        let got;
        try { got = vt.resolve(loc, port); } catch (e) { problems.push('resolve ' + name + ': threw ' + e.message); continue; }
        const url = wantUrl === null ? LEGACY(loc.host).url : wantUrl;
        if (got.url !== url || got.separate !== wantSep) {
            problems.push('resolve ' + name + ': expected ' + JSON.stringify([url, wantSep]) + ', got ' + JSON.stringify([got.url, got.separate]));
        }
        if (wantSep && (got.port !== port || got.hostname !== loc.hostname)) {
            problems.push('resolve ' + name + ': port/hostname fields wrong: ' + JSON.stringify([got.port, got.hostname]));
        }
        if (!wantSep && (got.port !== 0 || got.hostname !== '')) {
            problems.push('resolve ' + name + ': legacy result must carry port 0 and no hostname');
        }
    }
    for (const [name, target, wasOpen, want] of HINT_CASES) {
        let got;
        try { got = vt.needsTrustHint(target, wasOpen); } catch (e) { problems.push('needsTrustHint ' + name + ': threw ' + e.message); continue; }
        if (got !== want) problems.push('needsTrustHint ' + name + ': expected ' + want + ', got ' + got);
    }
    for (const [name, target, want] of TRUST_URL_CASES) {
        let got;
        try { got = vt.trustUrl(target); } catch (e) { problems.push('trustUrl ' + name + ': threw ' + e.message); continue; }
        if (got !== want) problems.push('trustUrl ' + name + ': expected ' + JSON.stringify(want) + ', got ' + JSON.stringify(got));
    }
    // pagePort は resolve を通して検査済み。直接も見る(location.port は '' = 既定の 443)。
    const pp = [[L('h', 'h', ''), 443], [L('h', 'h', '8443'), 8443], [L('h', 'h', '1'), 1], [L('h', 'h', 'x'), 443], [L('h', 'h', undefined), 443]];
    for (const [loc, want] of pp) {
        const got = vt.pagePort(loc);
        if (got !== want) problems.push('pagePort ' + JSON.stringify(loc) + ': expected ' + want + ', got ' + got);
    }
    return problems;
}

// ---------- (B) app.js 全体を偽の環境で動かす ----------
function parseIndexHtml() {
    const html = fs.readFileSync(INDEX_HTML, 'utf8');
    const elements = {};
    const re = /<(\w+)\b([^>]*)>/g;
    let m;
    while ((m = re.exec(html)) !== null) {
        const attrs = m[2];
        const id = /\bid="([^"]*)"/.exec(attrs);
        if (!id) continue;
        const cls = /\bclass="([^"]*)"/.exec(attrs);
        elements[id[1]] = { tag: m[1], classes: cls ? cls[1].split(/\s+/).filter(Boolean) : [], attrs };
    }
    return elements;
}

function makeElement(info) {
    const classes = new Set(info.classes);
    const attrs = {};
    const el = {
        classList: {
            add(c) { classes.add(c); },
            remove(c) { classes.delete(c); },
            contains(c) { return classes.has(c); },
            toggle(c) { if (classes.has(c)) classes.delete(c); else classes.add(c); },
        },
        setAttribute(k, v) { attrs[k] = String(v); },
        removeAttribute(k) { delete attrs[k]; },
        getAttribute(k) { return Object.prototype.hasOwnProperty.call(attrs, k) ? attrs[k] : null; },
        hasAttribute(k) { return Object.prototype.hasOwnProperty.call(attrs, k); },
        addEventListener() {},
        getContext() { return { drawImage() {} }; },
        getBoundingClientRect() { return { left: 0, top: 0, width: 1, height: 1 }; },
        textContent: '', value: '', width: 1280, height: 720, disabled: false,
        style: {},
        _classes: classes,
    };
    return new Proxy(el, {
        get(t, p) { return p in t ? t[p] : function () {}; },
    });
}

class FakeWebSocket {
    constructor(url) { this.url = url; this.readyState = 0; FakeWebSocket.instances.push(this); }
    send() {}
    close() { this.readyState = 3; }
}
FakeWebSocket.OPEN = 1;
FakeWebSocket.instances = [];

// loc: { host, hostname, port }. statusRef.value: 状態の応答の JSON (fetch のたびに読む)。
async function boot(source, loc, statusRef) {
    const infos = parseIndexHtml();
    const elements = {};
    for (const id of Object.keys(infos)) elements[id] = makeElement(infos[id]);
    const timers = [];
    let timerSeq = 0;
    const listeners = {};
    FakeWebSocket.instances = [];
    const sockets = FakeWebSocket.instances;
    const document = {
        getElementById(id) { return Object.prototype.hasOwnProperty.call(elements, id) ? elements[id] : null; },
        addEventListener(type, fn) { listeners[type] = fn; },
        pointerLockElement: null,
        hidden: false,
        exitPointerLock() {},
    };
    const ctxObj = {
        document,
        window: { addEventListener() {} },
        location: loc,
        WebSocket: FakeWebSocket,
        fetch: async () => ({ ok: true, status: 200, json: async () => JSON.parse(JSON.stringify(statusRef.value)), headers: { get() { return null; } } }),
        setTimeout(fn, delay) { const id = ++timerSeq; timers.push({ id, fn, delay }); return id; },
        clearTimeout(id) { const i = timers.findIndex((t) => t.id === id); if (i >= 0) timers.splice(i, 1); },
        setInterval() { return ++timerSeq; },
        clearInterval() {},
        requestAnimationFrame() { return 1; },
        createImageBitmap: async () => ({ width: 1, height: 1, close() {} }),
        TextEncoder,
    };
    const context = vm.createContext(ctxObj);
    vm.runInContext(source, context, { filename: 'app.js' });
    listeners.DOMContentLoaded();
    await flush();
    const h = {
        elements, timers, sockets,
        videoSockets() { return sockets.filter((s) => /\/video$/.test(s.url)); },
        lastVideo() { const v = h.videoSockets(); return v[v.length - 1]; },
        hintHidden() { return elements['video-trust-hint']._classes.has('trust-hint-hidden'); },
        linkHref() { return elements['video-trust-link'].getAttribute('href'); },
        linkText() { return elements['video-trust-link'].textContent; },
        // 映像の WebSocket を閉じて、再接続のタイマー(1 つ)の遅延を返す。run=true ならそのタイマーを実行する。
        async closeVideo(run) {
            const before = timers.length;
            h.lastVideo().onclose();
            const added = timers.slice(before);
            const t = added[added.length - 1];
            if (run && t) { timers.splice(timers.indexOf(t), 1); t.fn(); await flush(); }
            return t ? t.delay : null;
        },
    };
    return h;
}
const flush = () => new Promise((r) => setImmediate(r));

const READY_AUTH = (extra) => Object.assign({ state: 'ready', authenticated: true }, extra);

async function checkBrowser(source) {
    const problems = [];
    const eq = (name, got, want) => {
        if (JSON.stringify(got) !== JSON.stringify(want)) problems.push(name + ': expected ' + JSON.stringify(want) + ', got ' + JSON.stringify(got));
    };
    const guard = async (name, fn) => {
        try { await fn(); } catch (e) { problems.push(name + ': threw ' + (e && e.message)); }
    };
    const info = parseIndexHtml();

    // index.html の要素(app.js が getElementById で取る)と、リンクの属性
    for (const id of ['video-trust-hint', 'video-trust-link']) {
        if (!info[id]) problems.push('index.html lacks #' + id);
    }
    if (info['video-trust-hint'] && !info['video-trust-hint'].classes.includes('trust-hint-hidden')) problems.push('#video-trust-hint must start hidden');
    if (info['video-trust-link']) {
        if (!/target="_blank"/.test(info['video-trust-link'].attrs)) problems.push('#video-trust-link must open in a new tab');
        if (!/rel="noopener noreferrer"/.test(info['video-trust-link'].attrs)) problems.push('#video-trust-link needs rel="noopener noreferrer"');
        if (/\bhref=/.test(info['video-trust-link'].attrs)) problems.push('#video-trust-link must have no static href');
    }

    // 1. video_port あり: 別のポートの URL、開く前の失敗(onerror のみ)で案内
    await guard('separate/onerror', async () => {
        const st = { value: READY_AUTH({ video_port: 8444 }) };
        const h = await boot(source, L('kvm.local', 'kvm.local', ''), st);
        eq('1 urls', h.sockets.map((s) => s.url).sort(), ['wss://kvm.local/ws', 'wss://kvm.local:8444/video']);
        eq('1 hint initially hidden', h.hintHidden(), true);
        h.lastVideo().onerror({});
        eq('1 hint after onerror', h.hintHidden(), false);
        eq('1 link', h.linkHref(), 'https://kvm.local:8444/');
        eq('1 link text', h.linkText(), 'https://kvm.local:8444/');
    });
    // 2. onclose のみ(開く前)でも案内
    await guard('separate/onclose', async () => {
        const st = { value: READY_AUTH({ video_port: 8444 }) };
        const h = await boot(source, L('kvm.local', 'kvm.local', ''), st);
        await h.closeVideo(false);
        eq('2 hint after pre-open close', h.hintHidden(), false);
        eq('2 link', h.linkHref(), 'https://kvm.local:8444/');
    });
    // 3. 成功したら消える。開いた後の onerror / onclose では出さない
    await guard('separate/open', async () => {
        const st = { value: READY_AUTH({ video_port: 8444 }) };
        const h = await boot(source, L('kvm.local', 'kvm.local', ''), st);
        h.lastVideo().onerror({});
        await h.closeVideo(true);
        eq('3 still shown while failing', h.hintHidden(), false);
        h.lastVideo().onopen();
        eq('3 hidden after open', h.hintHidden(), true);
        eq('3 link cleared', h.linkHref(), null);
        eq('3 link text cleared', h.linkText(), '');
        h.lastVideo().onerror({});
        eq('3 error after open: no hint', h.hintHidden(), true);
        await h.closeVideo(false);
        eq('3 close after open: no hint', h.hintHidden(), true);
    });
    // 4. 次の接続で開き直す前に、案内は消えない(点滅しない)。open したら消える
    await guard('separate/reconnect', async () => {
        const st = { value: READY_AUTH({ video_port: 8444 }) };
        const h = await boot(source, L('kvm.local', 'kvm.local', ''), st);
        await h.closeVideo(true);
        eq('4 reconnect goes to the same url', h.videoSockets().map((s) => s.url), ['wss://kvm.local:8444/video', 'wss://kvm.local:8444/video']);
        eq('4 hint kept during reconnect', h.hintHidden(), false);
        h.lastVideo().onopen();
        eq('4 hint gone after open', h.hintHidden(), true);
    });
    // 5. video_port なし(従来): メインのポートの /video、案内は出さない
    await guard('legacy', async () => {
        const st = { value: READY_AUTH({}) };
        const h = await boot(source, L('kvm.local:8443', 'kvm.local', '8443'), st);
        eq('5 url', h.videoSockets().map((s) => s.url), ['wss://kvm.local:8443/video']);
        h.lastVideo().onerror({});
        await h.closeVideo(false);
        eq('5 no hint', h.hintHidden(), true);
        eq('5 no link', h.linkHref(), null);
    });
    // 6. 範囲外・型違いの video_port は無視(従来どおり)。案内も出さない
    for (const bad of [0, -1, 65536, 8444.5, '8444', null, true, [8444], 1e300]) {
        await guard('bad video_port ' + JSON.stringify(bad), async () => {
            const st = { value: READY_AUTH({ video_port: bad }) };
            const h = await boot(source, L('kvm.local', 'kvm.local', ''), st);
            eq('6 url for ' + JSON.stringify(bad), h.videoSockets().map((s) => s.url), ['wss://kvm.local/video']);
            h.lastVideo().onerror({});
            eq('6 no hint for ' + JSON.stringify(bad), h.hintHidden(), true);
        });
    }
    // 7. IPv6、メインのポートが 8443 のとき
    await guard('ipv6', async () => {
        const st = { value: READY_AUTH({ video_port: 8444 }) };
        const h = await boot(source, L('[::1]:8443', '[::1]', '8443'), st);
        eq('7 url', h.videoSockets().map((s) => s.url), ['wss://[::1]:8444/video']);
        h.lastVideo().onerror({});
        eq('7 link', h.linkHref(), 'https://[::1]:8444/');
    });
    // 8. 再接続のバックオフ(既存の挙動): 1000, 2000, 4000, 8000, 10000, 10000。open で 1000 に戻る
    await guard('backoff', async () => {
        const st = { value: READY_AUTH({ video_port: 8444 }) };
        const h = await boot(source, L('kvm.local', 'kvm.local', ''), st);
        const seq = [];
        for (let i = 0; i < 6; i++) seq.push(await h.closeVideo(true));
        eq('8 backoff', seq, [1000, 2000, 4000, 8000, 10000, 10000]);
        h.lastVideo().onopen();
        eq('8 backoff reset', await h.closeVideo(false), 1000);
    });
    // 9. ログイン画面に戻ったら案内を消す(切断の経路)
    await guard('logout', async () => {
        const st = { value: READY_AUTH({ video_port: 8444 }) };
        const h = await boot(source, L('kvm.local', 'kvm.local', ''), st);
        h.lastVideo().onerror({});
        eq('9 shown', h.hintHidden(), false);
        st.value = { state: 'ready', authenticated: false };
        await h.closeVideo(true);
        eq('9 hidden on the login view', h.hintHidden(), true);
        eq('9 link cleared', h.linkHref(), null);
        eq('9 no new video socket', h.videoSockets().length, 1);
    });
    // 10. 状態の応答が変わる(video_port が消える)と、次の接続は従来の URL
    await guard('port disappears', async () => {
        const st = { value: READY_AUTH({ video_port: 8444 }) };
        const h = await boot(source, L('kvm.local', 'kvm.local', ''), st);
        st.value = READY_AUTH({});
        await h.closeVideo(true);
        eq('10 urls', h.videoSockets().map((s) => s.url), ['wss://kvm.local:8444/video', 'wss://kvm.local/video']);
    });
    return problems;
}

async function checkAll(source) {
    const problems = checkPure(source);
    let browserProblems;
    try { browserProblems = await checkBrowser(source); } catch (e) { browserProblems = ['browser harness threw: ' + (e && e.message)]; }
    return problems.concat(browserProblems);
}

// ---------- ミュータント(app.js の追加分を 1 か所ずつ変える) ----------
// 同値(殺せないのが正しい)なものには equivalent: 理由 を付ける。
function mutantList() {
    const M = (id, desc, from, to, equivalent) => ({ id, desc, from, to, equivalent });
    return [
        M('J01', 'validPort: Number.isInteger removed', "typeof v === 'number' && Number.isInteger(v) && v >= 1", "typeof v === 'number' && v >= 1"),
        M('J02', 'validPort: lower bound >= 1 -> >= 0', 'v >= 1 && v <= 65535', 'v >= 0 && v <= 65535'),
        M('J03', 'validPort: lower bound >= 1 -> > 1', 'v >= 1 && v <= 65535', 'v > 1 && v <= 65535'),
        M('J04', 'validPort: upper bound <= 65535 -> < 65535', 'v >= 1 && v <= 65535', 'v >= 1 && v < 65535'),
        M('J05', 'validPort: upper bound <= 65535 -> <= 65536', 'v >= 1 && v <= 65535', 'v >= 1 && v <= 65536'),
        M('J06', 'validPort: upper bound removed', 'v >= 1 && v <= 65535', 'v >= 1'),
        M('J07', 'validPort: typeof check removed (Number.isInteger already rejects every non-number)',
            "typeof v === 'number' && Number.isInteger(v)", 'Number.isInteger(v)',
            'Number.isInteger は数値以外(文字列、真偽値、配列、オブジェクト)を常に false にするので、typeof は二重の防御'),
        M('J08', 'validHostname: length limit 255 -> 254', 'h.length > 255', 'h.length > 254'),
        M('J09', 'validHostname: length limit 255 -> 256', 'h.length > 255', 'h.length > 256'),
        M('J10', 'validHostname: explicit empty check removed (both regexes already need 1+ characters)', 'h.length === 0 ||', 'false ||',
            '2 つの正規表現が、どちらも 1 文字以上を要求する(+)ので、空文字は明示の検査がなくても拒否される(二重の防御)'),
        M('J11', 'validHostname: start anchor removed', '/^[A-Za-z0-9._-]+$/', '/[A-Za-z0-9._-]+$/'),
        M('J12', 'validHostname: end anchor removed', '/^[A-Za-z0-9._-]+$/', '/^[A-Za-z0-9._-]+/'),
        M('J13', 'validHostname: "_" not allowed', '/^[A-Za-z0-9._-]+$/', '/^[A-Za-z0-9.-]+$/'),
        M('J14', 'validHostname: "-" not allowed', '/^[A-Za-z0-9._-]+$/', '/^[A-Za-z0-9._]+$/'),
        M('J15', 'validHostname: "." not allowed', '/^[A-Za-z0-9._-]+$/', '/^[A-Za-z0-9_-]+$/'),
        M('J16', 'validHostname: upper case not allowed', '/^[A-Za-z0-9._-]+$/', '/^[a-z0-9._-]+$/'),
        M('J17', 'validHostname: IPv6 end anchor removed', '/^\\[[0-9A-Fa-f:.]+\\]$/', '/^\\[[0-9A-Fa-f:.]+\\]/'),
        M('J18', 'validHostname: IPv6 body allows anything', '/^\\[[0-9A-Fa-f:.]+\\]$/', '/^\\[.+\\]$/'),
        M('J19', 'validHostname: IPv6 body may be empty', '/^\\[[0-9A-Fa-f:.]+\\]$/', '/^\\[[0-9A-Fa-f:.]*\\]$/'),
        M('J20', 'pagePort: default 443 -> 80', 'return 443;\n        },\n        // No usable', 'return 80;\n        },\n        // No usable'),
        M('J21', 'pagePort: port not converted to a number', 'return Number(loc.port);', 'return loc.port;'),
        M('J22', 'pagePort: digits limit 5 -> 6 (equivalent for real ports)', '{1,5}$/.test(loc.port)', '{1,6}$/.test(loc.port)',
            'location.port は 0〜65535 の 10 進数(最大 5 桁)なので、6 桁まで許しても実際の値では結果が変わらない'),
        M('J23', 'pagePort: port syntax not checked', "typeof loc.port === 'string' && /^[0-9]{1,5}$/.test(loc.port)", "typeof loc.port === 'string' && loc.port !== ''"),
        M('J24', 'resolve: same-port check inverted', 'videoPort === this.pagePort(loc)', 'videoPort !== this.pagePort(loc)'),
        M('J25', 'resolve: same-port check removed', '            if (videoPort === this.pagePort(loc)) return legacy;\n', ''),
        M('J26', 'resolve: hostname check removed', '            if (!this.validHostname(loc.hostname)) return legacy;\n', ''),
        M('J27', 'resolve: port check removed', '            if (!this.validPort(videoPort)) return legacy;\n', ''),
        M('J28', 'resolve: uses location.host instead of hostname for the video URL', "'wss://' + loc.hostname + ':' + videoPort", "'wss://' + loc.host + ':' + videoPort"),
        M('J29', 'resolve: path /video -> /ws', "':' + videoPort + '/video'", "':' + videoPort + '/ws'"),
        M('J30', 'resolve: legacy URL uses hostname (drops the main port)', "'wss://' + loc.host + '/video', separate: false", "'wss://' + loc.hostname + '/video', separate: false"),
        M('J31', 'resolve: scheme ws instead of wss', "{ url: 'wss://' + loc.hostname", "{ url: 'ws://' + loc.hostname"),
        M('J32', 'resolve: separate false for a separate port', 'separate: true, port: videoPort', 'separate: false, port: videoPort'),
        M('J33', 'resolve: separate target loses its port', 'separate: true, port: videoPort, hostname: loc.hostname', 'separate: true, port: 0, hostname: loc.hostname'),
        M('J34', 'resolve: separate target loses its hostname', 'separate: true, port: videoPort, hostname: loc.hostname', "separate: true, port: videoPort, hostname: ''"),
        M('J35', 'needsTrustHint: wasOpen === false -> === true', 'wasOpen === false', 'wasOpen === true'),
        M('J36', 'needsTrustHint: wasOpen condition removed', 'target.separate === true && wasOpen === false', 'target.separate === true'),
        M('J37', 'needsTrustHint: separate condition removed', 'target.separate === true && wasOpen === false', 'wasOpen === false'),
        M('J38', 'needsTrustHint: null target guard removed', '!!target && target.separate === true', 'target.separate === true'),
        M('J39', 'needsTrustHint: strict === false loosened to !wasOpen', 'wasOpen === false', '!wasOpen'),
        M('J40', 'trustUrl: http instead of https', "'https:' + '//'", "'http:' + '//'"),
        M('J41', 'trustUrl: port dropped', "target.hostname + ':' + target.port + '/'", "target.hostname + '/'"),
        M('J42', 'trustUrl: trailing slash dropped', "':' + target.port + '/'", "':' + target.port"),
        M('J43', 'trustUrl: null guard for same-server removed', "if (!target || target.separate !== true) return null;", "if (!target) return null;"),
        M('J44', 'showTrustHint: link goes to the wrong attribute', "setAttribute('href', url)", "setAttribute('title', url)"),
        M('J45', 'showTrustHint: link text not set', '        elVideoTrustLink.textContent = url;\n', ''),
        M('J46', 'showTrustHint: hint stays hidden', "elVideoTrustHint.classList.remove('trust-hint-hidden');", "elVideoTrustHint.classList.add('trust-hint-hidden');"),
        M('J47', 'hideTrustHint: hint stays visible', "elVideoTrustHint.classList.add('trust-hint-hidden');\n        elVideoTrustLink.removeAttribute", "elVideoTrustHint.classList.remove('trust-hint-hidden');\n        elVideoTrustLink.removeAttribute"),
        M('J48', 'hideTrustHint: link href kept', "        elVideoTrustLink.removeAttribute('href');\n", ''),
        M('J49', 'connect: onopen does not mark the connection as opened', '            opened = true;\n', ''),
        M('J50', 'connect: onopen does not hide the hint', '            opened = true;\n            hideTrustHint();\n', '            opened = true;\n'),
        M('J51', 'connect: onclose does not show the hint', '            if (VideoTarget.needsTrustHint(target, opened)) showTrustHint(target);\n            // Exponential', '            // Exponential'),
        M('J52', 'connect: onerror does not show the hint', '            if (VideoTarget.needsTrustHint(target, opened)) showTrustHint(target);\n        };', '        };'),
        M('J53', 'connect: opened starts true', 'let opened = false;', 'let opened = true;'),
        M('J54', 'connect: always connects to the legacy URL', 'const target = videoTarget || VideoTarget.resolve(location, undefined);', 'const target = VideoTarget.resolve(location, undefined);'),
        M('J55', 'status: video_port of the status response ignored', 'videoTarget = VideoTarget.resolve(location, data.video_port);', 'videoTarget = VideoTarget.resolve(location, undefined);'),
        M('J56', 'status: video_port read from the wrong key', 'VideoTarget.resolve(location, data.video_port)', 'VideoTarget.resolve(location, data.port)'),
        M('J57', 'disconnect: hint not hidden on the login view', "        hideTrustHint();\n        elVideoStatus.textContent = '切断されました';", "        elVideoStatus.textContent = '切断されました';"),
        M('J58', 'backoff: first delay changed', 'let wsReconnectDelay = 1000;', 'let wsReconnectDelay = 500;'),
        M('J59', 'backoff: cap 10000 -> 20000', 'wsReconnectDelay = Math.min(wsReconnectDelay * 2, 10000);\n        };\n\n        ws.onerror', 'wsReconnectDelay = Math.min(wsReconnectDelay * 2, 20000);\n        };\n\n        ws.onerror'),
        M('J60', 'backoff: reset on open removed', "            wsReconnectDelay = 1000; // reset\n        };\n\n        ws.onmessage", "        };\n\n        ws.onmessage"),
    ];
}

(async () => {
    const source = fs.readFileSync(APP_JS, 'utf8');
    let status = 0;
    const problems = await checkAll(source);
    problems.forEach((p) => console.log('FAIL:', p));
    if (problems.length) status = 1;
    else console.log('video viewer ok (' + RESOLVE_CASES.length + ' resolve cases, ' + HINT_CASES.length + ' hint cases, ' + TRUST_URL_CASES.length + ' trust-url cases, browser scenarios)');

    if (process.argv.includes('--mutations')) {
        const list = mutantList();
        let killed = 0, equivalent = 0, survived = 0, nomatch = 0;
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
            else if (m.equivalent) { equivalent++; console.log('EQUIV    ' + m.id + ' ' + m.desc + ' [' + m.equivalent + ']'); }
            else { survived++; console.log('SURVIVED ' + m.id + ' ' + m.desc); }
        }
        console.log(list.length + ' mutants: ' + killed + ' killed, ' + equivalent + ' equivalent, ' + survived + ' survived, ' + nomatch + ' no-match');
        if (survived || nomatch) status = 1;
    }
    process.exit(status);
})();
