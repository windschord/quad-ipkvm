#!/usr/bin/env node
// ビューア(components/web_server/web/app.js)の、映像の接続先と「証明書の案内」の検査(docs/VIDEO_TRANSPORT.md 5 章)。
//  (A) 純関数 VideoTarget(resolve / needsTrustHint / trustUrl)の表のテスト。期待値は手書き(実装から計算しない)。
//  (B) app.js 全体を、偽の DOM / WebSocket / fetch / タイマーの中で動かし、状態の応答 -> 接続の URL -> 失敗 ->
//      案内の表示・消去、再接続のバックオフ(既存の挙動)を観察する。index.html の id と class も、実際のファイルから読む。
//  (C) 映像の状態(WebSocket のテキストフレーム。docs/WEB_API.md 5、docs/VIDEO_TRANSPORT.md 8 章)の純関数
//      VideoStatus(parse など)の表のテストと、(B) と同じ偽の環境での表示のシナリオ(重ねる表示、2 秒のタイマー、リセット)。
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

function loadVideoStatus(source) {
    const m = /const VideoStatus = (\{[\s\S]*?\n    \});/.exec(source);
    if (!m) throw new Error('VideoStatus not found in app.js');
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

// ---------- (C) 映像の状態の純関数 ----------
const V = (state, w, h, extra) => JSON.stringify(Object.assign({ t: 'video', state, w, h }, extra || {}));
// 全体の長さをちょうど n 文字にした、契約どおりの JSON(未知のキー x で埋める)
const padded = (n) => {
    const head = '{"t":"video","state":"locked","w":1280,"h":720,"x":"';
    return head + 'a'.repeat(n - head.length - 2) + '"}';
};
const LOCKED = (w, h) => ({ state: 'locked', w, h });
const NOSIG = { state: 'no_signal', w: 0, h: 0 };
const UNKNOWN = { state: 'unknown', w: 0, h: 0 };

// [名前, 入力, 期待(null = 捨てる)]。期待値は手書き。
const PARSE_CASES = [
    ['locked 1280x720', V('locked', 1280, 720), LOCKED(1280, 720)],
    ['locked 1920x1080 (w and h are not swapped)', V('locked', 1920, 1080), LOCKED(1920, 1080)],
    ['locked 1x1 (lowest)', V('locked', 1, 1), LOCKED(1, 1)],
    ['locked 65535x65535 (highest)', V('locked', 65535, 65535), LOCKED(65535, 65535)],
    ['no_signal 0x0', V('no_signal', 0, 0), NOSIG],
    ['unknown 0x0', V('unknown', 0, 0), UNKNOWN],
    ['the exact text the server sends', '{"t":"video","state":"locked","w":1280,"h":720}', LOCKED(1280, 720)],
    ['keys in another order', '{"h":720,"w":1280,"state":"locked","t":"video"}', LOCKED(1280, 720)],
    ['white space around it', ' \n{"t":"video","state":"no_signal","w":0,"h":0}\n ', NOSIG],
    // locked 以外は w = h = 0 に直す
    ['no_signal with w,h not 0 -> 0', V('no_signal', 1280, 720), NOSIG],
    ['unknown with w,h not 0 -> 0', V('unknown', 65535, 1), UNKNOWN],
    ['no_signal with only w not 0', V('no_signal', 5, 0), NOSIG],
    ['no_signal with only h not 0', V('no_signal', 0, 5), NOSIG],
    // locked で 0 は捨てる(契約: locked のときだけ 1 以上)
    ['locked 0x0', V('locked', 0, 0), null],
    ['locked w=0', V('locked', 0, 720), null],
    ['locked h=0', V('locked', 1280, 0), null],
    // t
    ['t is another string', V('locked', 1280, 720).replace('"video"', '"audio"'), null],
    ['t is upper case', V('locked', 1280, 720).replace('"video"', '"Video"'), null],
    ['t is missing', '{"state":"locked","w":1280,"h":720}', null],
    ['t is a number', '{"t":1,"state":"locked","w":1280,"h":720}', null],
    ['t is null', '{"t":null,"state":"locked","w":1280,"h":720}', null],
    ['t is an array', '{"t":["video"],"state":"locked","w":1280,"h":720}', null],
    // state
    ['state unknown value', V('bogus', 1280, 720), null],
    ['state upper case', V('LOCKED', 1280, 720), null],
    ['state empty', V('', 0, 0), null],
    ['state missing', '{"t":"video","w":1280,"h":720}', null],
    ['state is a number', '{"t":"video","state":1,"w":1280,"h":720}', null],
    ['state is null', '{"t":"video","state":null,"w":1280,"h":720}', null],
    ['state is an array', '{"t":"video","state":["locked"],"w":1280,"h":720}', null],
    ['state with trailing space', V('locked ', 1280, 720), null],
    ['state "constructor"', V('constructor', 0, 0), null],
    ['state "__proto__"', V('__proto__', 0, 0), null],
    // w と h(0〜65535 の整数だけ)。locked 以外でも検査する。
    ['w fraction', '{"t":"video","state":"locked","w":1280.5,"h":720}', null],
    ['w string', '{"t":"video","state":"locked","w":"1280","h":720}', null],
    ['w negative', '{"t":"video","state":"locked","w":-1,"h":720}', null],
    ['w 65536', '{"t":"video","state":"locked","w":65536,"h":720}', null],
    ['w null', '{"t":"video","state":"locked","w":null,"h":720}', null],
    ['w true', '{"t":"video","state":"locked","w":true,"h":720}', null],
    ['w array', '{"t":"video","state":"locked","w":[1280],"h":720}', null],
    ['w object', '{"t":"video","state":"locked","w":{},"h":720}', null],
    ['w missing', '{"t":"video","state":"locked","h":720}', null],
    ['w huge 1e300', '{"t":"video","state":"locked","w":1e300,"h":720}', null],
    ['w 1e5', '{"t":"video","state":"locked","w":1e5,"h":720}', null],
    ['w 1280.0 is the integer 1280', '{"t":"video","state":"locked","w":1280.0,"h":720}', LOCKED(1280, 720)],
    ['h fraction', '{"t":"video","state":"locked","w":1280,"h":720.5}', null],
    ['h string', '{"t":"video","state":"locked","w":1280,"h":"720"}', null],
    ['h negative', '{"t":"video","state":"locked","w":1280,"h":-1}', null],
    ['h 65536', '{"t":"video","state":"locked","w":1280,"h":65536}', null],
    ['h null', '{"t":"video","state":"locked","w":1280,"h":null}', null],
    ['h missing', '{"t":"video","state":"locked","w":1280}', null],
    ['h huge', '{"t":"video","state":"locked","w":1280,"h":1e300}', null],
    ['no_signal w fraction', '{"t":"video","state":"no_signal","w":0.5,"h":0}', null],
    ['no_signal h negative', '{"t":"video","state":"no_signal","w":0,"h":-1}', null],
    ['no_signal w 65536', '{"t":"video","state":"no_signal","w":65536,"h":0}', null],
    ['no_signal h string', '{"t":"video","state":"no_signal","w":0,"h":"0"}', null],
    ['unknown w null', '{"t":"video","state":"unknown","w":null,"h":0}', null],
    ['unknown w missing', '{"t":"video","state":"unknown","h":0}', null],
    ['unknown h missing', '{"t":"video","state":"unknown","w":0}', null],
    ['no_signal without w and h', '{"t":"video","state":"no_signal"}', null],
    // 未知のキーは無視して採用する
    ['unknown keys are ignored', V('locked', 1280, 720, { fps: 60, extra: { a: [1, 2] }, name: 'x' }), LOCKED(1280, 720)],
    ['unknown key on no_signal', V('no_signal', 0, 0, { reason: 'cable' }), NOSIG],
    ['key __proto__ does not provide t', '{"__proto__":{"t":"video"},"state":"locked","w":1280,"h":720}', null],
    ['key __proto__ is ignored', '{"__proto__":{"a":1},"t":"video","state":"locked","w":1280,"h":720}', LOCKED(1280, 720)],
    ['duplicate key: the last one wins', '{"t":"video","state":"locked","w":1,"h":1,"w":1280,"h":720}', LOCKED(1280, 720)],
    // JSON として成り立たない、または、オブジェクトでない
    ['array', '[' + V('locked', 1280, 720) + ']', null],
    ['empty array', '[]', null],
    ['null', 'null', null],
    ['empty string', '', null],
    ['white space only', '  ', null],
    ['string literal', '"video"', null],
    ['number literal', '123', null],
    ['true', 'true', null],
    ['empty object', '{}', null],
    ['broken JSON: missing brace', '{"t":"video","state":"locked","w":1280,"h":720', null],
    ['broken JSON: unquoted keys', '{t:"video",state:"locked",w:1280,h:720}', null],
    ['broken JSON: single quotes', "{'t':'video','state':'locked','w':1280,'h':720}", null],
    ['broken JSON: trailing comma', '{"t":"video","state":"locked","w":1280,"h":720,}', null],
    ['broken JSON: trailing garbage', V('locked', 1280, 720) + 'x', null],
    ['broken JSON: two objects', V('locked', 1280, 720) + V('locked', 1280, 720), null],
    ['not JSON at all', 'hello', null],
    ['JPEG magic as text', 'ÿØÿà', null],
    ['NUL', '\u0000', null],
    // 長さ(契約は 128 バイト以下。余裕を見て 1024 まで)
    ['length 1024 is accepted', padded(1024), LOCKED(1280, 720)],
    ['length 1025 is dropped', padded(1025), null],
    ['length 2000 (valid JSON) is dropped', padded(2000), null],
    ['1 MB of garbage', 'a'.repeat(1 << 20), null],
    ['1 MB valid JSON', padded(1 << 20), null],
    ['deeply nested', '['.repeat(100000) + ']'.repeat(100000), null],
    // 文字列でない入力(app.js は typeof で分けるが、純関数としても安全であること)
    ['input undefined', undefined, null],
    ['input object without text', { raw: null }, null],
    ['input number', 5, null],
    ['input object', { t: 'video', state: 'locked', w: 1280, h: 720 }, null],
    ['input array holding the JSON text', [V('locked', 1280, 720)], null],
    ['input object whose toString gives the JSON', { toString() { return V('locked', 1280, 720); }, length: 5 }, null],
    ['input boolean', true, null],
];

const LABEL_CASES = [[LOCKED(1280, 720), '1280x720'], [LOCKED(1, 1), '1x1'], [LOCKED(65535, 65535), '65535x65535'], [LOCKED(1920, 1080), '1920x1080']];
const STATUS_LINE_CASES = [
    [LOCKED(1280, 720), '接続完了 / 受信中 (1280x720)'],
    [LOCKED(1920, 1080), '接続完了 / 受信中 (1920x1080)'],
    [NOSIG, '接続完了 / 受信中'],
    [UNKNOWN, '接続完了 / 受信中'],
];
const OVERLAY_CASES = [['no_signal', '信号なし'], ['unknown', '映像の状態を確認中…'], ['locked', null], ['bogus', null], [undefined, null], ['', null]];
// [名前, prev, cur, 期待]
const CHANGE_CASES = [
    ['first status (no previous)', null, LOCKED(1280, 720), false],
    ['same resolution', LOCKED(1280, 720), LOCKED(1280, 720), false],
    ['another resolution', LOCKED(1280, 720), LOCKED(1920, 1080), true],
    ['only w differs', LOCKED(1280, 720), LOCKED(1920, 720), true],
    ['only h differs', LOCKED(1280, 720), LOCKED(1280, 1024), true],
    ['no_signal -> locked', NOSIG, LOCKED(1280, 720), false],
    ['unknown -> locked', UNKNOWN, LOCKED(1280, 720), false],
    ['locked -> no_signal', LOCKED(1280, 720), NOSIG, false],
    ['locked -> unknown', LOCKED(1280, 720), UNKNOWN, false],
    ['no_signal -> no_signal', NOSIG, NOSIG, false],
    ['null -> no_signal', null, NOSIG, false],
];

function checkVideoStatusPure(source) {
    const problems = [];
    let vs;
    try { vs = loadVideoStatus(source); } catch (e) { return [String(e.message)]; }
    for (const [name, input, want] of PARSE_CASES) {
        let got;
        try { got = vs.parse(input); } catch (e) { problems.push('parse ' + name + ': threw ' + e.message); continue; }
        if (JSON.stringify(got) !== JSON.stringify(want)) problems.push('parse ' + name + ': expected ' + JSON.stringify(want) + ', got ' + JSON.stringify(got));
    }
    for (const [st, want] of LABEL_CASES) {
        const got = vs.resolutionLabel(st);
        if (got !== want) problems.push('resolutionLabel ' + JSON.stringify(st) + ': expected ' + want + ', got ' + got);
    }
    for (const [st, want] of STATUS_LINE_CASES) {
        const got = vs.statusLine(st);
        if (got !== want) problems.push('statusLine ' + JSON.stringify(st) + ': expected ' + want + ', got ' + got);
    }
    for (const [state, want] of OVERLAY_CASES) {
        const got = vs.overlayMessage(state);
        if (got !== want) problems.push('overlayMessage ' + state + ': expected ' + want + ', got ' + got);
    }
    for (const [name, prev, cur, want] of CHANGE_CASES) {
        let got;
        try { got = vs.isResolutionChange(prev, cur); } catch (e) { problems.push('isResolutionChange ' + name + ': threw ' + e.message); continue; }
        if (got !== want) problems.push('isResolutionChange ' + name + ': expected ' + want + ', got ' + got);
    }
    const n = vs.changeNotice(LOCKED(1920, 1080));
    if (n !== '解像度が変わりました（1920x1080）') problems.push('changeNotice: got ' + n);
    return problems;
}

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
    return problems.concat(checkVideoStatusPure(source));
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
    const listeners = {};
    const ctx = { calls: [] };
    const ctxProxy = new Proxy(ctx, {
        get(t, p) { return p === 'calls' ? t.calls : (...a) => { t.calls.push(String(p)); }; },
    });
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
        addEventListener(type, fn) { listeners[type] = fn; },
        _listeners: listeners,
        // 2D コンテキスト: どのメソッドが呼ばれたかを記録する(clearRect などで絵を消していないことを調べる)。
        getContext() { return ctxProxy; },
        getBoundingClientRect() { return { left: 0, top: 0, width: 1, height: 1 }; },
        textContent: '', value: '', width: 1280, height: 720, disabled: false,
        style: {},
        _classes: classes,
        _ctx: ctx, // calls: 呼ばれたメソッド名の列
        _sets: [], // 代入されたプロパティの名前(innerHTML に代入していないことを調べる)
    };
    return new Proxy(el, {
        get(t, p) { return p in t ? t[p] : function () {}; },
        set(t, p, v) { t._sets.push(String(p)); t[p] = v; return true; },
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
        // 偽のフレーム: { w, h } は絵の大きさ、pending は解けるまで復号が終わらない Promise
        createImageBitmap: async (d) => {
            if (d && d.pending) await d.pending;
            return { width: (d && d.w) || 1, height: (d && d.h) || 1, close() {} };
        },
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
        // 映像の状態の表示(重ねる表示 #video-overlay と、ステータス行 #video-status)
        overlayHidden() { return elements['video-overlay']._classes.has('video-overlay-hidden'); },
        overlayText() { return elements['video-overlay'].textContent; },
        statusText() { return elements['video-status'].textContent; },
        canvas() { return elements['video']; },
        // 2 秒の「解像度が変わりました」のタイマー(再接続のタイマーは 1000 ms から始まるので、混ざらない場面で使う)
        noticeTimers() { return timers.filter((t) => t.delay === 2000); },
        runTimer(delay) {
            const t = timers.find((x) => x.delay === delay);
            if (!t) return false;
            timers.splice(timers.indexOf(t), 1);
            t.fn();
            return true;
        },
        // 映像の WebSocket にメッセージを届ける。テキストは文字列、バイナリは偽のフレームのオブジェクト。
        async msg(data) { await h.lastVideo().onmessage({ data }); },
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

    // ---- 映像の状態の表示(テキストフレーム。docs/WEB_API.md 5) ----
    const open = async () => {
        const st = { value: READY_AUTH({ video_port: 8444 }) };
        const h = await boot(source, L('kvm.local', 'kvm.local', ''), st);
        h.lastVideo().onopen();
        h.st = st;
        return h;
    };
    const frame = (w, hh) => ({ jpeg: true, w, h: hh });
    const draws = (h) => h.canvas()._ctx.calls.filter((c) => c === 'drawImage').length;
    // [重ねる表示が隠れているか, その文, ステータス行]
    const view = (h) => [h.overlayHidden(), h.overlayText(), h.statusText()];
    const PLAIN = '接続完了 / 受信中';
    const HIDDEN = [true, ''];

    // 11. index.html の要素
    if (!info['video-overlay']) problems.push('index.html lacks #video-overlay');
    else {
        if (!/\brole="status"/.test(info['video-overlay'].attrs)) problems.push('#video-overlay needs role="status"');
        if (!/\baria-live="polite"/.test(info['video-overlay'].attrs)) problems.push('#video-overlay needs aria-live="polite"');
        if (!info['video-overlay'].classes.includes('video-overlay-hidden')) problems.push('#video-overlay must start hidden');
    }
    // 12. 接続直後に locked: 重ねる表示なし、ステータス行に解像度
    await guard('status/locked first', async () => {
        const h = await open();
        eq('12 before any status', view(h), [true, '', PLAIN]);
        await h.msg(V('locked', 1280, 720));
        eq('12 locked', view(h), [true, '', PLAIN + ' (1280x720)']);
        eq('12 no notice timer', h.noticeTimers().length, 0);
        eq('12 binaryType is unchanged', h.lastVideo().binaryType, 'blob');
    });
    // 13. no_signal: 重ねる表示が出て、直前のフレームは残る。locked に戻ると消える
    await guard('status/no_signal', async () => {
        const h = await open();
        await h.msg(frame(640, 480));
        eq('13 first frame drawn', [draws(h), h.canvas().width, h.canvas().height], [1, 640, 480]);
        await h.msg(V('locked', 1280, 720));
        await h.msg(V('no_signal', 0, 0));
        eq('13 no_signal', view(h), [false, '信号なし', PLAIN]);
        eq('13 the last frame stays (nothing else drawn on the canvas)', h.canvas()._ctx.calls, ['drawImage']);
        eq('13 canvas size kept', [h.canvas().width, h.canvas().height], [640, 480]);
        await h.msg(V('locked', 1280, 720));
        eq('13 back to locked', view(h), [true, '', PLAIN + ' (1280x720)']);
        eq('13 no notice for no_signal -> locked', h.noticeTimers().length, 0);
        await h.msg(frame(1280, 720));
        eq('13 frames are drawn again', draws(h), 2);
    });
    // 14. unknown
    await guard('status/unknown', async () => {
        const h = await open();
        await h.msg(V('unknown', 0, 0));
        eq('14 unknown', view(h), [false, '映像の状態を確認中…', PLAIN]);
        await h.msg(V('locked', 1920, 1080));
        eq('14 locked after unknown: no notice', view(h), [true, '', PLAIN + ' (1920x1080)']);
        await h.msg(V('unknown', 0, 0));
        eq('14 unknown after locked: no resolution in the line', view(h), [false, '映像の状態を確認中…', PLAIN]);
        await h.msg(V('locked', 65535, 65535));
        eq('14 highest resolution', h.statusText(), PLAIN + ' (65535x65535)');
    });
    // 15. 解像度の変更: 2 秒だけ表示する
    await guard('status/resolution change', async () => {
        const h = await open();
        await h.msg(V('locked', 1280, 720));
        eq('15 first locked', h.overlayHidden(), true);
        await h.msg(V('locked', 1920, 1080));
        eq('15 notice', view(h), [false, '解像度が変わりました（1920x1080）', PLAIN + ' (1920x1080)']);
        eq('15 one 2000 ms timer', h.noticeTimers().map((t) => t.delay), [2000]);
        eq('15 still shown before the timer fires', h.overlayHidden(), false);
        eq('15 timer ran', h.runTimer(2000), true);
        eq('15 hidden after 2 s', view(h), [true, '', PLAIN + ' (1920x1080)']);
        eq('15 timer gone', h.noticeTimers().length, 0);
        await h.msg(V('locked', 1280, 720));
        eq('15 a second change shows again', view(h), [false, '解像度が変わりました（1280x720）', PLAIN + ' (1280x720)']);
        eq('15 timer again', h.noticeTimers().length, 1);
        await h.msg(V('locked', 1280, 720));
        eq('15 same resolution again: no notice', [h.overlayHidden(), h.noticeTimers().length], [true, 0]);
    });
    // 16. 変更の表示の最中に別の状態が来ると、すぐ消える(タイマーも消える)
    const afterChange = async () => {
        const h = await open();
        await h.msg(V('locked', 1280, 720));
        await h.msg(V('locked', 1920, 1080));
        return h;
    };
    await guard('status/notice interrupted by no_signal', async () => {
        const h = await afterChange();
        eq('16a shown', h.overlayText(), '解像度が変わりました（1920x1080）');
        await h.msg(V('no_signal', 0, 0));
        eq('16a no_signal replaces it', view(h), [false, '信号なし', PLAIN]);
        eq('16a its timer is gone', h.noticeTimers().length, 0);
        eq('16a no_signal -> locked (another resolution): no notice', (await h.msg(V('locked', 1280, 720)), view(h)), [true, '', PLAIN + ' (1280x720)']);
    });
    await guard('status/notice interrupted by unknown', async () => {
        const h = await afterChange();
        await h.msg(V('unknown', 0, 0));
        eq('16b unknown replaces it', view(h), [false, '映像の状態を確認中…', PLAIN]);
        eq('16b its timer is gone', h.noticeTimers().length, 0);
    });
    await guard('status/notice interrupted by the same locked', async () => {
        const h = await afterChange();
        await h.msg(V('locked', 1920, 1080));
        eq('16c hidden at once', [h.overlayHidden(), h.overlayText()], HIDDEN);
        eq('16c its timer is gone', h.noticeTimers().length, 0);
    });
    await guard('status/notice interrupted by another change', async () => {
        const h = await afterChange();
        await h.msg(V('locked', 1024, 768));
        eq('16d new notice', h.overlayText(), '解像度が変わりました（1024x768）');
        eq('16d only one timer', h.noticeTimers().length, 1);
        // 古いタイマーが残っていたら、ここで新しい表示を消してしまう
        eq('16d timer ran', h.runTimer(2000), true);
        eq('16d hidden', h.overlayHidden(), true);
    });
    // 17. 切断・再接続・ログアウトで消える
    await guard('status/reset on close', async () => {
        const h = await afterChange();
        await h.closeVideo(false);
        eq('17a overlay hidden on close', [h.overlayHidden(), h.overlayText()], HIDDEN);
        eq('17a notice timer cleared', h.noticeTimers().length, 0);
        eq('17a status line', h.statusText(), '切断されました。再接続します...');
        eq('17a reconnect timer', h.timers.map((t) => t.delay), [1000]);
        h.runTimer(1000);
        await flush();
        h.lastVideo().onopen();
        // 前の接続の解像度(1920x1080)を覚えていたら、ここで変更の表示が出てしまう
        await h.msg(V('locked', 1280, 720));
        eq('17a new connection starts clean', view(h), [true, '', PLAIN + ' (1280x720)']);
    });
    await guard('status/reset on close (no_signal)', async () => {
        const h = await open();
        await h.msg(V('no_signal', 0, 0));
        await h.closeVideo(true);
        eq('17b no stale no_signal after a reconnect', [h.overlayHidden(), h.overlayText()], HIDDEN);
    });
    await guard('status/reset on logout button', async () => {
        const h = await open();
        await h.msg(V('locked', 1280, 720));
        await h.msg(V('locked', 1920, 1080));
        h.st.value = { state: 'ready', authenticated: false };
        h.elements['btn-logout']._listeners.click();
        await flush();
        await flush();
        eq('17c video socket closed', h.lastVideo().readyState, 3);
        eq('17c overlay hidden', [h.overlayHidden(), h.overlayText()], HIDDEN);
        eq('17c notice timer cleared', h.noticeTimers().length, 0);
        eq('17c status line', h.statusText(), '切断されました');
    });
    await guard('status/reset on logout with no_signal', async () => {
        const h = await open();
        await h.msg(V('no_signal', 0, 0));
        h.st.value = { state: 'ready', authenticated: false };
        h.elements['btn-logout']._listeners.click();
        await flush();
        await flush();
        eq('17d overlay hidden', [h.overlayHidden(), h.overlayText()], HIDDEN);
    });
    // 18. バイナリのフレームは、状態の表示に影響しない
    await guard('status/binary does not touch the status', async () => {
        const h = await open();
        await h.msg(V('no_signal', 0, 0));
        await h.msg(frame(640, 480));
        eq('18a no_signal stays', view(h), [false, '信号なし', PLAIN]);
        eq('18a frame drawn', draws(h), 1);
        await h.msg(V('locked', 1280, 720));
        await h.msg(V('locked', 1920, 1080));
        await h.msg(frame(1920, 1080));
        eq('18b notice stays', view(h), [false, '解像度が変わりました（1920x1080）', PLAIN + ' (1920x1080)']);
        eq('18b its timer stays', h.noticeTimers().length, 1);
        eq('18b frames drawn', draws(h), 2);
        // ArrayBuffer / Blob のような別の種類のバイナリも、状態にしない
        await h.msg(new ArrayBuffer(8));
        eq('18c binary of another kind: still a frame', [draws(h), h.overlayText()], [3, '解像度が変わりました（1920x1080）']);
    });
    // 19. 不正なテキストは捨てる。表示も、バイナリの処理も変わらない
    await guard('status/bad text is ignored', async () => {
        const h = await open();
        await h.msg(frame(640, 480));
        await h.msg(V('locked', 1280, 720));
        await h.msg(V('locked', 1920, 1080));
        const before = view(h);
        const bad = ['', 'x', '{', 'null', '[]', '"video"', '123', 'true', '{}', V('locked', 1280, 720).replace('"video"', '"audio"'),
            V('bogus', 1, 1), V('locked', 0, 0), V('locked', -1, 5), V('locked', 1.5, 5), V('no_signal', 1.5, 0),
            '{"t":"video","state":"locked","w":"1280","h":720}', 'a'.repeat(5000), padded(2000), '[' + V('locked', 1, 1) + ']'];
        for (const b of bad) {
            await h.msg(b);
            eq('19 unchanged by ' + JSON.stringify(b.slice(0, 40)), view(h), before);
        }
        eq('19 notice timer kept', h.noticeTimers().length, 1);
        eq('19 text frames are not drawn', draws(h), 1);
        await h.msg(frame(800, 600));
        eq('19 the next frame is still drawn', [draws(h), h.canvas().width, h.canvas().height], [2, 800, 600]);
        // 不正なテキストが 1 つ目のとき(状態がまだ無い)
        const h2 = await open();
        await h2.msg('garbage');
        await h2.msg(frame(320, 240));
        eq('19 bad text first: no overlay, frame drawn', [view(h2), draws(h2)], [[true, '', PLAIN], 1]);
    });
    // 20. テキストは、復号の途中でも処理される(復号の重複防止の対象ではない)
    await guard('status/text during decode', async () => {
        const h = await open();
        let release;
        const pending = new Promise((r) => { release = r; });
        const decoding = h.msg({ jpeg: true, w: 320, h: 240, pending });
        await flush();
        await h.msg(V('no_signal', 0, 0));
        eq('20 status applied while a frame is decoding', view(h), [false, '信号なし', PLAIN]);
        await h.msg(frame(10, 10)); // 復号中のフレームは捨てる(既存の挙動)
        release();
        await decoding;
        eq('20 the decoding frame finishes', [draws(h), h.canvas().width], [1, 320]);
        await h.msg(frame(640, 480));
        eq('20 decoding works again afterwards', draws(h), 2);
    });
    // 21. innerHTML などへの代入をしない。表示は textContent
    await guard('status/textContent only', async () => {
        const h = await open();
        for (const t of [V('locked', 1280, 720), V('locked', 1920, 1080), V('no_signal', 0, 0), V('unknown', 0, 0)]) await h.msg(t);
        for (const [id, el] of Object.entries(h.elements)) {
            const bad = el._sets.filter((n) => /^(innerHTML|outerHTML|insertAdjacentHTML)$/.test(n));
            if (bad.length) problems.push('21 #' + id + ' was assigned ' + bad.join(','));
        }
        eq('21 overlay set through textContent', h.elements['video-overlay']._sets.includes('textContent'), true);
    });
    // 22. 入力用の WebSocket(/ws)は、映像のテキストフレームに影響されない
    await guard('status/input socket untouched', async () => {
        const h = await open();
        const inputBefore = h.elements['input-status'].textContent;
        const inputs = h.sockets.filter((s) => /\/ws$/.test(s.url));
        eq('22 one input socket', inputs.length, 1);
        await h.msg(V('no_signal', 0, 0));
        eq('22 no new sockets', h.sockets.length, 2);
        eq('22 input status text untouched', h.elements['input-status'].textContent, inputBefore);
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
            "typeof v === 'number' && Number.isInteger(v) && v >= 1", 'Number.isInteger(v) && v >= 1',
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
        M('J57', 'disconnect: hint not hidden on the login view', "        hideTrustHint();\n        resetVideoStatus();\n        elVideoStatus.textContent = '切断されました';", "        resetVideoStatus();\n        elVideoStatus.textContent = '切断されました';"),
        M('J58', 'backoff: first delay changed', 'let wsReconnectDelay = 1000;', 'let wsReconnectDelay = 500;'),
        M('J59', 'backoff: cap 10000 -> 20000', 'wsReconnectDelay = Math.min(wsReconnectDelay * 2, 10000);\n        };\n\n        ws.onerror', 'wsReconnectDelay = Math.min(wsReconnectDelay * 2, 20000);\n        };\n\n        ws.onerror'),
        M('J60', 'backoff: reset on open removed', "            wsReconnectDelay = 1000; // reset\n        };\n\n        ws.onmessage", "        };\n\n        ws.onmessage"),
        // ---- 映像の状態(C-6b): VideoStatus と、その表示 ----
        M('V01', 'validDimension: Number.isInteger removed', "typeof v === 'number' && Number.isInteger(v) && v >= 0", "typeof v === 'number' && v >= 0"),
        M('V02', 'validDimension: lower bound >= 0 -> >= 1', 'v >= 0 && v <= 65535', 'v >= 1 && v <= 65535'),
        M('V03', 'validDimension: lower bound >= 0 -> >= -1', 'v >= 0 && v <= 65535', 'v >= -1 && v <= 65535'),
        M('V04', 'validDimension: upper bound <= 65535 -> < 65535', 'v >= 0 && v <= 65535', 'v >= 0 && v < 65535'),
        M('V05', 'validDimension: upper bound <= 65535 -> <= 65536', 'v >= 0 && v <= 65535', 'v >= 0 && v <= 65536'),
        M('V06', 'validDimension: upper bound removed', 'v >= 0 && v <= 65535', 'v >= 0'),
        M('V07', 'validDimension: typeof check removed (Number.isInteger already rejects every non-number)',
            "typeof v === 'number' && Number.isInteger(v) && v >= 0", 'Number.isInteger(v) && v >= 0',
            'Number.isInteger は数値以外(文字列、真偽値、配列、オブジェクト、null)を常に false にするので、typeof は二重の防御'),
        M('V08', 'parse: length cap 1024 -> 1025', 'text.length > 1024', 'text.length > 1025'),
        M('V09', 'parse: length cap 1024 -> 1023', 'text.length > 1024', 'text.length > 1023'),
        M('V10', 'parse: length cap removed', ' || text.length > 1024) return null;', ') return null;'),
        M('V11', 'parse: empty-text check removed (JSON.parse of "" throws, so it is dropped anyway)', ' || text.length === 0 ||', ' ||',
            '空文字は JSON.parse が例外を投げ、catch が null を返すので、明示の検査がなくても捨てられる(二重の防御)'),
        M('V12', 'parse: typeof text check removed', "typeof text !== 'string' || text.length === 0", 'text.length === 0'),
        M('V13', 'parse: broken JSON is treated as "unknown" instead of dropped', '} catch (err) {\n                return null;', "} catch (err) {\n                return { state: 'unknown', w: 0, h: 0 };"),
        M('V14', 'parse: null check removed', 'obj === null || typeof obj', 'typeof obj'),
        M('V15', 'parse: typeof object check removed (a primitive has no t, so the t check drops it)', "typeof obj !== 'object' || Array.isArray(obj)", 'Array.isArray(obj)',
            'JSON の数・文字列・真偽値は t を持たないので、後ろの t の検査が捨てる(二重の防御)'),
        M('V16', 'parse: array check removed (an array has no t, so the t check drops it)', "typeof obj !== 'object' || Array.isArray(obj)", "typeof obj !== 'object'",
            'JSON の配列は t を持たないので、後ろの t の検査が捨てる(二重の防御)'),
        M('V17', 'parse: t check removed', "            if (obj.t !== 'video') return null;\n", ''),
        M('V18', 'parse: t compared with the wrong string', "obj.t !== 'video'", "obj.t !== 'audio'"),
        M('V19', 'parse: state check removed', "            if (state !== 'unknown' && state !== 'no_signal' && state !== 'locked') return null;\n", ''),
        M('V20', 'parse: state "unknown" not accepted', "state !== 'unknown' && state !== 'no_signal'", "state !== 'no_signal'"),
        M('V21', 'parse: state "no_signal" not accepted', "state !== 'unknown' && state !== 'no_signal' && state !== 'locked'", "state !== 'unknown' && state !== 'locked'"),
        M('V22', 'parse: w is not checked', '!this.validDimension(obj.w) || !this.validDimension(obj.h)', '!this.validDimension(obj.h)'),
        M('V23', 'parse: h is not checked', '!this.validDimension(obj.w) || !this.validDimension(obj.h)', '!this.validDimension(obj.w)'),
        M('V24', 'parse: locked with w = 0 is accepted', 'obj.w < 1 || obj.h < 1', 'obj.h < 1'),
        M('V25', 'parse: locked with h = 0 is accepted', 'obj.w < 1 || obj.h < 1', 'obj.w < 1'),
        M('V26', 'parse: locked with 0 is accepted', "                if (obj.w < 1 || obj.h < 1) return null;\n", ''),
        M('V27', 'parse: locked w and h swapped', 'return { state: state, w: obj.w, h: obj.h };', 'return { state: state, w: obj.h, h: obj.w };'),
        M('V28', 'parse: w, h of a state other than locked are kept (not made 0)', 'return { state: state, w: 0, h: 0 };', 'return { state: state, w: obj.w, h: obj.h };'),
        M('V29', 'resolutionLabel: separator changed', "+ 'x' + String(Math.trunc(st.h))", "+ 'X' + String(Math.trunc(st.h))"),
        M('V30', 'resolutionLabel: Math.trunc removed (w and h are integers already)', 'String(Math.trunc(st.w))', 'String(st.w)',
            'parse が w・h を整数に限っているので、Math.trunc は結果を変えない(念のための整数化)'),
        M('V31', 'statusLine: resolution shown for every state', "if (st.state === 'locked') return '接続完了", "if (st.state !== 'unknown') return '接続完了"),
        M('V32', 'statusLine: resolution never shown', "if (st.state === 'locked') return '接続完了", "if (st.state === 'never') return '接続完了"),
        M('V33', 'statusLine: plain line text changed', "            return '接続完了 / 受信中';\n        },", "            return '接続中';\n        },"),
        M('V34', 'overlayMessage: no_signal text changed', "return '信号なし';", "return '信号がありません';"),
        M('V35', 'overlayMessage: unknown text changed', "return '映像の状態を確認中…';", "return '確認中';"),
        M('V36', 'overlayMessage: no_signal shows nothing', "if (state === 'no_signal') return '信号なし';", "if (state === 'no_signal') return null;"),
        M('V37', 'overlayMessage: locked shows a text', "            return null;\n        },\n        // locked -> locked", "            return '映像あり';\n        },\n        // locked -> locked"),
        M('V38', 'isResolutionChange: null prev not handled', 'return prev !== null && prev.state', 'return prev.state'),
        M('V39', 'isResolutionChange: prev state not checked', "prev.state === 'locked' && cur.state", 'cur.state'),
        M('V40', 'isResolutionChange: cur state not checked', "cur.state === 'locked' && (prev.w", '(prev.w'),
        M('V41', 'isResolutionChange: w and h must both differ', 'prev.w !== cur.w || prev.h !== cur.h', 'prev.w !== cur.w && prev.h !== cur.h'),
        M('V42', 'isResolutionChange: only w compared', 'prev.w !== cur.w || prev.h !== cur.h', 'prev.w !== cur.w'),
        M('V43', 'changeNotice: text changed', "return '解像度が変わりました（'", "return '解像度が変更されました（'"),
        M('V44', 'notice duration 2000 -> 3000 ms', 'const RESOLUTION_NOTICE_MS = 2000;', 'const RESOLUTION_NOTICE_MS = 3000;'),
        M('V45', 'setVideoOverlay: hide adds no hidden class', "elVideoOverlay.classList.add('video-overlay-hidden');", "elVideoOverlay.classList.remove('video-overlay-hidden');"),
        M('V46', 'setVideoOverlay: hide leaves the old text', "            elVideoOverlay.textContent = '';\n", ''),
        M('V47', 'setVideoOverlay: show does not unhide', "            elVideoOverlay.classList.remove('video-overlay-hidden');\n", ''),
        M('V48', 'setVideoOverlay: text goes through innerHTML', 'elVideoOverlay.textContent = text;', 'elVideoOverlay.innerHTML = text;'),
        M('V49', 'resetVideoStatus: timer not cleared', "        if (resolutionNoticeTimer !== null) {\n            clearTimeout(resolutionNoticeTimer);\n            resolutionNoticeTimer = null;\n        }\n        videoStatus = null;",
            '        videoStatus = null;'),
        M('V50', 'resetVideoStatus: last status not forgotten', '        videoStatus = null;\n        setVideoOverlay(null);\n    }', '        setVideoOverlay(null);\n    }'),
        M('V51', 'resetVideoStatus: overlay not hidden', '        videoStatus = null;\n        setVideoOverlay(null);', '        videoStatus = null;'),
        M('V52', 'applyVideoStatus: last status not remembered', '        videoStatus = st;\n', ''),
        M('V53', 'applyVideoStatus: running notice not cancelled by the next status',
            '        // The next status removes the "resolution changed" notice at once.\n        if (resolutionNoticeTimer !== null) {\n            clearTimeout(resolutionNoticeTimer);\n            resolutionNoticeTimer = null;\n        }\n',
            '        // The next status removes the "resolution changed" notice at once.\n'),
        M('V54', 'applyVideoStatus: status line not updated', '        elVideoStatus.textContent = VideoStatus.statusLine(st);\n', ''),
        M('V55', 'applyVideoStatus: change notice not shown', 'setVideoOverlay(VideoStatus.changeNotice(st));', 'setVideoOverlay(VideoStatus.overlayMessage(st.state));'),
        M('V56', 'applyVideoStatus: notice never expires (timer not started)', '            resolutionNoticeTimer = setTimeout(() => {', '            void (() => {'),
        M('V57', 'applyVideoStatus: timer does not hide the notice', '                resolutionNoticeTimer = null;\n                setVideoOverlay(null);\n            }, RESOLUTION_NOTICE_MS);', '                resolutionNoticeTimer = null;\n            }, RESOLUTION_NOTICE_MS);'),
        M('V58', 'applyVideoStatus: timer does not forget itself (clearTimeout of a fired id is harmless)', '                resolutionNoticeTimer = null;\n                setVideoOverlay(null);', '                setVideoOverlay(null);',
            '実行済みのタイマーの id が残っても、次の clearTimeout は何もしない(結果は同じ。次の状態で `!== null` の枝を通るだけ)'),
        M('V59', 'applyVideoStatus: states other than a change show no overlay', 'setVideoOverlay(VideoStatus.overlayMessage(st.state));\n        }\n    }', 'setVideoOverlay(null);\n        }\n    }'),
        M('V60', 'applyVideoStatus: every locked is shown as a change', 'const changed = VideoStatus.isResolutionChange(videoStatus, st);', "const changed = st.state === 'locked';"),
        M('V61', 'onmessage: text frames are treated as binary', "if (typeof e.data === 'string') {", "if (typeof e.data === 'number') {"),
        M('V62', 'onmessage: a text frame also goes to the decoder', "                if (st !== null) applyVideoStatus(st);\n                return;\n", "                if (st !== null) applyVideoStatus(st);\n"),
        M('V63', 'onmessage: a text that does not parse is applied', 'if (st !== null) applyVideoStatus(st);', 'applyVideoStatus(st);'),
        M('V64', 'onmessage: text is handled only when no frame is decoding', "            if (typeof e.data === 'string') {", "            if (isDecoding) return;\n            if (typeof e.data === 'string') {"),
        M('V65', 'onmessage: a text frame is not handled (dropped)', 'if (st !== null) applyVideoStatus(st);', 'if (st === null) applyVideoStatus(st);'),
        M('V66', 'onclose: video status not reset', '            ws = null;\n            resetVideoStatus();\n', '            ws = null;\n'),
        M('V67', 'disconnect: video status not reset', '        hideTrustHint();\n        resetVideoStatus();\n', '        hideTrustHint();\n'),
        M('V68', 'connect: reset at the start of a connection removed (onclose and disconnect already reset)', '        if (ws) return;\n        resetVideoStatus();\n', '        if (ws) return;\n',
            '接続の前には、必ず onclose か disconnectWebSocket が先に走って状態を消している(最初の接続では、もともと空)ので、二重の防御'),
        M('V69', 'connect: binaryType changed', 'ws.binaryType = "blob";', 'ws.binaryType = "arraybuffer";'),
    ];
}

(async () => {
    const source = fs.readFileSync(APP_JS, 'utf8');
    let status = 0;
    const problems = await checkAll(source);
    problems.forEach((p) => console.log('FAIL:', p));
    if (problems.length) status = 1;
    else console.log('video viewer ok (' + RESOLVE_CASES.length + ' resolve cases, ' + HINT_CASES.length + ' hint cases, ' + TRUST_URL_CASES.length + ' trust-url cases, ' + PARSE_CASES.length + ' status-parse cases, ' + CHANGE_CASES.length + ' resolution-change cases, browser scenarios)');

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
