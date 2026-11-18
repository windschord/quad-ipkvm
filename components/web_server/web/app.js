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
    let h264Timestamp = 0;
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

    function decodeH264(bytes, key) {
        try {
            const chunk = new EncodedVideoChunk({ type: key ? 'key' : 'delta', timestamp: h264Timestamp, data: bytes });
            h264Timestamp += H264_TIMESTAMP_STEP;
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

    function connectWebSocket() {
        if (ws) return;
        resetVideoStatus();
        const acceptH264 = h264Supported && !h264Failed;
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
                    feedH264(bytes);
                    return;
                }
                if (kind !== 'jpeg') return;
                noteMjpeg();
                frameData = new Blob([bytes], { type: 'image/jpeg' });
            }
            if (isDecoding) return;
            isDecoding = true;
            try {
                const bmp = await createImageBitmap(frameData);
                elVideo.width = bmp.width;
                elVideo.height = bmp.height;
                noteVideoSize();
                ctx.drawImage(bmp, 0, 0);
                bmp.close();
            } catch (err) {
                // Decode error handled
            } finally {
                isDecoding = false;
            }
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
        } else {
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
                    await waitForProbe(); // so the first connection already knows whether to send accept=h264
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
