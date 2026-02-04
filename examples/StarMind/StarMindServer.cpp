#include "StarMindServer.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <curl/curl.h>

#include "spdlog/spdlog.h"

#include "tudou/http/HttpRequest.h"
#include "tudou/http/HttpResponse.h"
#include "tudou/http/HttpServer.h"
#include "tudou/router/Router.h"

namespace {

static const char* kCookieName = "starmind_token";

std::string trim(const std::string& s) {
    const size_t first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    const size_t last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

std::string to_lower(std::string s) {
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] >= 'A' && s[i] <= 'Z') {
            s[i] = static_cast<char>(s[i] - 'A' + 'a');
        }
    }
    return s;
}

std::string json_escape_minimal(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out.push_back(c); break;
        }
    }
    return out;
}

bool read_file_all(const std::string& realPath, std::string& outBody) {
    std::ifstream ifs(realPath, std::ios::binary);
    if (!ifs.is_open()) {
        return false;
    }
    std::ostringstream oss;
    oss << ifs.rdbuf();
    outBody = oss.str();
    return true;
}

std::string guess_content_type(const std::string& filepath) {
    const size_t pos = filepath.rfind('.');
    if (pos == std::string::npos) return "application/octet-stream";

    std::string ext = filepath.substr(pos + 1);
    ext = to_lower(ext);

    if (ext == "html" || ext == "htm") return "text/html; charset=utf-8";
    if (ext == "css") return "text/css; charset=utf-8";
    if (ext == "js") return "text/javascript; charset=utf-8";
    if (ext == "txt") return "text/plain; charset=utf-8";
    if (ext == "json") return "application/json; charset=utf-8";
    if (ext == "png") return "image/png";
    if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
    if (ext == "gif") return "image/gif";
    if (ext == "svg") return "image/svg+xml";
    if (ext == "ico") return "image/x-icon";
    if (ext == "webp") return "image/webp";
    return "application/octet-stream";
}

std::string get_header_or_empty(const HttpRequest& req, const std::string& key) {
    try {
        return req.get_header(key);
    }
    catch (...) {
        return std::string();
    }
}

void set_keep_alive(HttpResponse& resp, bool keepAlive) {
    resp.set_close_connection(!keepAlive);
    resp.add_header("Connection", keepAlive ? "Keep-Alive" : "close");
}

void respond_text(HttpResponse& resp, int status, const char* reason, const std::string& body, bool keepAlive, const char* contentType) {
    resp.set_http_version("HTTP/1.1");
    resp.set_status(status, reason);
    resp.set_body(body);
    resp.add_header("Content-Type", contentType);
    set_keep_alive(resp, keepAlive);
}

void respond_plain(HttpResponse& resp, int status, const char* reason, const std::string& body, bool keepAlive) {
    respond_text(resp, status, reason, body, keepAlive, "text/plain; charset=utf-8");
}

void respond_json(HttpResponse& resp, int status, const char* reason, const std::string& json, bool keepAlive) {
    respond_text(resp, status, reason, json, keepAlive, "application/json; charset=utf-8");
    resp.add_header("Cache-Control", "no-store");
}

// Very small JSON helper for bodies like {"user":"..","password":".."}
bool extract_json_string_field_from(const std::string& body, size_t fromPos, const std::string& key, std::string& outValue, size_t* outEndPos) {
    outValue.clear();
    const std::string pat = std::string("\"") + key + "\"";
    size_t pos = body.find(pat, fromPos);
    if (pos == std::string::npos) return false;

    pos = body.find(':', pos + pat.size());
    if (pos == std::string::npos) return false;

    while (pos < body.size() && (body[pos] == ':' || body[pos] == ' ' || body[pos] == '\t' || body[pos] == '\r' || body[pos] == '\n')) {
        ++pos;
    }
    if (pos >= body.size() || body[pos] != '"') return false;
    ++pos;

    std::string out;
    out.reserve(256);
    while (pos < body.size()) {
        const char c = body[pos++];
        if (c == '"') {
            outValue = out;
            if (outEndPos) *outEndPos = pos;
            return true;
        }
        if (c == '\\' && pos < body.size()) {
            const char esc = body[pos++];
            switch (esc) {
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            default: out.push_back(esc); break;
            }
            continue;
        }
        out.push_back(c);
    }
    return false;
}

bool extract_json_string_field(const std::string& body, const std::string& key, std::string& outValue) {
    return extract_json_string_field_from(body, 0, key, outValue, nullptr);
}

std::string get_cookie_value(const std::string& cookieHeader, const std::string& name) {
    // cookie format: a=b; c=d
    const std::string pat = name + "=";

    size_t pos = 0;
    while (pos < cookieHeader.size()) {
        // skip spaces and ';'
        while (pos < cookieHeader.size() && (cookieHeader[pos] == ' ' || cookieHeader[pos] == ';')) ++pos;
        if (pos >= cookieHeader.size()) break;

        if (cookieHeader.compare(pos, pat.size(), pat) == 0) {
            pos += pat.size();
            size_t end = cookieHeader.find(';', pos);
            if (end == std::string::npos) end = cookieHeader.size();
            return trim(cookieHeader.substr(pos, end - pos));
        }

        size_t next = cookieHeader.find(';', pos);
        if (next == std::string::npos) break;
        pos = next + 1;
    }

    return std::string();
}

std::string generate_hex_token32() {
    thread_local std::mt19937 gen(std::random_device{}());
    std::uniform_int_distribution<int> dis(0, 15);

    std::ostringstream oss;
    for (int i = 0; i < 32; ++i) {
        oss << std::hex << dis(gen);
    }
    return oss.str();
}

struct AuthConfig {
    bool enabled = true;
    std::string user;
    std::string password;
    int tokenTtlSeconds = 86400;

    AuthConfig() = default;
    AuthConfig(bool enabled_, std::string user_, std::string password_, int tokenTtlSeconds_)
        : enabled(enabled_),
        user(std::move(user_)),
        password(std::move(password_)),
        tokenTtlSeconds(tokenTtlSeconds_) {
    }
};

class AuthService {
public:
    explicit AuthService(AuthConfig cfg) : cfg_(std::move(cfg)) {}

    bool enabled() const { return cfg_.enabled; }

    bool check_credentials(const std::string& user, const std::string& password) const {
        return user == cfg_.user && password == cfg_.password;
    }

    std::string issue_token() {
        const std::string token = generate_hex_token32();
        const int64_t now = static_cast<int64_t>(::time(nullptr));
        const int64_t ttl = cfg_.tokenTtlSeconds > 0 ? cfg_.tokenTtlSeconds : 3600;

        std::lock_guard<std::mutex> lock(mu_);
        tokenExpiry_[token] = now + ttl;
        return token;
    }

    bool validate_token(const std::string& token) {
        if (token.empty()) return false;
        const int64_t now = static_cast<int64_t>(::time(nullptr));

        std::lock_guard<std::mutex> lock(mu_);
        cleanup_expired_locked(now);

        const auto it = tokenExpiry_.find(token);
        return it != tokenExpiry_.end() && it->second > now;
    }

    void invalidate_token(const std::string& token) {
        if (token.empty()) return;
        std::lock_guard<std::mutex> lock(mu_);
        tokenExpiry_.erase(token);
    }

    int ttl_seconds() const {
        return cfg_.tokenTtlSeconds > 0 ? cfg_.tokenTtlSeconds : 3600;
    }

private:
    void cleanup_expired_locked(int64_t now) {
        for (auto it = tokenExpiry_.begin(); it != tokenExpiry_.end();) {
            if (it->second <= now) it = tokenExpiry_.erase(it);
            else ++it;
        }
    }

private:
    AuthConfig cfg_;
    std::unordered_map<std::string, int64_t> tokenExpiry_;
    std::mutex mu_;
};

struct ChatMessage {
    std::string role;    // system | user | assistant
    std::string content;
};

struct Session {
    std::vector<ChatMessage> messages; // without system prompt
};

class SessionStore {
public:
    Session& get_or_create(const std::string& token) {
        std::lock_guard<std::mutex> lock(mu_);
        return sessions_[token];
    }

    void erase(const std::string& token) {
        std::lock_guard<std::mutex> lock(mu_);
        sessions_.erase(token);
    }

    void clear_history(const std::string& token) {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = sessions_.find(token);
        if (it != sessions_.end()) {
            it->second.messages.clear();
        }
    }

private:
    std::unordered_map<std::string, Session> sessions_;
    std::mutex mu_;
};

struct CurlGlobal {
    CurlGlobal() { curl_global_init(CURL_GLOBAL_DEFAULT); }
    ~CurlGlobal() { curl_global_cleanup(); }
};

size_t curl_write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    const size_t n = size * nmemb;
    std::string* out = static_cast<std::string*>(userdata);
    out->append(ptr, n);
    return n;
}

struct HttpResult {
    long httpCode = 0;
    std::string body;
    std::string error;
};

HttpResult http_post_json(const std::string& url,
    const std::string& jsonBody,
    const std::vector<std::string>& headers,
    int timeoutSeconds) {

    static CurlGlobal g;

    HttpResult r;
    CURL* curl = curl_easy_init();
    if (!curl) {
        r.error = "curl_easy_init failed";
        return r;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, jsonBody.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)jsonBody.size());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &r.body);

    if (timeoutSeconds > 0) {
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeoutSeconds);
    }

    struct curl_slist* slist = nullptr;
    for (const auto& h : headers) {
        slist = curl_slist_append(slist, h.c_str());
    }
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, slist);

    CURLcode code = curl_easy_perform(curl);
    if (code != CURLE_OK) {
        r.error = curl_easy_strerror(code);
    }
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &r.httpCode);

    if (slist) curl_slist_free_all(slist);
    curl_easy_cleanup(curl);
    return r;
}

std::string join_url(std::string base, const std::string& path) {
    if (base.empty()) return path;
    while (!base.empty() && base.back() == '/') base.pop_back();
    if (!path.empty() && path.front() == '/') return base + path;
    return base + "/" + path;
}

std::string build_openai_messages_json(const std::string& systemPrompt,
    const std::vector<ChatMessage>& history,
    const std::string& newUserMessage,
    int maxHistoryMessages) {

    std::vector<ChatMessage> msgs;
    msgs.reserve(history.size() + 2);

    // keep last N messages
    const int limit = (maxHistoryMessages > 0) ? maxHistoryMessages : 20;
    const int start = (static_cast<int>(history.size()) > limit) ? (static_cast<int>(history.size()) - limit) : 0;
    for (int i = start; i < static_cast<int>(history.size()); ++i) {
        msgs.push_back(history[static_cast<size_t>(i)]);
    }

    msgs.push_back(ChatMessage{ "user", newUserMessage });

    std::ostringstream oss;
    oss << "[";

    // system prompt first
    oss << "{\"role\":\"system\",\"content\":\"" << json_escape_minimal(systemPrompt) << "\"}";

    for (size_t i = 0; i < msgs.size(); ++i) {
        oss << ",{\"role\":\"" << json_escape_minimal(msgs[i].role) << "\",\"content\":\"" << json_escape_minimal(msgs[i].content) << "\"}";
    }

    oss << "]";
    return oss.str();
}

bool extract_assistant_content_openai_compat(const std::string& body, std::string& outContent) {
    // Try to find: choices[0].message.content
    // We do a pragmatic string scan to avoid pulling a heavy JSON library.
    size_t pos = 0;
    pos = body.find("\"choices\"");
    if (pos == std::string::npos) return false;

    pos = body.find("\"message\"", pos);
    if (pos == std::string::npos) return false;

    size_t endPos = 0;
    if (!extract_json_string_field_from(body, pos, "content", outContent, &endPos)) {
        return false;
    }
    return !outContent.empty();
}

bool is_safe_url_path(const std::string& urlPath) {
    if (urlPath.empty()) return true;
    if (urlPath.find("..") != std::string::npos) return false;
    if (urlPath.find('\\') != std::string::npos) return false;
    return true;
}

} // namespace

struct StarMindServer::StarMindState {
    explicit StarMindState(StarMindServerConfig cfg)
        : cfg(std::move(cfg)),
        auth(AuthConfig{ this->cfg.authEnabled, this->cfg.authUser, this->cfg.authPassword, this->cfg.authTokenTtlSeconds }),
        sessions() {
    }

    std::string current_token_from_cookie(const HttpRequest& req) const {
        const std::string cookie = get_header_or_empty(req, "Cookie");
        return get_cookie_value(cookie, kCookieName);
    }

    bool require_auth(const HttpRequest& req, HttpResponse& resp) {
        if (!auth.enabled()) {
            return true;
        }
        const std::string token = current_token_from_cookie(req);
        if (!auth.validate_token(token)) {
            respond_plain(resp, 401, "Unauthorized", "unauthorized", false);
            return false;
        }
        return true;
    }

    void handle_home(const HttpRequest& req, HttpResponse& resp) {
        const std::string token = current_token_from_cookie(req);
        const bool ok = (!auth.enabled()) || auth.validate_token(token);

        resp.set_http_version("HTTP/1.1");
        resp.set_status(302, "Found");
        resp.add_header("Location", ok ? "/chat" : "/login");
        resp.set_body("");
        set_keep_alive(resp, true);
    }

    void handle_me(const HttpRequest& req, HttpResponse& resp) {
        if (!require_auth(req, resp)) {
            return;
        }
        respond_json(resp, 200, "OK", "{\"ok\":true}", true);
    }

    void handle_page(const std::string& fileName, const HttpRequest& req, HttpResponse& resp, bool needAuth) {
        if (needAuth && !require_auth(req, resp)) {
            return;
        }

        std::string realPath = cfg.webRoot;
        if (!realPath.empty() && realPath.back() != '/') realPath.push_back('/');
        realPath += fileName;

        std::string body;
        if (!read_file_all(realPath, body)) {
            respond_plain(resp, 404, "Not Found", "Not Found", true);
            return;
        }

        resp.set_http_version("HTTP/1.1");
        resp.set_status(200, "OK");
        resp.set_body(body);
        resp.add_header("Content-Type", guess_content_type(realPath));
        set_keep_alive(resp, true);
    }

    void handle_static(const HttpRequest& req, HttpResponse& resp) {
        const std::string& method = req.get_method();
        if (method != "GET" && method != "HEAD") {
            respond_plain(resp, 405, "Method Not Allowed", "Method Not Allowed", false);
            resp.add_header("Allow", "GET, HEAD");
            return;
        }

        if (cfg.webRoot.empty()) {
            respond_plain(resp, 404, "Not Found", "Not Found", true);
            return;
        }

        std::string urlPath = req.get_path();
        if (urlPath.empty()) urlPath = "/";
        if (!is_safe_url_path(urlPath)) {
            respond_plain(resp, 404, "Not Found", "Not Found", true);
            return;
        }

        // map / -> /indexFile
        if (urlPath == "/") {
            urlPath = std::string("/") + (cfg.indexFile.empty() ? "login.html" : cfg.indexFile);
        }

        // directory -> append indexFile
        if (!urlPath.empty() && urlPath.back() == '/') {
            urlPath += (cfg.indexFile.empty() ? "login.html" : cfg.indexFile);
        }

        std::string realPath = cfg.webRoot;
        if (!realPath.empty() && realPath.back() != '/') realPath.push_back('/');
        if (!urlPath.empty() && urlPath.front() == '/') {
            realPath += urlPath.substr(1);
        }
        else {
            realPath += urlPath;
        }

        std::string body;
        if (!read_file_all(realPath, body)) {
            respond_plain(resp, 404, "Not Found", "Not Found", true);
            return;
        }
        if (method == "HEAD") body.clear();

        resp.set_http_version("HTTP/1.1");
        resp.set_status(200, "OK");
        resp.set_body(body);
        resp.add_header("Content-Type", guess_content_type(realPath));
        set_keep_alive(resp, true);
    }

    void handle_login(const HttpRequest& req, HttpResponse& resp) {
        if (!auth.enabled()) {
            respond_plain(resp, 404, "Not Found", "Not Found", false);
            return;
        }

        std::string user;
        std::string password;
        if (!extract_json_string_field(req.get_body(), "user", user) ||
            !extract_json_string_field(req.get_body(), "password", password)) {
            respond_plain(resp, 400, "Bad Request", "missing user/password", false);
            return;
        }

        if (!auth.check_credentials(user, password)) {
            respond_plain(resp, 401, "Unauthorized", "invalid credentials", false);
            return;
        }

        const std::string token = auth.issue_token();
        sessions.get_or_create(token); // create session

        const int ttl = auth.ttl_seconds();
        const std::string cookie = std::string(kCookieName) + "=" + token + "; Path=/; Max-Age=" + std::to_string(ttl) + "; HttpOnly; SameSite=Lax";

        resp.add_header("Set-Cookie", cookie);

        std::string json = std::string("{\"ok\":true,\"expiresIn\":") + std::to_string(ttl) + "}";
        respond_json(resp, 200, "OK", json, true);
    }

    void handle_logout(const HttpRequest& req, HttpResponse& resp) {
        const std::string token = current_token_from_cookie(req);
        auth.invalidate_token(token);
        sessions.erase(token);

        // Clear cookie
        resp.add_header("Set-Cookie", std::string(kCookieName) + "=; Path=/; Max-Age=0; HttpOnly; SameSite=Lax");
        respond_json(resp, 200, "OK", "{\"ok\":true}", true);
    }

    void handle_clear(const HttpRequest& req, HttpResponse& resp) {
        if (!require_auth(req, resp)) return;
        const std::string token = current_token_from_cookie(req);
        sessions.clear_history(token);
        respond_json(resp, 200, "OK", "{\"ok\":true}", true);
    }

    void handle_chat(const HttpRequest& req, HttpResponse& resp) {
        if (!require_auth(req, resp)) {
            return;
        }

        std::string userMessage;
        if (!extract_json_string_field(req.get_body(), "message", userMessage)) {
            respond_plain(resp, 400, "Bad Request", "missing message", false);
            return;
        }

        const std::string token = current_token_from_cookie(req);
        Session& s = sessions.get_or_create(token);

        // Call LLM
        if (cfg.llmProvider == "mock") {
            const std::string assistant = std::string("(mock) 你说：") + userMessage;
            s.messages.push_back(ChatMessage{ "user", userMessage });
            s.messages.push_back(ChatMessage{ "assistant", assistant });

            std::string json = std::string("{\"id\":\"mock\",\"object\":\"chat.completion\",\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\",\"content\":\"") +
                json_escape_minimal(assistant) + "\"},\"finish_reason\":\"stop\"}]}";

            respond_json(resp, 200, "OK", json, true);
            return;
        }

        if (cfg.llmProvider != "openai_compat") {
            respond_plain(resp, 500, "Internal Server Error", "unsupported llm.provider", false);
            return;
        }

        std::string apiKey = cfg.llmApiKey;
        const char* envKey = ::getenv("STARMIND_API_KEY");
        if (envKey && *envKey) {
            apiKey = envKey;
        }
        if (apiKey.empty() || apiKey == "YOUR_API_KEY") {
            respond_plain(resp, 500, "Internal Server Error", "llm.api_key is empty (or set STARMIND_API_KEY)", false);
            return;
        }

        const std::string endpoint = join_url(cfg.llmApiBase, "/chat/completions");
        const std::string messagesJson = build_openai_messages_json(cfg.llmSystemPrompt, s.messages, userMessage, cfg.llmMaxHistoryMessages);

        std::ostringstream reqJson;
        reqJson << "{";
        reqJson << "\"model\":\"" << json_escape_minimal(cfg.llmModel) << "\",";
        reqJson << "\"stream\":false,";
        reqJson << "\"messages\":" << messagesJson;
        reqJson << "}";

        std::vector<std::string> headers;
        headers.push_back("Content-Type: application/json");
        headers.push_back(std::string("Authorization: Bearer ") + apiKey);

        HttpResult r = http_post_json(endpoint, reqJson.str(), headers, cfg.llmTimeoutSeconds);
        if (!r.error.empty()) {
            spdlog::warn("LLM call failed: {}", r.error);
            respond_plain(resp, 502, "Bad Gateway", std::string("llm request failed: ") + r.error, false);
            return;
        }

        if (r.httpCode < 200 || r.httpCode >= 300) {
            spdlog::warn("LLM httpCode={} body={}", r.httpCode, r.body);
            respond_text(resp, 502, "Bad Gateway", r.body, false, "application/json; charset=utf-8");
            return;
        }

        // Update session history (best-effort)
        s.messages.push_back(ChatMessage{ "user", userMessage });
        std::string assistant;
        if (extract_assistant_content_openai_compat(r.body, assistant)) {
            s.messages.push_back(ChatMessage{ "assistant", assistant });
        }

        respond_text(resp, 200, "OK", r.body, true, "application/json; charset=utf-8");
        resp.add_header("Cache-Control", "no-store");
    }

    StarMindServerConfig cfg;
    AuthService auth;
    SessionStore sessions;
};

StarMindServer::StarMindServer(StarMindServerConfig cfg)
    : cfg_(std::move(cfg)), state_(nullptr), httpServer_(nullptr), router_(nullptr) {
    init();
}

StarMindServer::~StarMindServer() {
}

void StarMindServer::start() {
    spdlog::info("StarMind listening on {}:{} webRoot={} threadNum={} llm.provider={}",
        cfg_.ip, cfg_.port, cfg_.webRoot, cfg_.threadNum, cfg_.llmProvider);
    httpServer_->start();
}

void StarMindServer::init() {
    httpServer_.reset(new HttpServer(cfg_.ip, cfg_.port, cfg_.threadNum));
    router_.reset(new Router());

    state_.reset(new StarMindState(cfg_));

    // Route registration
    router_->add_get_route("/", [this](const HttpRequest& req, HttpResponse& resp) { state_->handle_home(req, resp); });
    router_->add_get_route("/login", [this](const HttpRequest& req, HttpResponse& resp) { state_->handle_page("login.html", req, resp, false); });
    router_->add_get_route("/chat", [this](const HttpRequest& req, HttpResponse& resp) { state_->handle_page("chat.html", req, resp, true); });
    router_->add_get_route("/api/me", [this](const HttpRequest& req, HttpResponse& resp) { state_->handle_me(req, resp); });

    router_->add_post_route("/api/login", [this](const HttpRequest& req, HttpResponse& resp) { state_->handle_login(req, resp); });
    router_->add_post_route("/api/logout", [this](const HttpRequest& req, HttpResponse& resp) { state_->handle_logout(req, resp); });
    router_->add_post_route("/api/clear", [this](const HttpRequest& req, HttpResponse& resp) { state_->handle_clear(req, resp); });
    router_->add_post_route("/api/chat", [this](const HttpRequest& req, HttpResponse& resp) { state_->handle_chat(req, resp); });

    router_->add_prefix_route("/", [this](const HttpRequest& req, HttpResponse& resp) { state_->handle_static(req, resp); });

    httpServer_->set_http_callback([this](const HttpRequest& req, HttpResponse& resp) {
        on_http_request(req, resp);
        });
}

void StarMindServer::on_http_request(const HttpRequest& req, HttpResponse& resp) {
    (void)router_->dispatch(req, resp);
}
