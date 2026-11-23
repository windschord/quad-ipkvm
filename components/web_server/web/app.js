// ==== ISO viewer (begin) ====
// ISO の仮想メディアの画面（docs/WEB_API.md 8、docs/ISO_MOUNT_DESIGN.md 6）。ブラウザ側だけのコードで、
// tools/test/check_iso_viewer.js が、偽の DOM / fetch / タイマーの中で動かして検査する。
// ここは DOM の外に出る値を `deps`（document、fetch、setTimeout など）で受け取る。ブラウザのログ出力は使わない
// （名前、セッション、データを残さないため）。名前は textContent だけで表示する（HTML として解釈させない）。
const IsoViewer = (function () {
    'use strict';

    const API = '/api/v1/iso';

    // ---- SHA-256（FIPS 180-4。逐次。ブラウザの crypto.subtle.digest は一括で、数 GiB のファイルに使えないため自前）----
    // 定数は、素数の平方根・立方根の小数部の上位 32 ビット（FIPS 180-4 4.2.2、5.3.3）。
    const SHA_K = new Uint32Array([
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
    ]);
    const SHA_H0 = new Uint32Array([0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19]);

    function Sha256() {
        this.h = new Uint32Array(SHA_H0);
        this.block = new Uint8Array(64);
        this.blockLen = 0;
        this.length = 0; // bytes so far (a double: exact up to 2^53)
        this.w = new Uint32Array(64);
        this.finished = false;
    }

    Sha256.prototype.compress = function (data, off) {
        const w = this.w;
        const h = this.h;
        for (let i = 0; i < 16; i++) {
            const j = off + i * 4;
            w[i] = (data[j] << 24) | (data[j + 1] << 16) | (data[j + 2] << 8) | data[j + 3];
        }
        for (let i = 16; i < 64; i++) {
            const x = w[i - 15];
            const y = w[i - 2];
            const s0 = ((x >>> 7) | (x << 25)) ^ ((x >>> 18) | (x << 14)) ^ (x >>> 3);
            const s1 = ((y >>> 17) | (y << 15)) ^ ((y >>> 19) | (y << 13)) ^ (y >>> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        let a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
        for (let i = 0; i < 64; i++) {
            const big1 = ((e >>> 6) | (e << 26)) ^ ((e >>> 11) | (e << 21)) ^ ((e >>> 25) | (e << 7));
            const ch = (e & f) ^ (~e & g);
            const t1 = (k + big1 + ch + SHA_K[i] + w[i]) | 0;
            const big0 = ((a >>> 2) | (a << 30)) ^ ((a >>> 13) | (a << 19)) ^ ((a >>> 22) | (a << 10));
            const maj = (a & b) ^ (a & c) ^ (b & c);
            const t2 = (big0 + maj) | 0;
            k = g; g = f; f = e; e = (d + t1) | 0; d = c; c = b; b = a; a = (t1 + t2) | 0;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += k;
    };

    Sha256.prototype.update = function (data) {
        if (this.finished) throw new Error('sha256: already finished');
        const n = data.length;
        let pos = 0;
        this.length += n;
        if (this.blockLen > 0) {
            const take = Math.min(64 - this.blockLen, n);
            this.block.set(data.subarray(0, take), this.blockLen);
            this.blockLen += take;
            pos = take;
            if (this.blockLen === 64) {
                this.compress(this.block, 0);
                this.blockLen = 0;
            }
        }
        while (pos + 64 <= n) {
            this.compress(data, pos);
            pos += 64;
        }
        if (pos < n) {
            this.block.set(data.subarray(pos), 0);
            this.blockLen = n - pos;
        }
        return this;
    };

    Sha256.prototype.digest = function () {
        const bits = this.length * 8;
        const padLen = (this.blockLen < 56 ? 56 : 120) - this.blockLen;
        const pad = new Uint8Array(padLen + 8);
        pad[0] = 0x80;
        const hi = Math.floor(bits / 4294967296);
        const lo = bits % 4294967296;
        for (let i = 0; i < 4; i++) {
            pad[padLen + i] = (hi >>> (24 - 8 * i)) & 0xff;
            pad[padLen + 4 + i] = (lo >>> (24 - 8 * i)) & 0xff;
        }
        this.update(pad);
        this.finished = true;
        let out = '';
        for (let i = 0; i < 8; i++) out += ('00000000' + (this.h[i] >>> 0).toString(16)).slice(-8);
        return out;
    };

    function sha256Hex(bytes) {
        return new Sha256().update(bytes).digest();
    }

    // ---- 表示用の純関数 ----
    function formatBytes(n) {
        if (typeof n !== 'number' || !isFinite(n) || n < 0) return '?';
        if (n < 1024) return n + ' B';
        const units = ['KiB', 'MiB', 'GiB', 'TiB'];
        let v = n;
        let u = -1;
        do {
            v /= 1024;
            u++;
        } while (v >= 1024 && u < units.length - 1);
        return v.toFixed(v >= 100 ? 0 : (v >= 10 ? 1 : 2)) + ' ' + units[u];
    }

    // UTF-8 のバイト数（TextEncoder に頼らない。対になっていないサロゲートは 3 バイト（U+FFFD）として数える）
    function utf8Length(s) {
        let n = 0;
        for (let i = 0; i < s.length; i++) {
            const c = s.charCodeAt(i);
            if (c < 0x80) n += 1;
            else if (c < 0x800) n += 2;
            else if (c >= 0xd800 && c <= 0xdbff && i + 1 < s.length && s.charCodeAt(i + 1) >= 0xdc00 && s.charCodeAt(i + 1) <= 0xdfff) {
                n += 4;
                i++;
            } else n += 3;
        }
        return n;
    }

    // 名前の規則（docs/WEB_API.md 8 の基本方針）。規則に合えば null、合わなければ理由の文。
    function nameProblem(name) {
        if (typeof name !== 'string' || name.length === 0) return '名前を入力してください。';
        const bytes = utf8Length(name);
        if (bytes < 1 || bytes > 63) return '名前は UTF-8 で 1〜63 バイトにしてください（今は ' + bytes + ' バイト）。';
        if (/[\u0000-\u001f\u007f-\u009f]/.test(name)) return '名前に制御文字は使えません。';
        if (/[/\\]/.test(name)) return '名前に / と \\ は使えません。';
        if (name.charAt(0) === ' ' || name.charAt(name.length - 1) === ' ') return '名前の前後に空白は使えません。';
        if (/[\ud800-\udbff](?![\udc00-\udfff])|(?:^|[^\ud800-\udbff])[\udc00-\udfff]/.test(name)) return '名前に不正な文字が含まれています。';
        return null;
    }

    // ファイル名から、名前の初期値を作る（拡張子 .iso を除き、使えない文字を _ にし、63 バイトに切る）
    function defaultName(fileName) {
        let s = String(fileName || '').replace(/\.iso$/i, '');
        s = s.replace(/[\u0000-\u001f\u007f-\u009f/\\]/g, '_').replace(/^ +| +$/g, '');
        let out = '';
        let bytes = 0;
        for (const ch of s) {
            const b = utf8Length(ch);
            if (bytes + b > 63) break;
            out += ch;
            bytes += b;
        }
        out = out.replace(/ +$/, '');
        return out.length > 0 ? out : 'image';
    }

    // ---- エラーの文言（docs/WEB_API.md 8 の各表） ----
    const ERROR_TEXT = {
        invalid_request: 'リクエストの内容が正しくありません（名前、サイズ、位置などを確認してください）。',
        not_found: 'そのイメージが見つかりません（削除された可能性があります）。',
        unsupported_media_type: 'リクエストの形式（Content-Type）が受け付けられません。ページを再読み込みしてください。',
        length_required: 'リクエストの長さ（Content-Length）がありません。ブラウザが対応していない可能性があります。',
        payload_too_large: '1 回に送る大きさが上限を超えています。',
        not_ready: 'SD カードの準備ができていません。カードの状態を確認してください。',
        sd_unavailable: 'SD カードの読み書きでエラーが起きました。カードが戻るまで待ってください。',
        busy: '機器が別の処理をしています。しばらくしてからやり直してください。',
        upload_in_progress: '別のイメージのアップロード、または最終処理が進んでいます。終わってからやり直してください。',
        name_exists: '同じ名前のイメージがあります。別の名前にしてください。',
        too_many_images: 'イメージの数が上限に達しています。不要なものを削除してください。',
        not_uploading: 'そのイメージは、受信できる状態ではありません（完了済み、最終処理中、または無効）。',
        image_not_ready: 'そのイメージは、まだ使えません（最終処理中、無効、またはアップロード未完了）。',
        mounted: 'マウント中のイメージは削除できません。先に取り外してください。',
        crosslinked: '取り込み中のファイルと領域が重なっています。削除できません。PC で SD カードのチェックディスクを実行してください。',
        locked_by_target: 'ターゲットが、メディアの取り外しを防止しています。',
        offset_mismatch: '送信位置がサーバーと合いません。',
        size_mismatch: 'アップロードがまだ最後まで届いていません。',
        insufficient_storage: 'SD カードの空き容量が足りません。',
        checksum_mismatch: 'SHA-256 が、アップロード時に申告した値と一致しません。ファイルが壊れた可能性があります。',
        not_iso: 'ISO イメージではありません（ISO 9660 / UDF の署名が見つかりません）。',
        too_fragmented: 'SD カード上のファイルが断片化しすぎていて、マウントできません。SD カードを整理（再フォーマット）してください。',
        part_mismatch: '保存されたパーツが欠けているか、サイズが合いません。削除して、アップロードし直してください。',
        meta_corrupt: 'イメージの管理情報が壊れています。削除してください。'
    };
    const STATUS_TEXT = {
        400: 'リクエストが正しくありません (400)。',
        401: 'ログインの有効期限が切れました。ログインし直してください (401)。',
        403: 'リクエストが拒否されました (403)。ページを再読み込みしてください。',
        404: '見つかりません (404)。',
        405: 'この操作は許可されていません (405)。',
        409: '現在の状態では実行できません (409)。',
        411: 'リクエストの長さ（Content-Length）がありません (411)。',
        413: 'リクエストが大きすぎます (413)。',
        415: 'リクエストの形式が受け付けられません (415)。',
        422: 'イメージの検査に失敗しました (422)。',
        503: '機器が使えない状態です (503)。しばらく待ってください。',
        507: 'SD カードの空き容量が足りません (507)。'
    };
    const REASON_TEXT = {
        checksum_mismatch: ERROR_TEXT.checksum_mismatch,
        not_iso: ERROR_TEXT.not_iso,
        too_fragmented: ERROR_TEXT.too_fragmented,
        part_mismatch: ERROR_TEXT.part_mismatch,
        meta_corrupt: ERROR_TEXT.meta_corrupt
    };
    const has = (o, k) => Object.prototype.hasOwnProperty.call(o, k);

    // status: HTTP ステータス、data: 応答の JSON（なければ null）。表示する日本語の文を返す。
    function describeError(status, data) {
        const code = data && typeof data === 'object' && typeof data.error === 'string' ? data.error : null;
        let text;
        if (code !== null && has(ERROR_TEXT, code)) text = ERROR_TEXT[code];
        else if (has(STATUS_TEXT, status)) text = STATUS_TEXT[status] + (code !== null ? '（' + code + '）' : '');
        else text = 'エラー (' + status + ')' + (code !== null ? '：' + code : '');
        if (data && typeof data === 'object') {
            if (code === 'insufficient_storage' && Number.isSafeInteger(data.free_bytes) && data.free_bytes >= 0) {
                text += '（空き: ' + formatBytes(data.free_bytes) + '）';
            }
            if (code === 'size_mismatch' && Number.isSafeInteger(data.received) && Number.isSafeInteger(data.size)) {
                text += '（受信済み: ' + formatBytes(data.received) + ' / ' + formatBytes(data.size) + '）';
            }
        }
        return text;
    }

    const STATE_TEXT = {
        uploading: '受信中',
        incomplete: '中断（再開できます）',
        finalizing: '最終処理中',
        ready: '利用可能',
        invalid: '無効',
        deleting: '削除中'
    };
    const SD_TEXT = {
        ready: 'SD カード: 使えます',
        no_card: 'SD カード: ありません（または初期化できません）',
        unsupported_fs: 'SD カード: FAT32 ではありません（FAT32 で再フォーマットしてください）',
        error: 'SD カード: 読み書きでエラーが起きています'
    };

    // ---- GET /api/v1/iso の応答を、検査して整える（形が違えば null） ----
    const isCount = (v) => typeof v === 'number' && Number.isSafeInteger(v) && v >= 0;
    function normalizeList(d) {
        if (!d || typeof d !== 'object' || !d.sd || typeof d.sd !== 'object' || typeof d.sd.state !== 'string') return null;
        const images = [];
        if (Array.isArray(d.images)) {
            for (const im of d.images) {
                if (!im || typeof im !== 'object' || typeof im.id !== 'string' || typeof im.name !== 'string' || typeof im.state !== 'string') continue;
                images.push({
                    id: im.id,
                    name: im.name,
                    state: im.state,
                    size: isCount(im.size) ? im.size : null,
                    received: isCount(im.received) ? im.received : null,
                    reason: typeof im.invalid_reason === 'string' ? im.invalid_reason : null
                });
            }
        }
        const up = d.upload && typeof d.upload === 'object' && typeof d.upload.id === 'string'
            ? { id: d.upload.id, state: typeof d.upload.state === 'string' ? d.upload.state : '' } : null;
        return {
            sd: { state: d.sd.state, total: isCount(d.sd.total_bytes) ? d.sd.total_bytes : null, free: isCount(d.sd.free_bytes) ? d.sd.free_bytes : null },
            images,
            mounted: d.mounted && typeof d.mounted === 'object' && typeof d.mounted.id === 'string' ? d.mounted.id : null,
            locked: !!(d.target && d.target.locked === true),
            ejectedByTarget: !!(d.target && d.target.ejected_by_target === true),
            chunk: isCount(d.chunk_size) && d.chunk_size > 0 ? d.chunk_size : 16384,
            hidden: isCount(d.hidden_images) ? d.hidden_images : 0,
            upload: up
        };
    }

    // ---- 定数 ----
    const MIN_CHUNK = 4096;
    const SLOW_MS = 1500; // 1 回の PUT がこれ以上かかったら、次から半分にする（サーバーは 2 秒で打ち切る）
    const FAST_MS = 500; // これ未満なら、倍にする（chunk_size まで）
    const HASH_SLICE = 1048576;
    const MAX_NET_FAILS = 6; // 進まないまま続けて失敗した回数の上限
    const MAX_WAIT_STEPS = 120; // 待ちの上限（1 回 1〜2 秒）
    const MAX_FINISH_ROUNDS = 5;
    const MAX_RESCAN_ROUNDS = 70000; // cursor は 0〜65535 で、各回で進む

    function create(deps) {
        const doc = deps.document;
        const $ = (id) => doc.getElementById(id);
        const ui = {
            toggle: $('btn-iso'), panel: $('iso-panel'), close: $('btn-iso-close'),
            sd: $('iso-sd'), target: $('iso-target'), mounted: $('iso-mounted'), eject: $('iso-eject'),
            message: $('iso-message'), list: $('iso-list'), hiddenNote: $('iso-hidden-note'),
            file: $('iso-file'), name: $('iso-name'), hash: $('iso-hash'), upload: $('iso-upload'), pause: $('iso-pause'),
            progress: $('iso-progress'), progressText: $('iso-progress-text'), rescan: $('iso-rescan')
        };
        const now = deps.now || (() => Date.now());
        const S = {
            running: false, gen: 0, disabled: false, panelVisible: false,
            list: null, listSig: null, pollTimer: null, refreshing: null,
            job: null, starting: null, opBusy: false, autoDone: new Set(), lastFile: null, lastFileId: null,
            message: null, rendered: []
        };

        const sleep = (ms) => new Promise((resolve) => { deps.setTimeout(resolve, ms); });
        const alive = (job) => S.gen === job.gen && !job.abort;

        // ---- 画面の小さな部品 ----
        function show(elm, on) {
            if (!elm) return;
            if (on) elm.classList.remove('iso-hidden');
            else elm.classList.add('iso-hidden');
        }
        function setMessage(text, kind) {
            S.message = text ? { text, kind: kind || 'info' } : null;
            ui.message.textContent = text || '';
            ui.message.classList.remove('iso-message-error', 'iso-message-ok');
            if (text && kind === 'error') ui.message.classList.add('iso-message-error');
            if (text && kind === 'ok') ui.message.classList.add('iso-message-ok');
            show(ui.message, !!text);
        }
        function setProgress(text, value, max) {
            ui.progressText.textContent = text || '';
            if (value === null || value === undefined) {
                show(ui.progress, false);
            } else {
                ui.progress.max = max;
                ui.progress.value = value;
                show(ui.progress, true);
            }
        }
        // ジョブの実行中、または、アップロードの準備（一覧の取得）中。準備中も、新しい開始を受け付けない
        function fileBusy() { return S.job !== null || S.starting !== null; }
        function sdReady() { return !!(S.list && S.list.sd.state === 'ready'); }
        function updateControls() {
            const busy = fileBusy();
            const ok = sdReady();
            ui.file.disabled = busy;
            ui.name.disabled = busy;
            ui.hash.disabled = busy;
            ui.upload.disabled = busy || S.opBusy || !ok;
            ui.rescan.disabled = busy || S.opBusy || !ok;
            ui.eject.disabled = S.opBusy;
            show(ui.pause, S.job !== null && S.job.kind === 'upload' && (S.job.phase === 'uploading' || S.job.phase === 'hashing'));
        }

        // ---- 通信 ----
        // 戻り値: { status, data, retryAfter, net }（net: ネットワークエラーで応答が読めなかった）
        async function request(method, url, opts) {
            opts = opts || {};
            const init = { method, credentials: 'same-origin', headers: {} };
            if (opts.json !== undefined) {
                init.headers['Content-Type'] = 'application/json';
                init.body = JSON.stringify(opts.json);
            }
            if (opts.chunk !== undefined) {
                init.headers['Content-Type'] = 'application/octet-stream';
                init.headers['X-Upload-Offset'] = String(opts.offset);
                init.body = opts.chunk;
            }
            let timer = null;
            if (opts.timeoutMs && deps.AbortController) {
                const ctl = new deps.AbortController();
                init.signal = ctl.signal;
                timer = deps.setTimeout(() => ctl.abort(), opts.timeoutMs);
            }
            const gen = S.gen; // 世代: この要求を出した時点の画面（stop() の後の古い応答で、新しい画面を止めない）
            try {
                const res = await deps.fetch(url, init);
                let data = null;
                try { data = await res.json(); } catch (e) { data = null; }
                let retryAfter = 0;
                try {
                    const ra = res.headers && res.headers.get ? Number(res.headers.get('Retry-After')) : NaN;
                    if (isFinite(ra) && ra > 0) retryAfter = Math.min(ra, 30);
                } catch (e) { retryAfter = 0; }
                if (res.status === 401 && deps.onUnauthorized && S.gen === gen) deps.onUnauthorized();
                return { status: res.status, data, retryAfter, net: false };
            } catch (e) {
                return { status: 0, data: null, retryAfter: 0, net: true };
            } finally {
                if (timer !== null && deps.clearTimeout) deps.clearTimeout(timer);
            }
        }

        // ---- 一覧の取得と表示 ----
        function schedulePoll() {
            if (S.pollTimer !== null) {
                deps.clearTimeout(S.pollTimer);
                S.pollTimer = null;
            }
            if (!S.running || S.disabled) return;
            const transient = S.job !== null || (S.list !== null && S.list.images.some((i) => i.state === 'finalizing' || i.state === 'deleting' || i.state === 'uploading'));
            if (!S.panelVisible && !transient) return;
            const gen = S.gen;
            S.pollTimer = deps.setTimeout(() => {
                S.pollTimer = null;
                if (S.gen !== gen) return;
                // 自分の PUT の最中は、一覧を取らない（進み具合は PUT の応答で分かる。単一スレッドのサーバーを空ける）
                if (S.job !== null && S.job.phase === 'uploading') {
                    schedulePoll();
                    return;
                }
                refreshList();
            }, transient ? 1000 : 3000);
        }

        // 一覧を取る（同時に 1 つだけ）。成功したら整えた一覧、失敗したら null。
        function refreshList() {
            if (S.refreshing) return S.refreshing;
            const gen = S.gen;
            const p = (async () => {
                const r = await request('GET', API);
                if (S.gen !== gen) return null;
                if (r.status === 404) {
                    // ISO_ENABLE が無効のビルド: 画面ごと隠す
                    S.disabled = true;
                    show(ui.toggle, false);
                    show(ui.panel, false);
                    return null;
                }
                if (r.status === 401) return null;
                show(ui.toggle, true);
                const list = r.status === 200 ? normalizeList(r.data) : null;
                if (list === null) {
                    if (S.list === null) setMessage('ISO の状態を取得できません。' + (r.net ? 'ネットワークを確認してください。' : describeError(r.status, r.data)), 'error');
                    return null;
                }
                S.list = list;
                const sig = JSON.stringify(list);
                if (sig !== S.listSig) {
                    S.listSig = sig;
                    render();
                }
                updateControls();
                maybeAutoFinish();
                return list;
            })().finally(() => {
                if (S.refreshing === p) S.refreshing = null;
                if (S.gen === gen) schedulePoll();
            });
            S.refreshing = p;
            return p;
        }

        function imageById(id) {
            if (!S.list) return null;
            for (const im of S.list.images) if (im.id === id) return im;
            return null;
        }

        function badgeClass(state) {
            return has(STATE_TEXT, state) ? 'iso-state-' + state : 'iso-state-unknown';
        }

        function button(label, key, handler, disabled) {
            const b = doc.createElement('button');
            b.setAttribute('type', 'button');
            b.setAttribute('data-key', key);
            b.textContent = label;
            if (disabled) b.disabled = true;
            b.addEventListener('click', handler);
            S.rendered.push(b);
            return b;
        }

        function activeKey() {
            const a = doc.activeElement;
            return a && a.getAttribute ? a.getAttribute('data-key') : null;
        }

        function render() {
            const list = S.list;
            const focusKey = activeKey();
            S.rendered = [];
            // SD、ターゲット、マウント
            ui.sd.textContent = (has(SD_TEXT, list.sd.state) ? SD_TEXT[list.sd.state] : 'SD カード: 不明な状態 (' + list.sd.state + ')') +
                (list.sd.state === 'ready' && list.sd.free !== null && list.sd.total !== null ? '（空き ' + formatBytes(list.sd.free) + ' / ' + formatBytes(list.sd.total) + '）' : '');
            const parts = [];
            if (list.locked) parts.push('ターゲットが取り外しを防止しています');
            if (list.ejectedByTarget) parts.push('ターゲットが取り外しました');
            ui.target.textContent = parts.join('。');
            show(ui.target, parts.length > 0);
            const mountedImage = list.mounted !== null ? list.images.find((i) => i.id === list.mounted) : null;
            ui.mounted.textContent = list.mounted !== null ? 'マウント中: ' + (mountedImage ? mountedImage.name : list.mounted) : 'マウント中のイメージはありません';
            show(ui.eject, list.mounted !== null);
            // イメージの一覧
            ui.list.textContent = '';
            for (const im of list.images) ui.list.appendChild(renderRow(im, list));
            ui.hiddenNote.textContent = list.hidden > 0 ? '一覧に表示されていないイメージが ' + list.hidden + ' 個あります（上限を超えています）。' : '';
            show(ui.hiddenNote, list.hidden > 0);
            if (focusKey !== null) {
                const again = S.rendered.find((b) => b.getAttribute('data-key') === focusKey);
                if (again && again.focus) again.focus();
            }
        }

        function renderRow(im, list) {
            const li = doc.createElement('li');
            li.className = 'iso-row ' + badgeClass(im.state);
            const main = doc.createElement('div');
            main.className = 'iso-row-main';
            const nm = doc.createElement('span');
            nm.className = 'iso-name';
            nm.textContent = im.name; // 名前は textContent だけ（HTML として解釈させない）
            const sz = doc.createElement('span');
            sz.className = 'iso-size';
            sz.textContent = im.size !== null ? formatBytes(im.size) : '';
            const badge = doc.createElement('span');
            badge.className = 'iso-badge';
            badge.textContent = has(STATE_TEXT, im.state) ? STATE_TEXT[im.state] : im.state;
            main.appendChild(nm);
            main.appendChild(sz);
            main.appendChild(badge);
            if (list.mounted === im.id) {
                const mt = doc.createElement('span');
                mt.className = 'iso-badge iso-badge-mounted';
                mt.textContent = 'マウント中';
                main.appendChild(mt);
            }
            li.appendChild(main);
            const local = S.job !== null && S.job.id === im.id;
            const note = doc.createElement('div');
            note.className = 'iso-row-note';
            let noteText = '';
            if (im.state === 'invalid') {
                noteText = im.reason !== null ? (has(REASON_TEXT, im.reason) ? REASON_TEXT[im.reason] : '理由: ' + im.reason) : '';
            } else if ((im.state === 'incomplete' || im.state === 'uploading') && im.size !== null && im.received !== null) {
                noteText = formatBytes(im.received) + ' / ' + formatBytes(im.size) + ' を受信済み';
                if (im.state === 'incomplete' && !local && im.received < im.size) noteText += '。元のファイルを選んで「再開」を押してください。';
            }
            note.textContent = noteText;
            if (noteText !== '') li.appendChild(note);
            if ((im.state === 'incomplete') && im.size !== null && im.received !== null && im.size > 0) {
                const pr = doc.createElement('progress');
                pr.max = im.size;
                pr.value = im.received;
                li.appendChild(pr);
            }
            const actions = doc.createElement('div');
            actions.className = 'iso-row-actions';
            const dis = S.opBusy;
            if (im.state === 'ready') {
                if (list.mounted === im.id) actions.appendChild(button('取り外し', 'eject:' + im.id, () => doEject(), dis));
                else actions.appendChild(button('マウント', 'mount:' + im.id, () => doMount(im.id), dis));
            }
            if (im.state === 'incomplete' && !local) {
                if (im.size !== null && im.received === im.size) actions.appendChild(button('完了処理', 'finish:' + im.id, () => doResume(im.id), fileBusy()));
                else actions.appendChild(button('再開', 'resume:' + im.id, () => doResume(im.id), fileBusy()));
            }
            if (im.state !== 'deleting') actions.appendChild(button('削除', 'delete:' + im.id, () => doDelete(im.id), dis));
            li.appendChild(actions);
            return li;
        }

        // ---- ジョブ（アップロード、完了処理の待ち）。同時に 1 つだけ ----
        function startJob(kind, id, file, body) {
            const job = { kind, id, file, gen: S.gen, abort: false, slotWaits: 0, phase: kind === 'upload' ? 'hashing' : 'waiting', received: 0, size: file ? file.size : 0 };
            S.job = job;
            if (id !== null) S.autoDone.add(id); // この画面が扱ったイメージには、自動の finish を送らない
            updateControls();
            render2();
            (async () => {
                try {
                    await body(job);
                } catch (e) {
                    if (alive(job)) setMessage('予期しないエラーが起きました。ページを再読み込みしてください。', 'error');
                } finally {
                    if (S.job === job) S.job = null;
                    if (S.gen === job.gen) {
                        setProgress('', null);
                        updateControls();
                        S.listSig = null; // 行を描き直す（進行中の印が変わる）
                        refreshList();
                    }
                }
            })();
            return job;
        }
        function render2() {
            if (S.list) {
                S.listSig = null;
                render();
            }
        }

        async function waitSdReady(job) {
            for (let i = 0; i < MAX_WAIT_STEPS && alive(job); i++) {
                const list = await refreshList();
                if (!alive(job)) return false;
                if (list && list.sd.state === 'ready') return true;
                await sleep(2000);
            }
            return false;
        }

        // アップロードが続けられるかを、一覧で調べる。'go'（received を更新した）、'complete'（全部届いている）、'stop'（理由は表示済み）、'retry'（一覧が取れなかった）
        async function resync(job) {
            const list = await refreshList();
            if (!alive(job)) return 'stop';
            if (list === null) return 'retry';
            if (list.sd.state !== 'ready') {
                if (!(await waitSdReady(job))) {
                    if (alive(job)) setMessage(describeError(503, { error: 'not_ready' }), 'error');
                    return 'stop';
                }
            }
            const im = imageById(job.id);
            if (im === null || im.state === 'deleting') {
                setMessage(describeError(404, { error: 'not_found' }), 'error');
                return 'stop';
            }
            if (im.state !== 'uploading' && im.state !== 'incomplete') {
                setMessage(describeError(409, { error: 'not_uploading' }), 'error');
                return 'stop';
            }
            if (S.list.upload !== null && S.list.upload.id !== job.id) {
                // 別のイメージが枠を使っている。最終処理（finalizing）なら、終わるのを待つ。受信中なら、続けられない
                if (S.list.upload.state === 'finalizing' && ++job.slotWaits <= MAX_WAIT_STEPS) {
                    await sleep(1000);
                    return alive(job) ? resync(job) : 'stop';
                }
                setMessage(describeError(409, { error: 'upload_in_progress' }), 'error');
                return 'stop';
            }
            if (im.received === null || im.size === null || im.received > job.size) {
                setMessage('サーバーの受信済みの大きさが、選んだファイルと合いません。', 'error');
                return 'stop';
            }
            job.received = im.received;
            job.chunkMax = list.chunk;
            if (job.chunk > job.chunkMax) job.chunk = job.chunkMax;
            return im.received >= job.size ? 'complete' : 'go';
        }

        function reportProgress(job) {
            const pct = job.size > 0 ? (job.received / job.size * 100) : 0;
            const secs = (now() - job.startedAt) / 1000;
            const speed = secs > 0 ? (job.received - job.startReceived) / secs : 0;
            setProgress(
                pct.toFixed(1) + '%（' + formatBytes(job.received) + ' / ' + formatBytes(job.size) + '）' + (speed > 0 ? '、' + formatBytes(speed) + '/秒' : ''),
                job.received, job.size);
        }

        // PUT のループ。戻り値: 'done'（全部届いた）、'stopped'（一時停止、または画面を離れた）、'failed'（理由は表示済み）
        async function uploadLoop(job) {
            job.phase = 'uploading';
            job.startedAt = now();
            job.startReceived = job.received;
            updateControls();
            let netFails = 0;
            let sameMismatch = 0;
            let waits = 0;
            let reachable = false; // 直前の一覧の取得が成功したか
            let freed = 0; // 409 upload_in_progress なのに、一覧では枠が空いている（直前に空いた）ときの再試行
            while (alive(job) && job.received < job.size) {
                const offset = job.received;
                const n = Math.min(job.chunk, job.size - offset);
                const t0 = now();
                const r = await request('PUT', API + '/' + job.id, { chunk: job.file.slice(offset, offset + n), offset, timeoutMs: 60000 });
                if (!alive(job)) return job.received >= job.size ? 'done' : 'stopped';
                const dt = now() - t0;
                if (r.status === 200 && r.data && Number.isSafeInteger(r.data.received) && r.data.received > offset && r.data.received <= job.size) {
                    job.received = r.data.received;
                    netFails = 0;
                    sameMismatch = 0;
                    waits = 0;
                    if (dt >= SLOW_MS && job.chunk > MIN_CHUNK) job.chunk = Math.max(MIN_CHUNK, job.chunk >> 1);
                    else if (dt < FAST_MS && job.chunk < job.chunkMax) job.chunk = Math.min(job.chunkMax, job.chunk * 2);
                    reportProgress(job);
                    continue;
                }
                if (r.net || r.status === 200) {
                    // 応答が読めない、または形の違う応答: 小さくして、一覧で received を取り直して続ける
                    netFails++;
                    if (job.chunk > MIN_CHUNK) job.chunk = Math.max(MIN_CHUNK, job.chunk >> 1);
                    await sleep(Math.min(1000 * netFails, 5000));
                    if (!alive(job)) return 'stopped';
                    const before = job.received;
                    const s = await resync(job);
                    reachable = s !== 'retry';
                    if (s === 'stop') return 'failed';
                    if (s === 'complete') return 'done';
                    if (job.received > before) {
                        netFails = 0; // 応答は失われたが、サーバーには届いている（位置が進んだ）: 失敗の数に入れない
                    } else if (netFails > MAX_NET_FAILS) {
                        // 位置が進まないまま続く。一覧は取れる（機器には届く）のに、チャンクだけが失敗し続けるときは、別の文にする
                        // （拒否のときに接続を閉じるため、理由の応答が読めないことがある。docs/WEB_API.md 8.3）
                        setMessage(reachable
                            ? '機器には接続できますが、チャンクの送信が繰り返し失敗します（空き容量不足、回線が遅すぎる、または機器が拒否している可能性があります）。しばらくしてから「再開」を押してください。'
                            : 'サーバーに接続できません。ネットワークを確認して、「再開」を押してください。', 'error');
                        return 'failed';
                    }
                    reportProgress(job);
                    continue;
                }
                const code = r.data && typeof r.data.error === 'string' ? r.data.error : null;
                if (r.status === 409 && code === 'offset_mismatch') {
                    const rec = r.data.received;
                    if (!Number.isSafeInteger(rec) || rec < 0 || rec > job.size) {
                        setMessage(describeError(409, r.data), 'error');
                        return 'failed';
                    }
                    sameMismatch = rec === offset ? sameMismatch + 1 : 0;
                    if (sameMismatch > 3) {
                        setMessage('送信位置がサーバーと合わないままです。ページを再読み込みして、再開してください。', 'error');
                        return 'failed';
                    }
                    job.received = rec;
                    reportProgress(job);
                    continue;
                }
                if (r.status === 409 && code === 'upload_in_progress') {
                    // 別のイメージが枠を使っている。最終処理（finalizing）なら、終わるのを待つ
                    const list = await refreshList();
                    if (!alive(job)) return 'stopped';
                    const holder = list && list.upload && list.upload.id !== job.id ? list.upload : null;
                    // 枠が空いた（holder がない）か、最終処理中なら、待ってやり直す。受信中の別のアップロードなら、続けられない
                    if (list !== null && (holder === null ? ++freed <= 5 : holder.state === 'finalizing' && ++waits <= MAX_WAIT_STEPS)) {
                        await sleep(1000);
                        continue;
                    }
                    setMessage(describeError(409, r.data), 'error');
                    return 'failed';
                }
                if (r.status === 409 && code === 'not_uploading') {
                    const s = await resync(job);
                    if (s === 'stop') return 'failed';
                    if (s === 'complete') return 'done';
                    setMessage(describeError(409, r.data), 'error');
                    return 'failed';
                }
                if (r.status === 413 && code === 'payload_too_large' && job.chunk > MIN_CHUNK) {
                    job.chunk = Math.max(MIN_CHUNK, job.chunk >> 1);
                    job.chunkMax = Math.min(job.chunkMax, job.chunk);
                    continue;
                }
                if (r.status === 503 && code === 'busy' && ++waits <= MAX_WAIT_STEPS) {
                    await sleep((r.retryAfter || 1) * 1000);
                    continue;
                }
                if (r.status === 503 && (code === 'not_ready' || code === 'sd_unavailable') && ++waits <= 10) {
                    // SD の復帰を待つ。書き込みの失敗は、チャンクが取り消されて incomplete になっている（received から再開できる）
                    if (!(await waitSdReady(job))) {
                        if (alive(job)) setMessage(describeError(r.status, r.data), 'error');
                        return 'failed';
                    }
                    const s = await resync(job);
                    if (s === 'stop') return 'failed';
                    if (s === 'complete') return 'done';
                    continue;
                }
                setMessage(describeError(r.status, r.data), 'error');
                if (r.status === 404) S.listSig = null;
                return 'failed';
            }
            return job.received >= job.size ? 'done' : 'stopped';
        }

        // 完了処理（finish）と、その待ち。戻り値: 'ready' / 'invalid' / 'failed'（理由は表示済み）/ 'stopped' / 'resume'（アップロードを続ける必要がある）
        async function finishFlow(job) {
            job.phase = 'finishing';
            updateControls();
            let rounds = 0;
            let waits = 0;
            let netFails = 0;
            setProgress('完了処理を始めています…', null);
            while (alive(job)) {
                const r = await request('POST', API + '/' + job.id + '/finish', { json: {} });
                if (!alive(job)) return 'stopped';
                if (r.status === 200) {
                    setMessage('アップロードが完了しました（利用可能）。', 'ok');
                    return 'ready';
                }
                if (r.status === 202) {
                    job.phase = 'waiting';
                    setProgress('最終処理中です（SHA-256 の確認、SD 上の配置の作成、署名の検査）。しばらくお待ちください…', null);
                    const w = await waitFinalize(job);
                    if (w === 'ready') {
                        setMessage('アップロードが完了しました（利用可能）。', 'ok');
                        return 'ready';
                    }
                    if (w === 'invalid') {
                        const im = imageById(job.id);
                        setMessage(describeError(422, { error: im && im.reason ? im.reason : 'invalid' }), 'error');
                        return 'invalid';
                    }
                    if (w === 'gone') {
                        setMessage(describeError(404, { error: 'not_found' }), 'error');
                        return 'failed';
                    }
                    if (w !== 'incomplete') return 'stopped';
                    // 最終処理中の SD のエラー: SD の復帰を待って、received で分ける
                    if (++rounds > MAX_FINISH_ROUNDS) {
                        setMessage('最終処理が完了しません。SD カードを確認して、「完了処理」または「再開」を押してください。', 'error');
                        return 'failed';
                    }
                    if (!(await waitSdReady(job))) {
                        if (alive(job)) setMessage(describeError(503, { error: 'not_ready' }), 'error');
                        return 'failed';
                    }
                    const im = imageById(job.id);
                    if (im === null) {
                        setMessage(describeError(404, { error: 'not_found' }), 'error');
                        return 'failed';
                    }
                    job.phase = 'finishing';
                    if (im.state === 'ready') {
                        setMessage('アップロードが完了しました（利用可能）。', 'ok');
                        return 'ready';
                    }
                    if (im.received !== null && im.size !== null && im.received < im.size) {
                        job.received = im.received;
                        return 'resume';
                    }
                    continue; // received == size: finish を送り直す
                }
                if (r.status === 422) {
                    setMessage(describeError(422, r.data), 'error');
                    return 'invalid';
                }
                const code = r.data && typeof r.data.error === 'string' ? r.data.error : null;
                if (r.status === 409 && code === 'size_mismatch') {
                    if (Number.isSafeInteger(r.data.received) && r.data.received >= 0 && r.data.received < job.size) {
                        job.received = r.data.received;
                        return 'resume';
                    }
                    setMessage(describeError(409, r.data), 'error');
                    return 'failed';
                }
                if (r.status === 409 && code === 'upload_in_progress' && !job.auto && ++waits <= MAX_WAIT_STEPS) {
                    await sleep(1000);
                    continue;
                }
                if (r.status === 503 && code === 'busy' && !job.auto && ++waits <= MAX_WAIT_STEPS) {
                    await sleep((r.retryAfter || 1) * 1000);
                    continue;
                }
                if (r.status === 503 && (code === 'not_ready' || code === 'sd_unavailable') && !job.auto && ++rounds <= MAX_FINISH_ROUNDS) {
                    if (!(await waitSdReady(job))) {
                        if (alive(job)) setMessage(describeError(503, r.data), 'error');
                        return 'failed';
                    }
                    continue;
                }
                if (r.net && ++netFails <= MAX_NET_FAILS) {
                    // 応答が読めない: 一覧で状態を見る（finalizing なら、始まっている）
                    await sleep(1000 * netFails);
                    const list = await refreshList();
                    if (!alive(job)) return 'stopped';
                    const im = list ? imageById(job.id) : null;
                    if (im && im.state === 'ready') {
                        setMessage('アップロードが完了しました（利用可能）。', 'ok');
                        return 'ready';
                    }
                    if (im && im.state === 'finalizing') {
                        job.phase = 'waiting';
                        const w = await waitFinalize(job);
                        if (w === 'ready') {
                            setMessage('アップロードが完了しました（利用可能）。', 'ok');
                            return 'ready';
                        }
                        if (w === 'invalid') {
                            setMessage(describeError(422, { error: (imageById(job.id) || {}).reason || 'invalid' }), 'error');
                            return 'invalid';
                        }
                        if (w !== 'incomplete') return w === 'gone' ? 'failed' : 'stopped';
                    }
                    if (job.auto) {
                        setMessage('完了処理の結果を確認できません。一覧の「完了処理」を押してください。', 'error');
                        return 'failed';
                    }
                    continue;
                }
                setMessage(r.net ? 'サーバーに接続できません。ネットワークを確認してください。' : describeError(r.status, r.data), 'error');
                return 'failed';
            }
            return 'stopped';
        }

        // finalizing が終わる（ready / invalid / incomplete）まで、一覧を見て待つ。戻り値: 'ready' / 'invalid' / 'incomplete' / 'gone' / 'stopped'
        async function waitFinalize(job) {
            let misses = 0;
            let sdMisses = 0;
            for (let i = 0; i < 100000 && alive(job); i++) {
                await sleep(1000);
                if (!alive(job)) return 'stopped';
                const list = await refreshList();
                if (!alive(job)) return 'stopped';
                if (list === null) {
                    if (++misses > 60) return 'stopped';
                    continue;
                }
                misses = 0;
                if (list.sd.state !== 'ready') {
                    // SD が使えない間は、一覧にイメージが出ない（docs/WEB_API.md 8.1）。「ない」ではなく、戻るのを待つ
                    if (++sdMisses > 600) return 'stopped';
                    continue;
                }
                sdMisses = 0;
                const im = imageById(job.id);
                if (im === null || im.state === 'deleting') return 'gone';
                if (im.state === 'ready') return 'ready';
                if (im.state === 'invalid') return 'invalid';
                if (im.state === 'incomplete') return 'incomplete';
                // finalizing（または、続いている uploading）: 待つ
            }
            return 'stopped';
        }

        // アップロードの後ろ（PUT のループ → finish）を、必要なだけ繰り返す
        async function uploadAndFinish(job) {
            for (let round = 0; round < MAX_FINISH_ROUNDS + 2 && alive(job); round++) {
                if (job.received < job.size) {
                    const u = await uploadLoop(job);
                    if (job.deleted) return;
                    if (u === 'stopped') {
                        if (S.gen === job.gen) setMessage('一時停止しました。「再開」で続きから送れます（約 5 分以内なら、サーバーの状態は保たれます）。', 'info');
                        return;
                    }
                    if (u === 'failed') return;
                }
                const f = await finishFlow(job);
                if (f !== 'resume') return;
            }
        }

        // ---- 操作: アップロード ----
        async function doUpload() {
            if (fileBusy() || S.opBusy) return;
            const file = ui.file.files && ui.file.files[0];
            if (!file) {
                setMessage('アップロードする ISO ファイルを選んでください。', 'error');
                return;
            }
            const name = ui.name.value;
            const bad = nameProblem(name);
            if (bad !== null) {
                setMessage(bad, 'error');
                return;
            }
            if (!(file.size >= 34816)) {
                setMessage('ファイルが小さすぎます（ISO は 34,816 バイト以上です）。', 'error');
                return;
            }
            setMessage('', null);
            // 開始権: 一覧の取得を待つ間も、他の開始（もう一度の押下、自動の finish）を入れない。準備に失敗したら、必ず放す
            const token = {};
            const gen = S.gen;
            S.starting = token;
            updateControls();
            try {
                const list = await refreshList();
                if (S.gen !== gen) return; // 待つ間に、ログアウトした
                if (list === null) {
                    setMessage('ISO の状態を取得できません。ネットワークを確認してください。', 'error');
                    return;
                }
                if (list.sd.state !== 'ready') {
                    setMessage(describeError(503, { error: 'not_ready' }), 'error');
                    return;
                }
                // 同じ名前で、同じ大きさの中断中のイメージがあれば、新しく作らず、その続きから送る
                const same = list.images.find((i) => i.name === name);
                if (same) {
                    if ((same.state === 'incomplete' || same.state === 'uploading') && same.size === file.size) {
                        resumeWith(same.id, file);
                        return;
                    }
                    setMessage(describeError(409, { error: 'name_exists' }), 'error');
                    return;
                }
                startNewUpload(file, name, list);
            } finally {
                if (S.starting === token) S.starting = null;
                if (S.gen === gen) updateControls();
            }
        }

        function startNewUpload(file, name, list) {
            startJob('upload', null, file, async (job) => {
                let sha = null;
                if (ui.hash.checked) {
                    job.phase = 'hashing';
                    updateControls();
                    sha = await hashFile(job);
                    if (sha === null) {
                        if (alive(job) || S.gen === job.gen) setMessage('SHA-256 の計算を中止しました。', 'info');
                        return;
                    }
                }
                job.phase = 'creating';
                updateControls();
                setProgress('アップロードを始めています…', null);
                const body = { name, size: file.size };
                if (sha !== null) body.sha256 = sha;
                const r = await request('POST', API, { json: body });
                if (!alive(job)) return;
                if (r.status !== 201 || !r.data || typeof r.data.id !== 'string' || !/^[0-9a-f]{8}$/.test(r.data.id)) {
                    setMessage(r.net ? 'サーバーに接続できません。ネットワークを確認してください。' : describeError(r.status, r.data), 'error');
                    return;
                }
                job.id = r.data.id;
                S.autoDone.add(job.id);
                // 再開用のファイルと id は、id が決まってから、組で更新する（begin が失敗したときは、前の組を保つ）
                S.lastFile = file;
                S.lastFileId = job.id;
                job.received = 0;
                job.chunkMax = Number.isSafeInteger(r.data.chunk_size) && r.data.chunk_size > 0 ? r.data.chunk_size : list.chunk;
                job.chunk = job.chunkMax;
                await uploadAndFinish(job);
            });
        }

        async function hashFile(job) {
            const sha = new Sha256();
            const size = job.file.size;
            for (let off = 0; off < size; off += HASH_SLICE) {
                if (!alive(job)) return null;
                const buf = await job.file.slice(off, Math.min(size, off + HASH_SLICE)).arrayBuffer();
                if (!alive(job)) return null;
                sha.update(new Uint8Array(buf));
                const done = Math.min(size, off + HASH_SLICE);
                setProgress('SHA-256 を計算中… ' + (done / size * 100).toFixed(1) + '%', done, size);
            }
            return sha.digest();
        }

        // 中断中のイメージ（id）を、選んだファイルで続ける
        function resumeWith(id, file) {
            S.lastFile = file;
            S.lastFileId = id;
            setMessage('', null);
            startJob('upload', id, file, async (job) => {
                job.phase = 'creating';
                updateControls();
                setProgress('サーバーの受信済みの大きさを確認しています…', null);
                job.chunk = 131072;
                job.chunkMax = 131072;
                const s = await resync(job); // received と chunk_size を取り直す（chunkMax と chunk を更新する）
                if (s === 'stop') return;
                await uploadAndFinish(job); // 'complete'（全部届いている）のときは、PUT をせずに finish だけになる
            });
        }

        async function doResume(id) {
            if (fileBusy()) return;
            const im = imageById(id);
            if (!im || im.size === null) return;
            // 全部届いている中断中のイメージは、ファイルが要らない（完了処理だけ）
            if (im.received !== null && im.received === im.size) {
                setMessage('', null);
                startJob('finish', id, null, async (job) => {
                    job.size = im.size;
                    job.received = im.size;
                    const f = await finishFlow(job);
                    if (f === 'resume') setMessage('アップロードが途中までです。元のファイルを選んで「再開」を押してください。', 'error');
                });
                return;
            }
            let file = ui.file.files && ui.file.files[0];
            if (!file || file.size !== im.size) file = S.lastFileId === id && S.lastFile && S.lastFile.size === im.size ? S.lastFile : null;
            if (!file) {
                setMessage('再開するには、元のファイル（' + formatBytes(im.size) + '、' + im.size + ' バイト）を選んでください。', 'error');
                if (ui.file.focus) ui.file.focus();
                return;
            }
            resumeWith(id, file);
        }

        // 再読み込みの後: incomplete で received == size のものに、finish を 1 回だけ自動で送る
        function maybeAutoFinish() {
            if (S.job !== null || S.starting !== null || S.opBusy || !S.list || S.list.sd.state !== 'ready' || S.list.upload !== null) return;
            for (const im of S.list.images) {
                if (im.state === 'incomplete' && im.size !== null && im.size > 0 && im.received === im.size && !S.autoDone.has(im.id)) {
                    S.autoDone.add(im.id);
                    startJob('finish', im.id, null, async (job) => {
                        job.size = im.size;
                        job.received = im.size;
                        job.auto = true; // 自動の経路: finish の POST は 1 回（失敗しても、勝手に再送しない）
                        const f = await finishFlow(job);
                        if (f === 'resume') setMessage('アップロードが途中までです。元のファイルを選んで「再開」を押してください。', 'error');
                    });
                    return;
                }
            }
        }

        // ---- 操作: マウント、取り外し、削除、取り込み ----
        async function runOp(fn) {
            if (S.opBusy) return;
            S.opBusy = true;
            S.listSig = null;
            if (S.list) render();
            updateControls();
            const gen = S.gen;
            try {
                await fn();
            } finally {
                if (S.gen === gen) {
                    S.opBusy = false;
                    S.listSig = null;
                    await refreshList();
                    if (S.listSig === null && S.list) render(); // 一覧が取れなかったとき、ボタンの無効を戻す
                    updateControls();
                }
            }
        }

        function netOrError(r) {
            return r.net ? 'サーバーに接続できません。ネットワークを確認してください。' : describeError(r.status, r.data);
        }

        function doMount(id) {
            return runOp(async () => {
                setMessage('', null);
                let r = await request('POST', API + '/mount', { json: { id } });
                if (r.status === 409 && r.data && r.data.error === 'locked_by_target') {
                    if (!deps.confirm('ターゲットが、メディアの取り外しを防止しています。強制的に差し替えると、ターゲット側でディスクの内容が失われる恐れがあります。差し替えますか？')) {
                        setMessage('マウントを取りやめました。', 'info');
                        return;
                    }
                    r = await request('POST', API + '/mount', { json: { id, force: true } });
                }
                if (r.status === 200 && r.data && (r.data.result === 'mounted' || r.data.result === 'already_mounted')) {
                    setMessage(r.data.result === 'mounted' ? 'マウントしました。ターゲットの BIOS / OS で、USB の CD-ROM が見えるか確認してください。' : 'すでにマウント中です。', 'ok');
                } else {
                    setMessage(netOrError(r), 'error');
                }
            });
        }

        function doEject() {
            return runOp(async () => {
                setMessage('', null);
                let r = await request('POST', API + '/eject', { json: {} });
                if (r.status === 409 && r.data && r.data.error === 'locked_by_target') {
                    if (!deps.confirm('ターゲットが、メディアの取り外しを防止しています。強制的に取り外すと、ターゲット側でデータが失われる恐れがあります。取り外しますか？')) {
                        setMessage('取り外しを取りやめました。', 'info');
                        return;
                    }
                    r = await request('POST', API + '/eject', { json: { force: true } });
                }
                if (r.status === 200 && r.data && r.data.result === 'ejected') setMessage('取り外しました。', 'ok');
                else if (r.status === 200 && r.data && r.data.result === 'not_mounted') setMessage('マウントされていません。', 'info');
                else setMessage(netOrError(r), 'error');
            });
        }

        function doDelete(id) {
            const im = imageById(id);
            const label = im ? im.name : id;
            if (!deps.confirm('「' + label + '」を削除しますか？（元に戻せません）')) return undefined;
            return runOp(async () => {
                setMessage('', null);
                const r = await request('DELETE', API + '/' + id);
                if (r.status === 202) {
                    if (S.job !== null && S.job.id === id) {
                        // その受信（PUT のループ、待ち）をやめる
                        S.job.abort = true;
                        S.job.deleted = true;
                    }
                    setMessage('削除を始めました。', 'info');
                } else {
                    setMessage(netOrError(r), 'error');
                }
            });
        }

        function doRescan() {
            if (fileBusy()) return undefined;
            const gen = S.gen; // 操作を始めたときの世代: ログアウトの後は、次のバッチを送らない
            return runOp(async () => {
                setMessage('取り込み中…', 'info');
                let cursor = null;
                let imported = 0;
                let skipped = 0;
                let busyTries = 0;
                for (let i = 0; i < MAX_RESCAN_ROUNDS; i++) {
                    if (S.gen !== gen) return;
                    const r = await request('POST', API + '/rescan', { json: cursor === null ? {} : { cursor } });
                    if (S.gen !== gen) return;
                    if (r.status === 503 && r.data && r.data.error === 'busy' && ++busyTries <= 30) {
                        await sleep((r.retryAfter || 1) * 1000);
                        i--;
                        continue;
                    }
                    const d = r.data;
                    const next = d ? d.next_cursor : undefined;
                    if (r.status !== 200 || !d || !isCount(d.imported) || !isCount(d.skipped) || !(next === null || (Number.isSafeInteger(next) && next >= 0 && next <= 65535))) {
                        setMessage((imported + skipped > 0 ? '取り込みを途中でやめました（取り込み ' + imported + ' 件、スキップ ' + skipped + ' 件）。' : '') + (r.status === 200 ? '応答の形が正しくありません。' : netOrError(r)), 'error');
                        return;
                    }
                    imported += d.imported;
                    skipped += d.skipped;
                    busyTries = 0;
                    if (next === null) {
                        setMessage('取り込みが終わりました（取り込み ' + imported + ' 件、スキップ ' + skipped + ' 件）。', 'ok');
                        return;
                    }
                    if (next === cursor && d.imported === 0) { // 取り込んだファイルは import/ から移るので、取り込みがあった回は、cursor が同じでもよい
                        setMessage('取り込みが進みません（cursor ' + next + '）。取り込み ' + imported + ' 件、スキップ ' + skipped + ' 件。', 'error');
                        return;
                    }
                    cursor = next;
                }
                setMessage('取り込みの回数が多すぎるため、やめました（取り込み ' + imported + ' 件、スキップ ' + skipped + ' 件）。', 'error');
            });
        }

        // ---- 開閉と、開始・終了 ----
        function setPanel(on) {
            if (S.disabled) return;
            S.panelVisible = on;
            show(ui.panel, on);
            if (ui.toggle) ui.toggle.setAttribute('aria-expanded', on ? 'true' : 'false');
            if (on) refreshList();
            else schedulePoll();
        }

        ui.toggle.addEventListener('click', () => setPanel(!S.panelVisible));
        ui.close.addEventListener('click', () => setPanel(false));
        ui.upload.addEventListener('click', () => { doUpload(); });
        ui.pause.addEventListener('click', () => {
            if (S.job !== null && S.job.kind === 'upload' && (S.job.phase === 'uploading' || S.job.phase === 'hashing')) S.job.abort = true;
        });
        ui.rescan.addEventListener('click', () => { doRescan(); });
        ui.eject.addEventListener('click', () => { doEject(); });
        ui.file.addEventListener('change', () => {
            const f = ui.file.files && ui.file.files[0];
            if (f) ui.name.value = defaultName(f.name);
        });

        function start() {
            if (S.running) return;
            S.running = true;
            S.gen++;
            S.disabled = false;
            refreshList();
        }

        // ログアウト、セッション切れ: 進行中の通信と待ちをやめ、画面を隠す
        function stop() {
            S.running = false;
            S.gen++;
            if (S.job !== null) S.job.abort = true;
            S.job = null;
            S.opBusy = false;
            S.refreshing = null;
            if (S.pollTimer !== null) {
                deps.clearTimeout(S.pollTimer);
                S.pollTimer = null;
            }
            S.list = null;
            S.listSig = null;
            S.autoDone = new Set();
            S.lastFile = null;
            S.lastFileId = null;
            S.starting = null;
            ui.file.value = ''; // ログアウト: 選んだファイルと名前を残さない（パネルを閉じるだけなら、残す）
            ui.name.value = '';
            S.panelVisible = false;
            show(ui.panel, false);
            show(ui.toggle, false);
            if (ui.toggle) ui.toggle.setAttribute('aria-expanded', 'false');
            ui.list.textContent = '';
            setMessage('', null);
            setProgress('', null);
            updateControls();
        }

        return { start, stop, refreshList, setPanel, _state: S };
    }

    return { create, Sha256, sha256Hex, formatBytes, utf8Length, nameProblem, defaultName, describeError, normalizeList };
})();
// ==== ISO viewer (end) ====

document.addEventListener('DOMContentLoaded', () => {
    const elError = document.getElementById('error-message');
    const viewSetup = document.getElementById('view-setup');
    const viewLogin = document.getElementById('view-login');
    const viewDashboard = document.getElementById('view-dashboard');

    const formSetup = document.getElementById('form-setup');
    const setupPassword = document.getElementById('setup-password');
    const setupPasswordConfirm = document.getElementById('setup-password-confirm');

    const formLogin = document.getElementById('form-login');
    const loginPassword = document.getElementById('login-password');
    const btnLogout = document.getElementById('btn-logout');

    const elAuthCard = document.getElementById('auth-card');
    const elVideo = document.getElementById('video');
    const elVideoWrap = document.getElementById('video-wrap');
    const btnViewFit = document.getElementById('btn-view-fit');
    const btnViewWidth = document.getElementById('btn-view-width');
    const btnViewActual = document.getElementById('btn-view-actual');
    const btnCodecMjpeg = document.getElementById('btn-codec-mjpeg');
    const btnCodecH264 = document.getElementById('btn-codec-h264');
    const btnFullscreen = document.getElementById('btn-fullscreen');
    const btnFullscreenExit = document.getElementById('btn-fullscreen-exit');
    const elVideoStatus = document.getElementById('video-status');
    const btnInputToggle = document.getElementById('btn-input-toggle');
    const btnPointerLock = document.getElementById('btn-pointer-lock');
    const elInputStatus = document.getElementById('input-status');
    const elUsbStatus = document.getElementById('usb-status');

    const elVideoTrustHint = document.getElementById('video-trust-hint');
    const elVideoTrustLink = document.getElementById('video-trust-link');
    const elVideoOverlay = document.getElementById('video-overlay');

    // The ISO (virtual media) screen. It is started when the dashboard is shown and stopped on any other view.
    const iso = IsoViewer.create({
        document: document,
        fetch: (url, init) => fetch(url, init),
        setTimeout: (fn, ms) => setTimeout(fn, ms),
        clearTimeout: (id) => clearTimeout(id),
        confirm: (msg) => window.confirm(msg),
        AbortController: typeof AbortController === 'function' ? AbortController : null,
        now: () => Date.now(),
        onUnauthorized: () => {
            // The session ended: stop at once and look at the status again (after a moment, so that a server that keeps
            // saying "authenticated" but answering 401 cannot make this a tight loop).
            iso.stop();
            setTimeout(fetchStatus, 1000);
        }
    });

    let ws = null;
    let videoTarget = null; // result of VideoTarget.resolve, set from the status response
    let wsReconnectTimer = null;
    let wsReconnectDelay = 1000;

    let wsInput = null;
    let wsInputReconnectTimer = null;
    let wsInputReconnectDelay = 1000;
    let inputEnabled = false;
    let inputGranted = false; // the server accepted this connection as the controller (GRANTED, 0x80)

    let isDecoding = false;
    let pendingMjpegFrame = null;
    let videoConnGen = 0;
    let ctx = elVideo.getContext('2d');

    function updateInputUI() {
        if (inputEnabled) {
            btnInputToggle.textContent = '入力を無効にする';
            elVideo.classList.add('input-active');
            btnPointerLock.disabled = false;
        } else {
            btnInputToggle.textContent = '入力を有効にする';
            elVideo.classList.remove('input-active');
            btnPointerLock.disabled = true;
            if (document.pointerLockElement === elVideo) {
                document.exitPointerLock();
            }
        }
        syncKeyboardLock();
    }

    btnInputToggle.addEventListener('click', () => {
        inputEnabled = !inputEnabled;
        updateInputUI();
        if (!inputEnabled && wsInput && wsInput.readyState === WebSocket.OPEN) {
            wsInput.send(MsgBuilder.releaseAll());
        }
    });

    btnPointerLock.addEventListener('click', () => {
        if (inputEnabled) {
            if (document.pointerLockElement !== elVideo) {
                elVideo.requestPointerLock();
            } else {
                document.exitPointerLock();
            }
        }
    });

    // Toggle input by clicking canvas
    elVideo.addEventListener('click', () => {
        if (!inputEnabled) {
            inputEnabled = true;
            updateInputUI();
        }
    });

    let keepaliveTimer = null;
    let pendingPtrAbs = false;
    let pendingPtrRel = false;
    let currentButtons = 0;
    let relDx = 0;
    let relDy = 0;

    // Keys that are down right now (by KeyboardEvent.code). KEEPALIVE is needed while any key or button is down.
    const pressedKeys = new Set();

    function sendWsInput(buf) {
        // Nothing is sent before GRANTED: a connection that is not the controller is closed by the server.
        if (inputGranted && wsInput && wsInput.readyState === WebSocket.OPEN) {
            wsInput.send(buf);
        }
    }

    function handleActiveInput() {
        const activeInputs = pressedKeys.size + (currentButtons !== 0 ? 1 : 0);
        if (activeInputs > 0 && !keepaliveTimer) {
            keepaliveTimer = setInterval(() => {
                sendWsInput(MsgBuilder.keepalive());
            }, 1000);
        } else if (activeInputs === 0 && keepaliveTimer) {
            clearInterval(keepaliveTimer);
            keepaliveTimer = null;
        }
    }

    let isEscPressed = false;
    let escTimeout = null;

    function resetEscLongPress() {
        isEscPressed = false;
        if (escTimeout) {
            clearTimeout(escTimeout);
            escTimeout = null;
        }
    }

    function doReleaseAll() {
        pressedKeys.clear();
        resetEscLongPress();
        currentButtons = 0;
        relDx = 0;
        relDy = 0;
        pendingPtrAbs = false;
        pendingPtrRel = false;
        handleActiveInput();
        sendWsInput(MsgBuilder.releaseAll());
    }

    window.addEventListener('keydown', (e) => {
        if (!inputEnabled) return;

        if (e.code === 'Escape') {
            if (!isEscPressed) {
                isEscPressed = true;
                escTimeout = setTimeout(() => {
                    inputEnabled = false;
                    updateInputUI();
                    doReleaseAll();
                }, 1000); // Long press Esc to disable input
            }
        }

        if (Object.prototype.hasOwnProperty.call(KEYMAP, e.code)) {
            e.preventDefault();
            if (!e.repeat && !pressedKeys.has(e.code)) {
                pressedKeys.add(e.code);
                handleActiveInput();
                sendWsInput(MsgBuilder.keyDown(KEYMAP[e.code]));
            }
        }
    });

    window.addEventListener('keyup', (e) => {
        // The Escape long-press state is reset even when input is already disabled (otherwise it would stay set
        // and the long press could never disable the input a second time).
        if (e.code === 'Escape') resetEscLongPress();
        if (!inputEnabled) return;

        if (Object.prototype.hasOwnProperty.call(KEYMAP, e.code)) {
            e.preventDefault();
            pressedKeys.delete(e.code);
            handleActiveInput();
            sendWsInput(MsgBuilder.keyUp(KEYMAP[e.code]));
        }
    });

    elVideo.addEventListener('contextmenu', (e) => {
        if (inputEnabled) {
            e.preventDefault();
        }
    });

    function getMousePos(e) {
        const rect = elVideo.getBoundingClientRect();
        // Ratio of the CSS content box (without the border) mapped to 0..32767
        const w = elVideo.clientWidth || rect.width;
        const h = elVideo.clientHeight || rect.height;
        let x = (e.clientX - rect.left - elVideo.clientLeft) / w * 32767;
        let y = (e.clientY - rect.top - elVideo.clientTop) / h * 32767;
        x = Math.max(0, Math.min(32767, Math.round(x)));
        y = Math.max(0, Math.min(32767, Math.round(y)));
        return { x, y };
    }

    let lastAbsX = 0;
    let lastAbsY = 0;

    function sendPointerUpdate() {
        if (!inputEnabled) return;

        const isLocked = document.pointerLockElement === elVideo;

        if (isLocked && pendingPtrRel) {
            // movementX/Y can be fractional: send the integer part (int16), keep the remainder for the next frame.
            const clampDx = Math.max(-32768, Math.min(32767, Math.trunc(relDx)));
            const clampDy = Math.max(-32768, Math.min(32767, Math.trunc(relDy)));
            if (clampDx !== 0 || clampDy !== 0) {
                sendWsInput(MsgBuilder.ptrRel(currentButtons, clampDx, clampDy, 0));
                relDx -= clampDx;
                relDy -= clampDy;
            }
            if (Math.trunc(relDx) === 0 && Math.trunc(relDy) === 0) {
                pendingPtrRel = false;
            } else {
                requestAnimationFrame(sendPointerUpdate);
                return;
            }
        } else if (!isLocked && pendingPtrAbs) {
            sendWsInput(MsgBuilder.ptrAbs(currentButtons, lastAbsX, lastAbsY, 0));
            pendingPtrAbs = false;
        }
    }

    function schedulePointerUpdate() {
        if (!pendingPtrAbs && !pendingPtrRel) {
            const isLocked = document.pointerLockElement === elVideo;
            if (isLocked) pendingPtrRel = true;
            else pendingPtrAbs = true;
            requestAnimationFrame(sendPointerUpdate);
        }
    }

    function updateButtons(e) {
        let buttons = 0;
        if (e.buttons & 1) buttons |= 1; // Left
        if (e.buttons & 2) buttons |= 2; // Right
        if (e.buttons & 4) buttons |= 4; // Middle

        if (buttons !== currentButtons) {
            currentButtons = buttons;
            handleActiveInput();

            // Send button changes immediately
            const isLocked = document.pointerLockElement === elVideo;
            if (isLocked) {
                sendWsInput(MsgBuilder.ptrRel(currentButtons, 0, 0, 0));
            } else {
                sendWsInput(MsgBuilder.ptrAbs(currentButtons, lastAbsX, lastAbsY, 0));
            }
        }
    }

    elVideo.addEventListener('mousedown', (e) => {
        if (!inputEnabled) return;
        e.preventDefault();

        const pos = getMousePos(e);
        lastAbsX = pos.x;
        lastAbsY = pos.y;

        updateButtons(e);
    });

    // On the window, not the canvas: a button released outside the canvas must not stay pressed on the target
    // (KEEPALIVE would keep it pressed forever).
    window.addEventListener('mouseup', (e) => {
        if (!inputEnabled) return;
        e.preventDefault();

        const pos = getMousePos(e);
        lastAbsX = pos.x;
        lastAbsY = pos.y;

        updateButtons(e);
    });

    elVideo.addEventListener('mousemove', (e) => {
        if (!inputEnabled) return;
        e.preventDefault();

        const isLocked = document.pointerLockElement === elVideo;
        updateButtons(e);

        if (isLocked) {
            relDx += e.movementX;
            relDy += e.movementY;
            schedulePointerUpdate();
        } else {
            const pos = getMousePos(e);
            lastAbsX = pos.x;
            lastAbsY = pos.y;
            schedulePointerUpdate();
        }
    });

    elVideo.addEventListener('wheel', (e) => {
        if (!inputEnabled) return;
        e.preventDefault();

        updateButtons(e);

        // Convert wheel delta to notches. Browser deltaY positive = down. HID positive = up.
        // We invert the sign.
        let wheel = e.deltaY > 0 ? -1 : (e.deltaY < 0 ? 1 : 0);
        if (wheel !== 0) {
            // Send wheel immediately
            const isLocked = document.pointerLockElement === elVideo;
            if (isLocked) {
                sendWsInput(MsgBuilder.ptrRel(currentButtons, 0, 0, wheel));
            } else {
                sendWsInput(MsgBuilder.ptrAbs(currentButtons, lastAbsX, lastAbsY, wheel));
            }
        }
    });

    // Handle release events
    window.addEventListener('blur', () => {
        if (inputEnabled) doReleaseAll();
    });

    document.addEventListener('visibilitychange', () => {
        if (document.hidden && inputEnabled) {
            doReleaseAll();
        }
    });

    window.addEventListener('pagehide', () => {
        if (inputEnabled) doReleaseAll();
    });

    document.addEventListener('pointerlockchange', () => {
        if (document.pointerLockElement !== elVideo && inputEnabled) {
            doReleaseAll();
        }
    });

    // Where the video WebSocket connects (docs/VIDEO_TRANSPORT.md 5). Pure functions, checked on the host by
    // tools/test/check_video_viewer.js. loc = { host, hostname, port } (same field names as window.location).
    const VideoTarget = {
        // The server's video_port is used only when it is a JSON integer in 1..65535 (no strings, fractions, null).
        validPort(v) {
            return typeof v === 'number' && Number.isInteger(v) && v >= 1 && v <= 65535;
        },
        // A DNS name / IPv4 address, or a bracketed IPv6 literal as location.hostname spells it.
        validHostname(h) {
            if (typeof h !== 'string' || h.length === 0 || h.length > 255) return false;
            return /^[A-Za-z0-9._-]+$/.test(h) || /^\[[0-9A-Fa-f:.]+\]$/.test(h);
        },
        // The port of the page itself: location.port is '' for the default HTTPS port.
        pagePort(loc) {
            if (typeof loc.port === 'string' && /^[0-9]{1,5}$/.test(loc.port)) return Number(loc.port);
            return 443;
        },
        // No usable video_port (or the same port as the page): the old URL on the main port. Otherwise the video
        // server's port on the page's host name. separate = the video server is on another port.
        resolve(loc, videoPort) {
            const legacy = { url: 'wss://' + loc.host + '/video', separate: false, port: 0, hostname: '' };
            if (!this.validPort(videoPort)) return legacy;
            if (!this.validHostname(loc.hostname)) return legacy;
            if (videoPort === this.pagePort(loc)) return legacy;
            return { url: 'wss://' + loc.hostname + ':' + videoPort + '/video', separate: true, port: videoPort, hostname: loc.hostname };
        },
        // The connection URL of the target. accept = true adds "?accept=h264" (docs/VIDEO_TRANSPORT.md 9.4: this
        // viewer can take H.264); false leaves the URL as it is (MJPEG only). Every other field is kept.
        withAccept(target, accept) {
            if (accept !== true) return target;
            return Object.assign({}, target, { url: target.url + '?accept=h264' });
        },
        // The "accept the certificate" hint is for a connection that failed before it opened, on another port.
        needsTrustHint(target, wasOpen) {
            return !!target && target.separate === true && wasOpen === false;
        },
        // The page the viewer links to so the person can accept the certificate (null when there is no hint).
        trustUrl(target) {
            if (!target || target.separate !== true) return null;
            return 'https:' + '//' + target.hostname + ':' + target.port + '/';
        }
    };

    function showTrustHint(target) {
        const url = VideoTarget.trustUrl(target);
        if (url === null) return;
        elVideoTrustLink.setAttribute('href', url);
        elVideoTrustLink.textContent = url;
        elVideoTrustHint.classList.remove('trust-hint-hidden');
    }

    function hideTrustHint() {
        elVideoTrustHint.classList.add('trust-hint-hidden');
        elVideoTrustLink.removeAttribute('href');
        elVideoTrustLink.textContent = '';
    }

    // The video state the server sends as a text frame on /video (docs/WEB_API.md 5, docs/VIDEO_TRANSPORT.md 8):
    // {"t":"video","state":"locked","w":1280,"h":720}. Pure functions, checked on the host by
    // tools/test/check_video_viewer.js. Anything that does not fit the contract is dropped (parse returns null).
    const VideoStatus = {
        // w and h: a JSON integer in 0..65535 (no strings, fractions, null, negative numbers).
        validDimension(v) {
            return typeof v === 'number' && Number.isInteger(v) && v >= 0 && v <= 65535;
        },
        // text frame -> { state, w, h } or null. Unknown keys are ignored. Only "locked" carries a resolution (>= 1);
        // the other states are reported with w = h = 0.
        parse(text) {
            if (typeof text !== 'string' || text.length === 0 || text.length > 1024) return null;
            let obj;
            try {
                obj = JSON.parse(text);
            } catch (err) {
                return null;
            }
            if (obj === null || typeof obj !== 'object' || Array.isArray(obj)) return null;
            if (obj.t !== 'video') return null;
            const state = obj.state;
            if (state !== 'unknown' && state !== 'no_signal' && state !== 'locked') return null;
            if (!this.validDimension(obj.w) || !this.validDimension(obj.h)) return null;
            if (state === 'locked') {
                if (obj.w < 1 || obj.h < 1) return null;
                return { state: state, w: obj.w, h: obj.h };
            }
            return { state: state, w: 0, h: 0 };
        },
        // "1280x720" (both are integers already; Math.trunc keeps the text an integer whatever the caller passes).
        resolutionLabel(st) {
            return String(Math.trunc(st.w)) + 'x' + String(Math.trunc(st.h));
        },
        // The line under the video: the resolution is shown only while the signal is locked.
        statusLine(st) {
            if (st.state === 'locked') return '接続完了 / 受信中 (' + this.resolutionLabel(st) + ')';
            return '接続完了 / 受信中';
        },
        // The text over the video for the states that have one (locked has none: null).
        overlayMessage(state) {
            if (state === 'no_signal') return '信号なし';
            if (state === 'unknown') return '映像の状態を確認中…';
            return null;
        },
        // locked -> locked with another resolution (prev is the last accepted status, or null).
        isResolutionChange(prev, cur) {
            return prev !== null && prev.state === 'locked' && cur.state === 'locked' && (prev.w !== cur.w || prev.h !== cur.h);
        },
        changeNotice(st) {
            return '解像度が変わりました（' + this.resolutionLabel(st) + '）';
        }
    };

    // The size of the video box (the view modes). Pure functions, checked on the host by tools/test/check_video_viewer.js.
    // mode: 'fit' = the largest box with the aspect of the picture that fits in avail (it enlarges too), 'width' = the
    // width of avail and the height from the aspect (the page scrolls when it is taller), 'actual' = one picture
    // pixel is one CSS pixel. avail and video are { w, h }. The result is { w, h } in whole CSS pixels: the sides are
    // rounded down (so a fit box is never larger than avail) and are at least 1. null when a side of avail or video is
    // not a finite number >= 1, or the mode is unknown.
    const VideoLayout = {
        MODES: ['fit', 'width', 'actual'],
        // A saved value is used only when it is exactly one of the modes; anything else is the default, 'fit'.
        normalizeMode(value) {
            for (let i = 0; i < this.MODES.length; i++) {
                if (value === this.MODES[i]) return value;
            }
            return 'fit';
        },
        validSize(v) {
            return v !== null && typeof v === 'object' && Number.isFinite(v.w) && Number.isFinite(v.h) && v.w >= 1 && v.h >= 1;
        },
        fitSize(mode, avail, video) {
            if (!this.validSize(avail) || !this.validSize(video)) return null;
            const aw = Math.floor(avail.w);
            const ah = Math.floor(avail.h);
            const vw = Math.floor(video.w);
            const vh = Math.floor(video.h);
            if (mode === 'actual') return { w: vw, h: vh };
            if (mode === 'width') return { w: aw, h: Math.max(1, Math.floor(aw * vh / vw)) };
            if (mode === 'fit') {
                // Limited by the width when avail is not wider than the picture in proportion (aw / ah <= vw / vh).
                if (aw * vh <= ah * vw) return { w: aw, h: Math.max(1, Math.floor(aw * vh / vw)) };
                return { w: Math.max(1, Math.floor(ah * vw / vh)), h: ah };
            }
            return null;
        }
    };

    const VideoCodec = {
        MODES: ['mjpeg', 'h264'],
        normalizeMode(mode) {
            return this.MODES.indexOf(mode) !== -1 ? mode : 'mjpeg';
        }
    };

    // ---- View modes and full screen ----
    // The canvas box is set here (CSSOM, never by CSS) to the exact size VideoLayout.fitSize gives: the mouse position
    // is computed from this box (getMousePos), so the box must be the picture itself, with no stretching or black bars.
    const VIEW_MODE_KEY = 'quadkvm.viewMode';
    const viewModeButtons = [['fit', btnViewFit], ['width', btnViewWidth], ['actual', btnViewActual]];

    // Storage can be missing or throw (private window, blocked site data): the page works without it.
    function loadViewMode() {
        try {
            return VideoLayout.normalizeMode(localStorage.getItem(VIEW_MODE_KEY));
        } catch (err) {
            return 'fit';
        }
    }

    function saveViewMode(mode) {
        try {
            localStorage.setItem(VIEW_MODE_KEY, mode);
        } catch (err) {
            // the mode is just not kept
        }
    }

    let viewMode = loadViewMode();
    let laidOutW = 0; // the picture size the box was last set for
    let laidOutH = 0;

    function isFullscreen() {
        return document.fullscreenElement === elVideoWrap;
    }

    // The border of the canvas is outside the size that is set (content box): it is taken off the area.
    function borderSize(whole, inner) {
        const b = whole - inner;
        return Number.isFinite(b) && b > 0 ? b : 0;
    }

    function updateVideoLayout() {
        const picture = { w: elVideo.width, h: elVideo.height };
        const avail = {
            w: elVideoWrap.clientWidth - borderSize(elVideo.offsetWidth, elVideo.clientWidth),
            h: elVideoWrap.clientHeight - borderSize(elVideo.offsetHeight, elVideo.clientHeight)
        };
        // Full screen: the wrap is the whole window, and the picture is always fitted into it.
        const size = VideoLayout.fitSize(isFullscreen() ? 'fit' : viewMode, avail, picture);
        if (size === null) return; // no resolution yet, or the dashboard is not shown: nothing to set
        elVideo.style.width = size.w + 'px';
        elVideo.style.height = size.h + 'px';
        laidOutW = picture.w;
        laidOutH = picture.h;
    }

    // Called after the canvas resolution may have changed (every JPEG frame, every H.264 frame).
    function noteVideoSize() {
        if (elVideo.width !== laidOutW || elVideo.height !== laidOutH) updateVideoLayout();
    }

    function updateViewModeUI() {
        for (let i = 0; i < viewModeButtons.length; i++) {
            const mode = viewModeButtons[i][0];
            viewModeButtons[i][1].setAttribute('aria-pressed', mode === viewMode ? 'true' : 'false');
            viewDashboard.classList.remove('mode-' + mode);
        }
        viewDashboard.classList.add('mode-' + viewMode);
    }

    function setViewMode(mode) {
        viewMode = VideoLayout.normalizeMode(mode);
        saveViewMode(viewMode);
        updateViewModeUI();
        updateVideoLayout();
    }

    viewModeButtons.forEach((entry) => {
        entry[1].addEventListener('click', () => setViewMode(entry[0]));
    });

    const CODEC_MODE_KEY = 'quadkvm.videoCodec';
    const codecModeButtons = [['mjpeg', btnCodecMjpeg], ['h264', btnCodecH264]];

    function loadCodecMode() {
        try {
            return VideoCodec.normalizeMode(localStorage.getItem(CODEC_MODE_KEY));
        } catch (err) {
            return 'mjpeg';
        }
    }

    function saveCodecMode(mode) {
        try {
            localStorage.setItem(CODEC_MODE_KEY, mode);
        } catch (err) {
            // the codec mode is just not kept
        }
    }

    let codecMode = loadCodecMode();
    let codecSwitchPending = false;

    function updateCodecModeUI() {
        for (let i = 0; i < codecModeButtons.length; i++) {
            const entry = codecModeButtons[i];
            entry[1].setAttribute('aria-pressed', entry[0] === codecMode ? 'true' : 'false');
        }
    }

    async function setCodecMode(mode) {
        const next = VideoCodec.normalizeMode(mode);
        saveCodecMode(next);
        if (next === codecMode && (next !== 'h264' || !h264Failed)) return;
        codecMode = next;
        if (next === 'h264') h264Failed = false;
        updateCodecModeUI();
        if (ws || wsReconnectTimer || codecSwitchPending) {
            codecSwitchPending = true;
            try {
                if (wsReconnectTimer) {
                    clearTimeout(wsReconnectTimer);
                    wsReconnectTimer = null;
                }
                if (ws) {
                    ws.onclose = null;
                    ws.close();
                    ws = null;
                }
                resetVideoStatus();
                wsReconnectDelay = 1000;
                if (codecMode === 'h264') await waitForProbe();
            } finally {
                codecSwitchPending = false;
            }
            if (!ws && !viewDashboard.classList.contains('view-hidden')) connectWebSocket();
        }
    }

    codecModeButtons.forEach((entry) => {
        entry[1].addEventListener('click', () => setCodecMode(entry[0]));
    });
    updateCodecModeUI();

    function swallowRejection(p) {
        if (p && typeof p.catch === 'function') p.catch(() => {});
    }

    // Chromium only: Escape is sent to the target (not used to leave full screen) while input is on in full screen
    // and this connection is the controller (inputGranted): nothing is sent to the target before that, so locking
    // Escape would only take the key away from the browser. Optional: any failure is ignored.
    let keyboardLocked = false;
    function syncKeyboardLock() {
        const want = isFullscreen() && inputEnabled && inputGranted;
        if (want === keyboardLocked) return;
        keyboardLocked = want;
        try {
            const kb = typeof navigator !== 'undefined' ? navigator.keyboard : undefined;
            if (!kb) return;
            if (want) {
                if (typeof kb.lock === 'function') swallowRejection(kb.lock(['Escape']));
            } else if (typeof kb.unlock === 'function') {
                kb.unlock();
            }
        } catch (err) {
            // not available here
        }
    }

    function fullscreenSupported() {
        return !!document.fullscreenEnabled && typeof elVideoWrap.requestFullscreen === 'function';
    }

    function toggleFullscreen() {
        try {
            if (isFullscreen()) swallowRejection(document.exitFullscreen());
            else swallowRejection(elVideoWrap.requestFullscreen());
        } catch (err) {
            // refused: the page stays as it is
        }
    }

    // The bar and the text under the video are hidden in full screen (the wrap is the only thing shown).
    function updateFullscreenUI() {
        // A request that finished after the dashboard was hidden (logout, session end): do not stay in full screen
        // over the login view.
        if (isFullscreen() && viewDashboard.classList.contains('view-hidden')) {
            try {
                swallowRejection(document.exitFullscreen());
            } catch (err) {
                // already out
            }
        }
        const on = isFullscreen();
        btnFullscreen.textContent = on ? '全画面を終了' : '全画面';
        if (on) viewDashboard.classList.add('is-fullscreen');
        else viewDashboard.classList.remove('is-fullscreen');
        syncKeyboardLock();
        updateVideoLayout();
    }

    btnFullscreen.addEventListener('click', toggleFullscreen);
    // The bar is hidden in full screen: this button (shown by CSS only while .is-fullscreen is set) is the way out
    // for a device without an Escape key.
    btnFullscreenExit.addEventListener('click', () => {
        if (isFullscreen()) toggleFullscreen();
    });
    document.addEventListener('fullscreenchange', updateFullscreenUI);
    window.addEventListener('resize', updateVideoLayout);
    if (typeof ResizeObserver === 'function') {
        try {
            new ResizeObserver(updateVideoLayout).observe(elVideoWrap);
        } catch (err) {
            // the resize event is enough
        }
    }
    if (!fullscreenSupported()) {
        btnFullscreen.disabled = true;
        btnFullscreen.title = 'このブラウザでは全画面を使えません';
    }
    updateViewModeUI();

    // H.264 over /video (docs/VIDEO_TRANSPORT.md 9.2, 9.3): one binary message = one Annex B access unit. Pure
    // functions on a Uint8Array, checked on the host by tools/test/check_video_viewer.js.
    const H264 = {
        // 'jpeg' (starts with FF D8), 'h264' (starts with the 4-byte start code 00 00 00 01) or null (anything else).
        classify(b) {
            if (b === null || b === undefined || typeof b.length !== 'number') return null;
            if (b.length >= 2 && b[0] === 0xFF && b[1] === 0xD8) return 'jpeg';
            if (b.length >= 4 && b[0] === 0 && b[1] === 0 && b[2] === 0 && b[3] === 1) return 'h264';
            return null;
        },
        // Annex B -> [{ type, data }]. data starts at the NAL header byte (the start code and the zero bytes before
        // the next start code are not part of it). Empty NALs and NALs whose forbidden_zero_bit is set are dropped.
        parseAnnexB(b) {
            const out = [];
            if (b === null || b === undefined || typeof b.length !== 'number') return out;
            const n = b.length;
            const starts = [];
            let pos = 0;
            while (pos + 3 <= n) {
                if (b[pos] === 0 && b[pos + 1] === 0 && b[pos + 2] === 1) {
                    starts.push(pos + 3);
                    pos += 3;
                } else {
                    pos++;
                }
            }
            for (let k = 0; k < starts.length; k++) {
                const s = starts[k];
                let end = k + 1 < starts.length ? starts[k + 1] - 3 : n;
                while (end > s && b[end - 1] === 0) end--;
                if (end <= s) continue;
                if ((b[s] & 0x80) !== 0) continue;
                out.push({ type: b[s] & 0x1F, data: b.subarray(s, end) });
            }
            return out;
        },
        // key: the access unit holds an SPS (type 7) or an IDR slice (type 5). sps: the first SPS NAL (or null). count: NALs.
        analyze(b) {
            const nals = this.parseAnnexB(b);
            let key = false;
            let sps = null;
            for (let i = 0; i < nals.length; i++) {
                if (nals[i].type === 7) {
                    key = true;
                    if (sps === null) sps = nals[i].data;
                } else if (nals[i].type === 5) {
                    key = true;
                }
            }
            return { key: key, sps: sps, count: nals.length };
        },
        isKeyAccessUnit(b) {
            return this.analyze(b).key;
        },
        // An SPS NAL (starting at its header byte, 67 42 c0 1f ...) -> "avc1.42c01f": profile_idc, the constraint
        // flags and level_idc. null when it is not an SPS or too short.
        avcCodecStringFromSps(nal) {
            if (nal === null || nal === undefined || typeof nal.length !== 'number' || nal.length < 4) return null;
            if ((nal[0] & 0x80) !== 0 || (nal[0] & 0x1F) !== 7) return null;
            const hex = (v) => (v < 16 ? '0' : '') + v.toString(16);
            return 'avc1.' + hex(nal[1]) + hex(nal[2]) + hex(nal[3]);
        },
        sameBytes(a, b) {
            if (a === null || b === null || a.length !== b.length) return false;
            for (let i = 0; i < a.length; i++) {
                if (a[i] !== b[i]) return false;
            }
            return true;
        }
    };

    const RESOLUTION_NOTICE_MS = 2000;
    let videoStatus = null; // last accepted status of this connection (null before the first one)
    let resolutionNoticeTimer = null;

    function setVideoOverlay(text) {
        if (text === null) {
            elVideoOverlay.classList.add('video-overlay-hidden');
            elVideoOverlay.textContent = '';
        } else {
            elVideoOverlay.textContent = text;
            elVideoOverlay.classList.remove('video-overlay-hidden');
        }
    }

    // One place for the reset: a new connection, a disconnect and a logout all end here.
    function resetVideoStatus() {
        videoConnGen++;
        pendingMjpegFrame = null;
        if (resolutionNoticeTimer !== null) {
            clearTimeout(resolutionNoticeTimer);
            resolutionNoticeTimer = null;
        }
        videoStatus = null;
        setVideoOverlay(null);
        resetH264(); // the decoder and the "H.264" label go with the connection
    }

    function applyVideoStatus(st) {
        const changed = VideoStatus.isResolutionChange(videoStatus, st);
        videoStatus = st;
        // The next status removes the "resolution changed" notice at once.
        if (resolutionNoticeTimer !== null) {
            clearTimeout(resolutionNoticeTimer);
            resolutionNoticeTimer = null;
        }
        elVideoStatus.textContent = VideoStatus.statusLine(st);
        if (h264Shown) elVideoStatus.textContent += H264_LABEL;
        if (changed) {
            setVideoOverlay(VideoStatus.changeNotice(st));
            resolutionNoticeTimer = setTimeout(() => {
                resolutionNoticeTimer = null;
                setVideoOverlay(null);
            }, RESOLUTION_NOTICE_MS);
        } else {
            // The last frame stays on the canvas (it is never cleared here).
            setVideoOverlay(VideoStatus.overlayMessage(st.state));
        }
    }

    // ---- H.264 (docs/VIDEO_TRANSPORT.md 9.8) ----
    // The decoder is WebCodecs' VideoDecoder, fed Annex B access units (no `description`). Any failure falls back
    // to MJPEG once: this tab stops sending `accept` and the connection is made again.
    const H264_PROBE_CODEC = 'avc1.42c01f'; // 9.4: used until the SPS of the stream is known
    const H264_PROBE_WAIT_MS = 1500; // TODO(hw-verify): how long to wait for isConfigSupported before connecting without accept
    const H264_MAX_QUEUE = 3; // TODO(hw-verify): decodeQueueSize at which P frames are dropped until the next IDR
    const H264_TIMESTAMP_STEP = 33333; // microseconds; the stream carries no time stamps (no B frames, so any increasing value)
    const H264_LABEL = ' / H.264（低帯域）';
    const STATUS_PLAIN = '接続完了 / 受信中';
    let h264Failed = false; // a failure happened in this tab: no `accept` from now on
    let h264Supported = false; // the start-up probe said the browser can decode H.264
    let h264Probe = Promise.resolve(); // settles when the probe is done
    let h264Dec = null; // { decoder, codec, sps } once configured
    let h264Gen = 0; // bumped by every reset / configure start, so a late isConfigSupported result is ignored
    let h264Pending = 0; // generation of a configure waiting for isConfigSupported (0 = none)
    let h264NeedKey = true; // P frames are dropped until an IDR has been decoded
    let h264Timestamp = -1;
    let h264Shown = false; // the status line says H.264

    // ArrayBuffer (or a view of one) -> Uint8Array; null for anything else. toString, not instanceof: it also works
    // for a buffer made in another realm.
    function toBytes(data) {
        if (Object.prototype.toString.call(data) === '[object ArrayBuffer]') return new Uint8Array(data);
        if (ArrayBuffer.isView(data)) return new Uint8Array(data.buffer, data.byteOffset, data.byteLength);
        return null;
    }

    function h264Available() {
        return typeof VideoDecoder === 'function' && typeof EncodedVideoChunk === 'function' &&
            typeof VideoDecoder.isConfigSupported === 'function';
    }

    function probeH264() {
        if (!h264Available()) return Promise.resolve();
        let p;
        try {
            p = VideoDecoder.isConfigSupported({ codec: H264_PROBE_CODEC, optimizeForLatency: true });
        } catch (err) {
            return Promise.resolve();
        }
        return Promise.resolve(p).then((res) => {
            h264Supported = !!res && res.supported === true;
        }, () => {});
    }
    h264Probe = probeH264();

    // The first connection waits for the probe so that it already knows whether to send accept=h264. A probe that
    // never settles must not keep the video away: after H264_PROBE_WAIT_MS the connection is made without accept.
    function waitForProbe() {
        return new Promise((resolve) => {
            const t = setTimeout(resolve, H264_PROBE_WAIT_MS);
            h264Probe.then(() => {
                clearTimeout(t);
                resolve();
            });
        });
    }

    function setH264Shown(on) {
        if (h264Shown === on) return;
        h264Shown = on;
        elVideoStatus.textContent = (videoStatus === null ? STATUS_PLAIN : VideoStatus.statusLine(videoStatus)) + (on ? H264_LABEL : '');
    }

    // Closes the decoder and forgets everything about the stream (a new connection, a disconnect, a resolution change).
    function resetH264() {
        h264Gen++;
        h264Pending = 0;
        h264NeedKey = true;
        h264Shown = false;
        if (h264Dec !== null) {
            const d = h264Dec.decoder;
            h264Dec = null; // first: an error of the closing decoder is then ignored
            try {
                d.close();
            } catch (err) {
                // already closed
            }
        }
    }

    // One fallback per tab: no more `accept`, and the connection is made again through the usual reconnect.
    function h264Fallback() {
        if (h264Failed) return;
        h264Failed = true;
        codecMode = 'mjpeg';
        updateCodecModeUI();
        resetH264();
        if (ws) ws.close();
    }

    function drawH264Frame(dec, frame) {
        try {
            if (h264Dec === null || h264Dec.decoder !== dec) return; // frame of a decoder that is gone
            const w = frame.displayWidth || frame.codedWidth;
            const h = frame.displayHeight || frame.codedHeight;
            if (elVideo.width !== w) elVideo.width = w;
            if (elVideo.height !== h) elVideo.height = h;
            noteVideoSize();
            ctx.drawImage(frame, 0, 0);
            setH264Shown(true);
        } catch (err) {
            // a frame that cannot be drawn is dropped
        } finally {
            frame.close();
        }
    }

    function nextH264Timestamp() {
        const nowUs = typeof performance !== 'undefined' && typeof performance.now === 'function'
            ? Math.round(performance.now() * 1000)
            : -1;
        if (nowUs >= 0) {
            h264Timestamp = nowUs > h264Timestamp ? nowUs : h264Timestamp + 1;
        } else {
            h264Timestamp = h264Timestamp < 0 ? 0 : h264Timestamp + H264_TIMESTAMP_STEP;
        }
        return h264Timestamp;
    }

    function decodeH264(bytes, key) {
        try {
            const chunk = new EncodedVideoChunk({ type: key ? 'key' : 'delta', timestamp: nextH264Timestamp(), data: bytes });
            h264Dec.decoder.decode(chunk);
            if (key) h264NeedKey = false;
        } catch (err) {
            h264Fallback();
        }
    }

    // First IDR of the stream, or an SPS that is not the one the decoder was configured with (resolution change).
    function startH264Decoder(codec, sps, bytes) {
        resetH264();
        const gen = h264Gen;
        h264Pending = gen;
        const config = { codec: codec, optimizeForLatency: true };
        let p;
        try {
            p = VideoDecoder.isConfigSupported(config);
        } catch (err) {
            h264Fallback();
            return;
        }
        Promise.resolve(p).then((res) => {
            if (gen !== h264Gen) return; // reset meanwhile
            h264Pending = 0;
            if (!res || res.supported !== true) {
                h264Fallback();
                return;
            }
            let dec = null;
            try {
                dec = new VideoDecoder({
                    output: (frame) => drawH264Frame(dec, frame),
                    error: () => {
                        if (h264Dec !== null && h264Dec.decoder === dec) h264Fallback();
                    }
                });
                dec.configure(config);
            } catch (err) {
                if (dec !== null) {
                    try {
                        dec.close(); // made but not usable
                    } catch (err2) {
                        // already closed
                    }
                }
                h264Fallback();
                return;
            }
            h264Dec = { decoder: dec, codec: codec, sps: sps.slice() }; // a copy: the message buffer is not kept alive
            decodeH264(bytes, true);
        }, () => {
            if (gen === h264Gen) h264Fallback();
        });
    }

    // One binary message that starts with the start code.
    function feedH264(bytes) {
        if (h264Failed || h264Pending !== 0) return;
        const au = H264.analyze(bytes);
        if (au.count === 0) return; // no NAL at all: garbage
        if (!au.key) {
            if (h264Dec === null || h264NeedKey) return; // no IDR yet
            if (h264Dec.decoder.decodeQueueSize >= H264_MAX_QUEUE) {
                h264NeedKey = true; // falling behind: wait for the next IDR instead of piling up delay
                return;
            }
            decodeH264(bytes, false);
            return;
        }
        if (au.sps === null) {
            if (h264Dec !== null) decodeH264(bytes, true); // an IDR slice without an SPS: the decoder is configured
            return;
        }
        const codec = H264.avcCodecStringFromSps(au.sps);
        if (codec === null) return;
        if (h264Dec !== null && h264Dec.codec === codec && H264.sameBytes(h264Dec.sps, au.sps)) {
            decodeH264(bytes, true);
            return;
        }
        startH264Decoder(codec, au.sps, bytes);
    }

    // A JPEG on a connection that asked for H.264 (the server fell back to MJPEG).
    function noteMjpeg() {
        h264NeedKey = true;
        setH264Shown(false);
    }

    // Coalesce MJPEG frames while a decode is in flight: keep only the newest pending frame so delay does not pile up.
    async function pumpMjpeg(initialFrame) {
        if (isDecoding) {
            pendingMjpegFrame = initialFrame;
            return;
        }
        isDecoding = true;
        let nextFrame = initialFrame;
        let nextGen = videoConnGen;
        try {
            while (nextFrame !== null) {
                const current = nextFrame;
                const frameGen = nextGen;
                nextFrame = null;
                try {
                    const bmp = await createImageBitmap(current);
                    if (frameGen === videoConnGen) {
                        elVideo.width = bmp.width;
                        elVideo.height = bmp.height;
                        noteVideoSize();
                        ctx.drawImage(bmp, 0, 0);
                    }
                    bmp.close();
                } catch (err) {
                    // Decode error handled
                }
                if (pendingMjpegFrame !== null) {
                    nextFrame = pendingMjpegFrame;
                    nextGen = videoConnGen;
                    pendingMjpegFrame = null;
                }
            }
        } finally {
            isDecoding = false;
        }
    }

    function connectWebSocket() {
        if (ws) return;
        resetVideoStatus();
        const acceptH264 = codecMode === 'h264' && h264Supported && !h264Failed;
        const target = VideoTarget.withAccept(videoTarget || VideoTarget.resolve(location, undefined), acceptH264);
        let opened = false;
        ws = new WebSocket(target.url);
        ws.binaryType = "blob";
        if (acceptH264) ws.binaryType = "arraybuffer"; // the first bytes tell JPEG from H.264, and the order must hold

        ws.onopen = () => {
            opened = true;
            hideTrustHint();
            elVideoStatus.textContent = '接続完了 / 受信中';
            wsReconnectDelay = 1000; // reset
        };

        ws.onmessage = async (e) => {
            // A text frame is the video state (JSON); a binary frame is a JPEG. A text frame that does not fit
            // the contract is dropped, and it never touches the decoding of the frames.
            if (typeof e.data === 'string') {
                const st = VideoStatus.parse(e.data);
                if (st !== null) applyVideoStatus(st);
                return;
            }
            let frameData = e.data;
            if (acceptH264) {
                // JPEG (FF D8) or H.264 (00 00 00 01); anything else is dropped.
                const bytes = toBytes(e.data);
                const kind = bytes === null ? null : H264.classify(bytes);
                if (kind === 'h264') {
                    pendingMjpegFrame = null;
                    feedH264(bytes);
                    return;
                }
                if (kind !== 'jpeg') return;
                noteMjpeg();
                frameData = new Blob([bytes], { type: 'image/jpeg' });
            }
            await pumpMjpeg(frameData);
        };

        ws.onclose = () => {
            ws = null;
            resetVideoStatus();
            elVideoStatus.textContent = '切断されました。再接続します...';
            if (VideoTarget.needsTrustHint(target, opened)) showTrustHint(target);
            // Exponential backoff
            // Re-check the session first: after a logout or an expired session the page must go back to the login view.
            wsReconnectTimer = setTimeout(fetchStatus, wsReconnectDelay);
            wsReconnectDelay = Math.min(wsReconnectDelay * 2, 10000);
        };

        ws.onerror = (e) => {
            // A failure before the connection opened (typically an untrusted certificate on the video port).
            if (VideoTarget.needsTrustHint(target, opened)) showTrustHint(target);
        };
    }

    function connectInputWebSocket() {
        if (wsInput) return;
        const url = 'wss://' + location.host + '/ws';
        wsInput = new WebSocket(url);
        wsInput.binaryType = "arraybuffer";

        wsInput.onopen = () => {
            elInputStatus.textContent = '入力: 接続中...';
            wsInputReconnectDelay = 1000;
        };

        wsInput.onmessage = (e) => {
            const data = new Uint8Array(e.data);
            if (data.length === 0) return;
            const type = data[0];
            if (type === 0x80) {
                inputGranted = true;
                syncKeyboardLock();
                elInputStatus.textContent = '入力: 有効';
            } else if (type === 0x81) {
                if (data.length >= 2) {
                    const reason = data[1];
                    if (reason === 1) {
                        elInputStatus.textContent = '入力: 他の端末が操作中';
                    } else {
                        elInputStatus.textContent = '入力: 拒否されました';
                    }
                }
                // Do not reconnect if denied
                inputGranted = false;
                syncKeyboardLock();
                wsInput.onclose = null;
                wsInput.close();
                wsInput = null;
            } else if (type === 0x82) {
                if (data.length >= 2) {
                    const state = data[1];
                    elUsbStatus.textContent = state === 1 ? 'ターゲット: 接続済み' : 'ターゲット: 未接続';
                }
            }
        };

        wsInput.onclose = () => {
            wsInput = null;
            inputGranted = false;
            syncKeyboardLock();
            elInputStatus.textContent = '入力: 未接続';
            elUsbStatus.textContent = 'ターゲット: 未接続';
            wsInputReconnectTimer = setTimeout(() => {
                if (document.getElementById('view-dashboard').classList.contains('view-hidden')) return;
                connectInputWebSocket();
            }, wsInputReconnectDelay);
            wsInputReconnectDelay = Math.min(wsInputReconnectDelay * 2, 10000);
        };

        wsInput.onerror = () => {
            // Handled by close
        };
    }

    function disconnectWebSocket() {
        if (wsReconnectTimer) {
            clearTimeout(wsReconnectTimer);
            wsReconnectTimer = null;
        }
        if (ws) {
            ws.onclose = null; // Prevent reconnect logic
            ws.close();
            ws = null;
        }
        hideTrustHint();
        resetVideoStatus();
        elVideoStatus.textContent = '切断されました';

        if (wsInputReconnectTimer) {
            clearTimeout(wsInputReconnectTimer);
            wsInputReconnectTimer = null;
        }
        if (wsInput) {
            wsInput.onclose = null;
            wsInput.close();
            wsInput = null;
        }
        inputGranted = false;
        syncKeyboardLock();
        elInputStatus.textContent = '入力: 未接続';
        elUsbStatus.textContent = 'ターゲット: 未接続';
    }


    // Messages for the "error" codes POST /api/v1/auth/setup returns with 400 (docs/WEB_API.md).
    const SETUP_ERRORS = {
        password_too_short: 'パスワードは 10 文字以上にしてください',
        password_too_long: 'パスワードは 128 バイト以下にしてください',
        password_contains_admin: 'パスワードに「admin」を含めないでください',
        password_all_same: '同じ文字だけの繰り返しは使えません',
        password_sequential: '連番（0123456789 や abcdefghij など）は使えません',
        password_common: 'よく使われるパスワードは使えません',
        bad_request: 'リクエストが不正です'
    };
    // The server limit is in bytes of the UTF-8 encoded password, not in characters.
    const MAX_PASSWORD_BYTES = 128;

    const KEYMAP = {
        'KeyA': 0x04, 'KeyB': 0x05, 'KeyC': 0x06, 'KeyD': 0x07,
        'KeyE': 0x08, 'KeyF': 0x09, 'KeyG': 0x0A, 'KeyH': 0x0B,
        'KeyI': 0x0C, 'KeyJ': 0x0D, 'KeyK': 0x0E, 'KeyL': 0x0F,
        'KeyM': 0x10, 'KeyN': 0x11, 'KeyO': 0x12, 'KeyP': 0x13,
        'KeyQ': 0x14, 'KeyR': 0x15, 'KeyS': 0x16, 'KeyT': 0x17,
        'KeyU': 0x18, 'KeyV': 0x19, 'KeyW': 0x1A, 'KeyX': 0x1B,
        'KeyY': 0x1C, 'KeyZ': 0x1D,

        'Digit1': 0x1E, 'Digit2': 0x1F, 'Digit3': 0x20, 'Digit4': 0x21,
        'Digit5': 0x22, 'Digit6': 0x23, 'Digit7': 0x24, 'Digit8': 0x25,
        'Digit9': 0x26, 'Digit0': 0x27,

        'ControlLeft': 0xE0, 'ShiftLeft': 0xE1, 'AltLeft': 0xE2, 'MetaLeft': 0xE3,
        'ControlRight': 0xE4, 'ShiftRight': 0xE5, 'AltRight': 0xE6, 'MetaRight': 0xE7,

        'Enter': 0x28, 'Escape': 0x29, 'Backspace': 0x2A, 'Tab': 0x2B, 'Space': 0x2C,

        'Minus': 0x2D, 'Equal': 0x2E, 'BracketLeft': 0x2F, 'BracketRight': 0x30,
        'Backslash': 0x31, 'IntlHash': 0x32, 'Semicolon': 0x33, 'Quote': 0x34, 'Backquote': 0x35,
        'Comma': 0x36, 'Period': 0x37, 'Slash': 0x38, 'CapsLock': 0x39,

        'F1': 0x3A, 'F2': 0x3B, 'F3': 0x3C, 'F4': 0x3D, 'F5': 0x3E, 'F6': 0x3F,
        'F7': 0x40, 'F8': 0x41, 'F9': 0x42, 'F10': 0x43, 'F11': 0x44, 'F12': 0x45,
        'F13': 0x68, 'F14': 0x69, 'F15': 0x6A, 'F16': 0x6B, 'F17': 0x6C, 'F18': 0x6D,
        'F19': 0x6E, 'F20': 0x6F, 'F21': 0x70, 'F22': 0x71, 'F23': 0x72, 'F24': 0x73,

        'PrintScreen': 0x46, 'ScrollLock': 0x47, 'Pause': 0x48, 'Insert': 0x49,
        'Home': 0x4A, 'PageUp': 0x4B, 'Delete': 0x4C, 'End': 0x4D, 'PageDown': 0x4E,
        'ArrowRight': 0x4F, 'ArrowLeft': 0x50, 'ArrowDown': 0x51, 'ArrowUp': 0x52,

        'NumLock': 0x53,
        'NumpadDivide': 0x54, 'NumpadMultiply': 0x55, 'NumpadSubtract': 0x56, 'NumpadAdd': 0x57,
        'NumpadEnter': 0x58,
        'Numpad1': 0x59, 'Numpad2': 0x5A, 'Numpad3': 0x5B, 'Numpad4': 0x5C,
        'Numpad5': 0x5D, 'Numpad6': 0x5E, 'Numpad7': 0x5F, 'Numpad8': 0x60,
        'Numpad9': 0x61, 'Numpad0': 0x62, 'NumpadDecimal': 0x63,

        'IntlBackslash': 0x64, 'ContextMenu': 0x65, 'NumpadEqual': 0x67,

        'IntlRo': 0x87, 'KanaMode': 0x88, 'IntlYen': 0x89,
        'Convert': 0x8A, 'NonConvert': 0x8B, 'Lang1': 0x90, 'Lang2': 0x91
    };

    const MsgBuilder = {
        keyDown(usage) {
            const buf = new ArrayBuffer(2);
            const v = new DataView(buf);
            v.setUint8(0, 0x01);
            v.setUint8(1, usage);
            return buf;
        },
        keyUp(usage) {
            const buf = new ArrayBuffer(2);
            const v = new DataView(buf);
            v.setUint8(0, 0x02);
            v.setUint8(1, usage);
            return buf;
        },
        releaseAll() {
            const buf = new ArrayBuffer(1);
            const v = new DataView(buf);
            v.setUint8(0, 0x03);
            return buf;
        },
        keepalive() {
            const buf = new ArrayBuffer(1);
            const v = new DataView(buf);
            v.setUint8(0, 0x04);
            return buf;
        },
        ptrAbs(buttons, x, y, wheel) {
            const buf = new ArrayBuffer(7);
            const v = new DataView(buf);
            v.setUint8(0, 0x10);
            v.setUint8(1, buttons);
            v.setUint16(2, x, true);
            v.setUint16(4, y, true);
            v.setInt8(6, wheel);
            return buf;
        },
        ptrRel(buttons, dx, dy, wheel) {
            const buf = new ArrayBuffer(7);
            const v = new DataView(buf);
            v.setUint8(0, 0x11);
            v.setUint8(1, buttons);
            v.setInt16(2, dx, true);
            v.setInt16(4, dy, true);
            v.setInt8(6, wheel);
            return buf;
        }
    };

    function showError(msg) {
        elError.textContent = msg;
        elError.classList.remove('error-hidden');
    }

    function hideError() {
        elError.classList.add('error-hidden');
        elError.textContent = '';
    }

    function showView(view) {
        viewSetup.classList.add('view-hidden');
        viewLogin.classList.add('view-hidden');
        viewDashboard.classList.add('view-hidden');
        view.classList.remove('view-hidden');
        if (view === viewDashboard) {
            // The dashboard has the whole window; the card (title and forms) is only for the setup and the login.
            elAuthCard.classList.add('view-hidden');
            updateVideoLayout();
            iso.start();
        } else {
            iso.stop();
            elAuthCard.classList.remove('view-hidden');
            if (isFullscreen()) {
                // e.g. the session ended while the video was full screen
                try {
                    swallowRejection(document.exitFullscreen());
                } catch (err) {
                    // already out
                }
            }
        }
    }

    async function fetchStatus() {
        try {
            const res = await fetch('/api/v1/auth/status', { credentials: 'same-origin' });
            if (!res.ok) throw new Error(`HTTP ${res.status}`);
            const data = await res.json();
            if (data.state === 'needs_setup') {
                disconnectWebSocket();
                showView(viewSetup);
            } else if (data.state === 'ready') {
                if (data.authenticated) {
                    showView(viewDashboard);
                    videoTarget = VideoTarget.resolve(location, data.video_port);
                    if (codecMode === 'h264') await waitForProbe(); // so the first connection already knows whether to send accept=h264
                    connectWebSocket();
                    connectInputWebSocket();
                } else {
                    disconnectWebSocket();
                    showView(viewLogin);
                }
            } else {
                throw new Error('不明な状態です');
            }
        } catch (e) {
            showError('状態の取得に失敗しました: ' + e.message);
        }
    }

    async function doAuthAction(url, data, successCallback, errorMessages) {
        hideError();
        try {
            const res = await fetch(url, {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify(data),
                credentials: 'same-origin'
            });
            if (!res.ok) {
                let msg = `エラー (${res.status})`;
                if (res.status === 429 || res.status === 503) {
                    const retry = res.headers.get('Retry-After');
                    if (retry) {
                        msg += `。 しばらく（${retry}秒）待ってください`;
                    } else {
                        msg += '。 しばらく待ってください';
                    }
                } else {
                    try {
                        const errData = await res.json();
                        if (errData.error) {
                            if (errorMessages && Object.prototype.hasOwnProperty.call(errorMessages, errData.error)) {
                                msg = errorMessages[errData.error];
                            } else {
                                msg += ': ' + errData.error;
                            }
                        }
                    } catch (e) {}
                }
                throw new Error(msg);
            }
            if (successCallback) await successCallback(res);
        } catch (e) {
            showError(e.message);
        }
    }

    formSetup.addEventListener('submit', (e) => {
        e.preventDefault();
        const pwd = setupPassword.value;
        const conf = setupPasswordConfirm.value;
        if (pwd !== conf) {
            showError('パスワードが一致しません');
            return;
        }
        if (new TextEncoder().encode(pwd).length > MAX_PASSWORD_BYTES) {
            showError(SETUP_ERRORS.password_too_long);
            return;
        }
        setupPassword.value = '';
        setupPasswordConfirm.value = '';
        doAuthAction('/api/v1/auth/setup', { password: pwd }, async (res) => {
            // Check if setup gave us a session cookie.
            // Setup returns 200 on success. We need to check if we are authenticated now.
            // If not, try to login.
            const statusRes = await fetch('/api/v1/auth/status', { credentials: 'same-origin' });
            if (statusRes.ok) {
                 const data = await statusRes.json();
                 if (!data.authenticated) {
                     await doAuthAction('/api/v1/auth/login', { password: pwd }, fetchStatus);
                 } else {
                     await fetchStatus();
                 }
            } else {
                await fetchStatus();
            }
        }, SETUP_ERRORS);
    });

    formLogin.addEventListener('submit', (e) => {
        e.preventDefault();
        const pwd = loginPassword.value;
        loginPassword.value = '';
        doAuthAction('/api/v1/auth/login', { password: pwd }, fetchStatus);
    });

    btnLogout.addEventListener('click', () => {
        doAuthAction('/api/v1/auth/logout', {}, fetchStatus);
    });

    fetchStatus();
});
