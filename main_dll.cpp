#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <psapi.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <string>
#include <vector>
#include <utility>
#include <cstring>
#include "MinHook.h"

#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "advapi32.lib")

static const char* kCredit = "made by lol123love for educational purposes and bcz love loves celestial";

static const char* kHookLog = "C:\\Users\\Public\\celestial_bypass.log";
static const char* kApiLog = "C:\\Users\\Public\\celestial_api.log";
static const int   kCdpPort = 9222;
static const int   kApiPortStart = 46321;
static const int   kApiPortEnd = 46340;
static const DWORD kSocketTimeoutHttp = 3000;
static const DWORD kSocketTimeoutWs = 10000;
static const DWORD kCdpEvalBudgetMs = 15000;

static HANDLE  g_hStopEvt = nullptr;
static int     g_apiPort = 0;
static CRITICAL_SECTION g_cdpLock;
static volatile LONG g_dllAttached = 0;

static void LogTo(const char* path, const char* fmt, va_list ap) {
    char buf[2048];
    int n = vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    if (n <= 0) return;
    if (n >= (int)sizeof(buf) - 1) n = (int)sizeof(buf) - 2;
    HANDLE h = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    SetFilePointer(h, 0, NULL, FILE_END);
    DWORD w = 0;
    WriteFile(h, buf, (DWORD)n, &w, NULL);
    WriteFile(h, "\r\n", 2, &w, NULL);
    CloseHandle(h);
}
static void LogInit(const char* path) {
    HANDLE h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
}

static void SetSocketTimeout(SOCKET s, DWORD ms) {
    DWORD tv = ms;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char*)&tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (char*)&tv, sizeof(tv));
}

namespace mj {
    struct JVal {
        enum T { NUL, BOOL, NUM, STR, ARR, OBJ } t = NUL;
        bool b = false; double n = 0; std::string s;
        std::vector<JVal> arr;
        std::vector<std::pair<std::string, JVal>> obj;

        static JVal obj_() { JVal v; v.t = OBJ; return v; }
        static JVal arr_() { JVal v; v.t = ARR; return v; }
        static JVal str(const std::string& x) { JVal v; v.t = STR; v.s = x; return v; }
        static JVal num(double x) { JVal v; v.t = NUM; v.n = x; return v; }
        static JVal boo(bool x) { JVal v; v.t = BOOL; v.b = x; return v; }
        static JVal nul() { return JVal{}; }

        void set(const std::string& k, JVal v) {
            if (t != OBJ) { t = OBJ; obj.clear(); }
            for (auto& kv : obj) if (kv.first == k) { kv.second = std::move(v); return; }
            obj.emplace_back(k, std::move(v));
        }
        void push(JVal v) { if (t != ARR) { t = ARR; arr.clear(); } arr.push_back(std::move(v)); }
        const JVal* get(const std::string& k) const {
            if (t != OBJ) return nullptr;
            for (auto& kv : obj) if (kv.first == k) return &kv.second;
            return nullptr;
        }
        std::string asStr(const std::string& d = "") const { return t == STR ? s : d; }
        int64_t     asInt(int64_t d = 0) const { return t == NUM ? (int64_t)n : d; }
        bool        asBool(bool d = false) const { return t == BOOL ? b : d; }

        std::string dump() const { std::string o; ser(o); return o; }
        void ser(std::string& o) const {
            switch (t) {
            case NUL:  o += "null"; break;
            case BOOL: o += b ? "true" : "false"; break;
            case NUM: {
                char bb[40];
                if (n == (double)(int64_t)n) snprintf(bb, sizeof(bb), "%lld", (long long)n);
                else snprintf(bb, sizeof(bb), "%g", n);
                o += bb; break;
            }
            case STR: esc(o, s); break;
            case ARR: { o += '['; for (size_t i = 0; i < arr.size(); ++i) { if (i)o += ','; arr[i].ser(o); } o += ']'; break; }
            case OBJ: { o += '{'; for (size_t i = 0; i < obj.size(); ++i) { if (i)o += ','; esc(o, obj[i].first); o += ':'; obj[i].second.ser(o); } o += '}'; break; }
            }
        }
        static void esc(std::string& o, const std::string& s) {
            o += '"';
            for (char c : s) {
                switch (c) {
                case '"': o += "\\\""; break; case '\\': o += "\\\\"; break;
                case '\n': o += "\\n"; break;  case '\r': o += "\\r"; break;
                case '\t': o += "\\t"; break;
                default: if ((unsigned char)c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
                       else o += c;
                }
            }
            o += '"';
        }
    };
    struct Parser {
        const char* p; const char* e; bool err = false;
        int depth = 0;
        void ws() { while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p; }
        bool eat(char c) { ws(); if (p < e && *p == c) { ++p; return true; } return false; }
        JVal parse() { return val(); }
        JVal val() {
            if (++depth > 200) { err = true; return {}; }
            ws(); if (p >= e) { err = true; depth--; return {}; }
            JVal res;
            if (*p == '{') res = ob();
            else if (*p == '[') res = ar();
            else if (*p == '"') res = JVal::str(str());
            else if (e - p >= 4 && !strncmp(p, "true", 4)) { p += 4; res = JVal::boo(true); }
            else if (e - p >= 5 && !strncmp(p, "false", 5)) { p += 5; res = JVal::boo(false); }
            else if (e - p >= 4 && !strncmp(p, "null", 4)) { p += 4; res = JVal::nul(); }
            else res = num();
            depth--;
            return res;
        }
        JVal ob() {
            JVal v = JVal::obj_(); ++p; ws();
            if (p < e && *p == '}') { ++p; return v; }
            while (p < e) {
                ws(); if (*p != '"') { err = true; return v; }
                std::string k = str(); ws();
                if (!eat(':')) { err = true; return v; }
                v.set(k, val()); ws();
                if (p < e && *p == ',') { ++p; continue; }
                if (p < e && *p == '}') { ++p; return v; }
                err = true; return v;
            }
            err = true; return v;
        }
        JVal ar() {
            JVal v = JVal::arr_(); ++p; ws();
            if (p < e && *p == ']') { ++p; return v; }
            while (p < e) {
                v.push(val()); ws();
                if (p < e && *p == ',') { ++p; continue; }
                if (p < e && *p == ']') { ++p; return v; }
                err = true; return v;
            }
            err = true; return v;
        }
        std::string str() {
            std::string o; if (p >= e || *p != '"') { err = true; return o; } ++p;
            while (p < e && *p != '"') {
                char c = *p++;
                if (c == '\\' && p < e) {
                    char x = *p++;
                    switch (x) {
                    case '"': o += '"'; break; case '\\': o += '\\'; break; case '/': o += '/'; break;
                    case 'n': o += '\n'; break; case 'r': o += '\r'; break; case 't': o += '\t'; break;
                    case 'b': o += '\b'; break; case 'f': o += '\f'; break;
                    case 'u': {
                        if (p + 4 > e) { err = true; return o; } unsigned c2 = 0;
                        for (int i = 0; i < 4; ++i) { char h = *p++; c2 = c2 * 16 + (h >= 'a' ? h - 'a' + 10 : h >= 'A' ? h - 'A' + 10 : h - '0'); }
                        if (c2 < 0x80) o += (char)c2;
                        else if (c2 < 0x800) { o += (char)(0xC0 | (c2 >> 6)); o += (char)(0x80 | (c2 & 0x3F)); }
                        else { o += (char)(0xE0 | (c2 >> 12)); o += (char)(0x80 | ((c2 >> 6) & 0x3F)); o += (char)(0x80 | (c2 & 0x3F)); }
                        break;
                    }
                    default: err = true; return o;
                    }
                }
                else o += c;
            }
            if (p >= e || *p != '"') { err = true; return o; }
            ++p; return o;
        }
        JVal num() {
            const char* s = p;
            if (p < e && *p == '-') ++p;
            while (p < e && ((*p >= '0' && *p <= '9') || *p == '.' || *p == 'e' || *p == 'E' || *p == '+' || *p == '-')) ++p;
            if (p == s) { err = true; return {}; }
            JVal v; v.t = JVal::NUM; v.n = atof(std::string(s, p - s).c_str()); return v;
        }
    };
    static JVal parse(const std::string& s) { Parser pp{ s.data(), s.data() + s.size() }; return pp.parse(); }
}

static std::string B64Enc(const uint8_t* d, size_t n) {
    static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o; o.reserve(((n + 2) / 3) * 4);
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = d[i] << 16;
        if (i + 1 < n) v |= d[i + 1] << 8;
        if (i + 2 < n) v |= d[i + 2];
        o += T[(v >> 18) & 63]; o += T[(v >> 12) & 63];
        o += (i + 1 < n) ? T[(v >> 6) & 63] : '=';
        o += (i + 2 < n) ? T[v & 63] : '=';
    }
    return o;
}

struct WsClient {
    SOCKET s = INVALID_SOCKET;
    bool open = false;

    bool connect(const std::string& wsUrl) {
        std::string u = wsUrl;
        if (u.size() < 5 || u.compare(0, 5, "ws://") != 0) return false;
        u = u.substr(5);
        size_t slash = u.find('/');
        std::string hostport = (slash == std::string::npos) ? u : u.substr(0, slash);
        std::string path = (slash == std::string::npos) ? "/" : u.substr(slash);
        std::string host = hostport; int port = kCdpPort;
        size_t colon = hostport.find(':');
        if (colon != std::string::npos) { host = hostport.substr(0, colon); port = atoi(hostport.c_str() + colon + 1); }

        addrinfo hints{}, * res = nullptr;
        hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
        char portStr[16]; snprintf(portStr, sizeof(portStr), "%d", port);
        if (getaddrinfo(host.c_str(), portStr, &hints, &res) != 0) return false;
        s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (s == INVALID_SOCKET) { freeaddrinfo(res); return false; }
        if (::connect(s, res->ai_addr, (int)res->ai_addrlen) != 0) {
            closesocket(s); s = INVALID_SOCKET; freeaddrinfo(res); return false;
        }
        freeaddrinfo(res);

        SetSocketTimeout(s, kSocketTimeoutWs);

        uint8_t keyRaw[16];
        for (int i = 0; i < 16; ++i) keyRaw[i] = (uint8_t)(rand() & 0xFF);
        std::string key = B64Enc(keyRaw, 16);
        std::string req =
            "GET " + path + " HTTP/1.1\r\nHost: " + host + ":" + portStr + "\r\n"
            "Upgrade: websocket\r\nConnection: Upgrade\r\n"
            "Sec-WebSocket-Key: " + key + "\r\nSec-WebSocket-Version: 13\r\n\r\n";
        send(s, req.c_str(), (int)req.size(), 0);

        std::string hdr; char b[1];
        while (hdr.find("\r\n\r\n") == std::string::npos) {
            int r = recv(s, b, 1, 0);
            if (r <= 0) { close(); return false; }
            hdr += b[0];
            if (hdr.size() > 8192) { close(); return false; }
        }
        if (hdr.find(" 101 ") == std::string::npos &&
            hdr.find(" 101\r") == std::string::npos) {
            close(); return false;
        }
        open = true;
        return true;
    }

    bool sendText(const std::string& t) {
        if (!open) return false;
        std::string frame;
        frame.push_back((char)0x81);
        size_t len = t.size();
        if (len < 126) frame.push_back((char)(0x80 | (uint8_t)len));
        else if (len < 65536) { frame.push_back((char)(0x80 | 126)); frame.push_back((char)((len >> 8) & 0xFF)); frame.push_back((char)(len & 0xFF)); }
        else { frame.push_back((char)(0x80 | 127)); for (int i = 7; i >= 0; --i) frame.push_back((char)((len >> (i * 8)) & 0xFF)); }
        uint8_t mk[4] = { (uint8_t)(rand() & 0xFF), (uint8_t)(rand() & 0xFF), (uint8_t)(rand() & 0xFF), (uint8_t)(rand() & 0xFF) };
        frame.append((char*)mk, 4);
        for (size_t i = 0; i < len; ++i) frame.push_back((char)(t[i] ^ mk[i % 4]));
        return send(s, frame.data(), (int)frame.size(), 0) == (int)frame.size();
    }

    bool recvExact(void* buf, size_t n) {
        size_t got = 0;
        while (got < n) {
            int r = recv(s, (char*)buf + got, (int)(n - got), 0);
            if (r <= 0) return false;
            got += (size_t)r;
        }
        return true;
    }

    std::string recvText() {
        std::string message;
        while (open) {
            uint8_t hdr[2];
            if (!recvExact(hdr, 2)) { open = false; return {}; }

            bool fin = (hdr[0] & 0x80) != 0;
            uint8_t opcode = hdr[0] & 0x0F;
            bool masked = (hdr[1] & 0x80) != 0;
            uint64_t len = hdr[1] & 0x7F;

            if (len == 126) {
                uint8_t b[2]; if (!recvExact(b, 2)) { open = false; return {}; }
                len = ((uint64_t)b[0] << 8) | b[1];
            }
            else if (len == 127) {
                uint8_t b[8]; if (!recvExact(b, 8)) { open = false; return {}; }
                len = 0; for (int i = 0; i < 8; ++i) len = (len << 8) | b[i];
            }

            if (len > 64 * 1024 * 1024) { open = false; return {}; }

            uint8_t mk[4] = {};
            if (masked && !recvExact(mk, 4)) { open = false; return {}; }

            std::string payload;
            if (len) {
                payload.resize((size_t)len);
                if (!recvExact(&payload[0], (size_t)len)) { open = false; return {}; }
                if (masked) for (size_t i = 0; i < len; ++i) payload[i] = (char)(payload[i] ^ mk[i % 4]);
            }

            if (opcode == 0x8) { open = false; return {}; }
            if (opcode == 0x9 || opcode == 0xA) continue;

            message += payload;
            if (fin) return message;
        }
        return {};
    }

    void close() {
        if (s != INVALID_SOCKET) { closesocket(s); s = INVALID_SOCKET; }
        open = false;
    }
};

static std::string HttpGet(int port, const char* path) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) return {};
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons((u_short)port);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (::connect(s, (sockaddr*)&a, sizeof(a)) != 0) { closesocket(s); return {}; }

    SetSocketTimeout(s, kSocketTimeoutHttp);

    char req[256];
    snprintf(req, sizeof(req), "GET %s HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n", path);
    send(s, req, (int)strlen(req), 0);

    std::string resp; char buf[4096]; int n;
    int expectedLen = -1;
    size_t hdrEnd = std::string::npos;

    while ((n = recv(s, buf, sizeof(buf), 0)) > 0) {
        resp.append(buf, n);
        if (hdrEnd == std::string::npos) {
            hdrEnd = resp.find("\r\n\r\n");
            if (hdrEnd != std::string::npos) {
                std::string h = resp.substr(0, hdrEnd);
                for (char& c : h) c = (char)tolower((unsigned char)c);
                size_t p = h.find("content-length:");
                if (p != std::string::npos) expectedLen = atoi(h.c_str() + p + 15);
            }
        }
        if (hdrEnd != std::string::npos && expectedLen >= 0) {
            if (resp.size() >= hdrEnd + 4 + expectedLen) break;
        }
    }
    closesocket(s);
    if (hdrEnd == std::string::npos) return {};
    return resp.substr(hdrEnd + 4);
}

static bool CdpAvailable() { return !HttpGet(kCdpPort, "/json/version").empty(); }

static std::string CdpFindWsUrl() {
    std::string body = HttpGet(kCdpPort, "/json");
    if (body.empty()) return {};
    mj::JVal arr = mj::parse(body);
    if (arr.t != mj::JVal::ARR) return {};
    for (auto& t : arr.arr) {
        if (t.get("type") && t.get("type")->asStr() == "page") {
            auto* url = t.get("url"); auto* wsu = t.get("webSocketDebuggerUrl");
            if (url && wsu && url->asStr().find("tauri.localhost") != std::string::npos) return wsu->asStr();
        }
    }
    for (auto& t : arr.arr) {
        auto* wsu = t.get("webSocketDebuggerUrl");
        if (wsu) return wsu->asStr();
    }
    return {};
}

static WsClient g_wsCache;
static int g_cdpId = 0;

static std::string CdpEvaluate(const std::string& jsExpr, int timeoutMs = (int)kCdpEvalBudgetMs) {
    if (!g_wsCache.open) {
        std::string wsUrl = CdpFindWsUrl();
        if (wsUrl.empty() || !g_wsCache.connect(wsUrl)) return {};
    }

    int myId = ++g_cdpId;
    mj::JVal req = mj::JVal::obj_();
    req.set("id", mj::JVal::num(myId));
    req.set("method", mj::JVal::str("Runtime.evaluate"));

    mj::JVal params = mj::JVal::obj_();
    params.set("expression", mj::JVal::str(jsExpr));
    params.set("awaitPromise", mj::JVal::boo(true));
    params.set("returnByValue", mj::JVal::boo(true));
    req.set("params", std::move(params));

    if (!g_wsCache.sendText(req.dump())) {
        g_wsCache.close();
        return {};
    }

    DWORD start = GetTickCount64(); 
    while (GetTickCount64() - start < (DWORD)timeoutMs) {
        std::string frame = g_wsCache.recvText();
        if (frame.empty()) { g_wsCache.close(); return {}; }

        mj::JVal rsp = mj::parse(frame);
        auto* id = rsp.get("id");
        if (!id || id->asInt() != myId) continue;

        auto* resultPtr = rsp.get("result");
        if (!resultPtr) return frame;
        auto* inner = resultPtr->get("result");
        if (!inner) return frame;
        auto* val = inner->get("value");
        return val ? val->dump() : inner->dump();
    }

    g_wsCache.close();
    return {};
}

static mj::JVal TauriInvoke(const std::string& command, const mj::JVal& args) {
    std::string js =
        "(async () => { try { const r = await window.__TAURI_INTERNALS__.invoke("
        + mj::JVal::str(command).dump() + ", " + args.dump() +
        "); return r; } catch (e) { return { ok: false, error: (e && e.message) ? e.message : String(e) }; } })()";

    BOOL entered = FALSE;
    for (int i = 0; i < 100 && !entered; ++i) {
        entered = TryEnterCriticalSection(&g_cdpLock);
        if (!entered) Sleep(20);
    }
    if (!entered) {
        mj::JVal err = mj::JVal::obj_();
        err.set("ok", mj::JVal::boo(false));
        err.set("error", mj::JVal::str("CDP busy (another eval in progress)"));
        return err;
    }

    std::string raw;
    try {
        raw = CdpEvaluate(js);
    }
    catch (...) { /* Catch-all to prevent DLL crash >: grr */ }

    LeaveCriticalSection(&g_cdpLock);

    if (raw.empty()) {
        mj::JVal err = mj::JVal::obj_();
        err.set("ok", mj::JVal::boo(false));
        err.set("error", mj::JVal::str("CDP unavailable or timed out"));
        return err;
    }
    return mj::parse(raw);
}

static bool VerifyPrologue(uintptr_t addr, const uint8_t* pat, size_t n) {
    __try { for (size_t i = 0; i < n; ++i) if (*(uint8_t*)(addr + i) != pat[i]) return false; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static const uint8_t PRO_BLOCK[16] = { 0x55,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x56,0x57,0x53,0x48,0x81,0xEC,0x48 };
static const uint8_t PRO_HASH[16] = { 0x55,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x56,0x57,0x53,0x48,0x81,0xEC,0x48 };
static const uint8_t PRO_HEUR[16] = { 0x55,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x56,0x57,0x53,0x48,0x81,0xEC,0xB8 };

typedef uint64_t* (__fastcall* tBlock)(uint64_t*, int64_t, int64_t);
static tBlock g_origBlock = nullptr;
static LONG g_cBlock = 0; static bool g_rBlock = false; static const char* g_mBlock = "(none)";
static uint64_t* __fastcall HookBlock(uint64_t* a1, int64_t, int64_t) {
    InterlockedIncrement(&g_cBlock);
    if (a1) { a1[0] = 0x8000000000000000ULL; a1[1] = 0; a1[2] = 0; }
    return a1;
}
typedef uint64_t(__fastcall* tHash)(uint64_t*, int64_t, int64_t, int64_t, int64_t);
static tHash g_origHash = nullptr;
static LONG g_cHash = 0; static bool g_rHash = false; static const char* g_mHash = "(none)";
static uint64_t __fastcall HookHash(uint64_t* a1, int64_t, int64_t, int64_t, int64_t) {
    InterlockedIncrement(&g_cHash);
    if (a1) { a1[0] = 0x8000000000000000ULL; a1[1] = 0; a1[2] = 0; }
    return 0x8000000000000000ULL;
}
typedef void* (__fastcall* tHeur)(void*, int64_t, int64_t, int64_t, int64_t, uint64_t, int64_t, int64_t);
static tHeur g_origHeur = nullptr;
static LONG g_cHeur = 0; static bool g_rHeur = false; static const char* g_mHeur = "(none)";
static void* __fastcall HookHeur(void* a1, int64_t, int64_t, int64_t, int64_t, uint64_t, int64_t, int64_t) {
    InterlockedIncrement(&g_cHeur);
    if (a1) { ((uint64_t*)a1)[0] = 0x8000000000000000ULL; ((uint8_t*)a1)[8] = 0; }
    return a1;
}

static BOOL CALLBACK HideOneWindowInternal(HWND hwnd, LPARAM lp) {
    DWORD pid = (DWORD)lp; DWORD wpid = 0;
    GetWindowThreadProcessId(hwnd, &wpid);
    if (wpid != pid) return TRUE;
    if (GetWindow(hwnd, GW_OWNER) != NULL) return TRUE;
    if (!IsWindowVisible(hwnd)) return TRUE;
    ShowWindow(hwnd, SW_HIDE);
    return TRUE;
}
static DWORD WINAPI HideThread(LPVOID) {
    DWORD pid = GetCurrentProcessId();
    while (WaitForSingleObject(g_hStopEvt, 100) == WAIT_TIMEOUT)
        EnumWindows(HideOneWindowInternal, (LPARAM)pid);
    return 0;
}

static bool WritePortFile(const char* path, int port) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%d\n", port);
    std::string tmp = std::string(path) + ".tmp";
    HANDLE h = CreateFileA(tmp.c_str(), GENERIC_WRITE, 0, NULL,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    BOOL ok = WriteFile(h, buf, (DWORD)strlen(buf), &w, NULL);
    CloseHandle(h);
    if (!ok) { DeleteFileA(tmp.c_str()); return false; }
    return MoveFileExA(tmp.c_str(), path,
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}
static void BroadcastPort(int port) {
    HKEY hk;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\LoveCelestial", 0, NULL, 0,
        KEY_WRITE, NULL, &hk, NULL) == ERROR_SUCCESS) {
        DWORD v = (DWORD)port;
        RegSetValueExA(hk, "ApiPort", 0, REG_DWORD, (BYTE*)&v, sizeof(v));
        RegCloseKey(hk);
    }
    char localApp[MAX_PATH];
    if (GetEnvironmentVariableA("LOCALAPPDATA", localApp, MAX_PATH) > 0) {
        std::string dir = std::string(localApp) + "\\LoveCelestial";
        CreateDirectoryA(dir.c_str(), NULL);
        std::string p = dir + "\\port.txt";
        WritePortFile(p.c_str(), port);
    }
    WritePortFile("C:\\Users\\Public\\LoveCelestial.port", port);
}
static void ClearPortBroadcast() {
    HKEY hk;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\LoveCelestial", 0,
        KEY_WRITE, &hk) == ERROR_SUCCESS) {
        RegDeleteValueA(hk, "ApiPort");
        RegCloseKey(hk);
    }
    char localApp[MAX_PATH];
    if (GetEnvironmentVariableA("LOCALAPPDATA", localApp, MAX_PATH) > 0) {
        std::string p = std::string(localApp) + "\\LoveCelestial\\port.txt";
        DeleteFileA(p.c_str());
    }
    DeleteFileA("C:\\Users\\Public\\LoveCelestial.port");
}

static mj::JVal CmdStatus() {
    mj::JVal r = mj::JVal::obj_();
    r.set("ok", mj::JVal::boo(true));
    r.set("pid", mj::JVal::num((double)GetCurrentProcessId()));
    mj::JVal hooks = mj::JVal::obj_();
    auto addHook = [&](const char* name, bool resolved, const char* method, LONG calls) {
        mj::JVal h = mj::JVal::obj_();
        h.set("resolved", mj::JVal::boo(resolved));
        h.set("method", mj::JVal::str(method));
        h.set("calls", mj::JVal::num((double)calls));
        hooks.set(name, std::move(h));
        };
    addHook("blocklist", g_rBlock, g_mBlock, g_cBlock);
    addHook("hash", g_rHash, g_mHash, g_cHash);
    addHook("heuristic", g_rHeur, g_mHeur, g_cHeur);
    r.set("hooks", std::move(hooks));

    mj::JVal cdp = mj::JVal::obj_();
    cdp.set("port", mj::JVal::num(kCdpPort));
    cdp.set("available", mj::JVal::boo(CdpAvailable()));
    r.set("cdp", std::move(cdp));

    mj::JVal api = mj::JVal::obj_();
    api.set("tcp_port", mj::JVal::num((double)g_apiPort));
    api.set("tcp_ok", mj::JVal::boo(g_apiPort != 0));
    r.set("api", std::move(api));
    return r;
}
static mj::JVal CmdList() {
    mj::JVal r = mj::JVal::obj_();
    mj::JVal resp = TauriInvoke("get_mod_library", mj::JVal::obj_());
    auto* ok = resp.get("ok");
    if (ok && ok->asBool()) {
        r.set("ok", mj::JVal::boo(true));
        auto* val = resp.get("value");
        r.set("library", val ? *val : resp);
    }
    else {
        r.set("ok", mj::JVal::boo(false));
        r.set("raw", resp);
    }
    return r;
}
static mj::JVal CmdImport(const std::string& path) {
    if (path.empty()) {
        mj::JVal r = mj::JVal::obj_();
        r.set("ok", mj::JVal::boo(false)); r.set("error", mj::JVal::str("missing 'path'"));
        return r;
    }
    DWORD attr = GetFileAttributesA(path.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) {
        mj::JVal r = mj::JVal::obj_();
        r.set("ok", mj::JVal::boo(false)); r.set("error", mj::JVal::str("not found: " + path));
        return r;
    }
    std::string lower = path; for (auto& c : lower) c = (char)tolower((unsigned char)c);
    std::string cmd;
    if (attr & FILE_ATTRIBUTE_DIRECTORY)                                      cmd = "import_raw_folder";
    else if (lower.size() > 7 && lower.substr(lower.size() - 7) == ".modpkg")   cmd = "import_modpkg";
    else if (lower.find(".wad") != std::string::npos)                         cmd = "import_wad_client";
    else                                                                      cmd = "import_fantome";

    mj::JVal args = mj::JVal::obj_();
    args.set("path", mj::JVal::str(path));
    args.set("filePath", mj::JVal::str(path));
    args.set("fantomePath", mj::JVal::str(path));
    args.set("modpkgPath", mj::JVal::str(path));
    args.set("wadPath", mj::JVal::str(path));
    args.set("folderPath", mj::JVal::str(path));

    mj::JVal resp = TauriInvoke(cmd, args);
    mj::JVal r = mj::JVal::obj_();
    r.set("ok", resp.get("ok") ? *resp.get("ok") : mj::JVal::boo(true));
    r.set("command", mj::JVal::str(cmd));
    r.set("response", resp);
    return r;
}
static mj::JVal CmdToggle(const std::string& target, bool enable) {
    if (target.empty()) {
        mj::JVal r = mj::JVal::obj_();
        r.set("ok", mj::JVal::boo(false)); r.set("error", mj::JVal::str("missing 'target'"));
        return r;
    }
    std::string modId = target;
    mj::JVal libResp = TauriInvoke("get_mod_library", mj::JVal::obj_());
    auto* libVal = libResp.get("value");
    if (libVal && libVal->t == mj::JVal::OBJ) {
        auto* mods = libVal->get("mods");
        if (mods && mods->t == mj::JVal::ARR) {
            for (auto& m : mods->arr) {
                auto* id = m.get("id"); auto* nm = m.get("name");
                if (!id) continue;
                if (id->asStr() == target) { modId = target; break; }
                if (nm) {
                    std::string a = nm->asStr(); std::string b = target;
                    for (auto& c : a) c = (char)tolower((unsigned char)c);
                    for (auto& c : b) c = (char)tolower((unsigned char)c);
                    if (a == b) { modId = id->asStr(); break; }
                }
            }
        }
    }
    mj::JVal args = mj::JVal::obj_();
    args.set("modId", mj::JVal::str(modId));
    args.set("enabled", mj::JVal::boo(enable));
    mj::JVal resp = TauriInvoke("toggle_mod", args);
    mj::JVal r = mj::JVal::obj_();
    r.set("ok", resp.get("ok") ? *resp.get("ok") : mj::JVal::boo(true));
    r.set("modId", mj::JVal::str(modId));
    r.set("response", resp);
    return r;
}
static mj::JVal CmdStartPatcher() {
    mj::JVal args = mj::JVal::obj_();
    args.set("config", mj::JVal::obj_());
    mj::JVal resp = TauriInvoke("start_patcher", args);
    mj::JVal r = mj::JVal::obj_();
    r.set("ok", resp.get("ok") ? *resp.get("ok") : mj::JVal::boo(true));
    r.set("response", resp); return r;
}
static mj::JVal CmdStopPatcher() {
    mj::JVal resp = TauriInvoke("stop_patcher", mj::JVal::obj_());
    mj::JVal r = mj::JVal::obj_();
    r.set("ok", resp.get("ok") ? *resp.get("ok") : mj::JVal::boo(true));
    r.set("response", resp); return r;
}
static mj::JVal CmdRawInvoke(const mj::JVal& req) {
    auto* cmd = req.get("command"); auto* args = req.get("args");
    if (!cmd) {
        mj::JVal r = mj::JVal::obj_();
        r.set("ok", mj::JVal::boo(false)); r.set("error", mj::JVal::str("missing 'command'"));
        return r;
    }
    mj::JVal resp = TauriInvoke(cmd->asStr(), args ? *args : mj::JVal::obj_());
    mj::JVal r = mj::JVal::obj_();
    r.set("ok", resp.get("ok") ? *resp.get("ok") : mj::JVal::boo(true));
    r.set("response", resp); return r;
}
static mj::JVal CmdHookControl(bool enable) {
    MH_STATUS st = enable ? MH_EnableHook(MH_ALL_HOOKS) : MH_DisableHook(MH_ALL_HOOKS);
    mj::JVal r = mj::JVal::obj_();
    r.set("ok", mj::JVal::boo(st == MH_OK));
    r.set("enabled", mj::JVal::boo(enable));
    r.set("status", mj::JVal::num((double)st)); return r;
}
static DWORD WINAPI KillThread(LPVOID) {
    Sleep(200);
    ExitProcess(0);
    return 0;
}
static mj::JVal CmdKill() {
    HANDLE th = CreateThread(NULL, 0, KillThread, NULL, 0, NULL);
    if (th) CloseHandle(th);
    mj::JVal r = mj::JVal::obj_();
    r.set("ok", mj::JVal::boo(true));
    r.set("message", mj::JVal::str("terminating Celestial.exe in 200 ms"));
    return r;
}

static std::string HandleCommand(const std::string& line) {
    mj::JVal req = mj::parse(line);
    if (req.t != mj::JVal::OBJ) {
        mj::JVal r = mj::JVal::obj_();
        r.set("ok", mj::JVal::boo(false)); r.set("error", mj::JVal::str("invalid JSON"));
        return r.dump();
    }
    auto* cmd = req.get("cmd"); std::string c = cmd ? cmd->asStr() : "";

    if (c == "hello") {
        mj::JVal r = mj::JVal::obj_();
        r.set("ok", mj::JVal::boo(true));
        r.set("service", mj::JVal::str("LoveCelestial"));
        r.set("version", mj::JVal::str("1.0"));
        r.set("pid", mj::JVal::num((double)GetCurrentProcessId()));
        r.set("port", mj::JVal::num((double)g_apiPort));
        r.set("credit", mj::JVal::str(kCredit));
        return r.dump();
    }
    if (c == "status")        return CmdStatus().dump();
    if (c == "ping") { mj::JVal r = mj::JVal::obj_(); r.set("ok", mj::JVal::boo(true)); r.set("pong", mj::JVal::num((double)GetTickCount64())); return r.dump(); }
    if (c == "list")          return CmdList().dump();
    if (c == "import")        return CmdImport(req.get("path") ? req.get("path")->asStr() : "").dump();
    if (c == "toggle")        return CmdToggle(req.get("target") ? req.get("target")->asStr() : "",
        req.get("enabled") ? req.get("enabled")->asBool() : true).dump();
    if (c == "start_patcher") return CmdStartPatcher().dump();
    if (c == "stop_patcher")  return CmdStopPatcher().dump();
    if (c == "raw_invoke")    return CmdRawInvoke(req).dump();
    if (c == "hook_enable")   return CmdHookControl(true).dump();
    if (c == "hook_disable")  return CmdHookControl(false).dump();
    if (c == "kill")          return CmdKill().dump();

    mj::JVal r = mj::JVal::obj_();
    r.set("ok", mj::JVal::boo(false)); r.set("error", mj::JVal::str("unknown: " + c));
    return r.dump();
}

static DWORD WINAPI TcpClientThread(LPVOID param) {
    SOCKET c = (SOCKET)(uintptr_t)param;
    std::string buf; char chunk[4096];

    while (true) {
        int r = recv(c, chunk, sizeof(chunk), 0);
        if (r <= 0) break;

        try {
            buf.append(chunk, r);
            if (buf.size() > 1024 * 1024) break;

            size_t nl;
            while ((nl = buf.find('\n')) != std::string::npos) {
                std::string line = buf.substr(0, nl);
                buf.erase(0, nl + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.empty()) continue;

                std::string resp = HandleCommand(line) + "\n";
                if (send(c, resp.data(), (int)resp.size(), 0) <= 0) goto done;
            }
        }
        catch (...) {
            
            break;
        }
    }
done:
    shutdown(c, SD_BOTH);
    closesocket(c);
    return 0;
}

static SOCKET TryBindPort(int port) {
    SOCKET ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls == INVALID_SOCKET) return INVALID_SOCKET;
    BOOL exclusive = TRUE;
    setsockopt(ls, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (char*)&exclusive, sizeof(exclusive));
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons((u_short)port);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (bind(ls, (sockaddr*)&a, sizeof(a)) != 0) { closesocket(ls); return INVALID_SOCKET; }
    if (listen(ls, SOMAXCONN) != 0) { closesocket(ls); return INVALID_SOCKET; }
    return ls;
}

static DWORD WINAPI TcpServerThread(LPVOID) {
    int rangeStart = kApiPortStart, rangeEnd = kApiPortEnd;
    char envPort[16] = {};
    if (GetEnvironmentVariableA("CELESTIAL_API_PORT", envPort, 16) > 0) {
        int p = atoi(envPort);
        if (p > 0 && p < 65536) { rangeStart = rangeEnd = p; }
    }
    SOCKET ls = INVALID_SOCKET;
    for (int p = rangeStart; p <= rangeEnd; ++p) {
        ls = TryBindPort(p);
        if (ls != INVALID_SOCKET) { g_apiPort = p; break; }
    }
    if (ls == INVALID_SOCKET) return 0;

    BroadcastPort(g_apiPort);

    while (WaitForSingleObject(g_hStopEvt, 0) != WAIT_OBJECT_0) {
        fd_set rs; FD_ZERO(&rs); FD_SET(ls, &rs);
        timeval tv{ 0, 200000 };
        int r = select(0, &rs, NULL, NULL, &tv);
        if (r <= 0) continue;
        SOCKET c = accept(ls, NULL, NULL);
        if (c == INVALID_SOCKET) continue;
        HANDLE th = CreateThread(NULL, 0, TcpClientThread, (LPVOID)(uintptr_t)c, 0, NULL);
        if (th) CloseHandle(th);
        else closesocket(c);
    }
    closesocket(ls);
    ClearPortBroadcast();
    g_apiPort = 0;
    return 0;
}

static void InstallHooks() {
    HMODULE target = GetModuleHandleA("Celestial.exe");
    if (!target) return;
    uintptr_t base = (uintptr_t)target;

    struct H { uintptr_t addr; bool ok; };
    H hb{ base + 0x4F11E0,  VerifyPrologue(base + 0x4F11E0, PRO_BLOCK, 16) };
    H hh{ base + 0x4E70F0,  VerifyPrologue(base + 0x4E70F0, PRO_HASH,  16) };
    H hr{ base + 0x161FD90, VerifyPrologue(base + 0x161FD90, PRO_HEUR, 16) };

    if (hb.ok && MH_CreateHook((LPVOID)hb.addr, (LPVOID)&HookBlock, (LPVOID*)&g_origBlock) == MH_OK) { g_rBlock = true; g_mBlock = "rva"; }
    if (hh.ok && MH_CreateHook((LPVOID)hh.addr, (LPVOID)&HookHash, (LPVOID*)&g_origHash) == MH_OK) { g_rHash = true; g_mHash = "rva"; }
    if (hr.ok && MH_CreateHook((LPVOID)hr.addr, (LPVOID)&HookHeur, (LPVOID*)&g_origHeur) == MH_OK) { g_rHeur = true; g_mHeur = "rva"; }

    MH_EnableHook(MH_ALL_HOOKS);
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        if (InterlockedCompareExchange(&g_dllAttached, 1, 0) != 0) return TRUE;
        DisableThreadLibraryCalls(hModule);
        LogInit(kHookLog); LogInit(kApiLog);

        InitializeCriticalSection(&g_cdpLock);

        wchar_t skip[8] = {};
        if (GetEnvironmentVariableW(L"CELESTIAL_NO_CDP", skip, 8) == 0) {
            if (GetEnvironmentVariableW(L"WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS", NULL, 0) == 0) {
                SetEnvironmentVariableW(L"WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS",
                    L"--remote-debugging-port=9222 --remote-allow-origins=*");
            }
        }

        WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);

        if (MH_Initialize() != MH_OK) return FALSE;
        InstallHooks();

        g_hStopEvt = CreateEventA(NULL, TRUE, FALSE, NULL);
        HANDLE t = CreateThread(NULL, 0, TcpServerThread, NULL, 0, NULL);
        if (t) CloseHandle(t);

        wchar_t noHide[8] = {};
        if (GetEnvironmentVariableW(L"CELESTIAL_NO_HIDE", noHide, 8) == 0) {
            HANDLE th = CreateThread(NULL, 0, HideThread, NULL, 0, NULL);
            if (th) CloseHandle(th);
        }
    }
    else if (reason == DLL_PROCESS_DETACH) {
        if (g_hStopEvt) SetEvent(g_hStopEvt);
        Sleep(400);
        g_wsCache.close();
        MH_DisableHook(MH_ALL_HOOKS);
        MH_Uninitialize();
        WSACleanup();
        if (g_hStopEvt) { CloseHandle(g_hStopEvt); g_hStopEvt = nullptr; }
        DeleteCriticalSection(&g_cdpLock);
    }
    return TRUE;
}
