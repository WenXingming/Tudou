const chatEl = document.getElementById('chat');
const inputEl = document.getElementById('input');
const sendEl = document.getElementById('send');
const statusEl = document.getElementById('status');
const sidebarEl = document.getElementById('sidebar');
const historyListEl = document.getElementById('historyList');

const mobileMenuBtn = document.getElementById('mobileMenuBtn');
const mobileNewChatBtn = document.getElementById('mobileNewChat');
const toggleSidebarBtn = document.getElementById('toggleSidebar');
const collapseSidebarBtn = document.getElementById('collapseSidebar');

let conversations = [];
let currentChatId = null;

function generateId() {
    return Date.now().toString(36) + Math.random().toString(36).substr(2);
}

function loadConversations() {
    try {
        const stored = localStorage.getItem('starmind_conversations');
        if (stored) conversations = JSON.parse(stored);
        if (!Array.isArray(conversations)) conversations = [];
    } catch (e) {
        conversations = [];
    }
    const storedId = localStorage.getItem('starmind_current_chat_id');
    if (storedId) currentChatId = storedId;
}

function saveConversations() {
    localStorage.setItem('starmind_conversations', JSON.stringify(conversations));
    if (currentChatId) localStorage.setItem('starmind_current_chat_id', currentChatId);
}

function getWelcomeHtml() {
    return `
        <div class="msg-content-wrapper">
            <div class="avatar avatar-assistant">
                <svg viewBox="0 0 24 24" class="icon" style="width:20px;height:20px;fill:white"><path d="M12 2C6.48 2 2 6.48 2 12s4.48 10 10 10 10-4.48 10-10S17.52 2 12 2zm-1 17.93c-3.95-.49-7-3.85-7-7.93 0-.62.08-1.21.21-1.79L9 15v1c0 1.1.9 2 2 2v1.93zm6.9-2.54c-.26-.81-1-1.39-1.9-1.39h-1v-3c0-.55-.45-1-1-1H8v-2h2c.55 0 1-.45 1-1V7h2c1.1 0 2-.9 2-2v-.41c2.93 1.19 5 4.06 5 7.41 0 2.08-.8 3.97-2.1 5.39z"></path></svg>
            </div>
            <div class="bubble-text">已开始新对话。</div>
        </div>`;
}

function renderHistory() {
    if (!historyListEl) return;
    historyListEl.innerHTML = '';
    conversations.forEach(c => {
        const div = document.createElement('div');
        div.className = 'history-item' + (c.id === currentChatId ? ' active' : '');
        div.innerHTML = `
            <svg viewBox="0 0 24 24" class="icon"><path d="M21 15a2 2 0 0 1-2 2H7l-4 4V5a2 2 0 0 1 2-2h14a2 2 0 0 1 2 2z"></path></svg>
            <div class="history-title">${c.title || '新对话'}</div>
            <div class="history-actions">
                <button class="history-action-btn edit-btn" title="重命名">
                    <svg viewBox="0 0 24 24" class="icon"><path d="M3 17.25V21h3.75L17.81 9.94l-3.75-3.75L3 17.25zM20.71 7.04c.39-.39.39-1.02 0-1.41l-2.34-2.34c-.39-.39-1.02-.39-1.41 0l-1.83 1.83 3.75 3.75 1.83-1.83z"></path></svg>
                </button>
                <button class="history-action-btn delete-btn" title="删除">
                    <svg viewBox="0 0 24 24" class="icon"><path d="M6 19c0 1.1.9 2 2 2h8c1.1 0 2-.9 2-2V7H6v12zM19 4h-3.5l-1-1h-5l-1 1H5v2h14V4z"></path></svg>
                </button>
            </div>
        `;
        div.onclick = (e) => {
            // If clicked inside actions, ignore main click
            if (e.target.closest('.history-actions')) return;
            switchChat(c.id);
        };

        // Bind actions
        const editBtn = div.querySelector('.edit-btn');
        const deleteBtn = div.querySelector('.delete-btn');

        editBtn.onclick = (e) => {
            e.stopPropagation();
            renameChat(c.id);
        };

        deleteBtn.onclick = (e) => {
            e.stopPropagation();
            deleteChat(c.id);
        };

        historyListEl.appendChild(div);
    });
}

function renameChat(id) {
    const chat = conversations.find(c => c.id === id);
    if (!chat) return;
    const newTitle = window.prompt("请输入新标题：", chat.title || "");
    if (newTitle !== null && newTitle.trim() !== "") {
        chat.title = newTitle.trim();
        saveConversations();
        renderHistory();
    }
}

async function deleteChat(id) {
    if (!window.confirm("确定要删除此对话吗？无法恢复。")) return;

    const index = conversations.findIndex(c => c.id === id);
    if (index === -1) return;

    conversations.splice(index, 1);

    // If we deleted the current chat
    if (currentChatId === id) {
        if (conversations.length > 0) {
            await switchChat(conversations[0].id);
        } else {
            // No chats left, create a new one
            await handleNewChat();
            return; // handleNewChat saves and renders
        }
    }

    saveConversations();
    renderHistory();
}

function setStatus(text, isError) {
    statusEl.textContent = text || '';
    if (isError) {
        statusEl.style.color = '#ef4444';
    } else {
        statusEl.style.color = '#8e8ea0';
    }
}

function scrollToBottom() {
    chatEl.scrollTo({ top: chatEl.scrollHeight, behavior: 'smooth' });
}

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

function addMessageToDOM(role, text) {
    const isUser = role === 'user';
    const wrap = document.createElement('div');
    wrap.className = 'msg ' + (isUser ? 'msg-user' : 'msg-assistant');

    const contentWrapper = document.createElement('div');
    contentWrapper.className = 'msg-content-wrapper';

    const avatar = document.createElement('div');
    avatar.className = 'avatar ' + (isUser ? 'avatar-user' : 'avatar-assistant');

    if (isUser) {
        avatar.innerHTML = `<svg viewBox="0 0 24 24" class="icon" style="width:20px;height:20px;fill:white"><path d="M12 12c2.21 0 4-1.79 4-4s-1.79-4-4-4-4 1.79-4 4 1.79 4 4 4zm0 2c-2.67 0-8 1.34-8 4v2h16v-2c0-2.66-5.33-4-8-4z"></path></svg>`;
    } else {
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

function addMessage(role, text, save = true) {
    addMessageToDOM(role, text);

    if (save && currentChatId) {
        const chat = conversations.find(c => c.id === currentChatId);
        if (chat) {
            chat.messages.push({ role, content: text });
            if (role === 'user' && chat.title === '新对话') {
                let title = text.replace(/[\n\r]/g, ' ').trim().substring(0, 20);
                if (text.length > 20) title += '...';
                if (title) chat.title = title;
            }
            saveConversations();
            renderHistory();
        }
    }
}

function addLoadingMessage() {
    const wrap = document.createElement('div');
    wrap.className = 'msg msg-assistant';
    wrap.id = 'loading-msg';

    const contentWrapper = document.createElement('div');
    contentWrapper.className = 'msg-content-wrapper';

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
    inputEl.style.height = 'auto';
    addMessage('user', message, true);

    sendEl.disabled = true;
    inputEl.disabled = true;
    setStatus('StarMind 正在思考...');
    const loadingMsg = addLoadingMessage();

    try {
        const r = await postJson('/api/chat', { message });

        if (loadingMsg && loadingMsg.parentNode) {
            loadingMsg.parentNode.removeChild(loadingMsg);
        }

        if (!r.ok) {
            addMessage('assistant', '错误：' + (r.text || r.status), true);
        } else {
            const content = r.json?.choices?.[0]?.message?.content;
            if (content) {
                addMessage('assistant', content, true);
            } else {
                addMessage('assistant', '（收到空回复）', true);
            }
        }
    } catch (err) {
        if (loadingMsg && loadingMsg.parentNode) {
            loadingMsg.parentNode.removeChild(loadingMsg);
        }
        addMessage('assistant', '网络错误：' + err, true);
    } finally {
        setStatus('');
        sendEl.disabled = false;
        inputEl.disabled = false;
        inputEl.focus();
    }
});

function toggleSidebar() {
    sidebarEl.classList.toggle('collapsed');
    const isCollapsed = sidebarEl.classList.contains('collapsed');
    if (isCollapsed) {
        collapseSidebarBtn.style.display = 'none';
    } else {
        collapseSidebarBtn.style.display = 'inline-flex';
    }
}

if (toggleSidebarBtn) toggleSidebarBtn.addEventListener('click', toggleSidebar);
if (collapseSidebarBtn) collapseSidebarBtn.addEventListener('click', toggleSidebar);

if (mobileMenuBtn) {
    mobileMenuBtn.addEventListener('click', () => {
        sidebarEl.classList.toggle('mobile-open');
    });
}
document.addEventListener('click', (e) => {
    if (window.innerWidth <= 768) {
        if (!sidebarEl.contains(e.target) && !mobileMenuBtn.contains(e.target) && sidebarEl.classList.contains('mobile-open')) {
            sidebarEl.classList.remove('mobile-open');
        }
    }
});

async function switchChat(id) {
    if (currentChatId === id) return;
    currentChatId = id;
    saveConversations();

    chatEl.innerHTML = '';

    await postJson('/api/clear', {});

    const chat = conversations.find(c => c.id === id);
    if (chat) {
        if (chat.messages.length === 0) {
            const welcomeWrap = document.createElement('div');
            welcomeWrap.className = 'msg msg-assistant';
            welcomeWrap.innerHTML = getWelcomeHtml();
            chatEl.appendChild(welcomeWrap);
        } else {
            chat.messages.forEach(msg => addMessageToDOM(msg.role, msg.content));
        }
    }

    renderHistory();
    if (window.innerWidth <= 768) sidebarEl.classList.remove('mobile-open');
}

async function handleNewChat() {
    await postJson('/api/clear', {});

    const newChat = {
        id: generateId(),
        title: '新对话',
        messages: [],
        timestamp: Date.now()
    };
    conversations.unshift(newChat);
    currentChatId = newChat.id;
    saveConversations();

    chatEl.innerHTML = '';
    const welcomeWrap = document.createElement('div');
    welcomeWrap.className = 'msg msg-assistant';
    welcomeWrap.innerHTML = getWelcomeHtml();
    chatEl.appendChild(welcomeWrap);

    renderHistory();
    if (window.innerWidth <= 768) sidebarEl.classList.remove('mobile-open');
}

const newChatBtn = document.getElementById('newChat');
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
    loadConversations();

    if (conversations.length === 0) {
        await handleNewChat();
    } else {
        if (!currentChatId || !conversations.find(c => c.id === currentChatId)) {
            currentChatId = conversations[0].id;
        }

        chatEl.innerHTML = '';
        const chat = conversations.find(c => c.id === currentChatId);
        if (chat && chat.messages.length > 0) {
            chat.messages.forEach(msg => addMessageToDOM(msg.role, msg.content));
            scrollToBottom();
        } else {
            const welcomeWrap = document.createElement('div');
            welcomeWrap.className = 'msg msg-assistant';
            welcomeWrap.innerHTML = getWelcomeHtml();
            chatEl.appendChild(welcomeWrap);
        }
    }

    renderHistory();
    autoGrow();
})();
