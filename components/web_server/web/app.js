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

    const elVideo = document.getElementById('video');
    const elVideoStatus = document.getElementById('video-status');
    const btnInputToggle = document.getElementById('btn-input-toggle');
    const btnPointerLock = document.getElementById('btn-pointer-lock');
    const elInputStatus = document.getElementById('input-status');
    const elUsbStatus = document.getElementById('usb-status');

    const elVideoTrustHint = document.getElementById('video-trust-hint');
    const elVideoTrustLink = document.getElementById('video-trust-link');

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

    function connectWebSocket() {
        if (ws) return;
        const target = videoTarget || VideoTarget.resolve(location, undefined);
        let opened = false;
        ws = new WebSocket(target.url);
        ws.binaryType = "blob";

        ws.onopen = () => {
            opened = true;
            hideTrustHint();
            elVideoStatus.textContent = '接続完了 / 受信中';
            wsReconnectDelay = 1000; // reset
        };

        ws.onmessage = async (e) => {
            if (isDecoding) return;
            isDecoding = true;
            try {
                const bmp = await createImageBitmap(e.data);
                elVideo.width = bmp.width;
                elVideo.height = bmp.height;
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
