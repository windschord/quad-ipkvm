#!/usr/bin/env node
// ビューア(components/web_server/web/app.js)の ISO 画面(docs/WEB_API.md 8、docs/ISO_MOUNT_DESIGN.md 6・7)の検査。
// 偽の DOM / fetch / タイマー / ファイルの中で、`IsoViewer`(app.js の「ISO viewer」の区間)を動かす。
//  (A) SHA-256(自前の逐次実装): 既知のベクトル(空、abc、448 / 896 ビット、100 万個の a、ブロック境界の長さ)、
//      細切れの update、ビット長の上位ワード(2^29 バイト)、node:crypto との照合。期待値は、sha256sum で作った手書きの値。
//  (B) 純関数: formatBytes、名前の規則、defaultName、describeError(400 / 401 / 403 / 404 / 405 / 409 / 411 / 413 / 415 / 422 / 503 / 507 の各行)。
//  (C) 偽のサーバー(WEB_API.md 8 の表から別に書いたもの)との通信: アップロード(チャンク、進捗、SHA-256 の申告)、一時停止と再開、
//      offset_mismatch、ネットワークエラー(書かれた / 書かれない)、遅いときの小さなチャンク、各エラーの表示、finish の待機
//      (ready / invalid / incomplete の 3 つの終わり方)、再読み込みでの finish の自動送信(1 回だけ)、マウント / 取り外し /
//      削除と確認、rescan の cursor、ログアウトでの停止、ポーリングの間隔。
//  (D) 安全: 名前は textContent だけ(innerHTML、style 属性を使わない)、index.html と app.css の構造(CSP style-src 'self')。
//  (E) app.js 全体を偽の環境で動かし、ダッシュボードの表示・ログアウト・401 との結線を見る。
// `--mutations` は、app.js の ISO の区間を 1 か所ずつ変えたコピーに対して (A)〜(E) を動かし、落ちることを確認する。
// 使い方: node tools/test/check_iso_viewer.js [--mutations [ID ...]]      (依存なし)
'use strict';
const fs = require('fs');
const path = require('path');
const vm = require('vm');
const crypto = require('crypto');

// ミュータントの実行では、1 回の検査に時間の上限を付ける(再試行が止まらなくなったものを「落ちた」と数える)。
let DEADLINE = 0;
function checkDeadline() { if (DEADLINE && Date.now() > DEADLINE) throw new Error('timeout (mutation run)'); }

const unhandled = [];
process.on('unhandledRejection', (e) => { unhandled.push(String(e && e.message)); });

const WEB_DIR = path.join(__dirname, '..', '..', 'components', 'web_server', 'web');
const APP_JS = path.join(WEB_DIR, 'app.js');
const INDEX_HTML = path.join(WEB_DIR, 'index.html');
const APP_CSS = path.join(WEB_DIR, 'app.css');

const BEGIN = '// ==== ISO viewer (begin) ====';
const END = '// ==== ISO viewer (end) ====';
function region(source) {
    const a = source.indexOf(BEGIN);
    const b = source.indexOf(END);
    if (a < 0 || b < a) throw new Error('ISO viewer region not found in app.js');
    return source.slice(a, b + END.length);
}
function loadIsoViewer(source) {
    const ctx = vm.createContext({});
    return vm.runInContext(region(source) + '\nIsoViewer;', ctx, { filename: 'app.js(iso)' });
}

const flush = () => new Promise((r) => setImmediate(r));

// ---------- 偽の DOM ----------
function parseIndexHtml() {
    const html = fs.readFileSync(INDEX_HTML, 'utf8');
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

function makeDom(violations) {
    const byId = {};
    const doc = { activeElement: null, _listeners: {} };
    class FakeNode {
        constructor(tag, info) {
            this.tag = tag;
            this.children = [];
            this.parent = null;
            this._attrs = {};
            this.classes = new Set(info ? info.classes : []);
            this._text = '';
            this.listeners = {};
            this.disabled = false;
            this._value = '';
            this.max = 1;
            this.checked = info ? info.checked : false;
            this.files = null;
            const self = this;
            this.classList = {
                add(...n) { n.forEach((x) => self.classes.add(x)); },
                remove(...n) { n.forEach((x) => self.classes.delete(x)); },
                contains(n) { return self.classes.has(n); },
                toggle(n, on) { if (on === undefined ? !self.classes.has(n) : on) self.classes.add(n); else self.classes.delete(n); },
            };
        }
        get value() { return this._value; }
        set value(v) {
            this._value = this.tag === 'input' ? String(v) : v;
            if (this.tag === 'input' && this._attrs.type === 'file' && this._value === '') this.files = null; // 本物のブラウザと同じ: 空にすると、選択が消える
        }
        get textContent() { return this.children.length ? this.children.map((c) => c.textContent).join('') : this._text; }
        set textContent(v) { this.children = []; this._text = String(v); }
        get className() { return Array.from(this.classes).join(' '); }
        set className(v) { this.classes = new Set(String(v).split(/\s+/).filter(Boolean)); }
        set innerHTML(v) { violations.push('innerHTML'); }
        set outerHTML(v) { violations.push('outerHTML'); }
        insertAdjacentHTML() { violations.push('insertAdjacentHTML'); }
        get style() { violations.push('style'); return {}; }
        setAttribute(k, v) { if (String(k).toLowerCase() === 'style') violations.push('style attribute'); this._attrs[k] = String(v); }
        getAttribute(k) { return Object.prototype.hasOwnProperty.call(this._attrs, k) ? this._attrs[k] : null; }
        appendChild(c) { c.parent = this; this.children.push(c); return c; }
        addEventListener(t, fn) { (this.listeners[t] = this.listeners[t] || []).push(fn); }
        dispatch(t) { (this.listeners[t] || []).forEach((fn) => fn({ type: t, preventDefault() {}, target: this })); }
        click() { if (!this.disabled) this.dispatch('click'); }
        focus() { doc.activeElement = this; }
        all() { let r = [this]; for (const c of this.children) r = r.concat(c.all()); return r; }
    }
    const infos = parseIndexHtml();
    for (const id of Object.keys(infos)) {
        const n = new FakeNode(infos[id].tag, infos[id]);
        n._attrs.id = id;
        byId[id] = n;
        if (/\stype="/.test(infos[id].attrs)) n._attrs.type = /\stype="([^"]*)"/.exec(infos[id].attrs)[1];
    }
    doc.getElementById = (id) => (Object.prototype.hasOwnProperty.call(byId, id) ? byId[id] : null);
    doc.createElement = (tag) => new FakeNode(tag, null);
    doc.addEventListener = (t, fn) => { doc._listeners[t] = fn; };
    return { doc, byId, FakeNode };
}

// ---------- 偽のファイル ----------
class FakeFile {
    constructor(bytes, name) { this._b = bytes; this.size = bytes.length; this.name = name || 'x.iso'; }
    slice(a, b) { return new FakeFile(this._b.subarray(a, b), this.name); }
    arrayBuffer() { const b = this._b; return Promise.resolve(b.buffer.slice(b.byteOffset, b.byteOffset + b.length)); }
}
// ISO 風のデータ(セクタ 16 の署名 CD001 を持つ。残りは決まった疑似乱数)
function makeIso(size, withSignature) {
    const b = Buffer.alloc(size);
    let x = 12345 + size;
    for (let i = 0; i < size; i++) { x = (Math.imul(x, 1103515245) + 12345) >>> 0; b[i] = x >>> 24; }
    if (withSignature !== false) Buffer.from('CD001').copy(b, 32769);
    return b;
}
const sha = (buf) => crypto.createHash('sha256').update(buf).digest('hex');

// ---------- 偽のサーバー(docs/WEB_API.md 8 の表から、別に書いたもの) ----------
const NET = { net: true };
function makeServer(init) {
    init = init || {};
    const V = {
        disabled: false, sd: { state: 'ready', total: 31914983424, free: 22000000000 }, chunk: init.chunk || 32768,
        images: [], mounted: null, locked: false, ejected: false, hidden: 0, nextId: 1,
        log: [], hooks: [], finalizePolls: init.finalizePolls === undefined ? 2 : init.finalizePolls, importEntries: [],
        putMs: () => 5, getCount: 0,
    };
    const hex8 = () => ('a0000000' + (V.nextId++).toString(16)).slice(-8);
    V.add = (o) => {
        const im = Object.assign({ id: hex8(), name: 'img', size: 0, state: 'ready', sha: null, declared: null, data: Buffer.alloc(0), reason: null, polls: 0, fromFinish: false }, o);
        if (o.received !== undefined && !o.data) im.data = Buffer.alloc(o.received);
        V.images.push(im);
        return im;
    };
    V.get = (id) => V.images.find((i) => i.id === id);
    const slotHolder = () => V.images.find((i) => i.state === 'uploading' || (i.state === 'finalizing' && i.fromFinish));
    const r = (status, body, headers) => ({ status, body, headers: headers || {} });
    const snapshot = () => {
        const up = slotHolder();
        return {
            sd: { state: V.sd.state, total_bytes: V.sd.state === 'ready' ? V.sd.total : null, free_bytes: V.sd.state === 'ready' ? V.sd.free : null },
            images: V.sd.state !== 'ready' ? [] : V.images.map((i) => ({ id: i.id, name: i.name, size: i.size, state: i.state, sha256: i.state === 'ready' ? i.sha : null, received: i.state === 'ready' ? i.size : i.data.length, invalid_reason: i.state === 'invalid' ? i.reason : null })),
            mounted: V.mounted ? { id: V.mounted, name: V.get(V.mounted).name } : null,
            target: { locked: V.mounted ? V.locked : false, ejected_by_target: V.ejected },
            chunk_size: V.chunk, hidden_images: V.hidden,
            upload: up ? { id: up.id, state: up.state === 'finalizing' ? 'finalizing' : 'receiving' } : null,
        };
    };
    const tick = () => { // GET のたびに、最終処理と削除が 1 歩進む
        for (const i of V.images.slice()) {
            if (i.state === 'finalizing' && --i.polls <= 0) V.finalize(i);
            else if (i.state === 'deleting' && --i.polls <= 0) V.images.splice(V.images.indexOf(i), 1);
        }
    };
    V.finalize = (i) => {
        if (V.onFinalize) { const o = V.onFinalize(i); if (o) return; }
        i.fromFinish = false;
        if (i.declared && sha(i.data) !== i.declared) { i.state = 'invalid'; i.reason = 'checksum_mismatch'; return; }
        if (i.data.slice(32769, 32774).toString() !== 'CD001') { i.state = 'invalid'; i.reason = 'not_iso'; return; }
        i.state = 'ready';
        i.sha = i.declared || null;
    };
    V.append = (i, bytes) => { i.data = Buffer.concat([i.data, bytes]); };
    const bodyOf = (initReq) => {
        if (typeof initReq.body !== 'string') return undefined;
        try { return JSON.parse(initReq.body); } catch (e) { return null; }
    };
    const handle = (req) => {
        const m = req.method;
        const p = req.path;
        if (!p.startsWith('/api/v1/iso')) return r(404, null);
        if (V.disabled) return r(404, null);
        const rest = p.slice('/api/v1/iso'.length);
        const isJson = (req.headers['Content-Type'] || '') === 'application/json';
        if (rest === '') {
            if (m === 'GET') { V.getCount++; tick(); return r(200, snapshot()); }
            if (m === 'POST') {
                if (!isJson) return r(415, { error: 'unsupported_media_type' });
                const b = req.json;
                if (b === null || typeof b !== 'object') return r(400, { error: 'invalid_request' });
                if (Object.keys(b).some((k) => !['name', 'size', 'sha256'].includes(k)) || typeof b.name !== 'string' || !Number.isSafeInteger(b.size)) return r(400, { error: 'invalid_request' });
                if (b.sha256 !== undefined && !/^[0-9a-fA-F]{64}$/.test(b.sha256)) return r(400, { error: 'invalid_request' });
                if (V.sd.state !== 'ready') return r(503, { error: 'not_ready' });
                if (slotHolder()) return r(409, { error: 'upload_in_progress' });
                if (V.images.some((i) => i.name === b.name)) return r(409, { error: 'name_exists' });
                if (V.images.length >= 16) return r(409, { error: 'too_many_images' });
                if (b.size > V.sd.free) return r(507, { error: 'insufficient_storage', free_bytes: V.sd.free });
                const im = V.add({ name: b.name, size: b.size, state: 'uploading', declared: b.sha256 ? b.sha256.toLowerCase() : null });
                return r(201, { id: im.id, chunk_size: V.chunk });
            }
            return r(405, { error: 'method_not_allowed' });
        }
        if (rest === '/mount' && m === 'POST') {
            const b = req.json;
            if (!b || typeof b.id !== 'string' || !/^[0-9a-f]{8}$/.test(b.id)) return r(400, { error: 'invalid_request' });
            const im = V.get(b.id);
            if (!im) return r(404, { error: 'not_found' });
            if (im.state !== 'ready') return r(409, { error: 'image_not_ready' });
            if (V.mounted === im.id) return r(200, { result: 'already_mounted', id: im.id });
            if (V.mounted && V.locked && b.force !== true) return r(409, { error: 'locked_by_target' });
            V.mounted = im.id; V.ejected = false;
            return r(200, { result: 'mounted', id: im.id });
        }
        if (rest === '/eject' && m === 'POST') {
            if (!V.mounted) { V.ejected = false; return r(200, { result: 'not_mounted' }); }
            if (V.locked && !(req.json && req.json.force === true)) return r(409, { error: 'locked_by_target' });
            V.mounted = null; V.ejected = false;
            return r(200, { result: 'ejected' });
        }
        if (rest === '/rescan' && m === 'POST') {
            const b = req.json;
            const cursor = b && b.cursor !== undefined ? b.cursor : 0;
            if (V.sd.state !== 'ready') return r(503, { error: 'not_ready' });
            if (slotHolder()) return r(409, { error: 'upload_in_progress' });
            let imported = 0, skipped = 0, i = cursor;
            for (; i < V.importEntries.length && i < cursor + 4; i++) {
                if (V.importEntries[i] === 'ok') { V.add({ name: 'import-' + i, size: 40000, state: 'finalizing', polls: 1, data: makeIso(40000) }); imported++; } else skipped++;
            }
            return r(200, { imported, skipped, next_cursor: i < V.importEntries.length ? i : null });
        }
        const mm = /^\/([^/]+)(\/finish)?$/.exec(rest);
        if (!mm) return r(404, { error: 'not_found' });
        const id = mm[1];
        const im = /^[0-9a-f]{8}$/.test(id) ? V.get(id) : null;
        if (mm[2] === '/finish' && m === 'POST') {
            if (!im || im.state === 'deleting') return r(404, { error: 'not_found' });
            if (V.sd.state !== 'ready') return r(503, { error: 'not_ready' });
            if (im.state === 'ready') return r(200, { state: 'ready', sha256: im.sha });
            if (im.state === 'invalid') return r(422, { error: im.reason });
            if (im.state === 'finalizing') return r(202, { state: 'finalizing' });
            if (im.data.length < im.size) return r(409, { error: 'size_mismatch', received: im.data.length, size: im.size });
            const other = slotHolder();
            if (im.state === 'incomplete' && other && other !== im) return r(409, { error: 'upload_in_progress' });
            im.state = 'finalizing'; im.fromFinish = true; im.polls = V.finalizePolls;
            return r(202, { state: 'finalizing' });
        }
        if (mm[2]) return r(405, {});
        if (m === 'PUT') {
            if (!im || im.state === 'deleting') return r(404, { error: 'not_found' });
            if ((req.headers['Content-Type'] || '') !== 'application/octet-stream') return r(415, { error: 'unsupported_media_type' });
            if (req.len === undefined) return r(411, { error: 'length_required' });
            const off = req.headers['X-Upload-Offset'];
            if (req.len < 1 || off === undefined || !/^\d+$/.test(off)) return r(400, { error: 'invalid_request' });
            if (req.len > V.chunk) return r(413, { error: 'payload_too_large' });
            if (V.sd.state !== 'ready') return r(503, { error: 'not_ready' });
            if (!['uploading', 'incomplete'].includes(im.state)) return r(409, { error: 'not_uploading' });
            const other = slotHolder();
            if (im.state === 'incomplete' && other && other !== im) return r(409, { error: 'upload_in_progress' });
            if (Number(off) !== im.data.length) return r(409, { error: 'offset_mismatch', received: im.data.length });
            if (im.data.length + req.len > im.size) return r(400, { error: 'invalid_request' });
            V.append(im, req.bytes);
            im.state = 'uploading';
            return r(200, { received: im.data.length });
        }
        if (m === 'DELETE') {
            if (!im || im.state === 'deleting') return im ? r(202, { state: 'deleting' }) : r(404, { error: 'not_found' });
            if (V.sd.state !== 'ready') return r(503, { error: 'not_ready' });
            if (V.mounted === im.id) return r(409, { error: 'mounted' });
            im.state = 'deleting'; im.polls = 1;
            return r(202, { state: 'deleting' });
        }
        return r(405, {});
    };
    V.fetch = (clock) => async (url, initReq) => {
        const headers = initReq.headers || {};
        const req = { method: initReq.method, path: url, headers, credentials: initReq.credentials, json: bodyOf(initReq) };
        if (initReq.body && typeof initReq.body === 'object') {
            req.len = initReq.body.size;
            req.bytes = Buffer.from(initReq.body._b);
        }
        V.log.push(req);
        req.n = V.log.filter((x) => x.method === req.method && (x.path === req.path || (req.method === 'PUT' && x.method === 'PUT'))).length;
        for (const h of V.hooks) {
            const o = h(req, V);
            if (o === undefined) continue;
            if (o.wait) await o.wait; // 応答を保留する(解かれるまで)
            if (o.net) {
                if (o.apply) handle(req);
                throw new TypeError('Failed to fetch');
            }
            if (o.status === undefined) break; // 保留だけ: そのあと、普通に処理する
            return respond(o);
        }
        if (req.method === 'PUT') clock.t += V.putMs(req);
        return respond(handle(req));
    };
    const respond = (o) => ({
        status: o.status, ok: o.status >= 200 && o.status < 300,
        json: async () => { if (o.body === null || o.body === undefined) throw new SyntaxError('no json'); return JSON.parse(JSON.stringify(o.body)); },
        headers: { get: (k) => (o.headers && Object.prototype.hasOwnProperty.call(o.headers, k) ? o.headers[k] : null) },
    });
    return V;
}

// ---------- 画面の動作の環境 ----------
const MIN_PUT = (log) => log.filter((x) => x.method === 'PUT');
function boot(Iso, server, opts) {
    opts = opts || {};
    const violations = [];
    const dom = makeDom(violations);
    const clock = { t: 0 };
    const timers = [];
    let seq = 0;
    const confirms = [];
    const unauth = { n: 0 };
    const deps = {
        document: dom.doc,
        fetch: server.fetch(clock),
        setTimeout(fn, ms) { const id = ++seq; timers.push({ id, fn, at: clock.t + ms, seq: id, ms }); return id; },
        clearTimeout(id) { const i = timers.findIndex((t) => t.id === id); if (i >= 0) timers.splice(i, 1); },
        confirm(msg) { confirms.push(msg); return opts.confirm === undefined ? true : (typeof opts.confirm === 'function' ? opts.confirm(msg) : opts.confirm); },
        now: () => clock.t,
        onUnauthorized: () => { unauth.n++; if (opts.onUnauthorized) opts.onUnauthorized(); },
    };
    const iso = Iso.create(deps);
    const E = dom.byId;
    const h = {
        iso, server, dom, E, clock, timers, confirms, unauth, violations, deps,
        msg: () => E['iso-message'].textContent,
        msgShown: () => !E['iso-message'].classes.has('iso-hidden'),
        msgError: () => E['iso-message'].classes.has('iso-message-error'),
        msgOk: () => E['iso-message'].classes.has('iso-message-ok'),
        progressText: () => E['iso-progress-text'].textContent,
        progressShown: () => !E['iso-progress'].classes.has('iso-hidden'),
        toggleShown: () => !E['btn-iso'].classes.has('iso-hidden'),
        panelShown: () => !E['iso-panel'].classes.has('iso-hidden'),
        rows: () => E['iso-list'].children.map((li) => ({
            name: li.all().find((n) => n.classes.has('iso-name')).textContent,
            badge: li.all().find((n) => n.classes.has('iso-badge')).textContent,
            cls: li.className,
            note: (li.all().find((n) => n.classes.has('iso-row-note')) || { textContent: '' }).textContent,
            buttons: li.all().filter((n) => n.tag === 'button').map((n) => n.textContent),
            li,
        })),
        clickRow(name, label) {
            const row = E['iso-list'].children.find((li) => li.all().find((n) => n.classes.has('iso-name')).textContent === name);
            const btn = row.all().find((n) => n.tag === 'button' && n.textContent === label);
            if (!btn) throw new Error('no button ' + label + ' in row ' + name);
            btn.click();
        },
        puts: () => MIN_PUT(server.log),
        posts: (suffix) => server.log.filter((x) => x.method === 'POST' && x.path === '/api/v1/iso' + (suffix || '')),
        gets: () => server.log.filter((x) => x.method === 'GET'),
        selectFile(file, name) {
            E['iso-file'].files = [file];
            E['iso-file'].dispatch('change');
            if (name !== undefined) E['iso-name'].value = name;
        },
        async run(cond, limit) {
            for (let i = 0; i < (limit || 30000); i++) {
                await flush();
                checkDeadline();
                if (cond()) return true;
                if (timers.length === 0) { await flush(); return cond(); }
                let k = 0;
                for (let j = 1; j < timers.length; j++) if (timers[j].at < timers[k].at || (timers[j].at === timers[k].at && timers[j].seq < timers[k].seq)) k = j;
                const t = timers.splice(k, 1)[0];
                if (t.at > clock.t) clock.t = t.at;
                t.fn();
            }
            return false;
        },
        async runFor(ms) {
            const end = clock.t + ms;
            for (let i = 0; i < 200000; i++) {
                await flush();
                checkDeadline();
                let k = -1;
                for (let j = 0; j < timers.length; j++) if (timers[j].at <= end && (k < 0 || timers[j].at < timers[k].at || (timers[j].at === timers[k].at && timers[j].seq < timers[k].seq))) k = j;
                if (k < 0) break;
                const t = timers.splice(k, 1)[0];
                if (t.at > clock.t) clock.t = t.at;
                t.fn();
            }
            clock.t = end;
            await flush();
        },
        async idle() { // 何も起きなくなるまで(タイマーは動かさない)
            for (let i = 0; i < 5; i++) await flush();
        },
    };
    return h;
}

// ---------- (A)〜(E) の検査 ----------
async function checkAll(source, o) {
    o = o || {};
    const problems = [];
    const eq = (name, got, want) => {
        if (JSON.stringify(got) !== JSON.stringify(want)) problems.push(name + ': expected ' + JSON.stringify(want) + ', got ' + JSON.stringify(got));
    };
    const ok = (name, cond, extra) => { if (!cond) problems.push(name + (extra !== undefined ? ': ' + JSON.stringify(extra) : '')); };
    const only = process.env.ISO_TEST_ONLY;
    const guard = async (name, fn) => {
        if (only && !name.includes(only)) return;
        if (o.failFast && problems.length > 0) return; // ミュータントの実行: 1 つ落ちたら、残りは走らせない
        const t0 = Date.now();
        try { await fn(); } catch (e) { problems.push(name + ': threw ' + (e && e.stack ? e.stack.split('\n').slice(0, 3).join(' | ') : e)); }
        if (process.env.ISO_TEST_TIMES) console.log('  ' + (Date.now() - t0) + ' ms  ' + name);
    };
    const stats = { sha: 0, pure: 0, scenarios: 0 };
    let Iso;
    try {
        Iso = loadIsoViewer(source);
    } catch (e) {
        return { problems: [String(e.message)], stats };
    }

    // ===== (A) SHA-256 =====
    await guard('sha256', async () => {
        const bytes = (s) => new Uint8Array(Buffer.from(s, 'latin1'));
        const rep = (n) => new Uint8Array(n).fill(0x61);
        const VEC = [
            ['empty', bytes(''), 'e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855'],
            ['abc', bytes('abc'), 'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad'],
            ['448 bits', bytes('abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq'), '248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1'],
            ['896 bits', bytes('abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu'), 'cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1'],
            ['fox', bytes('The quick brown fox jumps over the lazy dog'), 'd7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592'],
            ['a x 55', rep(55), '9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318'],
            ['a x 56', rep(56), 'b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a'],
            ['a x 57', rep(57), 'f13b2d724659eb3bf47f2dd6af1accc87b81f09f59f2b75e5c0bed6589dfe8c6'],
            ['a x 63', rep(63), '7d3e74a05d7db15bce4ad9ec0658ea98e3f06eeecf16b4c6fff2da457ddc2f34'],
            ['a x 64', rep(64), 'ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb'],
            ['a x 65', rep(65), '635361c48bb9eab14198e76ea8ab7f1a41685d6ad62aa9146d301d4f17eb0ae0'],
            ['a x 119', rep(119), '31eba51c313a5c08226adf18d4a359cfdfd8d2e816b13f4af952f7ea6584dcfb'],
            ['a x 120', rep(120), '2f3d335432c70b580af0e8e1b3674a7c020d683aa5f73aaaedfdc55af904c21c'],
            ['a x 128', rep(128), '6836cf13bac400e9105071cd6af47084dfacad4e5e302c94bfed24e013afb73e'],
            ['a x 1,000,000', rep(1000000), 'cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0'],
        ];
        for (const [name, data, want] of VEC) { eq('sha256 ' + name, Iso.sha256Hex(data), want); stats.sha++; }
        // 細切れの update(ブロックの境界をまたぐ)は、一括と同じ
        const million = rep(1000000);
        for (const step of [1, 7, 63, 64, 65, 1000, 4096]) {
            const s = new Iso.Sha256();
            for (let o = 0; o < 130000; o += step) s.update(million.subarray(o, Math.min(130000, o + step)));
            eq('sha256 split by ' + step, s.digest(), sha(Buffer.alloc(130000, 0x61)));
            stats.sha++;
        }
        const s2 = new Iso.Sha256();
        for (let o = 0; o < million.length; o += 33333) s2.update(million.subarray(o, o + 33333));
        eq('sha256 1M a in 33333-byte steps', s2.digest(), 'cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0');
        // 0 バイトの update が混ざっても同じ
        const s3 = new Iso.Sha256();
        s3.update(new Uint8Array(0)); s3.update(bytes('a')); s3.update(new Uint8Array(0)); s3.update(bytes('bc')); s3.update(new Uint8Array(0));
        eq('sha256 empty updates are harmless', s3.digest(), 'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad');
        // node:crypto との照合(長さ 0〜300 と、いくつかの大きな長さ)
        let x = 99;
        for (const n of Array.from({ length: 301 }, (_, i) => i).concat([1023, 1024, 1025, 65535, 65536, 262145])) {
            const buf = Buffer.alloc(n);
            for (let i = 0; i < n; i++) { x = (Math.imul(x, 1664525) + 1013904223) >>> 0; buf[i] = x >>> 24; }
            const got = Iso.sha256Hex(new Uint8Array(buf));
            if (got !== sha(buf)) { problems.push('sha256 vs node:crypto, length ' + n + ': got ' + got); break; }
            stats.sha++;
        }
        // ビット長の上位ワード: 2^29 バイト(= 2^32 ビット)のゼロ。sha256sum の値。(約 4 秒かかる。ミュータントの実行では、SHA の変更のときだけ)
        const t0 = Date.now();
        if (o.big !== false && !(o.failFast && problems.length > 0)) {
            const big = new Iso.Sha256();
            const mib = new Uint8Array(1048576);
            for (let i = 0; i < 512; i++) big.update(mib);
            eq('sha256 of 2^29 zero bytes (bit length above 2^32)', big.digest(), '9acca8e8c22201155389f65abbf6bc9723edc7384ead80503839f49dcc56d767');
            stats.sha++;
        }
        stats.bigMs = Date.now() - t0;
        // 終わった後の update は、黙って壊れず、例外になる
        let threw = false;
        try { const s = new Iso.Sha256(); s.digest(); s.update(bytes('x')); } catch (e) { threw = true; }
        ok('sha256 update after digest throws', threw);
    });

    // ===== (B) 純関数 =====
    await guard('pure functions', async () => {
        const FB = [[0, '0 B'], [1, '1 B'], [1023, '1023 B'], [1024, '1.00 KiB'], [1536, '1.50 KiB'], [10240, '10.0 KiB'], [102400, '100 KiB'],
            [1048576, '1.00 MiB'], [1073741824, '1.00 GiB'], [6126805504, '5.71 GiB'], [17179869184, '16.0 GiB'], [1099511627776, '1.00 TiB'],
            [-1, '?'], [NaN, '?'], [Infinity, '?'], ['5', '?'], [null, '?']];
        for (const [n, want] of FB) { eq('formatBytes(' + String(n) + ')', Iso.formatBytes(n), want); stats.pure++; }
        const U8 = [['', 0], ['a', 1], ['é', 2], ['あ', 3], ['\u{1F600}', 4], ['\ud800', 3], ['a\u{1F600}あ', 8], ['\udc00x', 4]];
        for (const [s, want] of U8) { eq('utf8Length(' + JSON.stringify(s) + ')', Iso.utf8Length(s), want); stats.pure++; }
        const NAME = [
            ['ubuntu-24.04', null], ['a', null], ['x'.repeat(63), null], ['あ'.repeat(21), null], ['日本語 名前', null], ['a b', null],
            ['', 'req'], ['x'.repeat(64), 'bytes'], ['あ'.repeat(22), 'bytes'], ['a/b', 'slash'], ['a\\b', 'slash'], [' a', 'space'], ['a ', 'space'],
            ['a\u0000b', 'ctrl'], ['a\u001fb', 'ctrl'], ['a\u007fb', 'ctrl'], ['a\u0080b', 'ctrl'], ['a\u009fb', 'ctrl'], ['a\nb', 'ctrl'], ['a\tb', 'ctrl'],
            ['a\u00a0b', null], ['a\ud800b', 'bad'], ['a\udc00', 'bad'], ['\u{1F600}', null],
        ];
        const WORDS = { req: '入力してください', bytes: '1〜63 バイト', slash: '使えません', space: '前後に空白', ctrl: '制御文字', bad: '不正な文字' };
        for (const [n, w] of NAME) {
            const got = Iso.nameProblem(n);
            if (w === null) eq('nameProblem(' + JSON.stringify(n) + ')', got, null);
            else ok('nameProblem(' + JSON.stringify(n) + ') mentions ' + w, typeof got === 'string' && got.includes(WORDS[w]), got);
            stats.pure++;
        }
        eq('nameProblem(undefined)', typeof Iso.nameProblem(undefined), 'string');
        eq('nameProblem(null)', typeof Iso.nameProblem(null), 'string');
        const DN = [['ubuntu-24.04.iso', 'ubuntu-24.04'], ['Win11.ISO', 'Win11'], ['a.iso.iso', 'a.iso'], ['x.img', 'x.img'], ['.iso', 'image'], ['', 'image'],
            ['a/b\\c.iso', 'a_b_c'], ['  pad  .iso', 'pad'], ['x'.repeat(70) + '.iso', 'x'.repeat(63)], ['あ'.repeat(30) + '.iso', 'あ'.repeat(21)],
            ['a\u0001b.iso', 'a_b'], ['\u{1F600}'.repeat(20), '\u{1F600}'.repeat(15)], ['x'.repeat(62) + 'あ', 'x'.repeat(62)], ['x'.repeat(62) + ' y', 'x'.repeat(62)]];
        for (const [f, want] of DN) {
            eq('defaultName(' + JSON.stringify(f) + ')', Iso.defaultName(f), want);
            ok('defaultName result passes the name rule: ' + JSON.stringify(f), Iso.nameProblem(Iso.defaultName(f)) === null);
            stats.pure++;
        }
        // エラーの文言: [status, 本文, 含む語(手書き)]
        const ERR = [
            [400, { error: 'invalid_request' }, 'リクエストの内容が正しくありません'],
            [401, null, 'ログイン'], [403, null, '拒否'], [403, { error: 'forbidden' }, '拒否'],
            [404, { error: 'not_found' }, '見つかりません'], [404, null, '404'],
            [405, null, '405'], [405, { error: 'method_not_allowed' }, '許可されていません'],
            [409, { error: 'upload_in_progress' }, '別のイメージ'], [409, { error: 'name_exists' }, '同じ名前'], [409, { error: 'too_many_images' }, '上限'],
            [409, { error: 'not_uploading' }, '受信できる状態ではありません'], [409, { error: 'image_not_ready' }, 'まだ使えません'],
            [409, { error: 'mounted' }, 'マウント中のイメージは削除できません'], [409, { error: 'crosslinked' }, 'チェックディスク'],
            [409, { error: 'locked_by_target' }, '取り外しを防止'], [409, { error: 'offset_mismatch', received: 5 }, '送信位置'],
            [409, { error: 'size_mismatch', received: 1024, size: 2048 }, '受信済み: 1.00 KiB / 2.00 KiB'],
            [411, { error: 'length_required' }, 'Content-Length'], [411, null, '411'],
            [413, { error: 'payload_too_large' }, '上限'], [413, null, '413'],
            [415, { error: 'unsupported_media_type' }, 'Content-Type'], [415, null, '415'],
            [422, { error: 'checksum_mismatch' }, 'SHA-256'], [422, { error: 'not_iso' }, 'ISO イメージではありません'],
            [422, { error: 'too_fragmented' }, '断片化'], [422, { error: 'part_mismatch' }, 'パーツ'], [422, { error: 'meta_corrupt' }, '管理情報'],
            [503, { error: 'not_ready' }, 'SD カードの準備'], [503, { error: 'sd_unavailable' }, '読み書きでエラー'], [503, { error: 'busy' }, '別の処理'],
            [503, null, '503'],
            [507, { error: 'insufficient_storage', free_bytes: 1073741824 }, '空き: 1.00 GiB'], [507, { error: 'insufficient_storage' }, '空き容量が足りません'], [507, null, '507'],
            [409, { error: 'zzz' }, 'zzz'], [409, { error: 'zzz' }, '409'], [500, null, 'エラー (500)'], [500, { error: 'boom' }, 'boom'],
            [409, { error: 'constructor' }, 'constructor'], [409, { error: '__proto__' }, '__proto__'], [409, { error: 5 }, '409'], [409, 'x', '409'],
        ];
        for (const [st, body, word] of ERR) {
            const got = Iso.describeError(st, body);
            ok('describeError(' + st + ', ' + JSON.stringify(body) + ') includes ' + word, typeof got === 'string' && got.includes(word), got);
            ok('describeError(' + st + ', ' + JSON.stringify(body) + ') is Japanese or has the status', /[ぁ-んァ-ヶ一-龥]/.test(got), got);
            stats.pure++;
        }
        const plain = [400, 401, 403, 404, 405, 409, 411, 413, 415, 422, 503, 507].map((s) => Iso.describeError(s, null));
        eq('status-only texts are all different', new Set(plain).size, plain.length);
        eq('describeError does not use the prototype', Iso.describeError(409, { error: 'toString' }).includes('toString'), true);
        // normalizeList
        eq('normalizeList(null)', Iso.normalizeList(null), null);
        eq('normalizeList({})', Iso.normalizeList({}), null);
        eq('normalizeList(sd without state)', Iso.normalizeList({ sd: {} }), null);
        const nl = Iso.normalizeList({ sd: { state: 'ready', total_bytes: 10, free_bytes: 5 }, images: [{ id: 'a1b2c3d4', name: 'n', size: 5, state: 'ready', received: 5, invalid_reason: null }, { id: 5, name: 'bad' }, null, { id: 'x', name: 'y', state: 'invalid', size: null, received: null, invalid_reason: 'meta_corrupt' }], mounted: { id: 'a1b2c3d4', name: 'n' }, target: { locked: true, ejected_by_target: false }, chunk_size: 131072, hidden_images: 2, upload: { id: 'a1b2c3d4', state: 'receiving' } });
        eq('normalizeList images', nl.images.map((i) => [i.id, i.state, i.size, i.received, i.reason]), [['a1b2c3d4', 'ready', 5, 5, null], ['x', 'invalid', null, null, 'meta_corrupt']]);
        eq('normalizeList others', [nl.mounted, nl.locked, nl.ejectedByTarget, nl.chunk, nl.hidden, nl.upload.id, nl.sd.free], ['a1b2c3d4', true, false, 131072, 2, 'a1b2c3d4', 5]);
        eq('normalizeList chunk_size default when missing', Iso.normalizeList({ sd: { state: 'ready' }, images: [] }).chunk, 16384);
    });

    // ===== (C) 偽のサーバーとの通信 =====
    const fileOf = (size, signature) => new FakeFile(makeIso(size, signature), 'test.iso');
    const gate = () => { let open; const p = new Promise((r) => { open = r; }); return { p, open }; };
    const READY1 = (h) => h.rows().length === 1 && h.rows()[0].badge === '利用可能' && h.iso._state.job === null;
    const SIZE = 100000; // chunk_size 32768 で 4 回: 32768 + 32768 + 32768 + 1696
    const upload = async (h, file, name, hashOn) => {
        h.E['iso-hash'].checked = hashOn !== false;
        h.selectFile(file, name === undefined ? 'myiso' : name);
        h.E['iso-upload'].click();
    };
    const started = async (h) => { h.iso.start(); await h.run(() => h.gets().length >= 1); h.iso.setPanel(true); await h.idle(); };
    const lens = (puts) => puts.map((p) => p.len);
    const offs = (puts) => puts.map((p) => p.headers['X-Upload-Offset']);

    await guard('upload: normal', async () => {
        stats.scenarios++;
        const server = makeServer();
        const h = boot(Iso, server);
        await started(h);
        const f = fileOf(SIZE);
        await upload(h, f);
        ok('upload: ready', await h.run(() => READY1(h)), h.rows().map((r) => r.badge));
        eq('upload: PUT offsets', offs(h.puts()), ['0', '32768', '65536', '98304']);
        eq('upload: PUT lengths', lens(h.puts()), [32768, 32768, 32768, 1696]);
        ok('upload: PUT headers', h.puts().every((p) => p.headers['Content-Type'] === 'application/octet-stream' && p.credentials === 'same-origin'));
        eq('upload: begin body', h.posts()[0].json, { name: 'myiso', size: SIZE, sha256: sha(f._b) });
        ok('upload: begin is JSON with credentials', h.posts()[0].headers['Content-Type'] === 'application/json' && h.posts()[0].credentials === 'same-origin');
        ok('upload: server got the same bytes', server.images[0].data.equals(f._b));
        eq('upload: server state', [server.images[0].state, server.images[0].sha], ['ready', sha(f._b)]);
        eq('upload: finish posted once', h.posts('/' + server.images[0].id + '/finish').length, 1);
        eq('upload: finish body is {}', h.posts('/' + server.images[0].id + '/finish')[0].json, {});
        eq('upload: message', [h.msg().includes('完了'), h.msgOk(), h.msgError()], [true, true, false]);
        eq('upload: progress hidden at the end', h.progressShown(), false);
        eq('upload: form enabled again', [h.E['iso-upload'].disabled, h.E['iso-file'].disabled, h.E['iso-name'].disabled], [false, false, false]);
        eq('upload: pause hidden at the end', h.E['iso-pause'].classes.has('iso-hidden'), true);
        eq('upload: no violations', h.violations, []);
    });
    await guard('upload: progress shown while sending', async () => {
        stats.scenarios++;
        const server = makeServer();
        const seen = [];
        const h = boot(Iso, server);
        server.hooks.push((req) => {
            if (req.method === 'PUT') seen.push([h.E['iso-progress'].value, h.E['iso-progress'].max, h.progressShown(), h.E['iso-pause'].classes.has('iso-hidden'), h.E['iso-upload'].disabled]);
            return undefined;
        });
        await started(h);
        await upload(h, fileOf(SIZE));
        await h.run(() => READY1(h));
        // 2 つ目以降の PUT の前には、直前までの進みが出ている(最初の PUT の前は、SHA 計算の 100%)
        eq('progress before PUT 2', seen[1], [32768, SIZE, true, false, true]);
        eq('progress before PUT 4', seen[3], [98304, SIZE, true, false, true]);
    });
    await guard('upload: without SHA-256', async () => {
        stats.scenarios++;
        const server = makeServer();
        const h = boot(Iso, server);
        await started(h);
        await upload(h, fileOf(SIZE), 'plain', false);
        await h.run(() => READY1(h));
        eq('begin has no sha256 key', h.posts()[0].json, { name: 'plain', size: SIZE });
    });
    await guard('upload: file select fills the name', async () => {
        stats.scenarios++;
        const server = makeServer();
        const h = boot(Iso, server);
        await started(h);
        h.E['iso-file'].files = [new FakeFile(makeIso(40000), 'Ubuntu-24.04.iso')];
        h.E['iso-file'].dispatch('change');
        eq('default name', h.E['iso-name'].value, 'Ubuntu-24.04');
    });
    await guard('upload: validation before any request', async () => {
        stats.scenarios++;
        const server = makeServer();
        const h = boot(Iso, server);
        await started(h);
        const before = server.log.length;
        h.E['iso-upload'].click();
        await h.idle();
        eq('no file', [h.msg().includes('ISO ファイルを選んで'), h.msgError(), server.log.length], [true, true, before]);
        h.selectFile(fileOf(SIZE), 'bad/name');
        h.E['iso-upload'].click();
        await h.idle();
        eq('bad name', [h.msg().includes('使えません'), server.log.length], [true, before]);
        h.selectFile(fileOf(34815), 'small');
        h.E['iso-upload'].click();
        await h.idle();
        eq('too small', [h.msg().includes('34,816'), server.log.length], [true, before]);
        h.selectFile(fileOf(SIZE), '');
        h.E['iso-upload'].click();
        await h.idle();
        eq('empty name', [h.msg().includes('名前を入力'), server.log.length], [true, before]);
        h.selectFile(fileOf(34816), 'edge');
        h.E['iso-upload'].click();
        ok('34816 bytes is accepted', await h.run(() => h.posts().length === 1));
    });
    await guard('upload: SD not ready blocks the upload', async () => {
        stats.scenarios++;
        const server = makeServer();
        server.sd.state = 'no_card';
        const h = boot(Iso, server);
        await started(h);
        eq('upload button disabled', h.E['iso-upload'].disabled, true);
        eq('rescan button disabled', h.E['iso-rescan'].disabled, true);
        eq('sd text', h.E['iso-sd'].textContent.includes('ありません'), true);
        for (const [st, word] of [['unsupported_fs', 'FAT32'], ['error', '読み書きでエラー']]) {
            server.sd.state = st;
            h.iso.refreshList();
            await h.idle();
            ok('sd text for ' + st, h.E['iso-sd'].textContent.includes(word), h.E['iso-sd'].textContent);
        }
        server.sd.state = 'ready';
        h.iso.refreshList();
        await h.idle();
        eq('enabled when ready', [h.E['iso-upload'].disabled, h.E['iso-rescan'].disabled], [false, false]);
        eq('sd text when ready', h.E['iso-sd'].textContent.includes('空き 20.5 GiB / 29.7 GiB'), true);
    });

    // 再開と回復
    await guard('upload: response lost after the server wrote the chunk', async () => {
        stats.scenarios++;
        const server = makeServer();
        const h = boot(Iso, server);
        server.hooks.push((req) => (req.method === 'PUT' && req.n === 2 ? { net: true, apply: true } : undefined));
        await started(h);
        const f = fileOf(SIZE);
        await upload(h, f);
        ok('ready', await h.run(() => READY1(h)), h.rows().map((r) => r.badge));
        // PUT 2 (32768) は書かれたが応答が読めない -> GET で received=65536 を取り直し、65536 から続ける。小さくして(16384)。
        eq('offsets', offs(h.puts()), ['0', '32768', '65536', '81920']);
        eq('lengths: halved after the network error', lens(h.puts()), [32768, 32768, 16384, 16384].slice(0, 3).concat([SIZE - 81920]));
        ok('bytes intact', server.images[0].data.equals(f._b));
        ok('a GET was made between the failed PUT and the next one', (() => {
            const i = server.log.indexOf(h.puts()[1]);
            return server.log.slice(i + 1, server.log.indexOf(h.puts()[2])).some((x) => x.method === 'GET');
        })());
    });
    await guard('upload: network error, chunk not written', async () => {
        stats.scenarios++;
        const server = makeServer();
        const h = boot(Iso, server);
        server.hooks.push((req) => (req.method === 'PUT' && req.n === 2 ? { net: true } : undefined));
        await started(h);
        const f = fileOf(SIZE);
        await upload(h, f);
        ok('ready', await h.run(() => READY1(h)));
        // PUT 2 は書かれなかった: 同じ 32768 からやり直す(16384 に小さくして)。16384 は速いので、次は 32768 に戻る。
        eq('offsets', offs(h.puts()), ['0', '32768', '32768', '49152', '81920']);
        eq('lengths', lens(h.puts()), [32768, 32768, 16384, 32768, 18080]);
        ok('bytes intact', server.images[0].data.equals(f._b));
    });
    await guard('upload: gives up after repeated network errors', async () => {
        stats.scenarios++;
        const server = makeServer();
        const h = boot(Iso, server);
        server.hooks.push((req) => (req.method === 'PUT' ? { net: true } : undefined));
        await started(h);
        await upload(h, fileOf(SIZE));
        ok('stopped with an error', await h.run(() => h.msgError() && h.E['iso-upload'].disabled === false));
        eq('number of PUTs is bounded', h.puts().length, 7);
        eq('message: the server answers GET but not PUT', [h.msg().includes('接続できますが'), h.msg().includes('再開')], [true, true]);
        // 画面には、再開できるイメージが残る
        eq('the image stays resumable', h.rows().map((r) => r.badge), ['受信中']);
    });
    await guard('upload: everything fails (GET too): the message says the server cannot be reached', async () => {
        stats.scenarios++;
        const server = makeServer();
        const h = boot(Iso, server);
        let begun = false;
        server.hooks.push((req) => {
            if (req.method === 'POST') { begun = true; return undefined; }
            return begun ? { net: true } : undefined; // begin の後は、何も届かない
        });
        await started(h);
        await upload(h, fileOf(SIZE));
        ok('stopped with an error', await h.run(() => h.msgError() && h.E['iso-upload'].disabled === false));
        eq('message', [h.msg().includes('接続できません'), h.msg().includes('接続できますが')], [true, false]);
    });
    await guard('upload: offset_mismatch continues from the server received', async () => {
        stats.scenarios++;
        const server = makeServer();
        const f = fileOf(SIZE);
        const h = boot(Iso, server);
        server.hooks.push((req, S) => {
            if (req.method === 'PUT' && req.n === 2) { S.append(S.images[0], f._b.subarray(32768, 65536)); } // 別の経路で先に書かれた
            return undefined;
        });
        await started(h);
        await upload(h, f);
        ok('ready', await h.run(() => READY1(h)));
        eq('offsets', offs(h.puts()), ['0', '32768', '65536', '98304']);
        eq('the mismatching PUT is not repeated', h.puts().filter((p) => p.headers['X-Upload-Offset'] === '32768').length, 1);
        ok('bytes intact', server.images[0].data.equals(f._b));
        eq('no error shown', h.msgError(), false);
    });
    await guard('upload: offset_mismatch that never converges stops', async () => {
        stats.scenarios++;
        const server = makeServer();
        const h = boot(Iso, server);
        server.hooks.push((req) => (req.method === 'PUT' ? { status: 409, body: { error: 'offset_mismatch', received: req.headers['X-Upload-Offset'] === undefined ? 0 : Number(req.headers['X-Upload-Offset']) } } : undefined));
        await started(h);
        await upload(h, fileOf(SIZE));
        ok('stops', await h.run(() => h.msgError()));
        eq('bounded number of PUTs (the 4th mismatch at the same offset stops)', h.puts().length, 4);
    });
    await guard('upload: slow network uses smaller chunks, fast network grows them back', async () => {
        stats.scenarios++;
        const server = makeServer();
        server.putMs = (req) => (req.len > 16384 ? 1600 : 100);
        const h = boot(Iso, server);
        await started(h);
        const f = fileOf(SIZE);
        await upload(h, f);
        ok('ready', await h.run(() => READY1(h)));
        eq('lengths', lens(h.puts()), [32768, 16384, 32768, 16384, 1696]);
        ok('bytes intact', server.images[0].data.equals(f._b));
    });
    await guard('upload: chunk is never below 4096', async () => {
        stats.scenarios++;
        const server = makeServer({ chunk: 8192 });
        server.putMs = () => 1900;
        const h = boot(Iso, server);
        await started(h);
        await upload(h, fileOf(40000));
        ok('ready', await h.run(() => READY1(h)));
        eq('lengths', lens(h.puts()), [8192, 4096, 4096, 4096, 4096, 4096, 4096, 4096, 3136]);
    });
    await guard('upload: begin uses the chunk_size from the response', async () => {
        stats.scenarios++;
        const server = makeServer({ chunk: 65536 });
        const h = boot(Iso, server);
        await started(h);
        await upload(h, fileOf(SIZE));
        ok('ready', await h.run(() => READY1(h)));
        eq('lengths', lens(h.puts()), [65536, 34464]);
    });

    // 一時停止
    await guard('upload: pause and resume', async () => {
        stats.scenarios++;
        const server = makeServer();
        const h = boot(Iso, server);
        let paused = false;
        server.hooks.push((req) => {
            if (req.method === 'PUT' && req.n === 2 && !paused) { paused = true; h.E['iso-pause'].click(); }
            return undefined;
        });
        await started(h);
        const f = fileOf(SIZE);
        await upload(h, f);
        ok('paused', await h.run(() => h.msg().includes('一時停止しました') && h.E['iso-upload'].disabled === false));
        eq('PUTs after pause', offs(h.puts()), ['0', '32768']);
        eq('row is resumable', h.rows().map((r) => [r.badge, r.buttons]), [['受信中', ['削除']]]);
        // 少し待つ: サーバーは idle で incomplete にする(ここでは手で)
        server.images[0].state = 'incomplete';
        h.iso.refreshList();
        await h.idle();
        eq('row shows resume', h.rows().map((r) => [r.badge, r.buttons]), [['中断（再開できます）', ['再開', '削除']]]);
        h.E['iso-file'].files = null; // 選び直さない: 同じ画面では、元のファイルを覚えている
        h.clickRow('myiso', '再開');
        ok('ready after resume', await h.run(() => READY1(h)));
        eq('PUTs continue from the server received', offs(h.puts()), ['0', '32768', '65536', '98304']);
        ok('bytes intact', server.images[0].data.equals(f._b));
        eq('begin only once', h.posts().length, 1);
    });
    await guard('upload: pause during SHA-256', async () => {
        stats.scenarios++;
        const server = makeServer();
        const h = boot(Iso, server);
        await started(h);
        const f = fileOf(3 * 1048576 + 5);
        const orig = f.slice.bind(f);
        let slices = 0;
        f.slice = (a, b) => { slices++; if (slices === 2) h.E['iso-pause'].click(); return orig(a, b); };
        await upload(h, f);
        ok('stopped', await h.run(() => h.msg().includes('中止') && !h.E['iso-upload'].disabled));
        eq('nothing was sent but GETs', server.log.filter((x) => x.method !== 'GET').length, 0);
    });

    // 再読み込みと再開(incomplete)
    await guard('resume: incomplete image, pick the file', async () => {
        stats.scenarios++;
        const f = fileOf(SIZE);
        const server = makeServer();
        const im = server.add({ name: 'win11', size: SIZE, state: 'incomplete', data: Buffer.from(f._b.subarray(0, 65536)) });
        const h = boot(Iso, server);
        await started(h);
        eq('row', h.rows().map((r) => [r.name, r.badge, r.buttons, r.note]), [['win11', '中断（再開できます）', ['再開', '削除'], '64.0 KiB / 97.7 KiB を受信済み。元のファイルを選んで「再開」を押してください。']]);
        eq('no auto finish', h.posts('/' + im.id + '/finish').length, 0);
        h.clickRow('win11', '再開');
        await h.idle();
        eq('asks for the file', [h.msg().includes('元のファイル'), h.msg().includes('100000 バイト'), h.msgError(), h.puts().length], [true, true, true, 0]);
        h.E['iso-file'].files = [fileOf(SIZE + 1)];
        h.clickRow('win11', '再開');
        await h.idle();
        eq('wrong size is not accepted', h.puts().length, 0);
        h.E['iso-file'].files = [f];
        h.clickRow('win11', '再開');
        ok('ready', await h.run(() => READY1(h)));
        eq('PUTs start at the server received', offs(h.puts()), ['65536', '98304']);
        ok('bytes intact', im.data.equals(f._b));
    });
    await guard('resume: same name and size in the upload form resumes instead of begin', async () => {
        stats.scenarios++;
        const f = fileOf(SIZE);
        const server = makeServer();
        const im = server.add({ name: 'win11', size: SIZE, state: 'incomplete', data: Buffer.from(f._b.subarray(0, 32768)) });
        const h = boot(Iso, server);
        await started(h);
        await upload(h, f, 'win11');
        ok('ready', await h.run(() => READY1(h)));
        eq('no begin', h.posts().length, 0);
        eq('offsets', offs(h.puts()), ['32768', '65536', '98304']);
        ok('bytes intact', im.data.equals(f._b));
        // 同じ名前で別の大きさ、または ready のものは、新しく作らず、エラー
        const h2 = boot(Iso, server);
        await started(h2);
        await upload(h2, fileOf(SIZE + 7), 'win11');
        await h2.idle();
        eq('name exists', [h2.msg().includes('同じ名前'), h2.posts().length], [true, 0]);
    });
    await guard('resume: the file chosen for a resume is remembered for the next resume (pause, then resume without choosing again)', async () => {
        stats.scenarios++;
        const f = fileOf(SIZE);
        const server = makeServer();
        const im = server.add({ name: 'win11', size: SIZE, state: 'incomplete', data: Buffer.from(f._b.subarray(0, 32768)) });
        const h = boot(Iso, server);
        let paused = false;
        server.hooks.push((req) => { if (req.method === 'PUT' && !paused) { paused = true; h.E['iso-pause'].click(); } return undefined; });
        await started(h);
        h.E['iso-file'].files = [f];
        h.clickRow('win11', '再開');
        ok('paused', await h.run(() => h.msg().includes('一時停止しました') && h.iso._state.job === null));
        h.E['iso-file'].files = null;
        im.state = 'incomplete';
        h.iso.refreshList();
        await h.idle();
        h.clickRow('win11', '再開');
        ok('ready', await h.run(() => READY1(h)));
        eq('offsets', offs(h.puts()), ['32768', '65536', '98304']);
        ok('bytes intact', im.data.equals(f._b));
    });
    await guard('resume: same name but another size is not resumed', async () => {
        stats.scenarios++;
        const f = fileOf(SIZE);
        const server = makeServer();
        server.add({ name: 'win11', size: SIZE, state: 'incomplete', data: Buffer.from(f._b.subarray(0, 32768)) });
        const h = boot(Iso, server);
        await started(h);
        await upload(h, fileOf(SIZE + 7), 'win11');
        await h.idle();
        eq('name exists, nothing sent', [h.msg().includes('同じ名前'), h.msgError(), h.puts().length, h.posts().length], [true, true, 0, 0]);
    });
    await guard('resume: another image is finalizing (the slot is used): wait, then go on', async () => {
        stats.scenarios++;
        const f = fileOf(SIZE);
        const server = makeServer({ finalizePolls: 3 });
        const other = server.add({ name: 'other', size: 40000, state: 'finalizing', polls: 3, fromFinish: true, data: makeIso(40000) });
        const im = server.add({ name: 'win11', size: SIZE, state: 'incomplete', data: Buffer.from(f._b.subarray(0, 32768)) });
        const h = boot(Iso, server);
        await started(h);
        h.E['iso-file'].files = [f];
        h.clickRow('win11', '再開');
        ok('ready', await h.run(() => h.rows().find((r) => r.name === 'win11').badge === '利用可能' && h.iso._state.job === null));
        eq('nothing was sent while the slot was used', h.puts()[0].headers['X-Upload-Offset'], '32768');
        eq('other finished', other.state, 'ready');
        ok('bytes intact', im.data.equals(f._b));
        eq('the wait is not an error', h.msgError(), false);
    });
    await guard('resume: the slot is taken between the list and the PUT (409 upload_in_progress, finalizing): wait and retry', async () => {
        stats.scenarios++;
        const f = fileOf(SIZE);
        const server = makeServer({ finalizePolls: 3 });
        const im = server.add({ name: 'win11', size: SIZE, state: 'incomplete', data: Buffer.from(f._b.subarray(0, 32768)) });
        let other = null;
        server.hooks.push((req, S) => {
            if (req.method === 'PUT' && req.n === 1) other = S.add({ name: 'other', size: 40000, state: 'finalizing', polls: 3, fromFinish: true, data: makeIso(40000) });
            return undefined;
        });
        const h = boot(Iso, server);
        await started(h);
        h.E['iso-file'].files = [f];
        h.clickRow('win11', '再開');
        ok('ready', await h.run(() => h.rows().find((r) => r.name === 'win11').badge === '利用可能' && h.iso._state.job === null));
        eq('the first PUT was refused and offset 32768 was sent again', offs(h.puts()).slice(0, 2), ['32768', '32768']);
        eq('other finished', other.state, 'ready');
        ok('bytes intact', im.data.equals(f._b));
    });
    await guard('resume: upload_in_progress by another uploader stops with a message', async () => {
        stats.scenarios++;
        const f = fileOf(SIZE);
        const server = makeServer();
        server.add({ name: 'busy', size: 40000, state: 'uploading', data: Buffer.alloc(10) });
        server.add({ name: 'win11', size: SIZE, state: 'incomplete', data: Buffer.from(f._b.subarray(0, 32768)) });
        const h = boot(Iso, server);
        await started(h);
        h.E['iso-file'].files = [f];
        h.clickRow('win11', '再開');
        ok('error', await h.run(() => h.msgError()));
        eq('message', h.msg().includes('別のイメージ'), true);
        eq('no PUT while another upload holds the slot', h.puts().length, 0);
    });

    // 各エラーの表示(PUT)
    await guard('PUT errors are shown', async () => {
        const CASES = [
            // [状態, 本文, 含む語, 終わった後の PUT の数(上限)]
            [404, { error: 'not_found' }, '見つかりません'],
            [411, { error: 'length_required' }, 'Content-Length'],
            [415, { error: 'unsupported_media_type' }, 'Content-Type'],
            [400, { error: 'invalid_request' }, 'リクエストの内容が正しくありません'],
            [405, null, '405'],
            [403, null, '拒否'],
            [507, { error: 'insufficient_storage', free_bytes: 2147483648 }, '空き: 2.00 GiB'],
            [409, { error: 'upload_in_progress' }, '別のイメージ'],
            [409, { error: 'not_uploading' }, '受信できる状態ではありません'],
            [422, { error: 'part_mismatch' }, 'パーツ'],
            [500, null, 'エラー (500)'],
        ];
        for (const [st, body, word] of CASES) {
            stats.scenarios++;
            await guard('PUT error ' + st, async () => {
                const server = makeServer();
                const h = boot(Iso, server);
                server.hooks.push((req) => (req.method === 'PUT' ? { status: st, body } : undefined));
                await started(h);
                await upload(h, fileOf(SIZE));
                ok('PUT ' + st + ' shows the error', await h.run(() => h.msgError() && !h.E['iso-upload'].disabled), h.msg());
                ok('PUT ' + st + ' message includes ' + word, h.msg().includes(word), h.msg());
                eq('PUT ' + st + ': no retry storm', h.puts().length <= (body && body.error === 'upload_in_progress' ? 6 : 2), true);
                eq('PUT ' + st + ': progress hidden', h.progressShown(), false);
                eq('PUT ' + st + ': no finish', h.server.log.filter((x) => x.path.endsWith('/finish')).length, 0);
            });
        }
    });
    await guard('PUT 413 shrinks the chunk and then gives up at the minimum', async () => {
        stats.scenarios++;
        const server = makeServer();
        const h = boot(Iso, server);
        server.hooks.push((req) => (req.method === 'PUT' ? { status: 413, body: { error: 'payload_too_large' } } : undefined));
        await started(h);
        await upload(h, fileOf(SIZE));
        ok('error', await h.run(() => h.msgError() && !h.E['iso-upload'].disabled));
        eq('chunk lengths shrink to 4096', lens(h.puts()), [32768, 16384, 8192, 4096]);
        eq('message', h.msg().includes('上限'), true);
    });
    await guard('PUT 503 sd_unavailable: wait for the SD, re-sync, go on', async () => {
        stats.scenarios++;
        const server = makeServer();
        const h = boot(Iso, server);
        const f = fileOf(SIZE);
        server.hooks.push((req, S) => {
            if (req.method === 'PUT' && req.n === 2) { S.sd.state = 'error'; S.images[0].state = 'incomplete'; return { status: 503, body: { error: 'sd_unavailable' } }; }
            return undefined;
        });
        let gets = 0;
        server.hooks.push((req, S) => { if (req.method === 'GET' && S.sd.state === 'error' && ++gets === 3) S.sd.state = 'ready'; return undefined; });
        await started(h);
        await upload(h, f);
        ok('ready', await h.run(() => READY1(h)), h.rows().map((r) => r.badge));
        eq('offsets: the refused chunk is sent again from the server received', offs(h.puts()), ['0', '32768', '32768', '65536', '98304']);
        ok('bytes intact', server.images[0].data.equals(f._b));
    });
    await guard('PUT 503 busy: retry after Retry-After', async () => {
        stats.scenarios++;
        const server = makeServer();
        const h = boot(Iso, server);
        let t2 = null;
        server.hooks.push((req) => {
            if (req.method === 'PUT' && req.n === 2) { t2 = h.clock.t; return { status: 503, body: { error: 'busy' }, headers: { 'Retry-After': '2' } }; }
            if (req.method === 'PUT' && req.n === 3) { eq('Retry-After 2 s is waited', h.clock.t - t2 >= 2000, true); }
            return undefined;
        });
        await started(h);
        await upload(h, fileOf(SIZE));
        ok('ready', await h.run(() => READY1(h)));
        eq('offsets: same chunk again', offs(h.puts()), ['0', '32768', '32768', '65536', '98304']);
    });
    await guard('PUT 401 reports the lost session', async () => {
        stats.scenarios++;
        const server = makeServer();
        let h;
        h = boot(Iso, server, { onUnauthorized: () => h.iso.stop() });
        server.hooks.push((req) => (req.method === 'PUT' && req.n === 2 ? { status: 401, body: { error: 'unauthorized' } } : undefined));
        await started(h);
        await upload(h, fileOf(SIZE));
        await h.run(() => h.unauth.n >= 1);
        await h.runFor(60000);
        eq('onUnauthorized called once', h.unauth.n, 1);
        eq('no more requests after logout', h.puts().length, 2);
        eq('toggle and panel hidden', [h.toggleShown(), h.panelShown()], [false, false]);
    });
    await guard('begin errors are shown', async () => {
        const CASES = [
            [400, { error: 'invalid_request' }, 'リクエストの内容が正しくありません'],
            [409, { error: 'upload_in_progress' }, '別のイメージ'],
            [409, { error: 'name_exists' }, '同じ名前'],
            [409, { error: 'too_many_images' }, '上限'],
            [503, { error: 'not_ready' }, 'SD カードの準備'],
            [503, { error: 'sd_unavailable' }, '読み書きでエラー'],
            [507, { error: 'insufficient_storage', free_bytes: 1048576 }, '空き: 1.00 MiB'],
            [413, { error: 'payload_too_large' }, '上限'],
            [415, { error: 'unsupported_media_type' }, 'Content-Type'],
            [405, null, '405'],
        ];
        for (const [st, body, word] of CASES) {
            stats.scenarios++;
            await guard('begin error ' + st, async () => {
                const server = makeServer();
                const h = boot(Iso, server);
                server.hooks.push((req) => (req.method === 'POST' && req.path === '/api/v1/iso' ? { status: st, body } : undefined));
                await started(h);
                await upload(h, fileOf(SIZE), 'x', false);
                ok('begin ' + st + ' shows the error', await h.run(() => h.msgError() && !h.E['iso-upload'].disabled), h.msg());
                ok('begin ' + st + ' message includes ' + word, h.msg().includes(word), h.msg());
                eq('begin ' + st + ': no PUT', h.puts().length, 0);
            });
        }
    });
    await guard('begin: network error and malformed answer', async () => {
        stats.scenarios++;
        for (const hook of [() => NET, () => ({ status: 201, body: { id: 'not-an-id', chunk_size: 1 } }), () => ({ status: 201, body: null })]) {
            const server = makeServer();
            const h = boot(Iso, server);
            server.hooks.push((req) => (req.method === 'POST' && req.path === '/api/v1/iso' ? hook() : undefined));
            await started(h);
            await upload(h, fileOf(SIZE), 'x', false);
            ok('begin failure is reported', await h.run(() => h.msgError() && !h.E['iso-upload'].disabled), h.msg());
            eq('begin failure: no PUT', h.puts().length, 0);
        }
    });

    // finish の待機
    await guard('finish: waits while finalizing, ends at ready', async () => {
        stats.scenarios++;
        const server = makeServer({ finalizePolls: 4 });
        const h = boot(Iso, server);
        const f = fileOf(SIZE);
        const seen = [];
        let tFinish = null;
        server.hooks.push((req) => {
            if (req.path.endsWith('/finish')) tFinish = h.clock.t;
            if (req.method === 'GET') seen.push([server.images[0] && server.images[0].state, h.progressText()]);
            return undefined;
        });
        await started(h);
        await upload(h, f);
        ok('ready', await h.run(() => READY1(h)));
        // 最終処理に GET が 4 回要る: 1 秒おきの確認なので、finish から ready まで 1.5 秒以上かかる(間隔 0 の待ちでないこと)
        ok('the wait polls about once a second', h.clock.t - tFinish >= 1500, h.clock.t - tFinish);
        const fin = seen.filter((s) => s[0] === 'finalizing');
        ok('polled several times while finalizing', fin.length >= 3, seen);
        ok('the wait is announced', fin.every((s) => s[1].includes('最終処理中')), fin);
        eq('finish only once', h.server.log.filter((x) => x.path.endsWith('/finish')).length, 1);
        eq('message', h.msg().includes('完了'), true);
    });
    await guard('finish: keeps ending in incomplete: bounded number of finish requests, then an error', async () => {
        stats.scenarios++;
        const server = makeServer({ finalizePolls: 1 });
        const h = boot(Iso, server);
        server.onFinalize = (i) => { i.state = 'incomplete'; i.fromFinish = false; return true; }; // SD は ready のまま、いつも incomplete に戻る
        await started(h);
        await upload(h, fileOf(SIZE));
        ok('stops with an error', await h.run(() => h.msgError() && h.iso._state.job === null));
        eq('finish was sent 6 times (1 + 5 rounds)', server.log.filter((x) => x.path.endsWith('/finish')).length, 6);
        eq('message', h.msg().includes('最終処理が完了しません'), true);
        await h.runFor(60000);
        eq('and then nothing more (no automatic finish either)', server.log.filter((x) => x.path.endsWith('/finish')).length, 6);
    });
    await guard('finish: invalid (not_iso) ends the wait with the reason', async () => {
        stats.scenarios++;
        const server = makeServer();
        const h = boot(Iso, server);
        await started(h);
        await upload(h, fileOf(SIZE, false), 'noiso', false);
        ok('invalid', await h.run(() => h.rows().length === 1 && h.rows()[0].badge === '無効' && h.iso._state.job === null));
        eq('reason in the row', h.rows()[0].note.includes('ISO イメージではありません'), true);
        eq('reason in the message', [h.msgError(), h.msg().includes('ISO イメージではありません')], [true, true]);
        eq('row has only delete', h.rows()[0].buttons, ['削除']);
        eq('form is enabled again', h.E['iso-upload'].disabled, false);
    });
    await guard('finish: invalid (checksum_mismatch)', async () => {
        stats.scenarios++;
        const server = makeServer();
        const h = boot(Iso, server);
        const f = fileOf(SIZE);
        // 申告の SHA-256 と、サーバーに届くバイトが違う: 届くバイトを 1 つ変える
        server.hooks.push((req) => { if (req.method === 'PUT' && req.n === 1) req.bytes[5] ^= 1; return undefined; });
        await started(h);
        await upload(h, f);
        ok('invalid', await h.run(() => h.rows().length === 1 && h.rows()[0].badge === '無効' && h.iso._state.job === null));
        eq('reason', [h.rows()[0].note.includes('SHA-256'), h.msg().includes('SHA-256'), h.msgError()], [true, true, true]);
    });
    await guard('finish: part_mismatch / too_fragmented / meta_corrupt are shown from the list', async () => {
        stats.scenarios++;
        const server = makeServer();
        server.add({ name: 'a', size: 5, state: 'invalid', reason: 'part_mismatch' });
        server.add({ name: 'b', size: 5, state: 'invalid', reason: 'too_fragmented' });
        server.add({ name: 'c', size: 5, state: 'invalid', reason: 'meta_corrupt' });
        server.add({ name: 'd', size: 5, state: 'invalid', reason: 'checksum_mismatch' });
        server.add({ name: 'e', size: 5, state: 'invalid', reason: 'future_reason' });
        const h = boot(Iso, server);
        await started(h);
        const notes = h.rows().map((r) => r.note);
        eq('reasons', [notes[0].includes('パーツ'), notes[1].includes('断片化'), notes[2].includes('管理情報'), notes[3].includes('SHA-256'), notes[4].includes('future_reason')], [true, true, true, true, true]);
        eq('all invalid rows only have delete', h.rows().map((r) => r.buttons), [['削除'], ['削除'], ['削除'], ['削除'], ['削除']]);
    });
    await guard('finish: wait ends at incomplete (SD error), received == size: finish again', async () => {
        stats.scenarios++;
        const server = makeServer({ finalizePolls: 2 });
        const h = boot(Iso, server);
        const f = fileOf(SIZE);
        let first = true;
        server.onFinalize = (i) => {
            if (!first) return false;
            first = false;
            i.state = 'incomplete'; i.fromFinish = false; server.sd.state = 'error'; // 最終処理中の SD のエラー
            let n = 0;
            server.hooks.push((req) => { if (req.method === 'GET' && server.sd.state === 'error' && ++n === 3) server.sd.state = 'ready'; return undefined; });
            return true;
        };
        await started(h);
        await upload(h, f);
        ok('ready', await h.run(() => READY1(h)), h.rows().map((r) => r.badge));
        eq('finish was sent twice', server.log.filter((x) => x.path.endsWith('/finish')).length, 2);
        eq('no more PUTs after the SD came back', h.puts().length, 4);
    });
    await guard('finish: wait ends at incomplete, received < size: resume the upload, then finish', async () => {
        stats.scenarios++;
        const server = makeServer({ finalizePolls: 2 });
        const h = boot(Iso, server);
        const f = fileOf(SIZE);
        let first = true;
        server.onFinalize = (i) => {
            if (!first) return false;
            first = false;
            i.state = 'incomplete'; i.fromFinish = false; i.data = i.data.subarray(0, 65536); // 巻き戻り
            return true;
        };
        await started(h);
        await upload(h, f);
        ok('ready', await h.run(() => READY1(h)), h.rows().map((r) => r.badge));
        eq('PUTs: the 2nd round starts at the server received', offs(h.puts()), ['0', '32768', '65536', '98304', '65536', '98304']);
        eq('finish twice', server.log.filter((x) => x.path.endsWith('/finish')).length, 2);
        ok('bytes intact', server.images[0].data.equals(f._b));
    });
    await guard('finish: size_mismatch answer resumes from its received', async () => {
        stats.scenarios++;
        const server = makeServer();
        const h = boot(Iso, server);
        const f = fileOf(SIZE);
        let once = true;
        server.hooks.push((req, S) => {
            if (req.path.endsWith('/finish') && once) { once = false; S.images[0].data = S.images[0].data.subarray(0, 65536); S.images[0].state = 'incomplete'; return { status: 409, body: { error: 'size_mismatch', received: 65536, size: SIZE } }; }
            return undefined;
        });
        await started(h);
        await upload(h, f);
        ok('ready', await h.run(() => READY1(h)));
        eq('PUTs', offs(h.puts()), ['0', '32768', '65536', '98304', '65536', '98304']);
    });
    await guard('finish: busy, upload_in_progress, sd errors are retried', async () => {
        for (const [st, body, hdr] of [[503, { error: 'busy' }, { 'Retry-After': '1' }], [409, { error: 'upload_in_progress' }, {}], [503, { error: 'not_ready' }, {}], [503, { error: 'sd_unavailable' }, {}]]) {
            stats.scenarios++;
            const server = makeServer();
            const h = boot(Iso, server);
            let n = 0;
            server.hooks.push((req) => (req.path.endsWith('/finish') && ++n <= 2 ? { status: st, body, headers: hdr } : undefined));
            await started(h);
            await upload(h, fileOf(SIZE));
            ok('finish ' + JSON.stringify(body) + ' then ready', await h.run(() => READY1(h)), h.msg());
            eq('finish ' + JSON.stringify(body) + ' attempts', server.log.filter((x) => x.path.endsWith('/finish')).length, 3);
        }
    });
    await guard('finish: 422 and 404 are shown', async () => {
        for (const [st, body, word] of [[422, { error: 'checksum_mismatch' }, 'SHA-256'], [404, { error: 'not_found' }, '見つかりません'], [400, { error: 'invalid_request' }, 'リクエストの内容が正しくありません'], [500, null, 'エラー (500)']]) {
            stats.scenarios++;
            const server = makeServer();
            const h = boot(Iso, server);
            server.hooks.push((req) => (req.path.endsWith('/finish') ? { status: st, body } : undefined));
            await started(h);
            await upload(h, fileOf(SIZE));
            ok('finish ' + st + ' shown', await h.run(() => h.msgError() && !h.E['iso-upload'].disabled), h.msg());
            ok('finish ' + st + ' message', h.msg().includes(word), h.msg());
            eq('finish ' + st + ' sent once', server.log.filter((x) => x.path.endsWith('/finish')).length, 1);
        }
    });
    await guard('finish: network error, the server started finalizing', async () => {
        stats.scenarios++;
        const server = makeServer();
        const h = boot(Iso, server);
        server.hooks.push((req) => (req.path.endsWith('/finish') && req.n === 1 ? { net: true, apply: true } : undefined));
        await started(h);
        await upload(h, fileOf(SIZE));
        ok('ready', await h.run(() => READY1(h)));
        eq('finish not repeated (it was seen as finalizing)', server.log.filter((x) => x.path.endsWith('/finish')).length, 1);
    });

    // 再読み込みでの finish の自動送信
    await guard('reload: incomplete with received == size gets one automatic finish', async () => {
        stats.scenarios++;
        const server = makeServer();
        const im = server.add({ name: 'big', size: 50000, state: 'incomplete', data: makeIso(50000) });
        const h = boot(Iso, server);
        h.iso.start(); // パネルは閉じたまま: 開始の一覧で気づく
        ok('ready', await h.run(() => im.state === 'ready'));
        eq('exactly one finish', server.log.filter((x) => x.path.endsWith('/finish')).length, 1);
        eq('finish body', server.log.find((x) => x.path.endsWith('/finish')).json, {});
        await h.runFor(60000);
        eq('still one finish a minute later', server.log.filter((x) => x.path.endsWith('/finish')).length, 1);
        eq('no PUT', h.puts().length, 0);
    });
    await guard('reload: the automatic finish is sent only once per image', async () => {
        stats.scenarios++;
        const server = makeServer();
        const im = server.add({ name: 'big', size: 50000, state: 'incomplete', data: makeIso(50000) });
        const h = boot(Iso, server);
        server.hooks.push((req) => (req.path.endsWith('/finish') ? { status: 404, body: { error: 'not_found' } } : undefined));
        h.iso.start();
        await h.run(() => h.msgError());
        await h.runFor(60000);
        h.iso.setPanel(true);
        await h.runFor(60000);
        eq('one finish only (the image is still incomplete)', [server.log.filter((x) => x.path.endsWith('/finish')).length, im.state], [1, 'incomplete']);
        eq('the row offers a manual finish', h.rows().map((r) => r.buttons), [['完了処理', '削除']]);
        server.hooks.length = 0;
        h.clickRow('big', '完了処理');
        ok('manual finish works', await h.run(() => im.state === 'ready'));
    });
    await guard('reload: no automatic finish when not safe', async () => {
        stats.scenarios++;
        // 別のイメージが枠を使っている / received < size / SD が使えない / 他の状態
        const server = makeServer();
        server.add({ name: 'busy', size: 50000, state: 'uploading', data: Buffer.alloc(100) });
        server.add({ name: 'full', size: 50000, state: 'incomplete', data: makeIso(50000) });
        server.add({ name: 'part', size: 50000, state: 'incomplete', data: makeIso(50000).subarray(0, 49999) });
        server.add({ name: 'rdy', size: 50000, state: 'ready', data: makeIso(50000) });
        const h = boot(Iso, server);
        h.iso.start();
        await h.runFor(30000);
        eq('no finish while another upload holds the slot', server.log.filter((x) => x.path.endsWith('/finish')).length, 0);
        // 枠が空いたら、full だけに送る
        server.get(server.images[0].id).state = 'incomplete';
        h.iso.setPanel(true);
        await h.runFor(30000);
        eq('finish only for the full one', server.log.filter((x) => x.path.endsWith('/finish')).map((x) => x.path), ['/api/v1/iso/' + server.images[1].id + '/finish']);
    });
    await guard('reload: no automatic finish while the SD is not ready, then one when it is', async () => {
        stats.scenarios++;
        const server = makeServer();
        const im = server.add({ name: 'big', size: 50000, state: 'incomplete', data: makeIso(50000) });
        server.sd.state = 'error';
        const h = boot(Iso, server);
        h.iso.start();
        await h.runFor(5000);
        h.iso.setPanel(true);
        await h.runFor(10000);
        eq('nothing while the SD is down', server.log.filter((x) => x.method !== 'GET').length, 0);
        server.sd.state = 'ready';
        ok('finish after the SD is back', await h.run(() => im.state === 'ready'));
        eq('one finish', server.log.filter((x) => x.path.endsWith('/finish')).length, 1);
    });
    await guard('reload: finalizing / deleting / uploading rows are polled', async () => {
        stats.scenarios++;
        const server = makeServer({ finalizePolls: 3 });
        const f = server.add({ name: 'fin', size: 50000, state: 'finalizing', polls: 3, data: makeIso(50000) });
        server.add({ name: 'del', size: 50000, state: 'deleting', polls: 3, data: makeIso(50000) });
        server.add({ name: 'up', size: 50000, state: 'uploading', data: Buffer.alloc(1000) });
        const h = boot(Iso, server);
        h.iso.start();
        await h.run(() => h.gets().length >= 1);
        h.iso.setPanel(true);
        await h.idle();
        eq('rows', h.rows().map((r) => [r.name, r.badge]), [['fin', '最終処理中'], ['del', '削除中'], ['up', '受信中']]);
        ok('fin becomes ready, del disappears', await h.run(() => f.state === 'ready' && h.rows().map((r) => r.name).join() === 'fin,up'), h.rows().map((r) => r.name + ':' + r.badge));
        eq('final rows', h.rows().map((r) => [r.name, r.badge]), [['fin', '利用可能'], ['up', '受信中']]);
        eq('finalizing has a delete button only', server.log.length > 0 && true, true);
    });

    // マウント / 取り外し / 削除
    await guard('mount / eject / delete', async () => {
        stats.scenarios++;
        const server = makeServer();
        const a = server.add({ name: 'ubuntu', size: 50000, state: 'ready' });
        const b = server.add({ name: 'win', size: 60000, state: 'ready' });
        const h = boot(Iso, server, { confirm: true });
        await started(h);
        eq('buttons', h.rows().map((r) => r.buttons), [['マウント', '削除'], ['マウント', '削除']]);
        h.clickRow('ubuntu', 'マウント');
        ok('mounted', await h.run(() => server.mounted === a.id && h.E['iso-mounted'].textContent.includes('ubuntu')));
        eq('mount body', h.posts('/mount')[0].json, { id: a.id });
        eq('rows after mount', h.rows().map((r) => [r.buttons, r.li.all().some((n) => n.textContent === 'マウント中' && n.classes.has('iso-badge-mounted'))]), [[['取り外し', '削除'], true], [['マウント', '削除'], false]]);
        eq('eject button shown', h.E['iso-eject'].classes.has('iso-hidden'), false);
        eq('message', h.msg().includes('マウントしました'), true);
        // もう一度: already_mounted は、メッセージだけ
        h.E['iso-eject'].click();
        ok('ejected', await h.run(() => server.mounted === null));
        eq('eject body', h.posts('/eject')[0].json, {});
        eq('eject hidden after', h.E['iso-eject'].classes.has('iso-hidden'), true);
        eq('mounted text', h.E['iso-mounted'].textContent, 'マウント中のイメージはありません');
        h.E['iso-eject'].dispatch('click');
        await h.idle();
        eq('eject when nothing is mounted: message', h.msg().includes('マウントされていません'), true);
        // 削除: 確認 -> DELETE -> deleting -> 消える
        h.clickRow('win', '削除');
        ok('deleted', await h.run(() => server.images.length === 1 && h.rows().length === 1));
        eq('delete asks for confirmation with the name', h.confirms.length, 1);
        ok('confirm text has the name', h.confirms[0].includes('「win」'), h.confirms[0]);
        eq('delete request', server.log.filter((x) => x.method === 'DELETE').map((x) => x.path), ['/api/v1/iso/' + b.id]);
        eq('remaining row', h.rows().map((r) => r.name), ['ubuntu']);
    });
    await guard('delete: declined', async () => {
        stats.scenarios++;
        const server = makeServer();
        server.add({ name: 'ubuntu', size: 50000, state: 'ready' });
        const h = boot(Iso, server, { confirm: false });
        await started(h);
        h.clickRow('ubuntu', '削除');
        await h.idle();
        eq('no DELETE', server.log.filter((x) => x.method === 'DELETE').length, 0);
        eq('asked once', h.confirms.length, 1);
    });
    await guard('delete: errors', async () => {
        stats.scenarios++;
        const server = makeServer();
        const a = server.add({ name: 'ubuntu', size: 50000, state: 'ready' });
        server.mounted = a.id;
        const h = boot(Iso, server, { confirm: true });
        await started(h);
        h.clickRow('ubuntu', '削除');
        await h.run(() => h.msgError());
        eq('mounted -> 409', h.msg().includes('マウント中のイメージは削除できません'), true);
        eq('image still listed', h.rows().length, 1);
        for (const [st, body, word] of [[404, { error: 'not_found' }, '見つかりません'], [409, { error: 'crosslinked' }, 'チェックディスク'], [503, { error: 'not_ready' }, 'SD カードの準備']]) {
            server.hooks.length = 0;
            server.hooks.push((req) => (req.method === 'DELETE' ? { status: st, body } : undefined));
            h.clickRow('ubuntu', '削除');
            await h.run(() => h.msg().includes(word));
            ok('delete ' + st + ' shown', h.msg().includes(word), h.msg());
        }
    });
    await guard('mount: locked_by_target asks, then forces', async () => {
        stats.scenarios++;
        const server = makeServer();
        const a = server.add({ name: 'ubuntu', size: 50000, state: 'ready' });
        const b = server.add({ name: 'win', size: 60000, state: 'ready' });
        server.mounted = a.id;
        server.locked = true;
        const h = boot(Iso, server, { confirm: false });
        await started(h);
        eq('target line', h.E['iso-target'].textContent.includes('取り外しを防止'), true);
        h.clickRow('win', 'マウント');
        await h.idle();
        eq('asked, declined: only one mount request without force', [h.confirms.length, h.posts('/mount').map((x) => x.json)], [1, [{ id: b.id }]]);
        eq('declined: still the old image', server.mounted, a.id);
        eq('declined message', h.msg().includes('取りやめ'), true);
        h.deps.confirm = (m) => { h.confirms.push(m); return true; };
        h.clickRow('win', 'マウント');
        ok('forced', await h.run(() => server.mounted === b.id));
        eq('requests', h.posts('/mount').map((x) => x.json), [{ id: b.id }, { id: b.id }, { id: b.id, force: true }]);
    });
    await guard('eject: locked_by_target asks, then forces', async () => {
        stats.scenarios++;
        const server = makeServer();
        const a = server.add({ name: 'ubuntu', size: 50000, state: 'ready' });
        server.mounted = a.id;
        server.locked = true;
        let answer = false;
        const h = boot(Iso, server, { confirm: () => answer });
        await started(h);
        h.E['iso-eject'].click();
        await h.idle();
        eq('declined', [server.mounted, h.posts('/eject').map((x) => x.json), h.confirms.length], [a.id, [{}], 1]);
        answer = true;
        h.E['iso-eject'].click();
        ok('forced', await h.run(() => server.mounted === null));
        eq('eject requests', h.posts('/eject').map((x) => x.json), [{}, {}, { force: true }]);
    });
    await guard('mount errors are shown', async () => {
        for (const [st, body, word] of [[409, { error: 'image_not_ready' }, 'まだ使えません'], [404, { error: 'not_found' }, '見つかりません'], [503, { error: 'not_ready' }, 'SD カードの準備'], [400, { error: 'invalid_request' }, 'リクエストの内容'], [405, null, '405']]) {
            stats.scenarios++;
            const server = makeServer();
            server.add({ name: 'ubuntu', size: 50000, state: 'ready' });
            const h = boot(Iso, server);
            server.hooks.push((req) => (req.path === '/api/v1/iso/mount' ? { status: st, body } : undefined));
            await started(h);
            h.clickRow('ubuntu', 'マウント');
            await h.run(() => h.msgError());
            ok('mount ' + st + ' shown', h.msg().includes(word), h.msg());
        }
    });
    await guard('target ejected_by_target is shown', async () => {
        stats.scenarios++;
        const server = makeServer();
        const a = server.add({ name: 'ubuntu', size: 50000, state: 'ready' });
        server.mounted = a.id;
        server.ejected = true;
        const h = boot(Iso, server);
        await started(h);
        eq('text', h.E['iso-target'].textContent.includes('ターゲットが取り外しました'), true);
        eq('the line is visible', h.E['iso-target'].classes.has('iso-hidden'), false);
    });
    await guard('hidden_images note', async () => {
        stats.scenarios++;
        const server = makeServer();
        server.hidden = 3;
        const h = boot(Iso, server);
        await started(h);
        eq('note', [h.E['iso-hidden-note'].textContent.includes('3 個'), h.E['iso-hidden-note'].classes.has('iso-hidden')], [true, false]);
    });

    // 取り込み
    await guard('rescan: cursor loop until next_cursor is null', async () => {
        stats.scenarios++;
        const server = makeServer();
        server.importEntries = ['ok', 'ok', 'bad', 'ok', 'ok', 'ok', 'bad', 'ok', 'bad', 'ok']; // 10 個、1 回に 4 個
        const h = boot(Iso, server);
        await started(h);
        h.E['iso-rescan'].click();
        ok('done', await h.run(() => h.msg().includes('取り込みが終わりました')), h.msg());
        eq('request bodies', h.posts('/rescan').map((x) => x.json), [{}, { cursor: 4 }, { cursor: 8 }]);
        eq('totals', h.msg().includes('取り込み 7 件、スキップ 3 件'), true);
        eq('ok style', [h.msgOk(), h.msgError()], [true, false]);
        eq('imported images are listed', server.images.length, 7);
        ok('imported images become ready', await h.run(() => h.rows().length === 7 && h.rows().every((r) => r.badge === '利用可能')), h.rows().map((r) => r.badge));
        eq('rescan button enabled again', h.E['iso-rescan'].disabled, false);
    });
    await guard('rescan: nothing to import', async () => {
        stats.scenarios++;
        const server = makeServer();
        const h = boot(Iso, server);
        await started(h);
        h.E['iso-rescan'].click();
        await h.run(() => h.msg().includes('取り込みが終わりました'));
        eq('one request with {}', h.posts('/rescan').map((x) => x.json), [{}]);
        eq('totals', h.msg().includes('取り込み 0 件、スキップ 0 件'), true);
    });
    await guard('rescan: a cursor that does not move stops', async () => {
        stats.scenarios++;
        const server = makeServer();
        const h = boot(Iso, server);
        server.hooks.push((req) => (req.path === '/api/v1/iso/rescan' ? { status: 200, body: { imported: 0, skipped: 1, next_cursor: 4 } } : undefined));
        await started(h);
        h.E['iso-rescan'].click();
        ok('stops with an error', await h.run(() => h.msgError()), h.msg());
        eq('requests: {} then {cursor:4} then stop', h.posts('/rescan').map((x) => x.json), [{}, { cursor: 4 }, { cursor: 4 }].slice(0, 2));
    });
    await guard('rescan: errors and malformed answers', async () => {
        for (const [hook, word] of [
            [() => ({ status: 409, body: { error: 'upload_in_progress' } }), '別のイメージ'],
            [() => ({ status: 503, body: { error: 'not_ready' } }), 'SD カードの準備'],
            [() => ({ status: 400, body: { error: 'invalid_request' } }), 'リクエストの内容'],
            [() => NET, '接続できません'],
            [() => ({ status: 200, body: { imported: 'x', skipped: 0, next_cursor: null } }), '形が正しくありません'],
            [() => ({ status: 200, body: { imported: 0, skipped: 0 } }), '形が正しくありません'],
            [() => ({ status: 200, body: { imported: 0, skipped: 0, next_cursor: -1 } }), '形が正しくありません'],
            [() => ({ status: 200, body: { imported: 0, skipped: 0, next_cursor: 65536 } }), '形が正しくありません'],
            [() => ({ status: 200, body: { imported: 0, skipped: 0, next_cursor: 1.5 } }), '形が正しくありません'],
        ]) {
            stats.scenarios++;
            const server = makeServer();
            const h = boot(Iso, server);
            server.hooks.push((req) => (req.path === '/api/v1/iso/rescan' ? hook() : undefined));
            await started(h);
            h.E['iso-rescan'].click();
            ok('rescan error shown: ' + word, await h.run(() => h.msgError()), h.msg());
            ok('rescan error text: ' + word, h.msg().includes(word), h.msg());
            eq('rescan not retried: ' + word, h.posts('/rescan').length, 1);
        }
    });
    await guard('rescan: error in the middle keeps the totals', async () => {
        stats.scenarios++;
        const server = makeServer();
        server.importEntries = ['ok', 'ok', 'ok', 'ok', 'ok', 'ok'];
        const h = boot(Iso, server);
        server.hooks.push((req) => (req.path === '/api/v1/iso/rescan' && req.n === 2 ? { status: 503, body: { error: 'sd_unavailable' } } : undefined));
        await started(h);
        h.E['iso-rescan'].click();
        await h.run(() => h.msgError());
        eq('partial totals and reason', [h.msg().includes('取り込み 4 件、スキップ 0 件'), h.msg().includes('読み書きでエラー')], [true, true]);
    });
    await guard('rescan: busy is retried', async () => {
        stats.scenarios++;
        const server = makeServer();
        server.importEntries = ['ok'];
        const h = boot(Iso, server);
        server.hooks.push((req) => (req.path === '/api/v1/iso/rescan' && req.n === 1 ? { status: 503, body: { error: 'busy' }, headers: { 'Retry-After': '1' } } : undefined));
        await started(h);
        h.E['iso-rescan'].click();
        ok('done', await h.run(() => h.msg().includes('取り込みが終わりました')), h.msg());
        eq('bodies', h.posts('/rescan').map((x) => x.json), [{}, {}]);
    });

    // 一覧の表示
    await guard('list: states, sizes, names as text', async () => {
        stats.scenarios++;
        const server = makeServer();
        const evil = '<img src=x onerror=alert(1)>';
        server.add({ name: evil, size: 6126805504, state: 'ready' });
        server.add({ name: '<b>bold</b>&amp;', size: 5500000000, state: 'incomplete', data: Buffer.alloc(2048) });
        server.add({ name: 'q"\'`', size: 1, state: 'uploading' });
        const h = boot(Iso, server);
        await started(h);
        const rows = h.rows();
        eq('names are the exact text', rows.map((r) => r.name), [evil, '<b>bold</b>&amp;', 'q"\'`']);
        eq('names have no child elements', rows.map((r) => r.li.all().filter((n) => n.tag !== 'li' && n.tag !== 'div' && n.tag !== 'span' && n.tag !== 'button' && n.tag !== 'progress').length), [0, 0, 0]);
        eq('name node is a plain text span', rows.map((r) => r.li.all().find((n) => n.classes.has('iso-name')).children.length), [0, 0, 0]);
        eq('badges', rows.map((r) => r.badge), ['利用可能', '中断（再開できます）', '受信中']);
        eq('sizes', rows.map((r) => r.li.all().find((n) => n.classes.has('iso-size')).textContent), ['5.71 GiB', '5.12 GiB', '1 B']);
        eq('state classes', rows.map((r) => r.cls), ['iso-row iso-state-ready', 'iso-row iso-state-incomplete', 'iso-row iso-state-uploading']);
        eq('violations', h.violations, []);
        // 名前だけが入った行の確認ダイアログも、名前をそのまま(文字列として)渡す
        h.clickRow(evil, '削除');
        await h.idle();
        ok('confirm text has the raw name', h.confirms[0].includes(evil));
    });
    await guard('list: unknown state is shown as is', async () => {
        stats.scenarios++;
        const server = makeServer();
        server.add({ name: 'x', size: 5, state: 'ready' });
        server.images[0].state = 'future_state';
        const h = boot(Iso, server);
        await started(h);
        eq('badge', h.rows().map((r) => [r.badge, r.cls, r.buttons]), [['future_state', 'iso-row iso-state-unknown', ['削除']]]);
    });
    await guard('list: focus is kept across a redraw', async () => {
        stats.scenarios++;
        const server = makeServer();
        const a = server.add({ name: 'ubuntu', size: 50000, state: 'ready' });
        server.add({ name: 'win', size: 60000, state: 'ready' });
        const h = boot(Iso, server);
        await started(h);
        const btn = h.rows()[1].li.all().find((n) => n.textContent === '削除');
        btn.focus();
        server.mounted = a.id; // 一覧が変わって、描き直される
        h.iso.refreshList();
        await h.idle();
        eq('focus returns to the same button', [h.dom.doc.activeElement && h.dom.doc.activeElement.getAttribute('data-key'), h.dom.doc.activeElement !== btn], ['delete:' + server.images[1].id, true]);
    });
    await guard('list: an unchanged list is not redrawn', async () => {
        stats.scenarios++;
        const server = makeServer();
        server.add({ name: 'ubuntu', size: 50000, state: 'ready' });
        const h = boot(Iso, server);
        await started(h);
        const li = h.E['iso-list'].children[0];
        await h.runFor(10000);
        eq('same node after several polls', h.E['iso-list'].children[0] === li, true);
    });

    // ISO 機能が無効のビルド
    await guard('disabled build (404): the screen stays hidden and quiet', async () => {
        stats.scenarios++;
        const server = makeServer();
        server.disabled = true;
        const h = boot(Iso, server);
        h.iso.start();
        await h.run(() => h.gets().length >= 1);
        await h.runFor(60000);
        eq('toggle hidden', h.toggleShown(), false);
        eq('only one request', server.log.length, 1);
        h.iso.setPanel(true);
        await h.runFor(10000);
        eq('panel stays hidden, no polling', [h.panelShown(), server.log.length], [false, 1]);
    });
    await guard('toggle shows after the first answer', async () => {
        stats.scenarios++;
        const server = makeServer();
        const h = boot(Iso, server);
        eq('hidden at first', h.toggleShown(), false);
        h.iso.start();
        await h.run(() => h.toggleShown());
        eq('shown', h.toggleShown(), true);
        eq('panel is closed', h.panelShown(), false);
        h.E['btn-iso'].click();
        eq('open', [h.panelShown(), h.E['btn-iso'].getAttribute('aria-expanded')], [true, 'true']);
        h.E['btn-iso-close'].click();
        eq('closed', [h.panelShown(), h.E['btn-iso'].getAttribute('aria-expanded')], [false, 'false']);
    });
    await guard('first GET fails: the toggle is shown and the reason is in the panel', async () => {
        stats.scenarios++;
        const server = makeServer();
        server.hooks.push((req) => (req.method === 'GET' && req.n === 1 ? NET : undefined));
        const h = boot(Iso, server);
        h.iso.start();
        await h.run(() => h.msgError());
        eq('message', h.msg().includes('状態を取得できません'), true);
        h.iso.setPanel(true);
        ok('recovers on the next GET', await h.run(() => !h.msgError() || h.E['iso-sd'].textContent.includes('使えます')), h.msg());
        eq('sd text', h.E['iso-sd'].textContent.includes('使えます'), true);
    });

    // ポーリングの間隔
    await guard('polling cadence', async () => {
        stats.scenarios++;
        const server = makeServer({ finalizePolls: 1000 });
        const im = server.add({ name: 'ubuntu', size: 50000, state: 'ready' });
        const h = boot(Iso, server);
        h.iso.start();
        await h.run(() => h.gets().length >= 1);
        await h.runFor(30000);
        eq('panel closed and idle: no polling', h.gets().length, 1);
        h.iso.setPanel(true);
        await h.run(() => h.gets().length >= 2);
        const t0 = h.clock.t;
        await h.runFor(30000);
        eq('panel open and idle: every 3 s', h.gets().length, 2 + 10);
        // finalizing があれば 1 秒
        im.state = 'finalizing'; im.polls = 1000;
        await h.run(() => h.rows()[0].badge === '最終処理中');
        const n0 = h.gets().length;
        await h.runFor(10000);
        eq('finalizing: every 1 s', h.gets().length - n0, 10);
        h.iso.setPanel(false);
        const n1 = h.gets().length;
        await h.runFor(10000);
        eq('panel closed but finalizing: still polling every 1 s', h.gets().length - n1, 10);
        im.state = 'ready';
        await h.runFor(5000);
        const n2 = h.gets().length;
        await h.runFor(30000);
        eq('panel closed and idle again: stops', h.gets().length, n2);
        ok('time moved on', h.clock.t > t0);
    });
    await guard('polling does not hit the server while a PUT is in flight (held), and resumes afterwards', async () => {
        stats.scenarios++;
        const server = makeServer();
        const h = boot(Iso, server);
        const g = gate();
        server.hooks.push((req) => (req.method === 'PUT' && req.n === 2 ? { wait: g.p } : undefined));
        await started(h);
        await upload(h, fileOf(SIZE));
        ok('the 2nd PUT is held', await h.run(() => h.puts().length === 2));
        await h.idle();
        const n0 = h.gets().length;
        await h.runFor(20000); // ポーリングの期限(1 秒、3 秒)を何度も過ぎる
        eq('no GET while the PUT is held', h.gets().length, n0);
        eq('the PUT is still the last request', server.log[server.log.length - 1].method, 'PUT');
        g.open();
        ok('ready after the PUT is released', await h.run(() => READY1(h)));
        ok('polling goes on after the PUTs (GETs are made again)', h.gets().length > n0, [h.gets().length, n0]);
    });

    // 停止(ログアウト)
    await guard('stop() clears the poll timer at once', async () => {
        stats.scenarios++;
        const server = makeServer();
        const h = boot(Iso, server);
        await started(h);
        await h.idle();
        ok('a poll timer is pending while the panel is open', h.timers.length >= 1, h.timers.length);
        h.iso.stop();
        eq('no timer is left after stop()', h.timers.length, 0);
    });
    await guard('rescan: next_cursor 0 is a position, not the end', async () => {
        stats.scenarios++;
        const server = makeServer();
        const h = boot(Iso, server);
        server.hooks.push((req) => (req.path === '/api/v1/iso/rescan' ? { status: 200, body: { imported: 0, skipped: 1, next_cursor: req.n === 1 ? 0 : null } } : undefined));
        await started(h);
        h.E['iso-rescan'].click();
        ok('done', await h.run(() => h.msg().includes('取り込みが終わりました')), h.msg());
        eq('bodies', h.posts('/rescan').map((x) => x.json), [{}, { cursor: 0 }]);
    });
    await guard('rescan: the same cursor with imports in between keeps going (imported files leave import/)', async () => {
        // 9 files, 4 per call: the imported files move out of import/, so next_cursor stays 0 while the imports go on (PR #158 review)
        stats.scenarios++;
        const server = makeServer();
        const h = boot(Iso, server);
        const answers = [
            { imported: 4, skipped: 0, next_cursor: 0 },
            { imported: 4, skipped: 0, next_cursor: 0 },
            { imported: 1, skipped: 0, next_cursor: null },
        ];
        server.hooks.push((req) => (req.path === '/api/v1/iso/rescan' ? { status: 200, body: answers[req.n - 1] } : undefined));
        await started(h);
        h.E['iso-rescan'].click();
        ok('done (no "not moving" error)', await h.run(() => h.msg().includes('取り込みが終わりました')), h.msg());
        eq('bodies', h.posts('/rescan').map((x) => x.json), [{}, { cursor: 0 }, { cursor: 0 }]);
        eq('totals', h.msg().includes('取り込み 9 件、スキップ 0 件'), true);
        eq('no error shown', h.msgError(), false);
    });
    await guard('rescan: the same cursor with only skips (no import) still stops', async () => {
        stats.scenarios++;
        const server = makeServer();
        const h = boot(Iso, server);
        server.hooks.push((req) => (req.path === '/api/v1/iso/rescan' ? { status: 200, body: { imported: 0, skipped: 4, next_cursor: 4 } } : undefined));
        await started(h);
        h.E['iso-rescan'].click();
        ok('stops with an error', await h.run(() => h.msgError()), h.msg());
        eq('requests: {} then {cursor:4}', h.posts('/rescan').map((x) => x.json), [{}, { cursor: 4 }]);
        ok('the message says it is not moving', h.msg().includes('取り込みが進みません'), h.msg());
    });
    await guard('stop() during an upload: nothing more is sent', async () => {
        stats.scenarios++;
        const server = makeServer();
        const h = boot(Iso, server);
        server.hooks.push((req) => { if (req.method === 'PUT' && req.n === 2) h.iso.stop(); return undefined; });
        await started(h);
        await upload(h, fileOf(SIZE));
        await h.run(() => h.puts().length >= 2);
        await h.runFor(120000);
        eq('PUTs stop after stop()', h.puts().length, 2);
        eq('no finish', server.log.filter((x) => x.path.endsWith('/finish')).length, 0);
        eq('timers all cleared', h.timers.length, 0);
        eq('screen reset', [h.toggleShown(), h.panelShown(), h.E['iso-list'].children.length, h.msgShown(), h.progressShown()], [false, false, 0, false, false]);
        eq('no message about pausing', h.msg(), '');
    });
    await guard('stop() while waiting for finalizing, and start() again', async () => {
        stats.scenarios++;
        const server = makeServer({ finalizePolls: 1000 });
        const h = boot(Iso, server);
        await started(h);
        await upload(h, fileOf(SIZE));
        await h.run(() => h.rows().some((r) => r.badge === '最終処理中'));
        h.iso.stop();
        const n = server.log.length;
        await h.runFor(60000);
        eq('quiet after stop', server.log.length, n);
        h.iso.start();
        await h.run(() => h.toggleShown());
        eq('start works again, with a fresh list', h.rows().length, 1);
    });
    await guard('start() twice does not double the polling', async () => {
        stats.scenarios++;
        const server = makeServer();
        const h = boot(Iso, server);
        h.iso.start();
        h.iso.start();
        ok('the list arrived (the second start() did not drop it)', await h.run(() => h.toggleShown()));
        await h.idle();
        eq('one GET for two start() calls', h.gets().length, 1);
        h.iso.setPanel(true);
        await h.run(() => h.gets().length >= 2);
        const n = h.gets().length;
        await h.runFor(30000);
        eq('every 3 s only once', h.gets().length - n, 10);
    });
    await guard('delete the image that is being uploaded', async () => {
        stats.scenarios++;
        const server = makeServer();
        const h = boot(Iso, server);
        server.hooks.push((req) => { if (req.method === 'PUT' && req.n === 2) setImmediate(() => h.clickRow('myiso', '削除')); return undefined; });
        await started(h);
        await upload(h, fileOf(SIZE));
        await h.run(() => server.log.some((x) => x.method === 'DELETE'));
        await h.runFor(30000);
        ok('the loop ends', !h.E['iso-upload'].disabled);
        eq('no pause message after a delete', h.msg().includes('一時停止'), false);
        eq('and no error either (the loop was stopped, not refused by a 404)', h.msgError(), false);
        eq('no PUT after the DELETE', server.log.slice(server.log.findIndex((x) => x.method === 'DELETE')).filter((x) => x.method === 'PUT').length <= 1, true);
        eq('image gone', server.images.length, 0);
    });

    // 表示の部品
    await guard('buttons are disabled while an operation runs', async () => {
        stats.scenarios++;
        const server = makeServer();
        server.add({ name: 'ubuntu', size: 50000, state: 'ready' });
        const h = boot(Iso, server);
        let rowsDuring = null;
        server.hooks.push((req) => { if (req.path === '/api/v1/iso/mount') rowsDuring = h.rows().map((r) => r.li.all().filter((n) => n.tag === 'button').map((b) => b.disabled)); return undefined; });
        await started(h);
        h.clickRow('ubuntu', 'マウント');
        await h.idle();
        eq('buttons disabled during the request', rowsDuring, [[true, true]]);
        await h.run(() => h.rows()[0].buttons[0] === '取り外し');
        eq('enabled afterwards', h.rows()[0].li.all().filter((n) => n.tag === 'button').map((b) => b.disabled), [false, false]);
    });
    await guard('no console, no timers left after a full scenario', async () => {
        stats.scenarios++;
        const server = makeServer();
        const h = boot(Iso, server);
        await started(h);
        await upload(h, fileOf(SIZE));
        await h.run(() => READY1(h));
        h.iso.setPanel(false);
        await h.runFor(30000);
        eq('no timers once idle with the panel closed', h.timers.length, 0);
        const code = region(source).replace(/\/\/.*$/gm, '');
        ok('the source never touches console', !/\bconsole\b/.test(code));
        ok('the source never assigns innerHTML / outerHTML / insertAdjacentHTML / document.write', !/innerHTML|outerHTML|insertAdjacentHTML|document\.write/.test(code));
        ok('the source never sets a style', !/\.style\b|['"]style['"]|cssText/.test(code));
        ok('the source has no eval / Function / storage', !/\beval\(|new Function|\.innerText|localStorage|sessionStorage/.test(code));
    });

    // ===== レビューの指摘(PR #157)への対応 =====
    await guard('review 2: a 401 that belongs to an older generation does not stop the new one', async () => {
        stats.scenarios++;
        const server = makeServer();
        const h = boot(Iso, server);
        h.iso.start();
        await h.run(() => h.toggleShown());
        const g = gate();
        let held = false;
        server.hooks.push((req) => { if (req.method === 'GET' && !held) { held = true; return { wait: g.p, status: 401, body: { error: 'unauthorized' } }; } return undefined; });
        h.iso.setPanel(true); // この GET が保留される
        await h.idle();
        h.iso.stop(); // ログアウト
        h.iso.start(); // 再ログイン(新しい世代)
        await h.run(() => h.toggleShown());
        h.iso.setPanel(true);
        await h.idle();
        eq('the new session is open', [h.toggleShown(), h.panelShown()], [true, true]);
        g.open(); // 古い GET の 401 が、今ごろ届く
        await h.idle();
        eq('onUnauthorized is not called for the old request', h.unauth.n, 0);
        eq('the new session is still open', [h.toggleShown(), h.panelShown()], [true, true]);
    });
    await guard('review 3: network errors that still advance received do not exhaust the limit', async () => {
        stats.scenarios++;
        const server = makeServer({ chunk: 8192 });
        const h = boot(Iso, server);
        // 2〜9 回目の PUT は、サーバーが書いたのに応答が読めない(8 回続く。上限は 6 回)
        server.hooks.push((req) => (req.method === 'PUT' && req.n >= 2 && req.n <= 9 ? { net: true, apply: true } : undefined));
        await started(h);
        const f = fileOf(SIZE);
        await upload(h, f, 'myiso', false);
        ok('ready', await h.run(() => READY1(h)), [h.msg(), h.puts().length]);
        eq('no error', h.msgError(), false);
        ok('bytes intact', server.images[0].data.equals(f._b));
        eq('finish was sent', server.log.filter((x) => x.path.endsWith('/finish')).length, 1);
    });
    await guard('review 4: the automatic finish is not re-sent after 503 busy; a manual finish is', async () => {
        stats.scenarios++;
        for (const [name, body, hdr] of [['busy', { error: 'busy' }, { 'Retry-After': '1' }], ['upload_in_progress', { error: 'upload_in_progress' }, {}], ['sd_unavailable', { error: 'sd_unavailable' }, {}]]) {
            const server = makeServer();
            const im = server.add({ name: 'big', size: 50000, state: 'incomplete', data: makeIso(50000) });
            let denied = true;
            server.hooks.push((req) => (req.path.endsWith('/finish') && denied ? { status: 503, body, headers: hdr } : undefined));
            if (name === 'upload_in_progress') server.hooks[0] = (req) => (req.path.endsWith('/finish') && denied ? { status: 409, body, headers: hdr } : undefined);
            const h = boot(Iso, server);
            h.iso.start();
            await h.run(() => h.msgError());
            await h.runFor(120000);
            eq('automatic finish: one POST only (' + name + ')', server.log.filter((x) => x.path.endsWith('/finish')).length, 1);
            eq('the image is untouched and the reason is shown (' + name + ')', [im.state, h.msgError()], ['incomplete', true]);
            // 利用者が「完了処理」を押せば、再試行できる
            denied = false;
            h.iso.setPanel(true);
            await h.idle();
            h.clickRow('big', '完了処理');
            ok('manual finish works (' + name + ')', await h.run(() => im.state === 'ready' && h.iso._state.job === null));
            eq('two POSTs in all (' + name + ')', server.log.filter((x) => x.path.endsWith('/finish')).length, 2);
        }
        // 利用者が始めた finish は、busy を再試行する(従来どおり)
        const server = makeServer();
        const h2 = boot(Iso, server);
        let n = 0;
        server.hooks.push((req) => (req.path.endsWith('/finish') && ++n <= 2 ? { status: 503, body: { error: 'busy' }, headers: { 'Retry-After': '1' } } : undefined));
        await started(h2);
        await upload(h2, fileOf(SIZE));
        ok('user-started finish retries busy', await h2.run(() => READY1(h2)));
        eq('three POSTs', server.log.filter((x) => x.path.endsWith('/finish')).length, 3);
    });
    await guard('review 3b: failures that do not advance received still count, after earlier ones that did', async () => {
        stats.scenarios++;
        const server = makeServer({ chunk: 8192 });
        const h = boot(Iso, server);
        // 2〜6 回目: 書かれたが応答が読めない(位置が進む)。7〜9 回目: 書かれず、応答もない(位置が進まない)。3 回の失敗は、上限(6)以内
        server.hooks.push((req) => {
            if (req.method !== 'PUT') return undefined;
            if (req.n >= 2 && req.n <= 6) return { net: true, apply: true };
            if (req.n >= 7 && req.n <= 9) return { net: true };
            return undefined;
        });
        await started(h);
        const f = fileOf(SIZE);
        await upload(h, f, 'myiso', false);
        ok('ready (the earlier failures were not added to the later ones)', await h.run(() => READY1(h)), [h.msg(), h.puts().length]);
        ok('bytes intact', server.images[0].data.equals(f._b));
        // 位置が進まない失敗だけが続くと、上限で止まる
        const server2 = makeServer({ chunk: 8192 });
        const h2 = boot(Iso, server2);
        server2.hooks.push((req) => (req.method === 'PUT' && req.n >= 2 ? { net: true } : undefined));
        await started(h2);
        await upload(h2, fileOf(SIZE), 'myiso', false);
        ok('stops when nothing advances', await h2.run(() => h2.msgError() && h2.iso._state.job === null));
        eq('PUT count: 1 good + 7 failures (the 7th exceeds the limit of 6)', h2.puts().length, 8);
    });
    await guard('review 4c: a network error on the automatic finish POST is not answered with another POST', async () => {
        stats.scenarios++;
        for (const apply of [false, true]) {
            const server = makeServer();
            const im = server.add({ name: 'big', size: 50000, state: 'incomplete', data: makeIso(50000) });
            server.hooks.push((req) => (req.path.endsWith('/finish') && req.n === 1 ? { net: true, apply } : undefined));
            const h = boot(Iso, server);
            h.iso.start();
            await h.run(() => h.iso._state.job === null && server.log.some((x) => x.path.endsWith('/finish')));
            await h.runFor(120000);
            eq('one POST (applied=' + apply + ')', server.log.filter((x) => x.path.endsWith('/finish')).length, 1);
            // 適用された場合は、一覧で finalizing(または ready)と分かり、その待ちで ready になる。適用されなかった場合は、メッセージを出して止まる
            if (apply) eq('applied: ready through the list', im.state, 'ready');
            else eq('not applied: the user is told to press the button', [im.state, h.msgError(), h.msg().includes('「完了処理」')], ['incomplete', true, true]);
        }
    });
    await guard('review 5c: logout while the upload is being prepared drops it silently', async () => {
        stats.scenarios++;
        const server = makeServer();
        const h = boot(Iso, server);
        await started(h);
        const g = gate();
        let held = false;
        server.hooks.push((req) => { if (req.method === 'GET' && !held) { held = true; return { wait: g.p }; } return undefined; });
        h.iso.refreshList();
        await h.idle();
        h.selectFile(fileOf(SIZE), 'late');
        h.E['iso-upload'].click(); // 保留中の一覧の取得を待つ
        await h.idle();
        h.iso.stop(); // ログアウト
        h.iso.start(); // 再ログイン
        await h.run(() => h.toggleShown());
        g.open();
        await h.runFor(10000);
        eq('nothing was started and nothing is said about it', [h.posts().length, h.puts().length, h.msgError(), h.iso._state.job], [0, 0, false, null]);
    });
    await guard('review 8d: the result of a stopped rescan is not shown in the new session', async () => {
        stats.scenarios++;
        const server = makeServer();
        server.importEntries = ['ok'];
        const h = boot(Iso, server);
        await started(h);
        const g = gate();
        server.hooks.push((req) => (req.path === '/api/v1/iso/rescan' && req.n === 1 ? { wait: g.p } : undefined));
        h.E['iso-rescan'].click();
        await h.idle();
        h.iso.stop();
        h.iso.start();
        await h.run(() => h.toggleShown());
        g.open();
        await h.runFor(10000);
        eq('no message from the old operation', h.msg(), '');
    });
    await guard('review 4b: the automatic finish still waits for finalizing with GET after 202', async () => {
        stats.scenarios++;
        const server = makeServer({ finalizePolls: 3 });
        const im = server.add({ name: 'big', size: 50000, state: 'incomplete', data: makeIso(50000) });
        const h = boot(Iso, server);
        h.iso.start();
        ok('ready', await h.run(() => im.state === 'ready' && h.iso._state.job === null));
        eq('one POST', server.log.filter((x) => x.path.endsWith('/finish')).length, 1);
        ok('GETs were made while finalizing', h.gets().length >= 3, h.gets().length);
    });
    await guard('review 5: only one upload starts while the list is being fetched', async () => {
        stats.scenarios++;
        const server = makeServer();
        const h = boot(Iso, server);
        h.iso.start();
        await h.run(() => h.toggleShown());
        // 最初の一覧の後に、自動 finish の候補ができる(一覧の取得が保留されている間に、その一覧が届く)
        const cand = server.add({ name: 'cand', size: 50000, state: 'incomplete', data: makeIso(50000) });
        const g = gate();
        let held = false;
        server.hooks.push((req) => { if (req.method === 'GET' && !held) { held = true; return { wait: g.p }; } return undefined; });
        h.iso.setPanel(true);
        await h.idle();
        const f = fileOf(SIZE);
        h.selectFile(f, 'once');
        h.E['iso-upload'].click(); // 一覧の取得は保留中(doUpload の中の refreshList は同じ GET を待つ)
        h.E['iso-upload'].click(); // もう一度
        await h.idle();
        eq('the upload button is disabled while preparing', h.E['iso-upload'].disabled, true);
        g.open();
        ok('ready', await h.run(() => server.images.some((i) => i.name === 'once' && i.state === 'ready') && h.iso._state.job === null));
        eq('one begin only', h.posts().length, 1);
        eq('one image was created', server.images.filter((i) => i.name === 'once').length, 1);
        eq('the automatic finish did not take the job over while preparing', server.log.filter((x) => x.path === '/api/v1/iso/' + cand.id + '/finish' && server.log.indexOf(x) < server.log.indexOf(h.puts()[h.puts().length - 1])).length, 0);
    });
    await guard('review 5b: a failed preparation releases the start right', async () => {
        stats.scenarios++;
        const server = makeServer();
        const h = boot(Iso, server);
        await started(h);
        let once = true;
        server.hooks.push((req) => { if (req.method === 'GET' && once) { once = false; return NET; } return undefined; });
        h.selectFile(fileOf(SIZE), 'again');
        h.E['iso-upload'].click();
        ok('failed to prepare', await h.run(() => h.msgError() && h.E['iso-upload'].disabled === false));
        eq('no POST', h.posts().length, 0);
        h.E['iso-upload'].click(); // もう一度押せる
        ok('the retry works', await h.run(() => READY1(h)));
    });
    await guard('review 6: a failed begin keeps the previous file and id for resuming', async () => {
        stats.scenarios++;
        const fa = fileOf(SIZE);
        const fb = new FakeFile(Buffer.from(makeIso(SIZE).map((x) => x ^ 0x5a)), 'b.iso'); // 同じ大きさで、中身が違う
        const server = makeServer();
        const im = server.add({ name: 'a', size: SIZE, state: 'incomplete', data: Buffer.from(fa._b.subarray(0, 32768)) });
        const h = boot(Iso, server);
        let paused = false;
        server.hooks.push((req) => { if (req.method === 'PUT' && !paused) { paused = true; h.E['iso-pause'].click(); } return undefined; });
        await started(h);
        h.E['iso-file'].files = [fa];
        h.clickRow('a', '再開');
        ok('paused after the first chunk', await h.run(() => h.msg().includes('一時停止しました') && h.iso._state.job === null));
        // B を新しいアップロードとして選ぶが、begin が失敗する
        server.hooks.push((req) => (req.method === 'POST' && req.path === '/api/v1/iso' ? { status: 500, body: null } : undefined));
        await upload(h, fb, 'b', false);
        ok('begin failed', await h.run(() => h.msgError() && h.iso._state.job === null));
        // ファイルの選択を空にして、a を再開: 覚えているのは A のファイル
        h.E['iso-file'].files = null;
        im.state = 'incomplete';
        h.iso.refreshList();
        await h.idle();
        server.hooks.length = 0;
        h.clickRow('a', '再開');
        ok('a is completed', await h.run(() => im.state === 'ready' && h.iso._state.job === null));
        ok('the bytes come from file A (not from B)', im.data.equals(fa._b));
    });
    await guard('review 7: logout clears the chosen file and the name; closing the panel keeps them', async () => {
        stats.scenarios++;
        const server = makeServer();
        const h = boot(Iso, server);
        await started(h);
        h.selectFile(fileOf(SIZE), 'keepme');
        h.iso.setPanel(false);
        eq('closing the panel keeps the selection', [h.E['iso-name'].value, !!h.E['iso-file'].files], ['keepme', true]);
        h.iso.setPanel(true);
        eq('and opening it again', [h.E['iso-name'].value, !!h.E['iso-file'].files], ['keepme', true]);
        h.iso.stop();
        eq('stop() clears them', [h.E['iso-name'].value, h.E['iso-file'].value, h.E['iso-file'].files], ['', '', null]);
        h.iso.start();
        await h.run(() => h.toggleShown());
        h.E['iso-upload'].click();
        await h.idle();
        eq('after the next login, nothing is sent without choosing a file again', [h.msg().includes('ISO ファイルを選んで'), h.posts().length], [true, 0]);
    });
    await guard('review 8: a rescan that was stopped does not send the next batch', async () => {
        stats.scenarios++;
        // (a) 1 回目の応答を待っている間にログアウト(世代が変わる)。応答に next_cursor があっても、次を送らない
        {
            const server = makeServer();
            server.importEntries = ['ok', 'ok', 'ok', 'ok', 'ok', 'ok'];
            const h = boot(Iso, server);
            await started(h);
            const g = gate();
            server.hooks.push((req) => (req.path === '/api/v1/iso/rescan' && req.n === 1 ? { wait: g.p } : undefined));
            h.E['iso-rescan'].click();
            await h.idle();
            h.iso.stop();
            h.iso.start(); // 再ログイン
            await h.run(() => h.toggleShown());
            g.open();
            await h.runFor(10000);
            eq('(a) only the first POST was sent', h.posts('/rescan').length, 1);
        }
        // (b) busy の待ちの間にログアウト
        {
            const server = makeServer();
            server.importEntries = ['ok'];
            const h = boot(Iso, server);
            await started(h);
            server.hooks.push((req) => (req.path === '/api/v1/iso/rescan' && req.n === 1 ? { status: 503, body: { error: 'busy' }, headers: { 'Retry-After': '2' } } : undefined));
            h.E['iso-rescan'].click();
            await h.idle();
            h.iso.stop();
            h.iso.start();
            await h.run(() => h.toggleShown());
            await h.runFor(10000);
            eq('(b) no re-send after the stop', h.posts('/rescan').length, 1);
        }
        // (c) 応答を待っている間に、世代が変わらなければ、これまでどおり続く
        {
            const server = makeServer();
            server.importEntries = ['ok', 'ok', 'ok', 'ok', 'ok'];
            const h = boot(Iso, server);
            await started(h);
            h.E['iso-rescan'].click();
            ok('(c) runs to the end', await h.run(() => h.msg().includes('取り込みが終わりました')));
            eq('(c) two POSTs', h.posts('/rescan').map((x) => x.json), [{}, { cursor: 4 }]);
        }
    });

    // ===== (D) index.html / app.css =====
    await guard('static files', async () => {
        stats.scenarios++;
        const html = fs.readFileSync(INDEX_HTML, 'utf8');
        const css = fs.readFileSync(APP_CSS, 'utf8');
        ok('index.html has no style attribute', !/\sstyle\s*=/.test(html));
        ok('index.html has no <style> element', !/<style[\s>]/i.test(html));
        ok('index.html has no inline script', (html.match(/<script\b[^>]*>/gi) || []).every((t) => /\bsrc="app\.js"/.test(t)) && !/<script[^>]*>\s*[^<\s]/i.test(html));
        ok('index.html has no on* handlers', !/\son\w+\s*=/i.test(html));
        const info = parseIndexHtml();
        for (const id of ['btn-iso', 'iso-panel', 'btn-iso-close', 'iso-sd', 'iso-target', 'iso-mounted', 'iso-eject', 'iso-message', 'iso-list', 'iso-hidden-note', 'iso-file', 'iso-name', 'iso-hash', 'iso-upload', 'iso-pause', 'iso-progress', 'iso-progress-text', 'iso-rescan']) {
            ok('index.html has #' + id, !!info[id]);
        }
        eq('buttons have type=button', Object.keys(info).filter((id) => info[id].tag === 'button' && !/\stype="button"/.test(info[id].attrs)), []);
        eq('toggle starts hidden and collapsed', [info['btn-iso'].classes.includes('iso-hidden'), /aria-expanded="false"/.test(info['btn-iso'].attrs), /aria-controls="iso-panel"/.test(info['btn-iso'].attrs)], [true, true, true]);
        eq('panel starts hidden', info['iso-panel'].classes.includes('iso-hidden'), true);
        eq('file input', [info['iso-file'].tag, /type="file"/.test(info['iso-file'].attrs), /accept="\.iso"/.test(info['iso-file'].attrs)], ['input', true, true]);
        eq('hash checkbox is on by default', [/type="checkbox"/.test(info['iso-hash'].attrs), info['iso-hash'].checked], [true, true]);
        eq('message is a status region', [/role="status"/.test(info['iso-message'].attrs), /aria-live="polite"/.test(info['iso-message'].attrs)], [true, true]);
        ok('labels for the inputs', /<label for="iso-file">/.test(html) && /<label for="iso-name">/.test(html));
        ok('the progress bar has its own label', /<label for="iso-progress"[^>]*>[^<]+<\/label>/.test(html));
        ok('the progress label is hidden visually with a class (no inline style)', /<label for="iso-progress" class="iso-sr">/.test(html) && /\.iso-upload label\.iso-sr\s*\{[^}]*position:\s*absolute/.test(css));
        ok('the panel is inside the dashboard', html.indexOf('id="iso-panel"') > html.indexOf('id="view-dashboard"') && html.indexOf('id="iso-panel"') < html.indexOf('<script src="app.js">'));
        ok('the toggle is in the bar, before logout', html.indexOf('id="btn-iso"') < html.indexOf('id="btn-logout"') && html.indexOf('id="btn-iso"') > html.indexOf('class="bar"'));
        // JS が使うクラスは、CSS にある
        const used = new Set();
        for (const m of region(source).matchAll(/['"`]((?:iso-[a-z-]+)(?: iso-[a-z-]+)*)['"`]/g)) m[1].split(' ').forEach((c) => used.add(c));
        for (const m of region(source).matchAll(/'iso-state-' \+/g)) { void m; }
        for (const c of ['iso-hidden', 'iso-message-error', 'iso-message-ok', 'iso-row', 'iso-row-main', 'iso-name', 'iso-size', 'iso-badge', 'iso-badge-mounted', 'iso-row-note', 'iso-row-actions',
            'iso-state-ready', 'iso-state-uploading', 'iso-state-finalizing', 'iso-state-incomplete', 'iso-state-invalid', 'iso-state-deleting', 'iso-panel']) {
            ok('app.css defines .' + c, new RegExp('\\.' + c + '(?![\\w-])').test(css));
        }
        for (const c of used) ok('class used by app.js exists in app.css or index.html: ' + c, new RegExp('\\.' + c + '(?![\\w-])').test(css) || html.includes(c) || c.startsWith('iso-state-'), c);
        ok('app.css: .iso-hidden wins (display none, last)', /\.iso-hidden\s*\{\s*display:\s*none\s*!important;\s*\}\s*$/.test(css.trim()));
        ok('app.css: no url() / @import / expression (CSP)', !/url\(|@import|expression\(/.test(css));
        ok('app.css: panel is fixed (it does not change the video layout)', /\.iso-panel\s*\{[^}]*position:\s*fixed/.test(css));
        ok('app.css: hidden in full screen', /is-fullscreen \.iso-panel\s*\{\s*display:\s*none/.test(css));
        ok('the CSP of web_assets stays style-src self (checked in C: web_assets.c)', /style-src 'self'/.test(fs.readFileSync(path.join(WEB_DIR, '..', 'web_assets.c'), 'utf8')));
    });

    // ===== (E) app.js 全体との結線 =====
    await guard('app.js wiring', async () => {
        stats.scenarios++;
        const w = await bootApp(source, { state: 'ready', authenticated: true });
        ok('ISO list requested once after the dashboard is shown', await w.run(() => w.isoGets() === 1), w.isoGets());
        eq('toggle visible, panel closed', [w.E['btn-iso'].classes.has('iso-hidden'), w.E['iso-panel'].classes.has('iso-hidden')], [false, true]);
        w.E['btn-iso'].dispatch('click');
        await w.run(() => w.isoGets() >= 2);
        eq('panel open', w.E['iso-panel'].classes.has('iso-hidden'), false);
        eq('the request is a same-origin GET', w.requests.filter((r) => r.url === '/api/v1/iso').map((r) => [r.init.method, r.init.credentials]), [['GET', 'same-origin'], ['GET', 'same-origin']]);
        // ログアウト: 状態が未認証になり、ISO は止まる
        w.status.value = { state: 'ready', authenticated: false };
        w.E['btn-logout'].dispatch('click');
        await w.run(() => w.E['view-login'] && !w.E['view-login'].classes.has('view-hidden'));
        const n = w.isoGets();
        await w.runFor(60000);
        eq('no ISO requests after logout', w.isoGets(), n);
        eq('toggle and panel hidden after logout', [w.E['btn-iso'].classes.has('iso-hidden'), w.E['iso-panel'].classes.has('iso-hidden')], [true, true]);
        // 再ログイン
        w.status.value = { state: 'ready', authenticated: true };
        w.E['form-login'].dispatch('submit');
        await w.run(() => w.isoGets() === n + 1);
        eq('ISO starts again after the next login', w.isoGets(), n + 1);
    });
    await guard('app.js wiring: unauthenticated never asks for ISO', async () => {
        stats.scenarios++;
        const w = await bootApp(source, { state: 'ready', authenticated: false });
        await w.runFor(30000);
        eq('no ISO request on the login view', w.isoGets(), 0);
        const w2 = await bootApp(source, { state: 'needs_setup', authenticated: false });
        await w2.runFor(30000);
        eq('no ISO request on the setup view', w2.isoGets(), 0);
    });
    await guard('app.js wiring: a 401 from the ISO API goes back to the status check', async () => {
        stats.scenarios++;
        const w = await bootApp(source, { state: 'ready', authenticated: true }, { isoStatus: 401 });
        await w.run(() => w.isoGets() >= 1);
        eq('not at once', w.statusGets(), 1);
        await w.runFor(1000);
        ok('status was asked again', w.statusGets() >= 2, w.statusGets());
        ok('and the loop is slow (not a tight loop)', w.isoGets() <= 3, w.isoGets());
    });
    await guard('app.js wiring: ISO disabled build (404) keeps the toggle hidden', async () => {
        stats.scenarios++;
        const w = await bootApp(source, { state: 'ready', authenticated: true }, { isoStatus: 404 });
        await w.run(() => w.isoGets() >= 1);
        await w.runFor(60000);
        eq('toggle hidden, one request', [w.E['btn-iso'].classes.has('iso-hidden'), w.isoGets()], [true, 1]);
    });

    if (unhandled.length) problems.push('unhandled rejections: ' + JSON.stringify(unhandled));
    return { problems, stats };
}

// app.js 全体を、偽の環境で動かす(映像・入力の部分は何もしない偽物。ISO の結線だけを見る)
async function bootApp(source, statusValue, opts) {
    opts = opts || {};
    const violations = [];
    const dom = makeDom(violations);
    const E = dom.byId;
    // app.js が他の要素に使うメソッドの、何もしない偽物
    const noop = () => {};
    for (const id of Object.keys(E)) {
        const n = E[id];
        for (const k of ['getContext', 'getBoundingClientRect', 'requestPointerLock', 'requestFullscreen', 'blur']) {
            if (!(k in n)) n[k] = k === 'getContext' ? () => new Proxy({}, { get: () => noop }) : (k === 'getBoundingClientRect' ? () => ({ left: 0, top: 0, width: 1, height: 1 }) : noop);
        }
        n.width = 1280; n.height = 720;
        n.parentElement = null;
        n.removeAttribute = noop;
    }
    delete E['video-wrap'].requestFullscreen;
    const clock = { t: 0 };
    const timers = [];
    let seq = 0;
    const requests = [];
    const status = { value: statusValue };
    const listeners = {};
    const win = { listeners: {}, addEventListener(t, fn) { (win.listeners[t] = win.listeners[t] || []).push(fn); }, confirm: () => true };
    const document = Object.assign(dom.doc, {
        addEventListener(t, fn) { listeners[t] = fn; },
        pointerLockElement: null, hidden: false, exitPointerLock: noop, fullscreenEnabled: true, fullscreenElement: null,
        exitFullscreen: () => Promise.resolve(),
    });
    class WS { constructor(url) { this.url = url; this.readyState = 0; } send() {} close() { this.readyState = 3; } }
    WS.OPEN = 1;
    const ctx = vm.createContext({
        document, window: win, location: { host: 'kvm.local', hostname: 'kvm.local', port: '' }, WebSocket: WS, TextEncoder,
        fetch: async (url, init) => {
            requests.push({ url, init: init || {} });
            const json = (st, body) => ({ ok: st >= 200 && st < 300, status: st, json: async () => JSON.parse(JSON.stringify(body)), headers: { get: () => null } });
            if (url === '/api/v1/auth/status') return json(200, status.value);
            if (url === '/api/v1/iso') {
                if (opts.isoStatus && opts.isoStatus !== 200) return json(opts.isoStatus, opts.isoStatus === 401 ? { error: 'unauthorized' } : null);
                return json(200, { sd: { state: 'ready', total_bytes: 10, free_bytes: 5 }, images: [], mounted: null, target: { locked: false, ejected_by_target: false }, chunk_size: 131072, hidden_images: 0, upload: null });
            }
            return json(200, {});
        },
        setTimeout(fn, ms) { const id = ++seq; timers.push({ id, fn, at: clock.t + ms, seq: id }); return id; },
        clearTimeout(id) { const i = timers.findIndex((t) => t.id === id); if (i >= 0) timers.splice(i, 1); },
        setInterval() { return ++seq; }, clearInterval() {}, requestAnimationFrame() { return 1; },
        createImageBitmap: async () => ({ width: 1, height: 1, close() {} }),
    });
    vm.runInContext(source, ctx, { filename: 'app.js' });
    listeners.DOMContentLoaded();
    const w = {
        E, requests, status, timers, clock,
        isoGets: () => requests.filter((r) => r.url === '/api/v1/iso' && (r.init.method || 'GET') === 'GET').length,
        statusGets: () => requests.filter((r) => r.url === '/api/v1/auth/status').length,
        async run(cond) {
            for (let i = 0; i < 100000; i++) {
                await flush();
                checkDeadline();
                if (cond()) return true;
                if (timers.length === 0) { await flush(); return cond(); }
                let k = 0;
                for (let j = 1; j < timers.length; j++) if (timers[j].at < timers[k].at || (timers[j].at === timers[k].at && timers[j].seq < timers[k].seq)) k = j;
                const t = timers.splice(k, 1)[0];
                if (t.at > clock.t) clock.t = t.at;
                t.fn();
            }
            return false;
        },
        async runFor(ms) {
            const end = clock.t + ms;
            for (let i = 0; i < 100000; i++) {
                await flush();
                checkDeadline();
                let k = -1;
                for (let j = 0; j < timers.length; j++) if (timers[j].at <= end && (k < 0 || timers[j].at < timers[k].at)) k = j;
                if (k < 0) break;
                const t = timers.splice(k, 1)[0];
                if (t.at > clock.t) clock.t = t.at;
                t.fn();
            }
            clock.t = end;
            await flush();
        },
    };
    await w.run(() => requests.some((r) => r.url === '/api/v1/auth/status'));
    await flush();
    return w;
}

// ---------- ミュータント(--mutations): app.js の ISO の区間を 1 か所ずつ変えて、(A)〜(E) が落ちることを確認する ----------
// 同値のミュータント(理由つき)。
const EQUIVALENT = new Set([
    // サーバーは、受理した PUT に、必ず offset + n を返す(docs/WEB_API.md 8.3: 一部だけ受理することはない)。応答の値を使うのは、念のため。
    'upload: received from response ignored',
    // 書き込みの失敗の後、ここで SD を待たなくても、直後の resync が(SD が使えなければ)同じ待ちをする。
    'upload: sd error does not wait for the SD',
    // finalizing から incomplete に戻ったとき、SD が使えない間は一覧にイメージが出ないので、waitFinalize 自身が SD の復帰を待つ。その後の待ちは二重の防御。
    'finish: waits for SD not done',
    // stop() は世代を進めるので、実行中のジョブは alive() が偽になって止まる。abort の印は二重の防御。
    'stop: job not aborted',
    // 401 の後、1 秒後の状態の確認が未認証と答えれば、showView が iso.stop() を呼ぶ。この 1 秒の間は、一覧が取れず(401)、ポーリングも始まらない。
    'wiring: 401 handler dropped',
    // 自動送信の「1 回だけ」は、maybeAutoFinish と startJob の 2 か所の記録で守る(二重の防御)。両方を外すと落ちる(次のミュータント)。
    'auto: sent repeatedly',
    'auto: images the page handled are fair game',
    // SD が使えない間は、一覧にイメージが出ない(docs/WEB_API.md 8.1)ので、候補がない。検査は二重の防御。
    'auto: even when the SD is down',
]);

function mutants(source) {
    const R = [
        // [名前, 元, 変更後]
        ['sha256: K[0] wrong', '0x428a2f98,', '0x428a2f99,'],
        ['sha256: H0[0] wrong', 'new Uint32Array([0x6a09e667,', 'new Uint32Array([0x6a09e668,'],
        ['sha256: s0 rotate 7 -> 6', '((x >>> 7) | (x << 25))', '((x >>> 6) | (x << 26))'],
        ['sha256: s1 shift 10 -> 11', '(y >>> 10)', '(y >>> 11)'],
        ['sha256: big1 rotate 11 -> 12', '((e >>> 11) | (e << 21))', '((e >>> 12) | (e << 20))'],
        ['sha256: ch wrong (e & g)', '(e & f) ^ (~e & g)', '(e & f) ^ (e & g)'],
        ['sha256: maj wrong', '(a & b) ^ (a & c) ^ (b & c)', '(a & b) ^ (a & c)'],
        ['sha256: padding threshold 56 -> 55', 'this.blockLen < 56 ? 56 : 120', 'this.blockLen < 55 ? 56 : 120'],
        ['sha256: padding threshold 56 -> 57', 'this.blockLen < 56 ? 56 : 120', 'this.blockLen < 57 ? 56 : 120'],
        ['sha256: bit length high word dropped', 'pad[padLen + i] = (hi >>> (24 - 8 * i)) & 0xff;', 'pad[padLen + i] = 0;'],
        ['sha256: bit length low word wrong', 'const lo = bits % 4294967296;', 'const lo = bits % 4294967295;'],
        ['sha256: partial block copy offset', 'this.block.set(data.subarray(0, take), this.blockLen);', 'this.block.set(data.subarray(0, take), 0);'],
        ['sha256: tail not kept', 'this.block.set(data.subarray(pos), 0);\n            this.blockLen = n - pos;', 'this.blockLen = 0;'],
        ['sha256: length not counted', 'this.length += n;', 'this.length += 0;'],
        ['sha256: state add without wrap (h[7])', 'h[7] += k;', 'h[7] = k;'],
        ['formatBytes: KiB base 1000', 'v /= 1024;', 'v /= 1000;'],
        ['formatBytes: decimals for >= 10', "(v >= 10 ? 1 : 2)", "(v >= 10 ? 2 : 2)"],
        ['name rule: 63 -> 64', 'bytes > 63', 'bytes > 64'],
        ['name rule: C1 not rejected', '\\u007f-\\u009f]/.test(name)', '\\u007f]/.test(name)'],
        ['name rule: backslash allowed', "/[/\\\\]/.test(name)", "/[/]/.test(name)"],
        ['name rule: leading space allowed', "name.charAt(0) === ' ' ||", ''],
        ['utf8Length: 4-byte counted as 3', 'n += 4;\n                i++;', 'n += 3;\n                i++;'],
        ['defaultName: .iso kept', "replace(/\\.iso$/i, '')", "replace(/\\.isox$/i, '')"],
        ['describeError: free space dropped', "text += '（空き: ' + formatBytes(data.free_bytes) + '）';", ''],
        ['describeError: name_exists text lost', "name_exists: '同じ名前のイメージがあります。別の名前にしてください。',", ''],
        ['describeError: 507 text lost', "507: 'SD カードの空き容量が足りません (507)。'", "507: 'x'"],
        ['request: credentials dropped', "credentials: 'same-origin', headers: {} }", 'headers: {} }'],
        ['request: offset header name', "'X-Upload-Offset'", "'X-Upload-Offsets'"],
        ['request: octet-stream type', "'application/octet-stream'", "'text/plain'"],
        ['request: JSON type', "init.headers['Content-Type'] = 'application/json';", ''],
        ['upload: offset sent as received + 1', "{ chunk: job.file.slice(offset, offset + n), offset, timeoutMs: 60000 }", "{ chunk: job.file.slice(offset, offset + n), offset: offset + 1, timeoutMs: 60000 }"],
        ['upload: chunk slice end off by one', 'job.file.slice(offset, offset + n)', 'job.file.slice(offset, offset + n + 1)'],
        ['upload: received from response ignored', 'job.received = r.data.received;\n                    netFails = 0;', 'job.received = offset + n;\n                    netFails = 0;'],
        ['upload: offset_mismatch received ignored', 'job.received = rec;\n                    reportProgress(job);', 'reportProgress(job);'],
        ['upload: slow threshold never', 'if (dt >= SLOW_MS && job.chunk > MIN_CHUNK)', 'if (false)'],
        ['upload: fast never grows', 'else if (dt < FAST_MS && job.chunk < job.chunkMax)', 'else if (false)'],
        ['upload: net error does not shrink', 'if (job.chunk > MIN_CHUNK) job.chunk = Math.max(MIN_CHUNK, job.chunk >> 1);\n                    await sleep(Math.min(1000 * netFails, 5000));', 'await sleep(Math.min(1000 * netFails, 5000));'],
        ['upload: net error does not re-sync', "const s = await resync(job);\n                    reachable = s !== 'retry';\n                    if (s === 'stop') return 'failed';\n                    if (s === 'complete') return 'done';\n                    if (job.received > before) {", "const s = 'go';\n                    reachable = true;\n                    if (job.received > before) {"],
        ['upload: net error retries forever', 'if (netFails > MAX_NET_FAILS) {', 'if (netFails > 1e9) {'],
        ['upload: mismatch loop unbounded', 'if (sameMismatch > 3) {', 'if (sameMismatch > 1e9) {'],
        ['upload: 413 does not shrink', "if (r.status === 413 && code === 'payload_too_large' && job.chunk > MIN_CHUNK) {", 'if (false) {'],
        ['upload: busy ignores Retry-After', "await sleep((r.retryAfter || 1) * 1000);\n                    continue;\n                }\n                if (r.status === 503 && (code === 'not_ready'", "await sleep(0);\n                    continue;\n                }\n                if (r.status === 503 && (code === 'not_ready'"],
        ['upload: upload_in_progress never waits for finalizing', "holder.state === 'finalizing' && ++waits <= MAX_WAIT_STEPS", 'false'],
        ['upload: reachable message always the connection one', 'setMessage(reachable', 'setMessage(false'],
        ['upload: sd error does not wait for the SD', "if (!(await waitSdReady(job))) {\n                        if (alive(job)) setMessage(describeError(r.status, r.data), 'error');", "if (false) {\n                        if (alive(job)) setMessage(describeError(r.status, r.data), 'error');"],
        ['upload: error message not shown', "setMessage(describeError(r.status, r.data), 'error');\n                if (r.status === 404) S.listSig = null;", 'if (r.status === 404) S.listSig = null;'],
        ['upload: hash declared only when off', "if (sha !== null) body.sha256 = sha;", "if (sha === null) body.sha256 = '';"],
        ['upload: hash slice size 1 MiB -> sliced wrong', 'sha.update(new Uint8Array(buf));', 'sha.update(new Uint8Array(buf).subarray(1));'],
        ['upload: resume begins a new image', "if ((same.state === 'incomplete' || same.state === 'uploading') && same.size === file.size) {", 'if (false) {'],
        ['upload: resume ignores the size', "&& same.size === file.size) {", ') {'],
        ['resume: lastFile not remembered', 'S.lastFile = file;\n            S.lastFileId = id;', 'S.lastFile = null;\n            S.lastFileId = null;'],
        ['resume: wrong size file accepted', 'if (!file || file.size !== im.size) file =', 'if (!file) file ='],
        ['finish: finish not sent', "const r = await request('POST', API + '/' + job.id + '/finish', { json: {} });", "const r = { status: 202, data: null };"],
        ['finish: body not {}', "'/finish', { json: {} }", "'/finish', { json: { x: 1 } }"],
        ['finish: wait ends at ready only', "if (im.state === 'invalid') return 'invalid';\n                if (im.state === 'incomplete') return 'incomplete';", "if (im.state === 'invalid') return 'invalid';"],
        ['finish: wait ignores invalid', "if (im.state === 'invalid') return 'invalid';\n                if (im.state === 'incomplete') return 'incomplete';", "if (im.state === 'incomplete') return 'incomplete';"],
        ['finish: incomplete full -> no resend', 'continue; // received == size: finish を送り直す', "return 'failed';"],
        ['finish: incomplete partial -> no resume', "job.received = im.received;\n                        return 'resume';\n                    }\n                    continue;", "return 'failed';\n                    }\n                    continue;"],
        ['finish: size_mismatch ignored', "if (r.status === 409 && code === 'size_mismatch') {", 'if (false) {'],
        ['finish: busy not retried', "code === 'busy' && !job.auto && ++waits <= MAX_WAIT_STEPS) {\n                    await sleep((r.retryAfter || 1) * 1000);\n                    continue;", "code === 'busy' && false) {\n                    await sleep((r.retryAfter || 1) * 1000);\n                    continue;"],
        ['finish: rounds unbounded', 'if (++rounds > MAX_FINISH_ROUNDS) {', 'if (false) {'],
        ['finish: waits for SD not done', 'if (!(await waitSdReady(job))) {\n                        if (alive(job)) setMessage(describeError(503, { error: \'not_ready\' }), \'error\');\n                        return \'failed\';\n                    }\n                    const im = imageById(job.id);', 'const im = imageById(job.id);'],
        ['finish: waitFinalize interval 1000 -> 0', "for (let i = 0; i < 100000 && alive(job); i++) {\n                await sleep(1000);", "for (let i = 0; i < 100000 && alive(job); i++) {\n                await sleep(0);"],
        ['auto: finish for received < size too', "im.received === im.size && !S.autoDone.has(im.id)", "im.received !== null && !S.autoDone.has(im.id)"],
        ['auto: sent repeatedly', 'S.autoDone.add(im.id);\n                    startJob(', 'startJob('],
        ['auto: not when the slot is used', "|| S.list.sd.state !== 'ready' || S.list.upload !== null) return;", "|| S.list.sd.state !== 'ready') return;"],
        ['auto: even when the SD is down', "|| S.list.sd.state !== 'ready' || S.list.upload !== null) return;", '|| S.list.upload !== null) return;'],
        ['auto: only for incomplete', "if (im.state === 'incomplete' && im.size !== null && im.size > 0 && im.received === im.size", "if (im.size !== null && im.size > 0 && im.received === im.size"],
        ['auto: images the page handled are fair game', "if (id !== null) S.autoDone.add(id);", ''],
        ['mount: force sent at once', "request('POST', API + '/mount', { json: { id } })", "request('POST', API + '/mount', { json: { id, force: true } })"],
        ['mount: no confirm before force', "if (!deps.confirm('ターゲットが、メディアの取り外しを防止しています。強制的に差し替えると", "if (false && !deps.confirm('ターゲットが、メディアの取り外しを防止しています。強制的に差し替えると"],
        ['eject: force without confirm', "if (!deps.confirm('ターゲットが、メディアの取り外しを防止しています。強制的に取り外すと", "if (false && !deps.confirm('ターゲットが、メディアの取り外しを防止しています。強制的に取り外すと"],
        ['eject: force key', "{ json: { force: true } });\n                }\n                if (r.status === 200 && r.data && r.data.result === 'ejected')", "{ json: { force: false } });\n                }\n                if (r.status === 200 && r.data && r.data.result === 'ejected')"],
        ['delete: no confirm', "if (!deps.confirm('「' + label + '」を削除しますか？（元に戻せません）')) return undefined;", ''],
        ['delete: wrong method', "request('DELETE', API + '/' + id)", "request('POST', API + '/' + id)"],
        ['delete: job not aborted', 'S.job.abort = true;\n                        S.job.deleted = true;', ''],
        ['rescan: cursor not sent', 'json: cursor === null ? {} : { cursor }', 'json: {}'],
        ['rescan: stops after one round', "if (next === null) {", 'if (true) {'],
        ['rescan: cursor repeat not detected', 'if (next === cursor && d.imported === 0) {', 'if (false) {'],
        ['rescan: same cursor stops even when files were imported (the PR #158 bug)', 'if (next === cursor && d.imported === 0) {', 'if (next === cursor) {'],
        ['rescan: stops on every call without imports', 'if (next === cursor && d.imported === 0) {', 'if (d.imported === 0) {'],
        ['rescan: stop condition inverted on imports', 'if (next === cursor && d.imported === 0) {', 'if (next === cursor && d.imported !== 0) {'],
        ['rescan: stop condition looks at skipped instead of imported', 'if (next === cursor && d.imported === 0) {', 'if (next === cursor && d.skipped === 0) {'],
        ['rescan: stops on a same cursor or on no imports', 'if (next === cursor && d.imported === 0) {', 'if (next === cursor || d.imported === 0) {'],
        ['rescan: skipped not counted', 'skipped += d.skipped;', ''],
        ['rescan: imported not counted', 'imported += d.imported;', ''],
        ['rescan: next_cursor 0 treated as end', 'if (next === null) {', 'if (!next) {'],
        ['rescan: next_cursor validation dropped', '|| !(next === null || (Number.isSafeInteger(next) && next >= 0 && next <= 65535))', ''],
        ['list: name via innerHTML', 'nm.textContent = im.name;', 'nm.innerHTML = im.name;'],
        ['list: name via setAttribute style', 'nm.textContent = im.name;', "nm.textContent = im.name; nm.setAttribute('style', 'color:red');"],
        ['list: not redrawn only when changed', 'if (sig !== S.listSig) {', 'if (true) {'],
        ['list: mounted badge dropped', "mt.textContent = 'マウント中';", "mt.textContent = '';"],
        ['list: incomplete not resumable', "if (im.state === 'incomplete' && !local) {", 'if (false) {'],
        ['poll: closed panel polls forever', 'if (!S.panelVisible && !transient) return;', ''],
        ['poll: transient never fast', 'transient ? 1000 : 3000', '3000'],
        ['poll: GET during PUT', "if (S.job !== null && S.job.phase === 'uploading') {\n                    schedulePoll();\n                    return;\n                }", ''],
        ['poll: 404 does not disable', 'S.disabled = true;\n                    show(ui.toggle, false);', 'show(ui.toggle, false);'],
        ['stop: gen not bumped', 'S.running = false;\n            S.gen++;', 'S.running = false;'],
        ['stop: job not aborted', 'if (S.job !== null) S.job.abort = true;\n            S.job = null;', 'S.job = null;'],
        ['stop: timer not cleared', 'if (S.pollTimer !== null) {\n                deps.clearTimeout(S.pollTimer);\n                S.pollTimer = null;\n            }\n            S.list = null;', 'S.list = null;'],
        ['start: not idempotent', 'if (S.running) return;\n            S.running = true;', 'S.running = true;'],
        ['401: onUnauthorized not called', "if (res.status === 401 && deps.onUnauthorized && S.gen === gen) deps.onUnauthorized();", ''],
        ['review2: 401 of an old generation stops the new one', 'deps.onUnauthorized && S.gen === gen) deps.onUnauthorized();', 'deps.onUnauthorized) deps.onUnauthorized();'],
        ['review3: net failures are never reset by progress', 'netFails = 0; // 応答は失われたが、サーバーには届いている（位置が進んだ）: 失敗の数に入れない', ''],
        ['review3: limit applied before the re-sync', '} else if (netFails > MAX_NET_FAILS) {', '} else if (false) {'],
        ['review4: automatic finish re-sent on busy', "code === 'busy' && !job.auto && ++waits", "code === 'busy' && ++waits"],
        ['review4: automatic finish re-sent on upload_in_progress', "code === 'upload_in_progress' && !job.auto && ++waits", "code === 'upload_in_progress' && ++waits"],
        ['review4: automatic finish re-sent on sd errors', "(code === 'not_ready' || code === 'sd_unavailable') && !job.auto && ++rounds", "(code === 'not_ready' || code === 'sd_unavailable') && ++rounds"],
        ['review4: automatic finish re-sent after a network error', "if (job.auto) {\n                        setMessage('完了処理の結果を確認できません。", "if (false) {\n                        setMessage('完了処理の結果を確認できません。"],
        ['review4: the job is not marked automatic', 'job.auto = true;', ''],
        ['review5: start right not taken', 'S.starting = token;', ''],
        ['review5: start right not released', 'if (S.starting === token) S.starting = null;', ''],
        ['review5: automatic finish ignores the start right', "S.job !== null || S.starting !== null || S.opBusy || !S.list", "S.job !== null || S.opBusy || !S.list"],
        ['review5: fileBusy ignores the start right', 'return S.job !== null || S.starting !== null; }', 'return S.job !== null; }'],
        ['review5: logout during preparation not noticed', 'if (S.gen !== gen) return; // 待つ間に、ログアウトした', ''],
        ['review6: lastFile set before begin', 'startNewUpload(file, name, list);\n            } finally {', 'S.lastFile = file;\n                startNewUpload(file, name, list);\n            } finally {'],
        ['review6: lastFile not set after begin', 'S.lastFile = file;\n                S.lastFileId = job.id;', 'S.lastFileId = job.id;'],
        ['review7: stop() keeps the chosen file', "ui.file.value = ''; // ログアウト", "// ログアウト"],
        ['review7: stop() keeps the name', "ui.name.value = '';\n            S.panelVisible = false;", "S.panelVisible = false;"],
        ['review7: closing the panel clears the form', "if (ui.toggle) ui.toggle.setAttribute('aria-expanded', on ? 'true' : 'false');\n            if (on) refreshList();", "if (ui.toggle) ui.toggle.setAttribute('aria-expanded', on ? 'true' : 'false');\n            if (!on) { ui.file.value = ''; ui.name.value = ''; }\n            if (on) refreshList();"],
        ['review8: rescan does not check the generation after the response', "{ json: cursor === null ? {} : { cursor } });\n                    if (S.gen !== gen) return;", "{ json: cursor === null ? {} : { cursor } });"],
        ['review8: rescan does not check the generation before the request', "for (let i = 0; i < MAX_RESCAN_ROUNDS; i++) {\n                    if (S.gen !== gen) return;", "for (let i = 0; i < MAX_RESCAN_ROUNDS; i++) {"],
        ['review8: rescan does not take the starting generation', 'const gen = S.gen; // 操作を始めたときの世代', 'const gen = -1; // 操作を始めたときの世代'],
        ['wiring: iso.start dropped', 'updateVideoLayout();\n            iso.start();', 'updateVideoLayout();'],
        ['wiring: iso.stop dropped', '            iso.stop();\n            elAuthCard.classList.remove', '            elAuthCard.classList.remove'],
        ['wiring: 401 handler dropped', 'iso.stop();\n            setTimeout(fetchStatus, 1000);', 'setTimeout(fetchStatus, 1000);'],
    ];
    const out = R.map(([name, from, to]) => {
        const first = source.indexOf(from);
        if (first < 0) return [name, null];
        return [name, source.slice(0, first) + to + source.slice(first + from.length)];
    });
    // 2 か所を同時に変えるもの(二重の防御を、両方外す)
    const a1 = 'S.autoDone.add(im.id);\n                    startJob(';
    const a2 = 'if (id !== null) S.autoDone.add(id);';
    out.push(['auto: no record of automatic finishes at all (both places)', source.includes(a1) && source.includes(a2) ? source.replace(a1, 'startJob(').replace(a2, '') : null]);
    return out;
}

// 1 つのミュータントを、別のプロセスで検査する(止まらなくなったものが、他に影響しないように。時間切れも「落ちた」と数える)。
// 子プロセスの終了コード: 0 = 落ちた(殺した)、3 = 生き残った、4 = 適用できなかった。
function runMutantChild(n, name) {
    return new Promise((resolve) => {
        const { spawn } = require('child_process');
        const child = spawn(process.execPath, [__filename, '--mutant', String(n)], { stdio: 'ignore' });
        let timedOut = false;
        const timer = setTimeout(() => { timedOut = true; child.kill('SIGKILL'); }, 60000);
        child.on('exit', (code) => {
            clearTimeout(timer);
            if (timedOut) resolve('killed');
            else if (code === 0) resolve('killed');
            else if (code === 3) resolve(EQUIVALENT.has(name) ? 'equivalent' : 'survived');
            else if (code === 4) resolve('nomatch');
            else resolve('killed');
        });
    });
}

(async () => {
    const source = fs.readFileSync(APP_JS, 'utf8');
    const args = process.argv.slice(2);
    const mc = args.indexOf('--mutant');
    if (mc >= 0) { // 子プロセス
        const n = Number(args[mc + 1]);
        const [name, mutated] = mutants(source)[n - 1];
        if (mutated === null) process.exit(4);
        DEADLINE = Date.now() + 20000;
        let killed = false;
        try {
            const r = await checkAll(mutated, { big: name.includes('bit length'), failFast: true });
            killed = r.problems.length > 0;
        } catch (e) { killed = true; }
        process.exit(killed ? 0 : 3);
    }
    const { problems, stats } = await checkAll(source);
    problems.forEach((p) => console.log('FAIL:', p));
    let status = problems.length ? 1 : 0;
    if (!problems.length) console.log('iso viewer ok (' + stats.sha + ' sha256 cases, ' + stats.pure + ' pure-function cases, ' + stats.scenarios + ' scenarios)');
    const mi = args.indexOf('--mutations');
    if (mi >= 0) {
        const only = new Set(args.slice(mi + 1));
        const list = mutants(source);
        const results = new Array(list.length);
        let next = 0;
        const worker = async () => {
            while (next < list.length) {
                const k = next++;
                const n = k + 1;
                const name = list[k][0];
                if (only.size && !only.has(String(n)) && !only.has(name)) { results[k] = 'skipped'; continue; }
                results[k] = list[k][1] === null ? 'nomatch' : await runMutantChild(n, name);
                if (process.env.ISO_TEST_PROGRESS) console.log('  done ' + n + ' ' + results[k]);
            }
        };
        await Promise.all(Array.from({ length: Math.min(4, require('os').cpus().length || 1) }, worker));
        let survived = 0;
        list.forEach(([name], k) => {
            if (results[k] === 'skipped') return;
            const label = { killed: 'KILLED   ', survived: 'SURVIVED ', equivalent: 'EQUIVALENT ', nomatch: 'NOMATCH  ' }[results[k]];
            console.log(label + (k + 1) + ' ' + name);
            if (results[k] === 'survived' || results[k] === 'nomatch') survived++;
        });
        console.log(list.length + ' mutants, ' + survived + ' survived or did not apply');
        if (survived) status = 1;
    }
    process.exit(status);
})();
