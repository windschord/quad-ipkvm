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
                showView(viewSetup);
            } else if (data.state === 'ready') {
                if (data.authenticated) {
                    showView(viewDashboard);
                } else {
                    showView(viewLogin);
                }
            } else {
                throw new Error('不明な状態です');
            }
        } catch (e) {
            showError('状態の取得に失敗しました: ' + e.message);
        }
    }

    async function doAuthAction(url, data, successCallback) {
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
                        if (errData.error) msg += ': ' + errData.error;
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
        });
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
