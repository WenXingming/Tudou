const chatEl = document.getElementById('chat');
const inputEl = document.getElementById('input');
const sendEl = document.getElementById('send');
const statusEl = document.getElementById('status');
const sidebarEl = document.getElementById('sidebar');

// Mobile & Sidebar Toggles
const mobileMenuBtn = document.getElementById('mobileMenuBtn');
const mobileNewChatBtn = document.getElementById('mobileNewChat');
const toggleSidebarBtn = document.getElementById('toggleSidebar');
const collapseSidebarBtn = document.getElementById('collapseSidebar');

function setStatus(text, isError) {
    statusEl.textContent = text || '';
    if (isError) {
        statusEl.style.color = '#ef4444';
    } else {
        statusEl.style.color = '#8e8ea0';
    }
}

function scrollToBottom() {
    // 只有当距离底部不远时才自动滚到底，或者强制滚到底
    chatEl.scrollTo({ top: chatEl.scrollHeight, behavior: 'smooth' });
}

// Markdown Init
let markedReady = false;
function ensureMarkdownReady() {
    if (markedReady) return;
    if (window.marked && typeof window.marked.parse === 'function') {
        window.marked.setOptions({
            gfm: true,
            breaks: true,
            headerIds: false,
            mangle: false
        });
        markedReady = true;
    }
}

function renderMarkdownSafe(text) {
    const raw = String(text ?? '');
    if (!window.marked || !window.DOMPurify) {
        return raw.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/\n/g, '<br/>');
    }
    ensureMarkdownReady();
    return window.DOMPurify.sanitize(window.marked.parse(raw), { USE_PROFILES: { html: true } });
}

function addMessage(role, text) {
    const isUser = role === 'user';
    const wrap = document.createElement('div');
    wrap.className = 'msg ' + (isUser ? 'msg-user' : 'msg-assistant');

    const contentWrapper = document.createElement('div');
    contentWrapper.className = 'msg-content-wrapper';

    // Avatar
    const avatar = document.createElement('div');
    avatar.className = 'avatar ' + (isUser ? 'avatar-user' : 'avatar-assistant');

    if (isUser) {
        // User Icon (Initial or SVG)
        avatar.innerHTML = `<svg viewBox="0 0 24 24" class="icon" style="width:20px;height:20px;fill:white"><path d="M12 12c2.21 0 4-1.79 4-4s-1.79-4-4-4-4 1.79-4 4 1.79 4 4 4zm0 2c-2.67 0-8 1.34-8 4v2h16v-2c0-2.66-5.33-4-8-4z"></path></svg>`;
    } else {
        // StarMind Icon
        avatar.innerHTML = `<svg viewBox="0 0 24 24" class="icon" style="width:20px;height:20px;fill:white"><path d="M12 2C6.48 2 2 6.48 2 12s4.48 10 10 10 10-4.48 10-10S17.52 2 12 2zm-1 17.93c-3.95-.49-7-3.85-7-7.93 0-.62.08-1.21.21-1.79L9 15v1c0 1.1.9 2 2 2v1.93zm6.9-2.54c-.26-.81-1-1.39-1.9-1.39h-1v-3c0-.55-.45-1-1-1H8v-2h2c.55 0 1-.45 1-1V7h2c1.1 0 2-.9 2-2v-.41c2.93 1.19 5 4.06 5 7.41 0 2.08-.8 3.97-2.1 5.39z"></path></svg>`;
    }

    const textEl = document.createElement('div');
    textEl.className = 'bubble-text md';
    textEl.innerHTML = renderMarkdownSafe(text);

    contentWrapper.appendChild(avatar);
    contentWrapper.appendChild(textEl);
    wrap.appendChild(contentWrapper);
    chatEl.appendChild(wrap);

    scrollToBottom();
}

function addLoadingMessage() {
    const wrap = document.createElement('div');
    wrap.className = 'msg msg-assistant';
    wrap.id = 'loading-msg'; // Identify for removal

    const contentWrapper = document.createElement('div');
    contentWrapper.className = 'msg-content-wrapper';

    // Avatar (Assistant)
    const avatar = document.createElement('div');
    avatar.className = 'avatar avatar-assistant';
    avatar.innerHTML = `<svg viewBox="0 0 24 24" class="icon" style="width:20px;height:20px;fill:white"><path d="M12 2C6.48 2 2 6.48 2 12s4.48 10 10 10 10-4.48 10-10S17.52 2 12 2zm-1 17.93c-3.95-.49-7-3.85-7-7.93 0-.62.08-1.21.21-1.79L9 15v1c0 1.1.9 2 2 2v1.93zm6.9-2.54c-.26-.81-1-1.39-1.9-1.39h-1v-3c0-.55-.45-1-1-1H8v-2h2c.55 0 1-.45 1-1V7h2c1.1 0 2-.9 2-2v-.41c2.93 1.19 5 4.06 5 7.41 0 2.08-.8 3.97-2.1 5.39z"></path></svg>`;

    const textEl = document.createElement('div');
    textEl.className = 'bubble-text';
    textEl.innerHTML = `<div class="loading-dots"><span></span><span></span><span></span></div>`;

    contentWrapper.appendChild(avatar);
    contentWrapper.appendChild(textEl);
    wrap.appendChild(contentWrapper);
    chatEl.appendChild(wrap);

    scrollToBottom();
    return wrap;
}

async function requireLogin() {
    try {
        const res = await fetch('/api/me', { credentials: 'same-origin' });
        if (!res.ok) {
            window.location.href = '/login';
            return false;
        }
        return true;
    } catch (_) {
        setStatus('无法连接到服务器', true);
        return false;
    }
}

async function postJson(url, data) {
    const res = await fetch(url, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(data || {}),
        credentials: 'same-origin'
    });
    const text = await res.text();
    let json = null;
    try { json = JSON.parse(text); } catch (_) { }
    return { ok: res.ok, status: res.status, text, json };
}

function autoGrow() {
    inputEl.style.height = 'auto';
    inputEl.style.height = Math.min(inputEl.scrollHeight, 200) + 'px';
}

inputEl.addEventListener('input', autoGrow);
inputEl.addEventListener('keydown', (e) => {
    if (e.key === 'Enter' && !e.shiftKey) {
        e.preventDefault();
        sendEl.click();
    }
});

sendEl.addEventListener('click', async () => {
    setStatus('');
    const message = inputEl.value.trim();
    if (!message) return;

    inputEl.value = '';
    inputEl.style.height = 'auto'; // reset height
    addMessage('user', message);

    sendEl.disabled = true;
    inputEl.disabled = true;
    setStatus('StarMind 正在思考...');
    const loadingMsg = addLoadingMessage();

    try {
        const r = await postJson('/api/chat', { message });

        // Remove loading message
        if (loadingMsg && loadingMsg.parentNode) {
            loadingMsg.parentNode.removeChild(loadingMsg);
        }

        if (!r.ok) {
            addMessage('assistant', '错误：' + (r.text || r.status));
        } else {
            const content = r.json?.choices?.[0]?.message?.content;
            if (content) {
                addMessage('assistant', content);
            } else {
                addMessage('assistant', '（收到空回复）');
            }
        }
    } catch (err) {
        // Remove loading message
        if (loadingMsg && loadingMsg.parentNode) {
            loadingMsg.parentNode.removeChild(loadingMsg);
        }
        addMessage('assistant', '网络错误：' + err);
    } finally {
        setStatus('');
        sendEl.disabled = false;
        inputEl.disabled = false;
        inputEl.focus();
    }
});

// Sidebar Logic
function toggleSidebar() {
    sidebarEl.classList.toggle('collapsed');

    // Manage button visibilities based on state
    const isCollapsed = sidebarEl.classList.contains('collapsed');
    if (isCollapsed) {
        collapseSidebarBtn.style.display = 'none'; // hide close btn inside sidebar area
    } else {
        collapseSidebarBtn.style.display = 'inline-flex';
    }
}

if (toggleSidebarBtn) toggleSidebarBtn.addEventListener('click', toggleSidebar);
if (collapseSidebarBtn) collapseSidebarBtn.addEventListener('click', toggleSidebar);

// Mobile Sidebar
if (mobileMenuBtn) {
    mobileMenuBtn.addEventListener('click', () => {
        sidebarEl.classList.toggle('mobile-open');
    });
}
// Click outside to close on mobile
document.addEventListener('click', (e) => {
    if (window.innerWidth <= 768) {
        if (!sidebarEl.contains(e.target) && !mobileMenuBtn.contains(e.target) && sidebarEl.classList.contains('mobile-open')) {
            sidebarEl.classList.remove('mobile-open');
        }
    }
});


// New chat
const newChatBtn = document.getElementById('newChat');
const handleNewChat = async () => {
    if (window.confirm('确定要开始新对话吗？历史记录将被清除。')) {
        await postJson('/api/clear', {});
        chatEl.innerHTML = '';
        // Add Welcome
        const welcomeWrap = document.createElement('div');
        welcomeWrap.className = 'msg msg-assistant';
        welcomeWrap.innerHTML = `
            <div class="msg-content-wrapper">
                <div class="avatar avatar-assistant">
                    <svg viewBox="0 0 24 24" class="icon" style="width:20px;height:20px;fill:white"><path d="M12 2C6.48 2 2 6.48 2 12s4.48 10 10 10 10-4.48 10-10S17.52 2 12 2zm-1 17.93c-3.95-.49-7-3.85-7-7.93 0-.62.08-1.21.21-1.79L9 15v1c0 1.1.9 2 2 2v1.93zm6.9-2.54c-.26-.81-1-1.39-1.9-1.39h-1v-3c0-.55-.45-1-1-1H8v-2h2c.55 0 1-.45 1-1V7h2c1.1 0 2-.9 2-2v-.41c2.93 1.19 5 4.06 5 7.41 0 2.08-.8 3.97-2.1 5.39z"></path></svg>
                </div>
                <div class="bubble-text">已开始新对话。</div>
            </div>`;
        chatEl.appendChild(welcomeWrap);

        if (window.innerWidth <= 768) sidebarEl.classList.remove('mobile-open');
    }
};

if (newChatBtn) newChatBtn.addEventListener('click', handleNewChat);
if (mobileNewChatBtn) mobileNewChatBtn.addEventListener('click', handleNewChat);

const logoutBtn = document.getElementById('logout');
if (logoutBtn) {
    logoutBtn.addEventListener('click', async () => {
        await postJson('/api/logout', {});
        window.location.href = '/login';
    });
}

(async () => {
    await requireLogin();
    autoGrow();
})();
