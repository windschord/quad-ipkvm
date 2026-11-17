#!/usr/bin/env node
// ビューア(components/web_server/web/app.js)の、映像の接続先と「証明書の案内」の検査(docs/VIDEO_TRANSPORT.md 5 章)。
//  (A) 純関数 VideoTarget(resolve / needsTrustHint / trustUrl)の表のテスト。期待値は手書き(実装から計算しない)。
//  (B) app.js 全体を、偽の DOM / WebSocket / fetch / タイマーの中で動かし、状態の応答 -> 接続の URL -> 失敗 ->
//      案内の表示・消去、再接続のバックオフ(既存の挙動)を観察する。index.html の id と class も、実際のファイルから読む。
//  (C) 映像の状態(WebSocket のテキストフレーム。docs/WEB_API.md 5、docs/VIDEO_TRANSPORT.md 8 章)の純関数
//      VideoStatus(parse など)の表のテストと、(B) と同じ偽の環境での表示のシナリオ(重ねる表示、2 秒のタイマー、リセット)。
//  (D) H.264(WebCodecs。docs/VIDEO_TRANSPORT.md 9.2〜9.4、9.8): 純関数 H264(classify / parseAnnexB / analyze / avcCodecStringFromSps)と
//      VideoTarget.withAccept の表のテスト、偽の VideoDecoder / EncodedVideoChunk / VideoFrame / Blob を入れた偽の環境での、
//      accept の申告、JPEG との判別、最初の IDR まで P を捨てる、解像度の変化での作り直し、decodeQueueSize、VideoFrame の close、
//      フォールバック(1 回だけ)、表示。
// `--mutations` は、app.js の追加分を 1 か所ずつ変えたコピーに対して (A)(B) を動かし、落ちることを確認する。
// 使い方: node tools/test/check_video_viewer.js [--mutations [ID ...]]      (依存なし。ID を付けると、そのミュータントだけ)
'use strict';
const fs = require('fs');
const path = require('path');
const vm = require('vm');

// 処理されない Promise の拒否(app.js の中の、どの catch にも届かない例外)は、失敗として数える。
const unhandled = [];
process.on('unhandledRejection', (e) => { unhandled.push(String(e && e.message)); });

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
    return problems.concat(checkVideoStatusPure(source), checkH264Pure(source));
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
    const ctx = { calls: [], args: [], failDraw: false };
    const ctxProxy = new Proxy(ctx, {
        get(t, p) {
            if (p === 'calls') return t.calls;
            if (p === 'args') return t.args; // [method, [arguments]] in call order
            if (p === 'failDraw') return t.failDraw;
            return (...a) => {
                if (t.failDraw && p === 'drawImage') throw new Error('drawImage failed');
                t.calls.push(String(p));
                t.args.push([String(p), a]);
            };
        },
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
async function boot(source, loc, statusRef, extraGlobals) {
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
            // an H.264-capable page wraps the JPEG bytes in a Blob (FakeBlob): the width is the third byte (FF D8 w h)
            const fromBlob = d && d.bytes ? d.bytes[2] : 0;
            return { width: (d && d.w) || fromBlob || 1, height: (d && d.h) || (d && d.bytes ? d.bytes[3] : 0) || 1, close() {} };
        },
        TextEncoder,
    };
    Object.assign(ctxObj, extraGlobals || {});
    const context = vm.createContext(ctxObj);
    vm.runInContext(source, context, { filename: 'app.js' });
    listeners.DOMContentLoaded();
    await flush();
    const h = {
        elements, timers, sockets,
        videoSockets() { return sockets.filter((s) => /\/video(\?accept=h264)?$/.test(s.url)); },
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

// ---------- (D) H.264(WebCodecs)。docs/VIDEO_TRANSPORT.md 9.2 / 9.3 / 9.4 / 9.8 ----------
// 実際のスタートコードと NAL ヘッダーを持つ、手で作ったバイト列。期待値は手書き(実装から計算しない)。
const hx = (...parts) => Uint8Array.from(parts.join(' ').trim().split(/\s+/).filter(Boolean).map((x) => parseInt(x, 16)));
const hexOf = (b) => Array.from(b, (v) => (v < 16 ? '0' : '') + v.toString(16)).join(' ');
const SC4 = '00 00 00 01';
const NAL_SPS720 = '67 42 c0 1f da 01 40 16 e8 06 d0 a1 35'; // 実機の SPS の先頭 4 バイト 67 42 c0 1f(以降は手書き)
const NAL_SPS1080 = '67 42 c0 29 da 01 e0 08 9f 96 10 00 00 03 00 10'; // 実機の 67 42 c0 29
const NAL_SPS_B = '67 42 c0 1f da 02 80 1e e8 06 d0 a1 35'; // NAL_SPS720 と同じ level(codec 文字列は同じ)で、中身(解像度)が違う
const NAL_PPS = '68 ce 3c 80';
const NAL_IDR = '65 88 84 00 33 ff';
const NAL_P = '41 9a 24 6c 41 ff';
const auHex = (...nals) => nals.map((n) => SC4 + ' ' + n).join(' ');
const AU_IDR720 = auHex(NAL_SPS720, NAL_PPS, NAL_IDR);
const AU_IDR1080 = auHex(NAL_SPS1080, NAL_PPS, NAL_IDR);
const AU_IDR_B = auHex(NAL_SPS_B, NAL_PPS, NAL_IDR);
const pAu = (n) => auHex('41 9a 24 ' + (n < 16 ? '0' : '') + n.toString(16));
const jpegBytes = (w, h) => Uint8Array.from([0xFF, 0xD8, w, h, 0xFF, 0xD9]);
const PROBE_CONFIG = { codec: 'avc1.42c01f', optimizeForLatency: true };
const LABEL = ' / H.264（低帯域）';

function loadH264(source) {
    const m = /const H264 = (\{[\s\S]*?\n    \});/.exec(source);
    if (!m) throw new Error('H264 not found in app.js');
    return new Function('return ' + m[1])();
}

// [名前, 16 進, 期待]
const CLASSIFY_CASES = [
    ['FF D8 only (2 bytes) is a JPEG', 'ff d8', 'jpeg'],
    ['JPEG with a body', 'ff d8 ff e0 00 10 4a 46 49 46', 'jpeg'],
    ['FF D8 FF', 'ff d8 ff', 'jpeg'],
    ['FF alone', 'ff', null],
    ['D8 FF (reversed)', 'd8 ff', null],
    ['FF D9', 'ff d9', null],
    ['FF D7', 'ff d7', null],
    ['FE D8', 'fe d8', null],
    ['empty', '', null],
    ['one zero', '00', null],
    ['00 00', '00 00', null],
    ['00 00 00 (3 bytes only)', '00 00 00', null],
    ['00 00 01 (3-byte start code)', '00 00 01', null],
    ['00 00 01 67 42 c0 1f (3-byte start code with a NAL)', '00 00 01 67 42 c0 1f', null],
    ['00 00 00 01 (exactly the start code) is H.264', '00 00 00 01', 'h264'],
    ['00 00 00 01 + SPS', '00 00 00 01 67 42 c0 1f', 'h264'],
    ['00 00 00 02', '00 00 00 02 67 42 c0 1f', null],
    ['00 00 00 00', '00 00 00 00 67', null],
    ['00 00 00 03', '00 00 00 03', null],
    ['00 00 01 01', '00 00 01 01', null],
    ['00 01 00 01', '00 01 00 01', null],
    ['01 00 00 00 01 (a byte before the start code)', '01 00 00 00 01', null],
    ['01 00 00 01', '01 00 00 01', null],
    ['00 00 00 01 is not a JPEG even with FF D8 later', '00 00 00 01 ff d8', 'h264'],
    ['GIF', '47 49 46 38 39 61', null],
    ['text "{"', '7b 22 74 22', null],
];

// [名前, 16 進, 期待: [[type, データの 16 進], ...]]
const PARSE_H264_CASES = [
    ['one NAL, 4-byte start code', '00 00 00 01 67 42 c0 1f', [[7, '67 42 c0 1f']]],
    ['one NAL, 3-byte start code', '00 00 01 67 42 c0 1f', [[7, '67 42 c0 1f']]],
    ['SPS + PPS + IDR', AU_IDR720, [[7, NAL_SPS720], [8, NAL_PPS], [5, NAL_IDR]]],
    ['P slice', auHex(NAL_P), [[1, NAL_P]]],
    ['3-byte and 4-byte start codes mixed', '00 00 00 01 67 42 c0 1f 00 00 01 68 ce 3c 80 00 00 01 65 88 84', [[7, '67 42 c0 1f'], [8, '68 ce 3c 80'], [5, '65 88 84']]],
    ['zero bytes before the next start code are not part of the NAL', '00 00 00 01 67 42 c0 1f 00 00 00 00 00 01 68 ce', [[7, '67 42 c0 1f'], [8, '68 ce']]],
    ['trailing zero bytes at the end', '00 00 00 01 67 42 c0 1f 00 00', [[7, '67 42 c0 1f']]],
    ['bytes before the first start code are ignored', 'aa bb 00 00 00 01 67 42 c0 1f', [[7, '67 42 c0 1f']]],
    ['no start code', '67 42 c0 1f', []],
    ['the start code alone', '00 00 00 01', []],
    ['the 3-byte start code alone', '00 00 01', []],
    ['empty', '', []],
    ['a 3-byte start code at the very end', '00 00 01 67 00 00 01', [[7, '67']]],
    ['two 3-byte start codes in a row', '00 00 01 00 00 01 67', [[7, '67']]],
    ['two start codes in a row (an empty NAL)', '00 00 00 01 00 00 00 01 67 42 c0 1f', [[7, '67 42 c0 1f']]],
    ['forbidden_zero_bit set: the NAL is dropped', '00 00 00 01 e7 42 c0 1f 00 00 00 01 68 ce', [[8, '68 ce']]],
    ['emulation prevention (00 00 03 01) is not a start code', '00 00 00 01 65 00 00 03 01 88', [[5, '65 00 00 03 01 88']]],
    ['a NAL that ends with 03 keeps it', '00 00 00 01 65 88 00 00 03 00 00 00 01 41 9a', [[5, '65 88 00 00 03'], [1, '41 9a']]],
    ['the type is the low 5 bits (nal_ref_idc 1)', '00 00 00 01 27 42 c0 1f', [[7, '27 42 c0 1f']]],
    ['the type is the low 5 bits (nal_ref_idc 3, IDR)', '00 00 00 01 65 88', [[5, '65 88']]],
    ['AUD + SEI + P', auHex('09 f0', '06 05 01 02', NAL_P), [[9, '09 f0'], [6, '06 05 01 02'], [1, NAL_P]]],
    ['a NAL of 1 byte', '00 00 00 01 09', [[9, '09']]],
    ['type 23 is not an SPS (the type is 5 bits, not 4)', '00 00 00 01 77 42 c0 1f', [[23, '77 42 c0 1f']]],
    ['00 00 00 00 only', '00 00 00 00', []],
    ['garbage without a start code', 'de ad be ef 00 01 02', []],
];

// [名前, 16 進, key か, SPS の 16 進(なければ null), NAL の数]
const ANALYZE_CASES = [
    ['SPS PPS IDR', AU_IDR720, true, NAL_SPS720, 3],
    ['1080p SPS PPS IDR', AU_IDR1080, true, NAL_SPS1080, 3],
    ['IDR slice only', auHex(NAL_IDR), true, null, 1],
    ['PPS + IDR slice', auHex(NAL_PPS, NAL_IDR), true, null, 2],
    ['P slice only', auHex(NAL_P), false, null, 1],
    ['SPS only', auHex(NAL_SPS720), true, NAL_SPS720, 1],
    ['PPS only', auHex(NAL_PPS), false, null, 1],
    ['SEI + P', auHex('06 05 01 02', NAL_P), false, null, 2],
    ['AUD + SPS + IDR', auHex('09 f0', NAL_SPS720, NAL_PPS, NAL_IDR), true, NAL_SPS720, 4],
    ['two SPS: the first one', auHex(NAL_SPS720, NAL_SPS1080, NAL_IDR), true, NAL_SPS720, 3],
    ['SPS after a P slice', auHex(NAL_P, NAL_SPS720), true, NAL_SPS720, 2],
    ['IDR with nal_ref_idc 0 (type 5 only)', auHex('05 88'), true, null, 1],
    ['slice data partitions (types 2, 3, 4)', auHex('42 aa', '43 aa', '44 aa'), false, null, 3],
    ['SEI only (type 6)', auHex('06 05 01 02'), false, null, 1],
    ['type 21 is not an IDR slice (the type is 5 bits, not 4)', auHex('75 88'), false, null, 1],
    ['type 23 is not an SPS', auHex('77 42 c0 1f'), false, null, 1],
    ['AUD only (type 9)', auHex('09 f0'), false, null, 1],
    ['empty', '', false, null, 0],
    ['garbage', 'de ad be ef', false, null, 0],
    ['SPS with forbidden_zero_bit set', auHex('e7 42 c0 1f'), false, null, 0],
    ['IDR with forbidden_zero_bit set', auHex('e5 88'), false, null, 0],
    ['the start code alone', '00 00 00 01', false, null, 0],
];

// [16 進(SPS の NAL。ヘッダーの 1 バイトから)、期待]
const CODEC_CASES = [
    ['67 42 c0 1f', 'avc1.42c01f'],
    ['67 42 c0 29', 'avc1.42c029'],
    ['67 64 00 1f', 'avc1.64001f'],
    ['67 4d 40 0a', 'avc1.4d400a'],
    ['67 58 e0 33', 'avc1.58e033'],
    ['67 00 00 00', 'avc1.000000'],
    ['67 ff ff ff', 'avc1.ffffff'],
    ['67 01 02 03', 'avc1.010203'],
    ['27 42 c0 1f', 'avc1.42c01f'],
    ['47 42 c0 1f', 'avc1.42c01f'],
    ['07 42 c0 1f', 'avc1.42c01f'],
    [NAL_SPS720, 'avc1.42c01f'],
    [NAL_SPS1080, 'avc1.42c029'],
    ['67 42 c0', null],
    ['67 42', null],
    ['67', null],
    ['', null],
    ['68 42 c0 1f', null],
    ['65 42 c0 1f', null],
    ['41 42 c0 1f', null],
    ['e7 42 c0 1f', null],
    ['87 42 c0 1f', null],
    ['27', null],
    ['67 0f 0f 0f', 'avc1.0f0f0f'],
    ['67 10 10 10', 'avc1.101010'],
    ['67 0a 0b 0c', 'avc1.0a0b0c'],
];

function checkH264Pure(source) {
    const problems = [];
    let h;
    try { h = loadH264(source); } catch (e) { return [String(e.message)]; }
    const safe = (name, fn) => { try { return fn(); } catch (e) { problems.push(name + ': threw ' + e.message); return undefined; } };
    for (const [name, hex, want] of CLASSIFY_CASES) {
        const got = safe('classify ' + name, () => h.classify(hx(hex)));
        if (got !== want) problems.push('classify ' + name + ': expected ' + want + ', got ' + got);
    }
    const strLen = (len, bytes) => Object.assign({ length: len, subarray() { return this; } }, bytes);
    for (const bad of [null, undefined, {}, { length: 'x' }, 5, strLen('4', [0, 0, 0, 1]), strLen('2', [0xFF, 0xD8])]) {
        const got = safe('classify ' + String(bad), () => h.classify(bad));
        if (got !== null) problems.push('classify of a non-array ' + String(bad) + ': expected null, got ' + got);
    }
    for (const [name, hex, want] of PARSE_H264_CASES) {
        const got = safe('parseAnnexB ' + name, () => h.parseAnnexB(hx(hex)).map((n) => [n.type, hexOf(n.data)]));
        if (got !== undefined && JSON.stringify(got) !== JSON.stringify(want)) problems.push('parseAnnexB ' + name + ': expected ' + JSON.stringify(want) + ', got ' + JSON.stringify(got));
    }
    for (const bad of [null, undefined, {}, { length: 'x' }, 5, { length: '8', 0: 0, 1: 0, 2: 0, 3: 1, 4: 0x67, 5: 0x42, 6: 0xc0, 7: 0x1f }]) {
        const got = safe('parseAnnexB ' + String(bad), () => h.parseAnnexB(bad));
        if (got !== undefined && JSON.stringify(got) !== '[]') problems.push('parseAnnexB of a non-array ' + String(bad) + ': expected [], got ' + JSON.stringify(got));
    }
    // 大きい入力でも、1 つの NAL として、すぐ終わる
    const big = new Uint8Array(1 << 20).fill(0xAA);
    big.set([0, 0, 0, 1, 0x65], 0);
    const t0 = Date.now();
    const bigNals = safe('parseAnnexB 1 MB', () => h.parseAnnexB(big));
    if (bigNals && (bigNals.length !== 1 || bigNals[0].type !== 5 || bigNals[0].data.length !== (1 << 20) - 4)) problems.push('parseAnnexB 1 MB: wrong result');
    if (Date.now() - t0 > 2000) problems.push('parseAnnexB 1 MB took ' + (Date.now() - t0) + ' ms');
    // 全部ゼロ(スタートコードだらけ)
    safe('parseAnnexB zeros', () => h.parseAnnexB(new Uint8Array(1 << 16)));
    for (const [name, hex, key, spsHex, count] of ANALYZE_CASES) {
        const got = safe('analyze ' + name, () => h.analyze(hx(hex)));
        if (got === undefined) continue;
        const gotSps = got.sps === null ? null : hexOf(got.sps);
        if (got.key !== key || gotSps !== spsHex || got.count !== count) problems.push('analyze ' + name + ': expected ' + JSON.stringify([key, spsHex, count]) + ', got ' + JSON.stringify([got.key, gotSps, got.count]));
        const k = safe('isKeyAccessUnit ' + name, () => h.isKeyAccessUnit(hx(hex)));
        if (k !== key) problems.push('isKeyAccessUnit ' + name + ': expected ' + key + ', got ' + k);
    }
    for (const [hex, want] of CODEC_CASES) {
        const got = safe('avcCodecStringFromSps ' + hex, () => h.avcCodecStringFromSps(hx(hex)));
        if (got !== want) problems.push('avcCodecStringFromSps ' + (hex || '(empty)') + ': expected ' + want + ', got ' + got);
    }
    for (const bad of [null, undefined, {}, { length: 'x' }, 5, { length: '4', 0: 0x67, 1: 0x42, 2: 0xc0, 3: 0x1f }]) {
        const got = safe('avcCodecStringFromSps ' + String(bad), () => h.avcCodecStringFromSps(bad));
        if (got !== null) problems.push('avcCodecStringFromSps of ' + String(bad) + ': expected null, got ' + got);
    }
    const same = [['67 42', '67 42', true], ['67 42', '67 43', false], ['67 42', '67 42 00', false], ['', '', true], ['67', '', false]];
    for (const [a, b, want] of same) {
        const got = safe('sameBytes', () => h.sameBytes(hx(a), hx(b)));
        if (got !== want) problems.push('sameBytes ' + JSON.stringify([a, b]) + ': expected ' + want + ', got ' + got);
    }
    if (safe('sameBytes null', () => h.sameBytes(null, hx('67'))) !== false) problems.push('sameBytes(null, x) must be false');
    if (safe('sameBytes null 2', () => h.sameBytes(hx('67'), null)) !== false) problems.push('sameBytes(x, null) must be false');
    // VideoTarget.withAccept
    let vt;
    try { vt = loadVideoTarget(source); } catch (e) { problems.push(String(e.message)); return problems; }
    const sep = { url: 'wss://kvm.local:8444/video', separate: true, port: 8444, hostname: 'kvm.local' };
    const old = { url: 'wss://kvm.local/video', separate: false, port: 0, hostname: '' };
    const snapshot = JSON.stringify(sep);
    const wa = [
        ['separate, accept', sep, true, { url: 'wss://kvm.local:8444/video?accept=h264', separate: true, port: 8444, hostname: 'kvm.local' }],
        ['legacy, accept', old, true, { url: 'wss://kvm.local/video?accept=h264', separate: false, port: 0, hostname: '' }],
        ['separate, no accept', sep, false, sep],
        ['legacy, no accept', old, false, old],
        ['accept undefined', old, undefined, old],
        ['accept "true" (a string)', old, 'true', old],
        ['accept 1', old, 1, old],
        ['accept null', old, null, old],
    ];
    for (const [name, target, accept, want] of wa) {
        const got = safe('withAccept ' + name, () => vt.withAccept(target, accept));
        if (got !== undefined && JSON.stringify(got) !== JSON.stringify(want)) problems.push('withAccept ' + name + ': expected ' + JSON.stringify(want) + ', got ' + JSON.stringify(got));
    }
    if (JSON.stringify(sep) !== snapshot) problems.push('withAccept changed its argument');
    return problems;
}

// ---- ブラウザ側(偽の VideoDecoder / EncodedVideoChunk / VideoFrame / Blob) ----
// opts.supported: (config) => boolean | Promise<boolean> (isConfigSupported の結果)。env.mode のフラグで、各所の例外を起こす。
function makeCodecs(opts) {
    const env = { decoders: [], chunks: [], probes: [], frames: [], blobs: [], mode: {}, supported: () => true, raw: undefined };
    Object.assign(env, opts || {});
    class FakeEncodedVideoChunk {
        constructor(init) {
            if (env.mode.chunkThrows) throw new Error('chunk');
            this.type = init.type;
            this.timestamp = init.timestamp;
            this.data = init.data;
            this.keys = Object.keys(init).sort();
            env.chunks.push(this);
        }
    }
    class FakeVideoDecoder {
        constructor(init) {
            if (env.mode.constructThrows) throw new Error('construct');
            this.output = init.output;
            this.error = init.error;
            this.configured = null;
            this.decoded = [];
            this.closed = false;
            this.decodeQueueSize = 0;
            env.decoders.push(this);
        }
        configure(cfg) {
            if (env.mode.configureThrows) throw new Error('configure');
            this.configured = JSON.parse(JSON.stringify(cfg));
        }
        decode(chunk) {
            if (env.mode.decodeThrows) throw new Error('decode');
            this.decoded.push(chunk);
        }
        close() { this.closed = true; if (env.mode.closeThrows) throw new Error('close'); }
        emit(frame) { this.output(frame); }
        fail() { this.error(new Error('decoder error')); }
        static isConfigSupported(cfg) {
            env.probes.push(JSON.parse(JSON.stringify(cfg)));
            if (env.mode.isCfgThrows) throw new Error('isConfigSupported');
            if (env.raw !== undefined) return Promise.resolve(env.raw);
            return Promise.resolve(env.supported(cfg)).then((v) => ({ supported: v, config: cfg }));
        }
    }
    class FakeBlob {
        constructor(parts, o) {
            this.parts = parts;
            this.type = o && o.type;
            this.bytes = parts && parts[0];
            env.blobs.push(this);
        }
    }
    env.frame = (w, h) => {
        const f = { displayWidth: w, displayHeight: h, codedWidth: w, codedHeight: h === 1080 ? 1088 : h, closed: 0, close() { this.closed++; } };
        env.frames.push(f);
        return f;
    };
    env.globals = { VideoDecoder: FakeVideoDecoder, EncodedVideoChunk: FakeEncodedVideoChunk, Blob: FakeBlob };
    return env;
}

const deferred = () => {
    const d = {};
    d.promise = new Promise((res, rej) => { d.resolve = res; d.reject = rej; });
    return d;
};

async function checkH264Browser(source) {
    const problems = [];
    const eq = (name, got, want) => {
        if (JSON.stringify(got) !== JSON.stringify(want)) problems.push(name + ': expected ' + JSON.stringify(want) + ', got ' + JSON.stringify(got));
    };
    const guard = async (name, fn) => {
        try { await fn(); } catch (e) { problems.push(name + ': threw ' + (e && e.message)); }
    };
    // 開いた、H.264 を受け取れるページ。opts: makeCodecs の引数、port: video_port、omit: 外すグローバル
    const page = async (opts, more) => {
        more = more || {};
        const env = makeCodecs(opts);
        const globals = Object.assign({}, env.globals);
        for (const k of more.omit || []) delete globals[k];
        const st = { value: READY_AUTH(more.port === null ? {} : { video_port: more.port || 8444 }) };
        const h = await boot(source, L('kvm.local', 'kvm.local', ''), st, globals);
        h.env = env;
        h.st = st;
        if (h.lastVideo() && more.open !== false) h.lastVideo().onopen();
        return h;
    };
    const feed = async (h, hex) => { await h.msg(hx(hex).buffer); await flush(); };
    const draws = (h) => h.canvas()._ctx.calls.filter((c) => c === 'drawImage').length;
    const chunksOf = (d) => d.decoded.map((c) => [c.type, hexOf(c.data)]);
    const urls = (h) => h.videoSockets().map((s) => s.url);
    const PLAIN = '接続完了 / 受信中';

    // 1. accept=h264 を付ける条件(9.4): VideoDecoder があり、isConfigSupported が真で、このタブで失敗していない
    await guard('accept/no WebCodecs', async () => {
        const st = { value: READY_AUTH({ video_port: 8444 }) };
        const h = await boot(source, L('kvm.local', 'kvm.local', ''), st);
        eq('1a url', urls(h), ['wss://kvm.local:8444/video']);
        eq('1a binaryType stays blob', h.lastVideo().binaryType, 'blob');
    });
    await guard('accept/supported', async () => {
        const h = await page();
        eq('1b url', urls(h), ['wss://kvm.local:8444/video?accept=h264']);
        eq('1b binaryType', h.lastVideo().binaryType, 'arraybuffer');
        eq('1b probe config (the H.264 baseline avc1.42c01f, no description)', h.env.probes, [PROBE_CONFIG]);
        eq('1b input socket has no query', h.sockets.filter((s) => /\/ws/.test(s.url)).map((s) => s.url), ['wss://kvm.local/ws']);
        eq('1b no decoder before a frame', h.env.decoders.length, 0);
    });
    await guard('accept/legacy url', async () => {
        const h = await page({}, { port: null });
        eq('1c url', urls(h), ['wss://kvm.local/video?accept=h264']);
    });
    await guard('accept/not supported', async () => {
        const h = await page({ supported: () => false });
        eq('1d url', urls(h), ['wss://kvm.local:8444/video']);
        eq('1d binaryType', h.lastVideo().binaryType, 'blob');
        eq('1d probed once', h.env.probes.length, 1);
    });
    await guard('accept/probe rejects', async () => {
        const h = await page({ supported: () => Promise.reject(new Error('x')) });
        eq('1e url', urls(h), ['wss://kvm.local:8444/video']);
    });
    await guard('accept/probe throws', async () => {
        const h = await page({ mode: { isCfgThrows: true } });
        eq('1f url', urls(h), ['wss://kvm.local:8444/video']);
    });
    await guard('accept/no EncodedVideoChunk', async () => {
        const h = await page({}, { omit: ['EncodedVideoChunk'] });
        eq('1g url', urls(h), ['wss://kvm.local:8444/video']);
        eq('1g not even probed', h.env.probes.length, 0);
    });
    await guard('accept/no VideoDecoder', async () => {
        const h = await page({}, { omit: ['VideoDecoder'] });
        eq('1h url', urls(h), ['wss://kvm.local:8444/video']);
    });
    for (const [name, raw] of [['supported: "yes"', { supported: 'yes' }], ['supported: 1', { supported: 1 }], ['supported missing', {}], ['null', null], ['undefined result', undefined]]) {
        await guard('accept/odd probe result ' + name, async () => {
            const env = makeCodecs({});
            env.raw = raw === undefined ? Promise.resolve(undefined) : raw;
            const st = { value: READY_AUTH({ video_port: 8444 }) };
            const h = await boot(source, L('kvm.local', 'kvm.local', ''), st, env.globals);
            eq('1i url for ' + name, urls(h), ['wss://kvm.local:8444/video']);
        });
    }
    await guard('accept/probe is awaited before the first connection', async () => {
        const gate = deferred();
        const env = makeCodecs({ supported: () => gate.promise });
        const st = { value: READY_AUTH({ video_port: 8444 }) };
        const h = await boot(source, L('kvm.local', 'kvm.local', ''), st, env.globals);
        eq('1j no socket while the probe is pending', h.sockets.length, 0);
        gate.resolve(true);
        await flush();
        await flush();
        eq('1j url after the probe', urls(h), ['wss://kvm.local:8444/video?accept=h264']);
        const gate2 = deferred();
        const env2 = makeCodecs({ supported: () => gate2.promise });
        const h2 = await boot(source, L('kvm.local', 'kvm.local', ''), st, env2.globals);
        gate2.resolve(false);
        await flush();
        await flush();
        eq('1j url after a negative probe', urls(h2), ['wss://kvm.local:8444/video']);
    });
    await guard('accept/a probe that never settles', async () => {
        const env = makeCodecs({ supported: () => new Promise(() => {}) });
        const st = { value: READY_AUTH({ video_port: 8444 }) };
        const h = await boot(source, L('kvm.local', 'kvm.local', ''), st, env.globals);
        eq('1l no socket yet, one wait timer of 1500 ms', [h.sockets.length, h.timers.map((t) => t.delay)], [0, [1500]]);
        h.runTimer(1500);
        await flush();
        eq('1l after the wait: connected without accept', urls(h), ['wss://kvm.local:8444/video']);
        const h2 = await page();
        eq('1l a settled probe leaves no timer behind', h2.timers.length, 0);
    });
    await guard('accept/trust hint is not changed by the query', async () => {
        const h = await page({}, { open: false });
        h.lastVideo().onerror({});
        eq('1k hint shown', h.hintHidden(), false);
        eq('1k link', h.linkHref(), 'https://kvm.local:8444/');
    });

    // 2. JPEG(FF D8)は、H.264 を受け取れる接続でも、従来どおり描く
    await guard('jpeg/on an accept connection', async () => {
        const h = await page();
        await h.msg(jpegBytes(80, 60).buffer);
        eq('2a drawn', [draws(h), h.canvas().width, h.canvas().height], [1, 80, 60]);
        eq('2a wrapped as image/jpeg', h.env.blobs.map((b) => b.type), ['image/jpeg']);
        eq('2a bytes kept', hexOf(h.env.blobs[0].bytes), 'ff d8 50 3c ff d9');
        eq('2a no decoder', h.env.decoders.length, 0);
        eq('2a status line has no codec', h.statusText(), PLAIN);
        await h.msg(Uint8Array.from([0xFF, 0xD8]).buffer);
        eq('2a FF D8 alone is still a JPEG', draws(h), 2);
        const big = Uint8Array.from([9, 9, 0xFF, 0xD8, 70, 50, 0xFF, 0xD9]);
        await h.msg(big.subarray(2));
        eq('2a a view with a byte offset', [draws(h), h.canvas().width], [3, 70]);
        await h.msg(jpegBytes(30, 20)); // a Uint8Array, not an ArrayBuffer
        eq('2a a Uint8Array', [draws(h), h.canvas().width], [4, 30]);
    });
    // 3. どちらでもないバイナリは、捨てる(落ちない)。3 バイトのスタートコード、00 00 00 02 など
    await guard('classify/drops the rest', async () => {
        const h = await page();
        await h.msg(jpegBytes(40, 30).buffer);
        const bad = ['', 'ff', 'd8 ff', 'ff d9', '00', '00 00', '00 00 00', '00 00 01', '00 00 01 67 42 c0 1f', '00 00 00 02 67 42 c0 1f',
            '00 00 00 00', '01 00 00 00 01', '47 49 46 38', '7b 22 74 22 3a', '00 00 00 03 65 88'];
        for (const b of bad) {
            await h.msg(hx(b).buffer);
            await flush();
        }
        eq('3 nothing drawn, no decoder, no probe', [draws(h), h.env.decoders.length, h.env.probes.length], [1, 0, 1]);
        eq('3 the connection stays', [h.lastVideo().readyState, h.videoSockets().length, h.timers.length], [0, 1, 0]);
        for (const odd of [{ jpeg: true, w: 5, h: 5 }, null, undefined, 5, {}, [1, 2, 3], new Blob([1])]) {
            await h.msg(odd);
        }
        eq('3 a non-binary object is dropped too', draws(h), 1);
        eq('3 the status line is untouched', h.statusText(), PLAIN);
        await h.msg(jpegBytes(50, 40).buffer);
        eq('3 a JPEG is still drawn afterwards', [draws(h), h.canvas().width], [2, 50]);
        await feed(h, AU_IDR720);
        eq('3 H.264 is still decoded afterwards', h.env.decoders.length, 1);
    });
    // 4. H.264 の流れ: 最初の IDR まで P は捨てる。SPS から codec 文字列を作って configure。`description` は付けない
    await guard('h264/first IDR', async () => {
        const h = await page();
        await feed(h, pAu(1));
        await feed(h, pAu(2));
        eq('4 P before the first IDR: nothing', [h.env.probes.length, h.env.decoders.length, h.env.chunks.length], [1, 0, 0]);
        await feed(h, AU_IDR720);
        eq('4 isConfigSupported of the stream codec', h.env.probes[1], PROBE_CONFIG);
        eq('4 one decoder', h.env.decoders.length, 1);
        const d = h.env.decoders[0];
        eq('4 configure: codec from the SPS, optimizeForLatency, no description', d.configured, { codec: 'avc1.42c01f', optimizeForLatency: true });
        eq('4 first chunk is a key chunk with the whole access unit', chunksOf(d), [['key', AU_IDR720]]);
        eq('4 chunk init has exactly type, timestamp, data', h.env.chunks[0].keys, ['data', 'timestamp', 'type']);
        eq('4 timestamp is a number', typeof h.env.chunks[0].timestamp, 'number');
        await feed(h, pAu(3));
        await feed(h, pAu(4));
        eq('4 then delta chunks', chunksOf(d), [['key', AU_IDR720], ['delta', pAu(3)], ['delta', pAu(4)]]);
        const ts = h.env.chunks.map((c) => c.timestamp);
        eq('4 timestamps increase', ts[0] < ts[1] && ts[1] < ts[2], true);
        eq('4 nothing is drawn before an output', draws(h), 0);
        eq('4 the status line has no codec before a frame', h.statusText(), PLAIN);
        // 描画: VideoFrame を canvas に描いて、close する
        const f1 = h.env.frame(1280, 720);
        d.emit(f1);
        eq('4 frame drawn with drawImage(frame, 0, 0)', h.canvas()._ctx.args.filter((a) => a[0] === 'drawImage').map((a) => [a[1][0] === f1, a[1][1], a[1][2]]), [[true, 0, 0]]);
        eq('4 canvas size from the frame', [h.canvas().width, h.canvas().height], [1280, 720]);
        eq('4 frame closed', f1.closed, 1);
        eq('4 status line says H.264', h.statusText(), PLAIN + LABEL);
        const sizeSets = () => h.canvas()._sets.filter((n) => n === 'width' || n === 'height').length;
        const setsBefore = sizeSets();
        d.emit(h.env.frame(1280, 720));
        eq('4 the label is not doubled', h.statusText(), PLAIN + LABEL);
        eq('4 the canvas is not resized (and so not cleared) for a frame of the same size', sizeSets(), setsBefore);
        // C-6b の状態表示は、そのまま動く
        await h.msg(V('locked', 1280, 720));
        eq('4 locked: resolution and codec side by side', h.statusText(), PLAIN + ' (1280x720)' + LABEL);
        eq('4 locked: no overlay', h.overlayHidden(), true);
        await h.msg(V('no_signal', 0, 0));
        eq('4 no_signal: overlay', [h.overlayHidden(), h.overlayText(), h.statusText()], [false, '信号なし', PLAIN + LABEL]);
        await h.msg(V('unknown', 0, 0));
        eq('4 unknown: overlay', [h.overlayHidden(), h.overlayText()], [false, '映像の状態を確認中…']);
        await h.msg(V('locked', 1280, 720));
        await h.msg(V('locked', 1920, 1080));
        eq('4 resolution change notice', [h.overlayText(), h.statusText()], ['解像度が変わりました（1920x1080）', PLAIN + ' (1920x1080)' + LABEL]);
        eq('4 and its timer', h.noticeTimers().length, 1);
        await h.msg('not json');
        eq('4 bad text is ignored', h.statusText(), PLAIN + ' (1920x1080)' + LABEL);
        // 映像の表示サイズは、displayWidth を優先し、無ければ codedWidth
        const f2 = { displayWidth: 0, displayHeight: 0, codedWidth: 1920, codedHeight: 1088, closed: 0, close() { this.closed++; } };
        d.emit(f2);
        eq('4 canvas size falls back to the coded size', [h.canvas().width, h.canvas().height, f2.closed], [1920, 1088, 1]);
        d.emit(h.env.frame(1920, 1080));
        eq('4 canvas size from the display size (the coded height of 1080p is 1088)', [h.canvas().width, h.canvas().height], [1920, 1080]);
        const f3 = { displayWidth: 1366, displayHeight: 768, codedWidth: 1376, codedHeight: 768, closed: 0, close() { this.closed++; } };
        d.emit(f3);
        eq('4 the display width wins over a wider coded width', [h.canvas().width, h.canvas().height, f3.closed], [1366, 768, 1]);
    });
    // 5. SPS のない IDR スライス、SPS だけのアクセスユニット
    await guard('h264/IDR without SPS', async () => {
        const h = await page();
        await feed(h, auHex(NAL_IDR));
        await feed(h, auHex(NAL_PPS, NAL_IDR));
        eq('5a no decoder can be made without an SPS', [h.env.probes.length, h.env.decoders.length], [1, 0]);
        eq('5a and it is no failure', [h.lastVideo().readyState, h.timers.length], [0, 0]);
        await feed(h, auHex(NAL_SPS720));
        eq('5a an SPS-only access unit is a key access unit', [h.env.decoders.length, chunksOf(h.env.decoders[0])], [1, [['key', auHex(NAL_SPS720)]]]);
        await feed(h, auHex(NAL_IDR));
        eq('5a an IDR slice with the decoder configured is a key chunk', chunksOf(h.env.decoders[0]).map((c) => c[0]), ['key', 'key']);
        await feed(h, pAu(1));
        eq('5a P follows', chunksOf(h.env.decoders[0]).map((c) => c[0]), ['key', 'key', 'delta']);
    });
    // 6. 解像度の変化(SPS の変化)でデコーダを作り直す
    await guard('h264/resolution change', async () => {
        const h = await page();
        await feed(h, AU_IDR720);
        await feed(h, pAu(1));
        const d0 = h.env.decoders[0];
        await feed(h, AU_IDR720);
        eq('6a the same SPS: the decoder is kept', [h.env.decoders.length, chunksOf(d0).map((c) => c[0])], [1, ['key', 'delta', 'key']]);
        await feed(h, AU_IDR1080);
        eq('6b new SPS (avc1.42c029): probed', h.env.probes[h.env.probes.length - 1], { codec: 'avc1.42c029', optimizeForLatency: true });
        eq('6b the old decoder is closed, a new one is made', [d0.closed, h.env.decoders.length], [true, 2]);
        const d1 = h.env.decoders[1];
        eq('6b configured for 1080p', d1.configured, { codec: 'avc1.42c029', optimizeForLatency: true });
        eq('6b the IDR goes to the new decoder', [chunksOf(d1), chunksOf(d0).length], [[['key', AU_IDR1080]], 3]);
        await feed(h, pAu(2));
        eq('6b P goes to the new decoder', [chunksOf(d1).map((c) => c[0]), chunksOf(d0).length], [['key', 'delta'], 3]);
        // 古いデコーダの出力は、描かずに close する
        const stale = h.env.frame(1280, 720);
        d0.emit(stale);
        eq('6b a frame of the old decoder is closed, not drawn', [stale.closed, draws(h)], [1, 0]);
        const fresh = h.env.frame(1920, 1080);
        d1.emit(fresh);
        eq('6b a frame of the new decoder is drawn', [fresh.closed, draws(h)], [1, 1]);
        // 古いデコーダのエラーは無視する(フォールバックしない)
        d0.fail();
        eq('6b an error of the old decoder is ignored', [h.lastVideo().readyState, h.timers.length], [0, 0]);
        // codec 文字列が同じで、SPS の中身が違う(解像度の違い)ときも、作り直す
        await feed(h, AU_IDR720);
        eq('6c back to 720p', [h.env.decoders.length, h.env.decoders[1].closed], [3, true]);
        await feed(h, AU_IDR_B);
        eq('6c same codec string, other SPS bytes: rebuilt', [h.env.decoders.length, h.env.decoders[2].closed], [4, true]);
        eq('6c configured with the same codec', h.env.decoders[3].configured, { codec: 'avc1.42c01f', optimizeForLatency: true });
        await feed(h, AU_IDR_B);
        eq('6c the same SPS again: kept', h.env.decoders.length, 4);
    });
    // 7. 構成の途中(isConfigSupported の待ち)に来たフレームは捨てる。待ちの間の 2 つ目の IDR は、構成を 2 回しない
    await guard('h264/pending configure', async () => {
        const h = await page();
        const gate = deferred();
        h.env.supported = () => gate.promise;
        await feed(h, AU_IDR720);
        eq('7 pending: no decoder yet, one probe more', [h.env.decoders.length, h.env.probes.length], [0, 2]);
        await feed(h, pAu(1));
        await feed(h, AU_IDR720);
        eq('7 pending: P and a second IDR are dropped, no second probe', [h.env.decoders.length, h.env.probes.length, h.env.chunks.length], [0, 2, 0]);
        gate.resolve(true);
        await flush();
        eq('7 resolved: one decoder with the first IDR only', [h.env.decoders.length, chunksOf(h.env.decoders[0])], [1, [['key', AU_IDR720]]]);
        await feed(h, pAu(2));
        eq('7 then P', chunksOf(h.env.decoders[0]).map((c) => c[0]), ['key', 'delta']);
    });
    await guard('h264/pending configure is cancelled by a reset', async () => {
        const h = await page();
        const gates = [];
        h.env.supported = () => { const g = deferred(); gates.push(g); return g.promise; };
        await feed(h, AU_IDR720);
        await h.closeVideo(false); // the connection closes while the check is pending
        gates[0].resolve(true);
        await flush();
        eq('7b a late answer after the reset makes no decoder', h.env.decoders.length, 0);
        h.runTimer(1000);
        await flush();
        h.lastVideo().onopen();
        eq('7b reconnects with accept (no fallback happened)', urls(h)[1], 'wss://kvm.local:8444/video?accept=h264');
        await feed(h, AU_IDR720);
        eq('7b a new check on the new connection', gates.length, 2);
        gates[1].resolve(true);
        await flush();
        eq('7b only the new check makes a decoder', [h.env.decoders.length, chunksOf(h.env.decoders[0])], [1, [['key', AU_IDR720]]]);
        // a late "not supported" / rejection of a check that was reset is not a failure of the stream
        const late = [];
        h.env.supported = () => { const g = deferred(); late.push(g); return g.promise; };
        await feed(h, AU_IDR1080);
        await h.closeVideo(false);
        late[0].resolve(false);
        await flush();
        eq('7b a late "not supported" after the reset is ignored: only the reconnect timer', h.timers.length, 1);
        h.runTimer(1000);
        await flush();
        h.lastVideo().onopen();
        eq('7b still no fallback: accept again', urls(h)[2], 'wss://kvm.local:8444/video?accept=h264');
        await feed(h, AU_IDR1080);
        await h.closeVideo(false);
        late[1].reject(new Error('late'));
        await flush();
        eq('7b a late rejection after the reset is ignored: only the reconnect timer', h.timers.length, 1);
        h.runTimer(1000);
        await flush();
        eq('7b still no fallback after the rejection', urls(h)[3], 'wss://kvm.local:8444/video?accept=h264');
    });
    // 8. decodeQueueSize が溜まったら、次の IDR まで P を捨てる(3 以上)
    await guard('h264/backlog', async () => {
        const h = await page();
        await feed(h, AU_IDR720);
        const d = h.env.decoders[0];
        d.decodeQueueSize = 2;
        await feed(h, pAu(1));
        eq('8 queue 2: decoded', chunksOf(d).length, 2);
        d.decodeQueueSize = 3;
        await feed(h, pAu(2));
        eq('8 queue 3: dropped', chunksOf(d).length, 2);
        d.decodeQueueSize = 0;
        await feed(h, pAu(3));
        await feed(h, pAu(4));
        eq('8 the queue is empty again, but P stays dropped until an IDR', chunksOf(d).length, 2);
        d.decodeQueueSize = 5;
        await feed(h, AU_IDR720);
        eq('8 an IDR is decoded even when the queue is long', chunksOf(d).map((c) => c[0]), ['key', 'delta', 'key']);
        d.decodeQueueSize = 0;
        await feed(h, pAu(5));
        eq('8 after the IDR, P is decoded again', chunksOf(d).map((c) => c[0]), ['key', 'delta', 'key', 'delta']);
        d.decodeQueueSize = 100;
        await feed(h, pAu(6));
        eq('8 a very long queue: dropped', chunksOf(d).length, 4);
        eq('8 no fallback', [h.lastVideo().readyState, h.timers.length], [0, 0]);
        d.decodeQueueSize = undefined;
        d.decodeQueueSize = 0;
    });
    // 9. VideoFrame は必ず close する
    await guard('h264/frames are always closed', async () => {
        const h = await page();
        await feed(h, AU_IDR720);
        const d = h.env.decoders[0];
        const frames = [h.env.frame(1280, 720), h.env.frame(1280, 720), h.env.frame(640, 480)];
        frames.forEach((f) => d.emit(f));
        eq('9 every frame closed once', frames.map((f) => f.closed), [1, 1, 1]);
        h.canvas()._ctx.failDraw = true;
        const bad = h.env.frame(1280, 720);
        d.emit(bad);
        eq('9 a frame whose drawing throws is closed', bad.closed, 1);
        eq('9 and it is not a decoder failure', [h.lastVideo().readyState, h.timers.length], [0, 0]);
        h.canvas()._ctx.failDraw = false;
        const ok = h.env.frame(1280, 720);
        d.emit(ok);
        eq('9 drawing works again', [ok.closed, draws(h)], [1, 4]);
        await h.closeVideo(false);
        const late = h.env.frame(1280, 720);
        d.emit(late);
        eq('9 a frame after the connection closed: closed, not drawn', [late.closed, draws(h)], [1, 4]);
    });
    // 10. フォールバック: そのタブでは以後 accept を付けず、接続を 1 回だけ張り直す
    const FALLBACKS = [
        ['isConfigSupported says no', async (h) => { h.env.supported = () => false; await feed(h, AU_IDR720); }],
        ['isConfigSupported rejects', async (h) => { h.env.supported = () => Promise.reject(new Error('x')); await feed(h, AU_IDR720); }],
        ['isConfigSupported throws', async (h) => { h.env.mode.isCfgThrows = true; await feed(h, AU_IDR720); }],
        ['isConfigSupported answers null', async (h) => { h.env.raw = null; await feed(h, AU_IDR720); }],
        ['isConfigSupported answers without "supported"', async (h) => { h.env.raw = {}; await feed(h, AU_IDR720); }],
        ['the decoder constructor throws', async (h) => { h.env.mode.constructThrows = true; await feed(h, AU_IDR720); }],
        ['configure throws', async (h) => { h.env.mode.configureThrows = true; await feed(h, AU_IDR720); }],
        ['EncodedVideoChunk throws', async (h) => { h.env.mode.chunkThrows = true; await feed(h, AU_IDR720); }],
        ['decode throws on the IDR', async (h) => { h.env.mode.decodeThrows = true; await feed(h, AU_IDR720); }],
        ['decode throws on a P frame', async (h) => { await feed(h, AU_IDR720); h.env.mode.decodeThrows = true; await feed(h, pAu(1)); }],
        ['the error callback', async (h) => { await feed(h, AU_IDR720); h.env.decoders[0].fail(); }],
    ];
    for (const [name, trigger] of FALLBACKS) {
        await guard('fallback/' + name, async () => {
            const h = await page();
            await trigger(h);
            const probesBefore = h.env.probes.length;
            const tag = '10 [' + name + '] ';
            eq(tag + 'the connection is closed', h.lastVideo().readyState, 3);
            eq(tag + 'no decoder is left open', h.env.decoders.every((d) => d.closed), true);
            // more input on the closing connection changes nothing
            const decodersBefore = h.env.decoders.length;
            await feed(h, AU_IDR720);
            await feed(h, pAu(1));
            eq(tag + 'input after the fallback is ignored', [h.env.decoders.length, h.env.probes.length], [decodersBefore, probesBefore]);
            if (h.env.decoders[0]) { h.env.decoders[0].fail(); h.env.decoders[0].fail(); }
            eq(tag + 'repeated errors do not repeat the fallback', h.timers.length, 0);
            const delay = await h.closeVideo(true);
            eq(tag + 'reconnect delay (the usual one)', delay, 1000);
            eq(tag + 'one reconnect, without accept', urls(h), ['wss://kvm.local:8444/video?accept=h264', 'wss://kvm.local:8444/video']);
            eq(tag + 'MJPEG connection: binaryType blob', h.lastVideo().binaryType, 'blob');
            h.lastVideo().onopen();
            if (h.env.decoders[0]) h.env.decoders[0].fail();
            eq(tag + 'an old decoder cannot close the new connection', [h.lastVideo().readyState, h.timers.length], [0, 0]);
            await h.msg({ jpeg: true, w: 90, h: 70 });
            eq(tag + 'frames of the MJPEG connection are drawn as before', [draws(h), h.canvas().width], [1, 90]);
            await h.closeVideo(true);
            eq(tag + 'and never again', urls(h), ['wss://kvm.local:8444/video?accept=h264', 'wss://kvm.local:8444/video', 'wss://kvm.local:8444/video']);
            eq(tag + 'no new decoder or probe', [h.env.decoders.length, h.env.probes.length], [decodersBefore, probesBefore]);
        });
    }
    await guard('fallback/after the stream was running', async () => {
        const h = await page();
        await feed(h, AU_IDR720);
        const d = h.env.decoders[0];
        d.emit(h.env.frame(1280, 720));
        eq('10b label shown', h.statusText(), PLAIN + LABEL);
        d.fail();
        eq('10b closed', [h.lastVideo().readyState, d.closed], [3, true]);
        await h.closeVideo(false);
        eq('10b the status line is the usual one for a closed connection', h.statusText(), '切断されました。再接続します...');
    });
    // 11. サーバーが MJPEG に下げたとき(JPEG が来た)
    await guard('h264/server falls back to MJPEG', async () => {
        const h = await page();
        await h.msg(V('locked', 1280, 720));
        await feed(h, AU_IDR720);
        const d = h.env.decoders[0];
        d.emit(h.env.frame(1280, 720));
        eq('11 H.264 shown', h.statusText(), PLAIN + ' (1280x720)' + LABEL);
        await h.msg(jpegBytes(64, 48).buffer);
        eq('11 a JPEG is drawn and the label goes', [draws(h), h.canvas().width, h.statusText()], [2, 64, PLAIN + ' (1280x720)']);
        await feed(h, pAu(1));
        eq('11 a P frame after the JPEG is dropped (wait for an IDR)', chunksOf(d).length, 1);
        await feed(h, AU_IDR720);
        eq('11 the next IDR is decoded by the same decoder', [h.env.decoders.length, chunksOf(d).map((c) => c[0])], [1, ['key', 'key']]);
        d.emit(h.env.frame(1280, 720));
        eq('11 and the label is back', h.statusText(), PLAIN + ' (1280x720)' + LABEL);
        await feed(h, pAu(2));
        eq('11 P again', chunksOf(d).map((c) => c[0]), ['key', 'key', 'delta']);
        eq('11 no fallback', [h.lastVideo().readyState, h.timers.length], [0, 0]);
    });
    // 12. 切断・再接続・ログアウトで、デコーダを閉じる
    await guard('h264/reset on close', async () => {
        const h = await page();
        await feed(h, AU_IDR720);
        const d0 = h.env.decoders[0];
        d0.emit(h.env.frame(1280, 720));
        await h.closeVideo(false);
        eq('12a decoder closed with the connection', d0.closed, true);
        eq('12a status line of a closed connection', h.statusText(), '切断されました。再接続します...');
        await h.runTimer(1000);
        await flush();
        eq('12a reconnect asks for H.264 again', urls(h), ['wss://kvm.local:8444/video?accept=h264', 'wss://kvm.local:8444/video?accept=h264']);
        h.lastVideo().onopen();
        eq('12a a new connection starts without the label', h.statusText(), PLAIN);
        await feed(h, pAu(1));
        eq('12a P before the first IDR of the new connection is dropped', [h.env.decoders.length, h.env.chunks.length], [1, 1]);
        await feed(h, AU_IDR720);
        eq('12a a new decoder for the new connection', [h.env.decoders.length, chunksOf(h.env.decoders[1])], [2, [['key', AU_IDR720]]]);
        h.env.decoders[1].emit(h.env.frame(1280, 720));
        eq('12a the label comes back on the new connection', h.statusText(), PLAIN + LABEL);
        const f = h.env.frame(1280, 720);
        d0.emit(f);
        eq('12a the old decoder cannot draw', [f.closed, draws(h)], [1, 2]);
    });
    await guard('h264/close() throws', async () => {
        const h = await page();
        await feed(h, AU_IDR720);
        h.env.mode.closeThrows = true;
        await h.closeVideo(false);
        eq('12c a decoder that cannot be closed does not break the close of the connection', [h.env.decoders[0].closed, h.timers.length], [true, 1]);
        h.env.mode.closeThrows = false;
        h.runTimer(1000);
        await flush();
        h.lastVideo().onopen();
        await feed(h, AU_IDR720);
        eq('12c the next connection decodes', h.env.decoders.length, 2);
    });
    await guard('h264/reset on logout', async () => {
        const h = await page();
        await feed(h, AU_IDR720);
        const d = h.env.decoders[0];
        d.emit(h.env.frame(1280, 720));
        h.st.value = { state: 'ready', authenticated: false };
        h.elements['btn-logout']._listeners.click();
        await flush();
        await flush();
        eq('12b decoder closed', d.closed, true);
        eq('12b status line', h.statusText(), '切断されました');
    });
    // 13. 不正な入力で落ちない(空、短い、壊れた NAL)。フォールバックもしない
    await guard('h264/bad input', async () => {
        const h = await page();
        const bad = ['00 00 00 01', '00 00 00 01 67', '00 00 00 01 67 42', '00 00 00 01 67 42 c0', '00 00 00 01 e7 42 c0 1f', '00 00 00 01 00',
            '00 00 00 01 ff ff ff ff', '00 00 00 01 00 00 00 01', '00 00 00 01 65', '00 00 00 01 41', '00 00 00 01 00 00 00 00 00 00',
            '00 00 00 01 68 ce 3c 80', '00 00 00 01 06 05 01 02', '00 00 00 01 09 f0', '00 00 00 01 78 42 c0 1f'];
        for (const b of bad) {
            await feed(h, b);
        }
        eq('13 nothing decoded (no valid SPS or no decoder)', [h.env.decoders.length, h.env.chunks.length, h.env.probes.length], [0, 0, 1]);
        eq('13 no fallback', [h.lastVideo().readyState, h.timers.length], [0, 0]);
        // 有効なストリームの途中の不正な入力は、デコーダに渡さない
        await feed(h, AU_IDR720);
        const d = h.env.decoders[0];
        for (const b of ['00 00 00 01', '00 00 00 01 e7 42 c0 1f', '00 00 00 01 ff ff ff ff', '00 00 00 01 67', '00 00 00 01 67 42 c0']) {
            await feed(h, b);
        }
        eq('13 garbage inside a running stream is not decoded', chunksOf(d).length, 1);
        eq('13 no fallback after garbage', [h.lastVideo().readyState, h.timers.length, d.closed], [0, 0, false]);
        await feed(h, pAu(1));
        eq('13 the stream goes on', chunksOf(d).map((c) => c[0]), ['key', 'delta']);
        // 大きい入力
        const big = new Uint8Array(2 << 20).fill(0x55);
        big.set([0, 0, 0, 1, 0x41], 0);
        await h.msg(big.buffer);
        await flush();
        eq('13 a 2 MB access unit is decoded as a delta chunk', chunksOf(d).length, 3);
        await h.msg(new ArrayBuffer(0));
        eq('13 an empty message', chunksOf(d).length, 3);
    });
    // 14. 従来の経路(accept を付けない)は変わらない: Blob はそのまま createImageBitmap に渡る。H.264 には触れない
    await guard('legacy path unchanged', async () => {
        const seen = [];
        const st = { value: READY_AUTH({ video_port: 8444 }) };
        const h = await boot(source, L('kvm.local', 'kvm.local', ''), st, {
            createImageBitmap: async (d) => { seen.push(d); return { width: 7, height: 5, close() {} }; },
        });
        const blobLike = { size: 123 };
        await h.msg(blobLike);
        eq('14 the received object itself is decoded', [seen.length, seen[0] === blobLike], [1, true]);
        eq('14 drawn', [draws(h), h.canvas().width, h.canvas().height], [1, 7, 5]);
        // H.264 らしいバイト列も、従来の経路に入る(この接続はサーバーに H.264 を頼んでいないので、来ない前提)
        const ab = hx(AU_IDR720).buffer;
        await h.msg(ab);
        eq('14 no WebCodecs involved, passed on as before', [seen.length, seen[1] === ab], [2, true]);
        h.lastVideo().onopen();
        eq('14 status line has no codec', h.statusText(), PLAIN);
    });
    return problems;
}


async function checkAll(source) {
    const problems = checkPure(source);
    let browserProblems;
    try { browserProblems = await checkBrowser(source); } catch (e) { browserProblems = ['browser harness threw: ' + (e && e.message)]; }
    let h264Problems;
    try { h264Problems = await checkH264Browser(source); } catch (e) { h264Problems = ['h264 harness threw: ' + (e && e.message)]; }
    await flush();
    await flush();
    const rejected = unhandled.splice(0).map((m) => 'unhandled promise rejection: ' + m);
    return problems.concat(browserProblems, h264Problems, rejected);
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
        M('J54', 'connect: always connects to the legacy URL', 'VideoTarget.withAccept(videoTarget || VideoTarget.resolve(location, undefined), acceptH264)', 'VideoTarget.withAccept(VideoTarget.resolve(location, undefined), acceptH264)'),
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
        M('V50', 'resetVideoStatus: last status not forgotten', '        videoStatus = null;\n        setVideoOverlay(null);\n        resetH264();', '        setVideoOverlay(null);\n        resetH264();'),
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
        // ---- H.264(WebCodecs): withAccept、H264 の純関数、デコーダの制御、フォールバック、表示 ----
        M('D01', 'withAccept: accept compared loosely (anything but false adds the query)', "if (accept !== true) return target;", "if (accept === false) return target;"),
        M('D02', 'withAccept: the query is always added', "            if (accept !== true) return target;\n", ''),
        M('D03', 'withAccept: other codec name', "'?accept=h264'", "'?accept=h265'"),
        M('D04', 'withAccept: query joined with &', "'?accept=h264'", "'&accept=h264'"),
        M('D05', 'withAccept: changes the target it was given', "Object.assign({}, target, { url: target.url + '?accept=h264' })", "Object.assign(target, { url: target.url + '?accept=h264' })"),
        M('D06', 'withAccept: the other fields of the target are lost', "Object.assign({}, target, { url: target.url + '?accept=h264' })", "{ url: target.url + '?accept=h264' }"),
        M('D07', 'classify: null guard removed', "if (b === null || b === undefined || typeof b.length !== 'number') return null;", "if (b === undefined || typeof b.length !== 'number') return null;"),
        M('D08', 'classify: undefined guard removed', "if (b === null || b === undefined || typeof b.length !== 'number') return null;", "if (b === null || typeof b.length !== 'number') return null;"),
        M('D09', 'classify: the length type check removed', "if (b === null || b === undefined || typeof b.length !== 'number') return null;", "if (b === null || b === undefined) return null;"),
        M('D10', 'classify: JPEG needs 3 bytes', 'if (b.length >= 2 && b[0] === 0xFF', 'if (b.length >= 3 && b[0] === 0xFF'),
        M('D11', 'classify: JPEG length check 2 -> 1 (b[1] of a 1-byte input is undefined, so it is not FF D8 anyway)', 'if (b.length >= 2 && b[0] === 0xFF', 'if (b.length >= 1 && b[0] === 0xFF',
            '長さ 1 の入力では b[1] が undefined で、0xD8 と一致しないので、長さの検査は(範囲外の読み出しが undefined を返すことで)二重の防御'),
        M('D12', 'classify: JPEG first byte', 'b[0] === 0xFF && b[1] === 0xD8', 'b[0] === 0xFE && b[1] === 0xD8'),
        M('D13', 'classify: JPEG second byte', 'b[0] === 0xFF && b[1] === 0xD8', 'b[0] === 0xFF && b[1] === 0xD9'),
        M('D14', 'classify: JPEG first byte not checked', 'b[0] === 0xFF && b[1] === 0xD8', 'b[1] === 0xD8'),
        M('D15', 'classify: JPEG second byte not checked', 'b[0] === 0xFF && b[1] === 0xD8', 'b[0] === 0xFF'),
        M('D16', 'classify: H.264 needs 5 bytes', 'if (b.length >= 4 && b[0] === 0', 'if (b.length >= 5 && b[0] === 0'),
        M('D17', 'classify: H.264 length check 4 -> 3 (b[3] of a 3-byte input is undefined, so it is not 1 anyway)', 'if (b.length >= 4 && b[0] === 0', 'if (b.length >= 3 && b[0] === 0',
            '長さ 3 の入力では b[3] が undefined で、1 と一致しないので、長さの検査は二重の防御'),
        M('D18', 'classify: H.264 b[0] not checked', 'b[0] === 0 && b[1] === 0 && b[2] === 0 && b[3] === 1', 'b[1] === 0 && b[2] === 0 && b[3] === 1'),
        M('D19', 'classify: H.264 b[1] not checked', 'b[0] === 0 && b[1] === 0 && b[2] === 0 && b[3] === 1', 'b[0] === 0 && b[2] === 0 && b[3] === 1'),
        M('D20', 'classify: H.264 b[2] not checked', 'b[0] === 0 && b[1] === 0 && b[2] === 0 && b[3] === 1', 'b[0] === 0 && b[1] === 0 && b[3] === 1'),
        M('D21', 'classify: H.264 last byte 1 -> 2', 'b[2] === 0 && b[3] === 1', 'b[2] === 0 && b[3] === 2'),
        M('D22', 'classify: H.264 last byte not checked', 'b[2] === 0 && b[3] === 1', 'b[2] === 0'),
        M('D23', 'classify: labels swapped', "return 'jpeg';", "return 'h264';"),
        M('D24', 'parseAnnexB: null guard removed', "if (b === null || b === undefined || typeof b.length !== 'number') return out;", "if (b === undefined || typeof b.length !== 'number') return out;"),
        M('D25', 'parseAnnexB: the length type check removed', "if (b === null || b === undefined || typeof b.length !== 'number') return out;", "if (b === null || b === undefined) return out;"),
        M('D26', 'parseAnnexB: scan stops one byte early', 'while (pos + 3 <= n) {', 'while (pos + 3 < n) {'),
        M('D27', 'parseAnnexB: skips 4 after a start code', "                    starts.push(pos + 3);\n                    pos += 3;", "                    starts.push(pos + 3);\n                    pos += 4;"),
        M('D28', 'parseAnnexB: advances 1 after a start code (the next start code cannot begin inside the last bytes of this one)', "                    starts.push(pos + 3);\n                    pos += 3;", "                    starts.push(pos + 3);\n                    pos += 1;",
            '開始コードは 00 00 01 で、最後のバイトが 01 なので、次の開始コードは、その後ろの 2 バイトの内側では始まれない。1 つ進めても結果は同じ'),
        M('D29', 'parseAnnexB: the NAL starts at the start code', 'starts.push(pos + 3);', 'starts.push(pos + 2);'),
        M('D30', 'parseAnnexB: the NAL starts one byte late', 'starts.push(pos + 3);', 'starts.push(pos + 4);'),
        M('D31', 'parseAnnexB: the NAL ends 4 before the next start code', 'starts[k + 1] - 3 : n;', 'starts[k + 1] - 4 : n;'),
        M('D32', 'parseAnnexB: the NAL ends 2 before the next start code (the zero byte is cut off again below)', 'starts[k + 1] - 3 : n;', 'starts[k + 1] - 2 : n;',
            '開始コードの先頭の 00 を含めても、直後の「後ろのゼロを削る」処理が取り除くので結果は同じ'),
        M('D33', 'parseAnnexB: the last NAL ends one byte early', 'starts[k + 1] - 3 : n;', 'starts[k + 1] - 3 : n - 1;'),
        M('D34', 'parseAnnexB: the next start code is looked up past the end', 'let end = k + 1 < starts.length ?', 'let end = k + 1 <= starts.length ?'),
        M('D35', 'parseAnnexB: trailing zero bytes are kept', '                while (end > s && b[end - 1] === 0) end--;\n', ''),
        M('D36', 'parseAnnexB: trailing zero trimming may reach the start of the NAL (b[s - 1] is the 01 of the start code, so it cannot)', 'while (end > s && b[end - 1] === 0) end--;', 'while (end >= s && b[end - 1] === 0) end--;',
            'b[s - 1] は開始コードの最後のバイト(01)で 0 でないので、ゼロの削りは s で必ず止まる。end > s の検査は二重の防御'),
        M('D37', 'parseAnnexB: an empty NAL is kept', 'if (end <= s) continue;', 'if (end < s) continue;'),
        M('D38', 'parseAnnexB: forbidden_zero_bit not checked', '                if ((b[s] & 0x80) !== 0) continue;\n', ''),
        M('D39', 'parseAnnexB: type mask 0x1F -> 0x3F', 'type: b[s] & 0x1F', 'type: b[s] & 0x3F'),
        M('D40', 'parseAnnexB: type mask 0x1F -> 0x0F', 'type: b[s] & 0x1F', 'type: b[s] & 0x0F'),
        M('D41', 'parseAnnexB: the data starts after the NAL header', 'data: b.subarray(s, end)', 'data: b.subarray(s + 1, end)'),
        M('D42', 'parseAnnexB: the data includes the next byte', 'data: b.subarray(s, end)', 'data: b.subarray(s, end + 1)'),
        M('D43', 'analyze: SPS is not type 7', 'if (nals[i].type === 7) {', 'if (nals[i].type === 8) {'),
        M('D44', 'analyze: the last SPS wins', 'if (sps === null) sps = nals[i].data;', 'sps = nals[i].data;'),
        M('D45', 'analyze: an SPS is not a key access unit', "                    key = true;\n                    if (sps === null)", "                    if (sps === null)"),
        M('D46', 'analyze: IDR slice is type 1 (a P slice counts as key)', '} else if (nals[i].type === 5) {', '} else if (nals[i].type === 1) {'),
        M('D47', 'analyze: an IDR slice is not a key access unit', "                } else if (nals[i].type === 5) {\n                    key = true;\n                }", "                } else if (nals[i].type === 5) {\n                }"),
        M('D48', 'analyze: NAL count not reported', 'count: nals.length', 'count: 1'),
        M('D49', 'isKeyAccessUnit: SPS decides', 'return this.analyze(b).key;', 'return this.analyze(b).sps !== null;'),
        M('D50', 'avcCodecStringFromSps: null guard removed', "if (nal === null || nal === undefined || typeof nal.length !== 'number' || nal.length < 4) return null;", "if (nal === undefined || typeof nal.length !== 'number' || nal.length < 4) return null;"),
        M('D51', 'avcCodecStringFromSps: the length type check removed', "|| typeof nal.length !== 'number' || nal.length < 4) return null;", "|| nal.length < 4) return null;"),
        M('D52', 'avcCodecStringFromSps: 3 bytes are enough', 'nal.length < 4) return null;', 'nal.length < 3) return null;'),
        M('D53', 'avcCodecStringFromSps: 5 bytes are needed', 'nal.length < 4) return null;', 'nal.length < 5) return null;'),
        M('D54', 'avcCodecStringFromSps: forbidden_zero_bit not checked', "if ((nal[0] & 0x80) !== 0 || (nal[0] & 0x1F) !== 7) return null;", "if ((nal[0] & 0x1F) !== 7) return null;"),
        M('D55', 'avcCodecStringFromSps: NAL type not checked', "if ((nal[0] & 0x80) !== 0 || (nal[0] & 0x1F) !== 7) return null;", "if ((nal[0] & 0x80) !== 0) return null;"),
        M('D56', 'avcCodecStringFromSps: another NAL type', "(nal[0] & 0x1F) !== 7) return null;", "(nal[0] & 0x1F) !== 8) return null;"),
        M('D57', 'avcCodecStringFromSps: nal_ref_idc must be 3', "(nal[0] & 0x1F) !== 7) return null;", "(nal[0] & 0x7F) !== 0x67) return null;"),
        M('D58', 'avcCodecStringFromSps: no zero padding', "(v < 16 ? '0' : '')", "''"),
        M('D59', 'avcCodecStringFromSps: padding up to 14 only', "(v < 16 ? '0' : '')", "(v < 15 ? '0' : '')"),
        M('D60', 'avcCodecStringFromSps: padding up to 16', "(v < 16 ? '0' : '')", "(v < 17 ? '0' : '')"),
        M('D61', 'avcCodecStringFromSps: upper case hex', 'v.toString(16)', 'v.toString(16).toUpperCase()'),
        M('D62', 'avcCodecStringFromSps: prefix', "'avc1.' + hex(nal[1])", "'avc3.' + hex(nal[1])"),
        M('D63', 'avcCodecStringFromSps: bytes in another order', "hex(nal[1]) + hex(nal[2]) + hex(nal[3])", "hex(nal[1]) + hex(nal[3]) + hex(nal[2])"),
        M('D64', 'avcCodecStringFromSps: level taken from the next byte', "+ hex(nal[3]);", "+ hex(nal[4]);"),
        M('D65', 'sameBytes: null a not handled', 'if (a === null || b === null || a.length !== b.length) return false;', 'if (b === null || a.length !== b.length) return false;'),
        M('D66', 'sameBytes: null b not handled', 'if (a === null || b === null || a.length !== b.length) return false;', 'if (a === null || a.length !== b.length) return false;'),
        M('D67', 'sameBytes: lengths not compared', 'if (a === null || b === null || a.length !== b.length) return false;', 'if (a === null || b === null) return false;'),
        M('D68', 'sameBytes: the last byte is not compared', 'for (let i = 0; i < a.length; i++) {\n                if (a[i] !== b[i])', 'for (let i = 0; i < a.length - 1; i++) {\n                if (a[i] !== b[i])'),
        M('D69', 'sameBytes: result inverted', 'if (a[i] !== b[i]) return false;', 'if (a[i] === b[i]) return false;'),
        M('D70', 'toBytes: ArrayBuffer not recognised', "=== '[object ArrayBuffer]'", "=== '[object Blob]'"),
        M('D71', 'toBytes: views not accepted', '        if (ArrayBuffer.isView(data)) return new Uint8Array(data.buffer, data.byteOffset, data.byteLength);\n', ''),
        M('D72', 'toBytes: the offset of a view is ignored', 'new Uint8Array(data.buffer, data.byteOffset, data.byteLength)', 'new Uint8Array(data.buffer, 0, data.byteLength)'),
        M('D73', 'toBytes: the length of the whole buffer is used for a view', 'new Uint8Array(data.buffer, data.byteOffset, data.byteLength)', 'new Uint8Array(data.buffer, data.byteOffset, data.buffer.byteLength)'),
        M('D74', 'h264Available: EncodedVideoChunk not required', "typeof VideoDecoder === 'function' && typeof EncodedVideoChunk === 'function' &&\n            typeof VideoDecoder.isConfigSupported === 'function'", "typeof VideoDecoder === 'function' &&\n            typeof VideoDecoder.isConfigSupported === 'function'"),
        M('D75', 'h264Available: isConfigSupported not required (the call is inside try/catch)', "typeof VideoDecoder === 'function' && typeof EncodedVideoChunk === 'function' &&\n            typeof VideoDecoder.isConfigSupported === 'function'", "typeof VideoDecoder === 'function' && typeof EncodedVideoChunk === 'function'",
            'isConfigSupported が無い VideoDecoder では、呼び出しが TypeError になり、probeH264 の try/catch が「対応しない」にする。結果は同じ(二重の防御)'),
        M('D76', 'h264Available: availability not checked (the probe runs without EncodedVideoChunk)', "        if (!h264Available()) return Promise.resolve();\n", ''),
        M('D77', 'probe: other codec', "const H264_PROBE_CODEC = 'avc1.42c01f';", "const H264_PROBE_CODEC = 'avc1.640028';"),
        M('D78', 'probe: optimizeForLatency off', "VideoDecoder.isConfigSupported({ codec: H264_PROBE_CODEC, optimizeForLatency: true })", "VideoDecoder.isConfigSupported({ codec: H264_PROBE_CODEC, optimizeForLatency: false })"),
        M('D79', 'probe: an exception of isConfigSupported is not handled', "        } catch (err) {\n            return Promise.resolve();\n        }\n        return Promise.resolve(p)", "        } catch (err) {\n            throw err;\n        }\n        return Promise.resolve(p)"),
        M('D80', 'probe: support assumed', 'h264Supported = !!res && res.supported === true;', 'h264Supported = true;'),
        M('D81', 'probe: anything but false counts as supported', 'h264Supported = !!res && res.supported === true;', 'h264Supported = !!res && res.supported !== false;'),
        M('D82', 'probe: a truthy value counts as supported', 'h264Supported = !!res && res.supported === true;', 'h264Supported = !!res && !!res.supported;'),
        M('D83', 'probe: a missing answer is not handled', 'h264Supported = !!res && res.supported === true;', 'h264Supported = res.supported === true;'),
        M('D84', 'probe: a rejected probe is not handled', "            h264Supported = !!res && res.supported === true;\n        }, () => {});", "            h264Supported = !!res && res.supported === true;\n        });"),
        M('D85', 'probe: not started', '    h264Probe = probeH264();\n', ''),
        M('D86', 'waitForProbe: probe not awaited by fetchStatus', '                    await waitForProbe();', '                    void waitForProbe();'),
        M('D87', 'waitForProbe: no time limit', '            const t = setTimeout(resolve, H264_PROBE_WAIT_MS);', '            const t = 0;'),
        M('D88', 'waitForProbe: the limit is not cancelled when the probe settles', '                clearTimeout(t);\n                resolve();', '                resolve();'),
        M('D89', 'waitForProbe: a settled probe does not release the connection', '                clearTimeout(t);\n                resolve();', '                clearTimeout(t);'),
        M('D90', 'waitForProbe: other limit', 'const H264_PROBE_WAIT_MS = 1500;', 'const H264_PROBE_WAIT_MS = 1000;'),
        M('D91', 'setH264Shown: label text', "const H264_LABEL = ' / H.264（低帯域）';", "const H264_LABEL = ' / H.264';"),
        M('D92', 'setH264Shown: the shown flag is not kept', '        h264Shown = on;\n        elVideoStatus.textContent', '        elVideoStatus.textContent'),
        M('D93', 'setH264Shown: a change is not skipped (only a speed guard: the same text is written again)', '        if (h264Shown === on) return;\n', '',
            '同じ状態への呼び出しを飛ばすのは、書き換えを減らすためだけ。飛ばさなくても、同じ文字列が書かれるので結果は同じ'),
        M('D94', 'setH264Shown: the resolution is not kept in the line', "(videoStatus === null ? STATUS_PLAIN : VideoStatus.statusLine(videoStatus))", 'STATUS_PLAIN'),
        M('D95', 'setH264Shown: label for the other state', "(on ? H264_LABEL : '')", "(on ? '' : H264_LABEL)"),
        M('D96', 'setH264Shown: the label is never removed', "(on ? H264_LABEL : '')", 'H264_LABEL'),
        M('D97', 'setH264Shown: line before the first status', "(videoStatus === null ? STATUS_PLAIN : VideoStatus.statusLine(videoStatus))", "(videoStatus !== null ? STATUS_PLAIN : VideoStatus.statusLine(videoStatus))"),
        M('D98', 'setH264Shown: plain line text', "const STATUS_PLAIN = '接続完了 / 受信中';", "const STATUS_PLAIN = '接続完了';"),
        M('D99', 'resetH264: generation not bumped (a late isConfigSupported answer still creates a decoder)', '    function resetH264() {\n        h264Gen++;\n', '    function resetH264() {\n'),
        M('D100', 'resetH264: pending not cleared', '        h264Gen++;\n        h264Pending = 0;\n', '        h264Gen++;\n'),
        M('D101', 'resetH264: need-key not set (a decoder is made only by startH264Decoder, which decodes a key chunk and clears it)', '        h264Pending = 0;\n        h264NeedKey = true;\n        h264Shown = false;', '        h264Pending = 0;\n        h264Shown = false;',
            'リセットの後にデコーダができるのは startH264Decoder だけで、その最初の chunk は key で、そこで h264NeedKey が偽になる。リセットで真にしておくかどうかは、結果に影響しない(初期値と noteMjpeg / 溜まりでの真は別に持つ)'),
        M('D102', 'resetH264: shown flag not cleared', '        h264NeedKey = true;\n        h264Shown = false;\n        if (h264Dec !== null) {', '        h264NeedKey = true;\n        if (h264Dec !== null) {'),
        M('D103', 'resetH264: the decoder is not forgotten', '            h264Dec = null; // first: an error of the closing decoder is then ignored\n', ''),
        M('D104', 'resetH264: the decoder is not closed', '            try {\n                d.close();\n            } catch (err) {\n                // already closed\n            }\n        }\n    }', '        }\n    }'),
        M('D105', 'resetH264: an exception of close() is not handled', "            try {\n                d.close();\n            } catch (err) {\n                // already closed\n            }\n        }\n    }", "            d.close();\n        }\n    }"),
        M('D106', 'h264Fallback: not remembered', '        if (h264Failed) return;\n        h264Failed = true;\n', '        if (h264Failed) return;\n'),
        M('D107', 'h264Fallback: repeat guard removed (the callers cannot reach it twice)', '        if (h264Failed) return;\n        h264Failed = true;\n', '        h264Failed = true;\n',
            '2 回目の呼び出しは、世代の検査(待ちの結果)と、デコーダを忘れた後のエラーの無視で、そもそも来ない。あっても ws.close() が重なるだけ(二重の防御)'),
        M('D108', 'h264Fallback: decoder not closed', '        h264Failed = true;\n        resetH264();\n', '        h264Failed = true;\n'),
        M('D109', 'h264Fallback: connection not closed', '        if (ws) ws.close();\n    }\n\n    function drawH264Frame', '    }\n\n    function drawH264Frame'),
        M('D110', 'h264Fallback: connection guard removed (never called without one)', '        if (ws) ws.close();\n    }\n\n    function drawH264Frame', '        ws.close();\n    }\n\n    function drawH264Frame',
            '切断するとデコーダも閉じる(世代と「忘れたデコーダ」の検査で古い通知を捨てる)ので、ws が無いときにフォールバックには来ない。ws の検査は二重の防御'),
        M('D111', 'drawH264Frame: frames of an old decoder are drawn', "            if (h264Dec === null || h264Dec.decoder !== dec) return; // frame of a decoder that is gone\n", ''),
        M('D112', 'drawH264Frame: only a missing decoder is checked', "if (h264Dec === null || h264Dec.decoder !== dec) return;", "if (h264Dec === null) return;"),
        M('D113', 'drawH264Frame: a missing decoder is not checked (the property read then throws inside the try and the frame is dropped)', "if (h264Dec === null || h264Dec.decoder !== dec) return;", "if (h264Dec.decoder !== dec) return;",
            'h264Dec が null なら h264Dec.decoder が TypeError になり、すぐ下の catch が捨てて finally で close する。結果は同じ(二重の防御)'),
        M('D114', 'drawH264Frame: the coded size is used', 'const w = frame.displayWidth || frame.codedWidth;', 'const w = frame.codedWidth;'),
        M('D115', 'drawH264Frame: the coded height is used', 'const h = frame.displayHeight || frame.codedHeight;', 'const h = frame.codedHeight;'),
        M('D116', 'drawH264Frame: no fallback to the coded size', 'const w = frame.displayWidth || frame.codedWidth;', 'const w = frame.displayWidth;'),
        M('D117', 'drawH264Frame: no fallback to the coded height', 'const h = frame.displayHeight || frame.codedHeight;', 'const h = frame.displayHeight;'),
        M('D118', 'drawH264Frame: the width is set for every frame', 'if (elVideo.width !== w) elVideo.width = w;', 'elVideo.width = w;'),
        M('D119', 'drawH264Frame: the height is set for every frame', 'if (elVideo.height !== h) elVideo.height = h;', 'elVideo.height = h;'),
        M('D120', 'drawH264Frame: the width never changes', 'if (elVideo.width !== w) elVideo.width = w;', 'if (elVideo.width === w) elVideo.width = w;'),
        M('D121', 'drawH264Frame: the height never changes', 'if (elVideo.height !== h) elVideo.height = h;', 'if (elVideo.height === h) elVideo.height = h;'),
        M('D122', 'drawH264Frame: drawn at another position', 'ctx.drawImage(frame, 0, 0);\n            setH264Shown(true);', 'ctx.drawImage(frame, 1, 0);\n            setH264Shown(true);'),
        M('D123', 'drawH264Frame: the label is not shown', '            ctx.drawImage(frame, 0, 0);\n            setH264Shown(true);', '            ctx.drawImage(frame, 0, 0);'),
        M('D124', 'drawH264Frame: the frame is not closed', '        } finally {\n            frame.close();\n        }', '        } finally {\n        }'),
        M('D125', 'drawH264Frame: the frame is closed only when drawn', '            setH264Shown(true);\n        } catch (err) {\n            // a frame that cannot be drawn is dropped\n        } finally {\n            frame.close();\n        }', '            setH264Shown(true);\n            frame.close();\n        } catch (err) {\n            // a frame that cannot be drawn is dropped\n        }'),
        M('D126', 'drawH264Frame: a drawing error is thrown to the decoder', '        } catch (err) {\n            // a frame that cannot be drawn is dropped\n        } finally {', '        } catch (err) {\n            throw err;\n        } finally {'),
        M('D127', 'decodeH264: chunk types swapped', "type: key ? 'key' : 'delta'", "type: key ? 'delta' : 'key'"),
        M('D128', 'decodeH264: every chunk is a key chunk', "type: key ? 'key' : 'delta'", "type: 'key'"),
        M('D129', 'decodeH264: no time stamp', 'timestamp: h264Timestamp, data: bytes', 'timestamp: 0, data: bytes'),
        M('D130', 'decodeH264: the time stamp does not advance', '            h264Timestamp += H264_TIMESTAMP_STEP;\n', ''),
        M('D131', 'decodeH264: time stamp step 0', 'const H264_TIMESTAMP_STEP = 33333;', 'const H264_TIMESTAMP_STEP = 0;'),
        M('D132', 'decodeH264: another step (any increasing value works)', 'const H264_TIMESTAMP_STEP = 33333;', 'const H264_TIMESTAMP_STEP = 16667;',
            'ストリームに時刻が無く、B フレームもないので、増える値なら何でもよい'),
        M('D133', 'decodeH264: another buffer is sent', 'timestamp: h264Timestamp, data: bytes', 'timestamp: h264Timestamp, data: bytes.subarray(4)'),
        M('D134', 'decodeH264: a key chunk does not clear need-key', '            if (key) h264NeedKey = false;', '            if (!key) h264NeedKey = false;'),
        M('D135', 'decodeH264: need-key never cleared', '            if (key) h264NeedKey = false;\n', ''),
        M('D136', 'decodeH264: every chunk clears need-key (a delta chunk is sent only when it is already clear)', '            if (key) h264NeedKey = false;', '            h264NeedKey = false;',
            'delta chunk を送るのは h264NeedKey が既に偽のときだけなので、delta でも偽にして結果は同じ'),
        M('D137', 'decodeH264: a decode error is not a failure', '        } catch (err) {\n            h264Fallback();\n        }\n    }\n\n    // First IDR', '        } catch (err) {\n        }\n    }\n\n    // First IDR'),
        M('D138', 'startH264Decoder: the old decoder is not closed', '    function startH264Decoder(codec, sps, bytes) {\n        resetH264();\n', '    function startH264Decoder(codec, sps, bytes) {\n'),
        M('D139', 'startH264Decoder: not marked pending', '        h264Pending = gen;\n', ''),
        M('D140', 'startH264Decoder: a throwing isConfigSupported is not a failure', '        } catch (err) {\n            h264Fallback();\n            return;\n        }\n        Promise.resolve(p)', '        } catch (err) {\n            return;\n        }\n        Promise.resolve(p)'),
        M('D141', 'startH264Decoder: a throwing isConfigSupported is not caught', "        let p;\n        try {\n            p = VideoDecoder.isConfigSupported(config);\n        } catch (err) {\n            h264Fallback();\n            return;\n        }", "        const p = VideoDecoder.isConfigSupported(config);"),
        M('D142', 'startH264Decoder: a late answer is not ignored', '            if (gen !== h264Gen) return; // reset meanwhile\n', ''),
        M('D143', 'startH264Decoder: pending not cleared by the answer', '            h264Pending = 0;\n            if (!res', '            if (!res'),
        M('D144', 'startH264Decoder: an unsupported codec is not a failure', "            if (!res || res.supported !== true) {\n                h264Fallback();\n                return;\n            }", "            if (!res || res.supported !== true) {\n                return;\n            }"),
        M('D145', 'startH264Decoder: only an explicit "false" is a failure', 'if (!res || res.supported !== true) {', 'if (!res || res.supported === false) {'),
        M('D146', 'startH264Decoder: a missing answer is not handled', 'if (!res || res.supported !== true) {', 'if (res.supported !== true) {'),
        M('D147', 'startH264Decoder: the decoder gets another frame owner', 'output: (frame) => drawH264Frame(dec, frame),', 'output: (frame) => drawH264Frame(null, frame),'),
        M('D148', 'startH264Decoder: errors of any decoder are a failure', 'if (h264Dec !== null && h264Dec.decoder === dec) h264Fallback();', 'if (h264Dec !== null) h264Fallback();'),
        M('D149', 'startH264Decoder: an error after the reset throws', 'if (h264Dec !== null && h264Dec.decoder === dec) h264Fallback();', 'if (h264Dec.decoder === dec) h264Fallback();'),
        M('D150', 'startH264Decoder: errors of the decoder are not a failure', 'if (h264Dec !== null && h264Dec.decoder === dec) h264Fallback();', 'if (h264Dec !== null && h264Dec.decoder === dec) return;'),
        M('D151', 'startH264Decoder: not configured', '                dec.configure(config);\n', ''),
        M('D152', 'startH264Decoder: optimizeForLatency off', "const config = { codec: codec, optimizeForLatency: true };", "const config = { codec: codec, optimizeForLatency: false };"),
        M('D153', 'startH264Decoder: a fixed codec string', "const config = { codec: codec, optimizeForLatency: true };", "const config = { codec: 'avc1.42c01f', optimizeForLatency: true };"),
        M('D154', 'startH264Decoder: a decoder that cannot be configured is left open', "                if (dec !== null) {\n                    try {\n                        dec.close(); // made but not usable\n                    } catch (err2) {\n                        // already closed\n                    }\n                }\n", ''),
        M('D155', 'startH264Decoder: close() of the unusable decoder without the null check (the TypeError is caught by the try next to it)', "                if (dec !== null) {\n                    try {\n                        dec.close(); // made but not usable", "                {\n                    try {\n                        dec.close(); // made but not usable",
            'dec が null(コンストラクタの例外)なら dec.close() は TypeError だが、すぐ隣の try/catch が捨てる。null の検査は二重の防御'),
        M('D156', 'startH264Decoder: a configure error is not a failure', "                }\n                h264Fallback();\n                return;\n            }\n            h264Dec =", "                }\n                return;\n            }\n            h264Dec ="),
        M('D157', 'startH264Decoder: the codec of the decoder is not kept', 'h264Dec = { decoder: dec, codec: codec, sps: sps.slice() };', 'h264Dec = { decoder: dec, codec: null, sps: sps.slice() };'),
        M('D158', 'startH264Decoder: the SPS of the decoder is not kept', 'h264Dec = { decoder: dec, codec: codec, sps: sps.slice() };', 'h264Dec = { decoder: dec, codec: codec, sps: new Uint8Array(0) };'),
        M('D159', 'startH264Decoder: the SPS is kept as a view (not a copy)', 'sps: sps.slice() };', 'sps: sps };',
            'SPS をコピーせず、メッセージのバッファの view のまま持っても、比較の結果は変わらない(コピーは、バッファを保持し続けないため)'),
        M('D160', 'startH264Decoder: the first IDR is not decoded', '            h264Dec = { decoder: dec, codec: codec, sps: sps.slice() }; // a copy: the message buffer is not kept alive\n            decodeH264(bytes, true);', '            h264Dec = { decoder: dec, codec: codec, sps: sps.slice() }; // a copy: the message buffer is not kept alive'),
        M('D161', 'startH264Decoder: the first IDR is sent as a delta chunk', '            decodeH264(bytes, true);\n        }, () => {', '            decodeH264(bytes, false);\n        }, () => {'),
        M('D162', 'startH264Decoder: a rejected check is not a failure', '        }, () => {\n            if (gen === h264Gen) h264Fallback();\n        });', '        }, () => {\n        });'),
        M('D163', 'startH264Decoder: a late rejection is a failure', 'if (gen === h264Gen) h264Fallback();', 'h264Fallback();'),
        M('D164', 'feedH264: the failed flag is not checked', '        if (h264Failed || h264Pending !== 0) return;', '        if (h264Pending !== 0) return;'),
        M('D165', 'feedH264: a pending configure does not hold the stream', '        if (h264Failed || h264Pending !== 0) return;', '        if (h264Failed) return;'),
        M('D166', 'feedH264: an access unit without a NAL is not dropped', '        if (au.count === 0) return; // no NAL at all: garbage\n', ''),
        M('D167', 'feedH264: P frames are decoded before the first IDR (the decoder check removed; need-key is always set while there is no decoder)', '            if (h264Dec === null || h264NeedKey) return; // no IDR yet', '            if (h264NeedKey) return; // no IDR yet',
            'デコーダが無いとき h264NeedKey は必ず真(初期値と resetH264 が真にする)なので、デコーダの検査は二重の防御。D101(resetH264 の NeedKey)とは、どちらか一方だけなら同値(両方外すと、デコーダなしの P で落ちる)'),
        M('D168', 'feedH264: P frames are decoded while waiting for an IDR', '            if (h264Dec === null || h264NeedKey) return; // no IDR yet', '            if (h264Dec === null) return; // no IDR yet'),
        M('D169', 'feedH264: queue limit is >', 'decodeQueueSize >= H264_MAX_QUEUE', 'decodeQueueSize > H264_MAX_QUEUE'),
        M('D170', 'feedH264: queue limit 3 -> 4', 'const H264_MAX_QUEUE = 3;', 'const H264_MAX_QUEUE = 4;'),
        M('D171', 'feedH264: queue limit 3 -> 2', 'const H264_MAX_QUEUE = 3;', 'const H264_MAX_QUEUE = 2;'),
        M('D172', 'feedH264: a long queue does not set need-key', '                h264NeedKey = true; // falling behind: wait for the next IDR instead of piling up delay\n', ''),
        M('D173', 'feedH264: a long queue does not drop the frame', "                h264NeedKey = true; // falling behind: wait for the next IDR instead of piling up delay\n                return;\n            }", "                h264NeedKey = true; // falling behind: wait for the next IDR instead of piling up delay\n            }"),
        M('D174', 'feedH264: a decoded P frame goes on as a key frame', "            decodeH264(bytes, false);\n            return;\n        }", "            decodeH264(bytes, false);\n        }"),
        M('D175', 'feedH264: a P frame is sent as a key chunk', 'decodeH264(bytes, false);', 'decodeH264(bytes, true);'),
        M('D176', 'feedH264: an IDR slice without an SPS is decoded without a decoder', "            if (h264Dec !== null) decodeH264(bytes, true); // an IDR slice without an SPS: the decoder is configured\n            return;", "            decodeH264(bytes, true);\n            return;"),
        M('D177', 'feedH264: an IDR slice without an SPS is dropped', "            if (h264Dec !== null) decodeH264(bytes, true); // an IDR slice without an SPS: the decoder is configured\n", ''),
        M('D178', 'feedH264: an IDR slice without an SPS goes on to the SPS handling (a missing SPS gives no codec string)', "            if (h264Dec !== null) decodeH264(bytes, true); // an IDR slice without an SPS: the decoder is configured\n            return;\n        }", "            if (h264Dec !== null) decodeH264(bytes, true); // an IDR slice without an SPS: the decoder is configured\n        }",
            'SPS が無い(null)と、次の avcCodecStringFromSps(null) が null を返し、その次の行で捨てられる。ここの return は二重の防御'),
        M('D179', 'feedH264: a broken SPS starts a decoder', '        if (codec === null) return;\n', ''),
        M('D180', 'feedH264: the decoder is reused without a decoder', 'if (h264Dec !== null && h264Dec.codec === codec && H264.sameBytes(h264Dec.sps, au.sps)) {', 'if (h264Dec.codec === codec && H264.sameBytes(h264Dec.sps, au.sps)) {'),
        M('D181', 'feedH264: the codec string is not compared (the same SPS bytes always give the same codec string)', 'if (h264Dec !== null && h264Dec.codec === codec && H264.sameBytes(h264Dec.sps, au.sps)) {', 'if (h264Dec !== null && H264.sameBytes(h264Dec.sps, au.sps)) {',
            '同じ SPS のバイト列からは、同じ codec 文字列ができるので、codec 文字列の比較は、SPS の比較に含まれる(二重の防御)'),
        M('D182', 'feedH264: the SPS is not compared (only the codec string)', 'if (h264Dec !== null && h264Dec.codec === codec && H264.sameBytes(h264Dec.sps, au.sps)) {', 'if (h264Dec !== null && h264Dec.codec === codec) {'),
        M('D183', 'feedH264: the codec string is not compared in the other direction', 'if (h264Dec !== null && h264Dec.codec === codec && H264.sameBytes(h264Dec.sps, au.sps)) {', 'if (h264Dec !== null && h264Dec.codec !== codec && H264.sameBytes(h264Dec.sps, au.sps)) {'),
        M('D184', 'feedH264: a decoder is rebuilt for every IDR', "            decodeH264(bytes, true);\n            return;\n        }\n        startH264Decoder", "            decodeH264(bytes, true);\n        }\n        startH264Decoder"),
        M('D185', 'feedH264: the IDR with the same SPS is sent as a delta chunk', "            decodeH264(bytes, true);\n            return;\n        }\n        startH264Decoder", "            decodeH264(bytes, false);\n            return;\n        }\n        startH264Decoder"),
        M('D186', 'feedH264: a new decoder gets the wrong buffer', 'startH264Decoder(codec, au.sps, bytes);', 'startH264Decoder(codec, au.sps, bytes.subarray(1));'),
        M('D187', 'noteMjpeg: need-key not set', '    function noteMjpeg() {\n        h264NeedKey = true;\n', '    function noteMjpeg() {\n'),
        M('D188', 'noteMjpeg: the label stays', '        setH264Shown(false);\n    }', '    }'),
        M('D189', 'connect: accept even when not supported', 'const acceptH264 = h264Supported && !h264Failed;', 'const acceptH264 = !h264Failed;'),
        M('D190', 'connect: accept even after a failure', 'const acceptH264 = h264Supported && !h264Failed;', 'const acceptH264 = h264Supported;'),
        M('D191', 'connect: accept is never sent', 'VideoTarget.resolve(location, undefined), acceptH264);', 'VideoTarget.resolve(location, undefined), false);'),
        M('D192', 'connect: accept is always sent', 'VideoTarget.resolve(location, undefined), acceptH264);', 'VideoTarget.resolve(location, undefined), true);'),
        M('D193', 'connect: binaryType stays blob when H.264 is accepted', '        if (acceptH264) ws.binaryType = "arraybuffer";', '        if (false) ws.binaryType = "arraybuffer";'),
        M('D194', 'connect: binaryType is arraybuffer when H.264 is not accepted', '        if (acceptH264) ws.binaryType = "arraybuffer";', '        if (!acceptH264) ws.binaryType = "arraybuffer";'),
        M('D195', 'onmessage: the kind of the message is not looked at', '            if (acceptH264) {\n                // JPEG', '            if (false) {\n                // JPEG'),
        M('D196', 'onmessage: a binary that is not a JPEG is decoded as one', "                if (kind !== 'jpeg') return;\n", ''),
        M('D197', 'onmessage: an H.264 message goes on to the JPEG path (it is dropped by the next line)', '                    feedH264(bytes);\n                    return;\n                }', '                    feedH264(bytes);\n                }',
            'H.264 のメッセージは、次の kind !== \'jpeg\' の検査で捨てられる。ここの return は二重の防御'),
        M('D198', 'onmessage: H.264 is not fed to the decoder', '                    feedH264(bytes);\n', ''),
        M('D199', 'onmessage: a JPEG does not reset the H.264 state', '                noteMjpeg();\n', ''),
        M('D200', 'onmessage: the JPEG is not wrapped in a Blob', "frameData = new Blob([bytes], { type: 'image/jpeg' });", 'frameData = e.data;'),
        M('D201', 'onmessage: the Blob has no type', "new Blob([bytes], { type: 'image/jpeg' })", 'new Blob([bytes])'),
        M('D202', 'onmessage: the Blob has another type', "{ type: 'image/jpeg' }", "{ type: 'image/png' }"),
        M('D203', 'onmessage: a message that is not binary is classified (classify handles null)', "const kind = bytes === null ? null : H264.classify(bytes);", "const kind = H264.classify(bytes);",
            'classify が null を受けて null を返す(D07)ので、bytes の null の検査は二重の防御'),
        M('D205', 'onmessage: createImageBitmap gets the message, not the Blob', 'const bmp = await createImageBitmap(frameData);', 'const bmp = await createImageBitmap(e.data);'),
        M('D206', 'reset: the status reset does not reset H.264', '        resetH264(); // the decoder and the "H.264" label go with the connection\n', ''),
        M('D207', 'status: the label is dropped by the next status', '        if (h264Shown) elVideoStatus.textContent += H264_LABEL;\n', ''),
        M('D208', 'status: the label replaces the resolution', '        if (h264Shown) elVideoStatus.textContent += H264_LABEL;', '        if (h264Shown) elVideoStatus.textContent = STATUS_PLAIN + H264_LABEL;'),
        M('D209', 'status: the label is shown for a connection without H.264', '        if (h264Shown) elVideoStatus.textContent += H264_LABEL;', '        elVideoStatus.textContent += H264_LABEL;'),
    ];
}

(async () => {
    const source = fs.readFileSync(APP_JS, 'utf8');
    let status = 0;
    const problems = await checkAll(source);
    problems.forEach((p) => console.log('FAIL:', p));
    if (problems.length) status = 1;
    else console.log('video viewer ok (' + RESOLVE_CASES.length + ' resolve cases, ' + HINT_CASES.length + ' hint cases, ' + TRUST_URL_CASES.length + ' trust-url cases, ' + PARSE_CASES.length + ' status-parse cases, ' + CHANGE_CASES.length + ' resolution-change cases, ' + CLASSIFY_CASES.length + ' classify cases, ' + PARSE_H264_CASES.length + ' annex-b cases, ' + ANALYZE_CASES.length + ' key-unit cases, ' + CODEC_CASES.length + ' codec-string cases, browser scenarios)');

    if (process.argv.includes('--mutations')) {
        const only = process.argv.filter((a) => /^[A-Z][0-9]+$/.test(a)); // 例: --mutations D114 D155(指定したものだけ)
        const list = mutantList().filter((m) => only.length === 0 || only.includes(m.id));
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
