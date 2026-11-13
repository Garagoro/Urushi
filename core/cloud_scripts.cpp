#include "cloud_scripts.h"
#include "logger.h"
#include "settings.h"
#include "updater.h"   // URUSHI_VERSION

#include <winhttp.h>
#include <bcrypt.h>
#include <atomic>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>
#include <functional>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")

namespace CloudScripts {
namespace {

constexpr char kOwnerRepo[] = "/Garagoro/Urushi/";
constexpr char kDefaultRef[] = "verified";
constexpr uint64_t kMaxCatalogBytes = 1024ull * 1024ull;
constexpr uint64_t kMaxFileBytes = 512ull * 1024ull;
constexpr size_t kMaxFilesPerScript = 32;
constexpr size_t kMaxItems = 500;

std::wstring g_exeDir;
std::mutex g_mtx;
std::vector<Item> g_items;
CatalogState g_state = CatalogState::Idle;
std::string g_error;
std::map<std::string, std::pair<std::string, std::string>> g_installState; // id -> (state, error)
std::atomic<bool> g_stop{ false };
std::atomic<bool> g_busy{ false };
std::thread g_worker;
FetchFn g_fetchOverride = nullptr;

// ------------------------------------------------------------------ minimal JSON
struct JVal {
    enum T { Null, Bool, Num, Str, Arr, Obj } t = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<JVal> a;
    std::vector<std::pair<std::string, JVal>> o;
    const JVal* get(const char* k) const {
        for (const auto& kv : o) if (kv.first == k) return &kv.second;
        return nullptr;
    }
};

struct JParser {
    const std::string& s;
    size_t i = 0;
    bool ok = true;
    explicit JParser(const std::string& str) : s(str) {}

    void ws() { while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i; }
    bool fail() { ok = false; return false; }

    static void putUtf8(std::string& out, unsigned cp) {
        if (cp < 0x80) out += static_cast<char>(cp);
        else if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
        else { out += static_cast<char>(0xE0 | (cp >> 12)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
    }

    bool str(std::string& out) {
        if (i >= s.size() || s[i] != '"') return fail();
        ++i;
        while (i < s.size()) {
            char c = s[i++];
            if (c == '"') return true;
            if (static_cast<unsigned char>(c) < 0x20) return fail();
            if (c != '\\') { out += c; continue; }
            if (i >= s.size()) return fail();
            char e = s[i++];
            switch (e) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                if (i + 4 > s.size()) return fail();
                unsigned cp = 0;
                for (int k = 0; k < 4; ++k) {
                    char h = s[i++];
                    cp <<= 4;
                    if (h >= '0' && h <= '9') cp |= h - '0';
                    else if (h >= 'a' && h <= 'f') cp |= h - 'a' + 10;
                    else if (h >= 'A' && h <= 'F') cp |= h - 'A' + 10;
                    else return fail();
                }
                putUtf8(out, cp); // surrogate pairs are not needed for catalog text
                break;
            }
            default: return fail();
            }
        }
        return fail();
    }

    bool val(JVal& v, int depth) {
        if (depth > 16) return fail();
        ws();
        if (i >= s.size()) return fail();
        char c = s[i];
        if (c == '{') {
            ++i; v.t = JVal::Obj; ws();
            if (i < s.size() && s[i] == '}') { ++i; return true; }
            for (;;) {
                ws();
                std::string k;
                if (!str(k)) return false;
                ws();
                if (i >= s.size() || s[i] != ':') return fail();
                ++i;
                JVal child;
                if (!val(child, depth + 1)) return false;
                v.o.emplace_back(std::move(k), std::move(child));
                ws();
                if (i < s.size() && s[i] == ',') { ++i; continue; }
                if (i < s.size() && s[i] == '}') { ++i; return true; }
                return fail();
            }
        }
        if (c == '[') {
            ++i; v.t = JVal::Arr; ws();
            if (i < s.size() && s[i] == ']') { ++i; return true; }
            for (;;) {
                JVal child;
                if (!val(child, depth + 1)) return false;
                v.a.push_back(std::move(child));
                ws();
                if (i < s.size() && s[i] == ',') { ++i; continue; }
                if (i < s.size() && s[i] == ']') { ++i; return true; }
                return fail();
            }
        }
        if (c == '"') { v.t = JVal::Str; return str(v.s); }
        if (s.compare(i, 4, "true") == 0) { v.t = JVal::Bool; v.b = true; i += 4; return true; }
        if (s.compare(i, 5, "false") == 0) { v.t = JVal::Bool; v.b = false; i += 5; return true; }
        if (s.compare(i, 4, "null") == 0) { v.t = JVal::Null; i += 4; return true; }
        char* end = nullptr;
        v.n = strtod(s.c_str() + i, &end);
        if (end == s.c_str() + i) return fail();
        v.t = JVal::Num;
        i = static_cast<size_t>(end - s.c_str());
        return true;
    }
};

std::string StrField(const JVal& o, const char* k) {
    const JVal* v = o.get(k);
    return (v && v->t == JVal::Str) ? v->s : std::string();
}

// ------------------------------------------------------------------ helpers
std::wstring Widen(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

std::wstring CloudRoot() { return g_exeDir + L"\\scripts\\cloud"; }
// Shader files of a cloud package live where CarFX is allowed to load from (shaders/cloud/<id>/ under the game folder)
std::wstring ShaderCloudRoot() { return g_exeDir + L"\\shaders\\cloud"; }
bool IsShaderFile(const std::string& path) {
    const size_t dot = path.rfind('.');
    if (dot == std::string::npos) return false;
    std::string ext = path.substr(dot);
    for (char& c : ext) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return ext == ".hlsl" || ext == ".hlsli";
}

bool AllowedExt(const std::string& path) {
    const size_t dot = path.rfind('.');
    if (dot == std::string::npos) return false;
    std::string ext = path.substr(dot);
    for (char& c : ext) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return ext == ".lua" || ext == ".json" || ext == ".txt" || ext == ".png" || ext == ".hlsl" || ext == ".hlsli";
}

bool DeleteTree(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            const std::wstring full = dir + L"\\" + fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {   // never follow links
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) RemoveDirectoryW(full.c_str());
                else DeleteFileW(full.c_str());
            } else if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                DeleteTree(full);
            } else {
                SetFileAttributesW(full.c_str(), FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(full.c_str());
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return RemoveDirectoryW(dir.c_str()) != 0 || GetFileAttributesW(dir.c_str()) == INVALID_FILE_ATTRIBUTES;
}

void EnsureDirs(const std::wstring& dir) {
    for (size_t i = 3; i < dir.size(); ++i) {
        if (dir[i] == L'\\') CreateDirectoryW(dir.substr(0, i).c_str(), nullptr);
    }
    CreateDirectoryW(dir.c_str(), nullptr);
}

struct WinHandle {
    HINTERNET h;
    explicit WinHandle(HINTERNET v) : h(v) {}
    ~WinHandle() { if (h) WinHttpCloseHandle(h); }
};

bool RealFetch(const std::string& repoRelPath, uint64_t maxBytes, std::vector<uint8_t>& out, DWORD& status) {
    out.clear();
    status = 0;
    std::string ref = Settings::GetString("cloud", "ref", kDefaultRef);
    if (ref.empty() || ref.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") != std::string::npos) {
        ref = kDefaultRef;
    }
    const std::wstring path = Widen(std::string(kOwnerRepo) + ref + "/" + repoRelPath);

    WinHandle session(WinHttpOpen(L"URUSHI-Cloud/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                  WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.h) return false;
    WinHttpSetTimeouts(session.h, 10000, 10000, 30000, 30000);
    WinHandle conn(WinHttpConnect(session.h, L"raw.githubusercontent.com", INTERNET_DEFAULT_HTTPS_PORT, 0));
    if (!conn.h) return false;
    WinHandle req(WinHttpOpenRequest(conn.h, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                     WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
    if (!req.h) return false;
    if (!WinHttpSendRequest(req.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req.h, nullptr)) {
        return false;
    }
    DWORD code = 0, len = sizeof(code);
    if (!WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &code, &len, WINHTTP_NO_HEADER_INDEX)) {
        return false;
    }
    status = code;
    if (code != 200) return false;

    std::vector<uint8_t> buf(32 * 1024);
    for (;;) {
        if (g_stop.load(std::memory_order_acquire)) { out.clear(); return false; }
        DWORD got = 0;
        if (!WinHttpReadData(req.h, buf.data(), (DWORD)buf.size(), &got)) { out.clear(); return false; }
        if (got == 0) break;
        if (out.size() + got > maxBytes) { out.clear(); return false; }
        out.insert(out.end(), buf.begin(), buf.begin() + got);
    }
    return true;
}

bool Fetch(const std::string& rel, uint64_t maxBytes, std::vector<uint8_t>& out, DWORD& status) {
    return (g_fetchOverride ? g_fetchOverride : RealFetch)(rel, maxBytes, out, status);
}

std::string HttpErr(DWORD status) {
    if (status == 0) return "no connection to GitHub";
    if (status == 404) return "not found (is the 'verified' branch published?)";
    return "HTTP " + std::to_string(status);
}

bool StartWorker(std::function<void()> fn) {
    if (g_busy.exchange(true)) return false;
    if (g_worker.joinable()) g_worker.join();
    g_worker = std::thread([fn]() {
        try { fn(); } catch (...) { Logger::Error("CloudScripts worker exception"); }
        g_busy.store(false, std::memory_order_release);
    });
    return true;
}

} // namespace

// ------------------------------------------------------------------ public helpers
std::string Sha256Hex(const uint8_t* data, size_t size) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::string hex;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return hex;
    if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) >= 0) {
        UCHAR digest[32]{};
        if ((size == 0 || BCryptHashData(hash, const_cast<PUCHAR>(data), (ULONG)size, 0) >= 0) &&
            BCryptFinishHash(hash, digest, sizeof(digest), 0) >= 0) {
            static const char k[] = "0123456789abcdef";
            for (UCHAR b : digest) { hex += k[b >> 4]; hex += k[b & 15]; }
        }
        BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return hex;
}

bool ValidId(const std::string& id) {
    if (id.empty() || id.size() > 40) return false;
    for (char c : id) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    }
    return true;
}

bool ValidRelPath(const std::string& path) {
    if (path.empty() || path.size() > 100 || !AllowedExt(path)) return false;
    size_t start = 0;
    while (start <= path.size()) {
        size_t slash = path.find('/', start);
        if (slash == std::string::npos) slash = path.size();
        const std::string part = path.substr(start, slash - start);
        if (part.empty() || part == "." || part == ".." || part[0] == '.') return false;
        for (char c : part) {
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) return false;
        }
        start = slash + 1;
    }
    return true;
}

bool VersionAtLeast(const std::string& have, const std::string& need) {
    auto parse = [](const std::string& s) {
        std::vector<int> v;
        int cur = 0; bool any = false;
        for (char c : s) {
            if (c >= '0' && c <= '9') { cur = cur * 10 + (c - '0'); any = true; }
            else if (c == '.') { v.push_back(cur); cur = 0; any = false; }
            else break;
        }
        if (any) v.push_back(cur);
        return v;
    };
    std::vector<int> a = parse(have), b = parse(need);
    const size_t n = a.size() > b.size() ? a.size() : b.size();
    a.resize(n, 0); b.resize(n, 0);
    for (size_t i = 0; i < n; ++i) {
        if (a[i] != b[i]) return a[i] > b[i];
    }
    return true;
}

bool ParseCatalog(const std::string& json, std::vector<Item>& out, std::string& err) {
    out.clear();
    JParser p(json);
    JVal root;
    if (!p.val(root, 0) || root.t != JVal::Obj) { err = "catalog.json is not valid JSON"; return false; }
    const JVal* scripts = root.get("scripts");
    if (!scripts || scripts->t != JVal::Arr) { err = "catalog.json has no 'scripts' array"; return false; }

    for (const JVal& e : scripts->a) {
        if (out.size() >= kMaxItems) break;
        if (e.t != JVal::Obj) continue;
        Item it;
        it.id = StrField(e, "id");
        if (!ValidId(it.id)) continue;                       // silently skip malformed entries
        it.name = StrField(e, "name");
        if (it.name.empty()) it.name = it.id;
        it.nameRu = StrField(e, "name_ru");
        it.author = StrField(e, "author");
        it.version = StrField(e, "version");
        it.description = StrField(e, "description");
        it.descriptionRu = StrField(e, "description_ru");
        it.minUrushi = StrField(e, "min_urushi");
        it.entry = StrField(e, "entry");

        const JVal* files = e.get("files");
        bool good = files && files->t == JVal::Arr && !files->a.empty() && files->a.size() <= kMaxFilesPerScript;
        bool hasEntry = false;
        if (good) {
            for (const JVal& f : files->a) {
                FileSpec fs;
                fs.path = StrField(f, "path");
                fs.sha256 = StrField(f, "sha256");
                for (char& c : fs.sha256) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
                if (!ValidRelPath(fs.path) || fs.sha256.size() != 64 ||
                    fs.sha256.find_first_not_of("0123456789abcdef") != std::string::npos) { good = false; break; }
                        it.files.push_back(std::move(fs));
            }
        }
        if (!good) continue;
        // entry: explicit, else init.lua, else "" for packages without any Lua file (shader-only packs)
        bool anyLua = false, hasInit = false;
        for (const FileSpec& f : it.files) {
            if (f.path.size() > 4 && f.path.compare(f.path.size() - 4, 4, ".lua") == 0) anyLua = true;
            if (f.path == "init.lua") hasInit = true;
        }
        if (it.entry.empty()) it.entry = hasInit ? "init.lua" : (anyLua ? "init.lua" : "");
        if (!it.entry.empty() && !ValidRelPath(it.entry)) continue;
        hasEntry = it.entry.empty() ? true : false;
        for (const FileSpec& f : it.files) if (!it.entry.empty() && f.path == it.entry) hasEntry = true;
        if (hasEntry) out.push_back(std::move(it));
    }
    return true;
}

void SetFetchForTests(FetchFn fn) { g_fetchOverride = fn; }

// ------------------------------------------------------------------ install / remove / list
bool InstallBlocking(const Item& item, std::string& err) {
    if (!ValidId(item.id)) { err = "invalid id"; return false; }
    if (!item.minUrushi.empty() && !VersionAtLeast(URUSHI_VERSION, item.minUrushi)) {
        err = "requires URUSHI " + item.minUrushi + " or newer";
        return false;
    }

    const std::wstring root = CloudRoot();
    EnsureDirs(root);
    const std::wstring stage = root + L"\\.stage_" + Widen(item.id);
    const std::wstring finalDir = root + L"\\" + Widen(item.id);
    const std::wstring shRoot = ShaderCloudRoot();
    const std::wstring shStage = shRoot + L"\\.stage_" + Widen(item.id);
    const std::wstring shFinal = shRoot + L"\\" + Widen(item.id);
    bool hasShaders = false;
    for (const FileSpec& f : item.files) if (IsShaderFile(f.path)) hasShaders = true;
    auto cleanupStage = [&]() { DeleteTree(stage); DeleteTree(shStage); };
    cleanupStage();
    EnsureDirs(stage);
    if (hasShaders) { EnsureDirs(shRoot); EnsureDirs(shStage); }

    for (const FileSpec& f : item.files) {
        if (g_stop.load()) { cleanupStage(); err = "cancelled"; return false; }
        std::vector<uint8_t> data;
        DWORD status = 0;
        if (!Fetch("community/" + item.id + "/" + f.path, kMaxFileBytes, data, status)) {
            cleanupStage();
            err = "download failed for " + f.path + ": " + HttpErr(status);
            return false;
        }
        if (Sha256Hex(data.data(), data.size()) != f.sha256) {
            cleanupStage();
            err = "checksum mismatch for " + f.path + " (file differs from the reviewed version)";
            return false;
        }
        std::wstring dest = (IsShaderFile(f.path) ? shStage : stage) + L"\\" + Widen(f.path);
        for (wchar_t& c : dest) if (c == L'/') c = L'\\';
        EnsureDirs(dest.substr(0, dest.rfind(L'\\')));
        std::ofstream out(dest, std::ios::binary | std::ios::trunc);
        if (!out) { cleanupStage(); err = "cannot write " + f.path; return false; }
        out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        out.close();
        if (!out) { cleanupStage(); err = "write error for " + f.path; return false; }
    }

    // Metadata last: its presence marks a complete install.
    {
        std::ofstream meta(stage + L"\\.install", std::ios::binary | std::ios::trunc);
        meta << "version=" << item.version << "\n"
             << "name=" << item.name << "\n"
             << "name_ru=" << item.nameRu << "\n"
             << "entry=" << item.entry << "\n";
        if (!meta) { cleanupStage(); err = "cannot write install metadata"; return false; }
    }

    DeleteTree(finalDir);
    DeleteTree(shFinal);
    if (hasShaders && !MoveFileW(shStage.c_str(), shFinal.c_str())) {
        cleanupStage();
        err = "cannot move shader files into place";
        return false;
    }
    if (!MoveFileW(stage.c_str(), finalDir.c_str())) {
        cleanupStage();
        DeleteTree(shFinal);
        err = "cannot move files into place";
        return false;
    }
    Logger::Info("Cloud script installed: " + item.id + " v" + item.version);
    return true;
}

static bool ReadInstallMeta(const std::wstring& dir, Installed& out) {
    std::ifstream in(dir + L"\\.install");
    if (!in) return false;
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "version") out.version = v;
        else if (k == "name") out.name = v;
        else if (k == "name_ru") out.nameRu = v;
        else if (k == "entry") out.entry = v;
    }
    return out.entry.empty() || ValidRelPath(out.entry);   // empty entry = shader-only package
}

std::vector<Installed> ListInstalled() {
    std::vector<Installed> res;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((CloudRoot() + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return res;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        if (fd.cFileName[0] == L'.') continue;
        Installed ins;
        ins.id = Logger::ToNarrow(fd.cFileName);
        if (!ValidId(ins.id)) continue;
        if (!ReadInstallMeta(CloudRoot() + L"\\" + fd.cFileName, ins)) continue;
        if (ins.name.empty()) ins.name = ins.id;
        res.push_back(std::move(ins));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return res;
}

bool ResolveEntry(const std::string& id, std::wstring& outFile, std::wstring& outDir) {
    if (!ValidId(id)) return false;
    const std::wstring dir = CloudRoot() + L"\\" + Widen(id);
    Installed ins;
    if (!ReadInstallMeta(dir, ins) || ins.entry.empty()) return false;
    std::wstring file = dir + L"\\" + Widen(ins.entry);
    for (wchar_t& c : file) if (c == L'/') c = L'\\';
    if (GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
    outFile = file;
    outDir = dir;
    return true;
}

bool Remove(const std::string& id, std::string& err) {
    if (!ValidId(id)) { err = "invalid id"; return false; }
    const std::wstring dir = CloudRoot() + L"\\" + Widen(id);
    if (GetFileAttributesW(dir.c_str()) == INVALID_FILE_ATTRIBUTES) { err = "not installed"; return false; }
    if (!DeleteTree(dir)) { err = "cannot delete (files in use?)"; return false; }
    DeleteTree(ShaderCloudRoot() + L"\\" + Widen(id));   // shader files, if any
    Settings::SetString("scripts", "cloud:" + id, "");   // drop the enabled flag
    Logger::Info("Cloud script removed: " + id);
    return true;
}

// ------------------------------------------------------------------ async API
void Init(const std::wstring& exeDir) {
    g_exeDir = exeDir;
    g_stop.store(false);
}

void Shutdown() {
    g_stop.store(true, std::memory_order_release);
    if (g_worker.joinable()) g_worker.join();
}

void RefreshAsync() {
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        if (g_state == CatalogState::Fetching) return;
        g_state = CatalogState::Fetching;
        g_error.clear();
    }
    if (!StartWorker([]() {
            std::vector<uint8_t> data;
            DWORD status = 0;
            std::vector<Item> items;
            std::string err;
            bool ok = Fetch("catalog.json", kMaxCatalogBytes, data, status);
            if (!ok) err = HttpErr(status);
            else ok = ParseCatalog(std::string(data.begin(), data.end()), items, err);
            std::lock_guard<std::mutex> lk(g_mtx);
            if (ok) { g_items = std::move(items); g_state = CatalogState::Ready; g_error.clear(); }
            else { g_state = CatalogState::Error; g_error = err; }
        })) {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_state = CatalogState::Idle; // another operation is running; the user can press again
    }
}

void InstallAsync(const std::string& id) {
    Item item;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        bool found = false;
        for (const Item& it : g_items) if (it.id == id) { item = it; found = true; break; }
        if (!found) { g_installState[id] = { "error", "not in the catalog (refresh the list)" }; return; }
        g_installState[id] = { "installing", "" };
    }
    if (!StartWorker([item]() {
            std::string err;
            const bool ok = InstallBlocking(item, err);
            std::lock_guard<std::mutex> lk(g_mtx);
            g_installState[item.id] = ok ? std::make_pair(std::string("ok"), std::string())
                                         : std::make_pair(std::string("error"), err);
            if (!ok) Logger::Warn("Cloud install of " + item.id + " failed: " + err);
        })) {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_installState[id] = { "error", "another operation is running, try again" };
    }
}

Snapshot GetSnapshot() {
    Snapshot s;
    std::vector<Installed> inst = ListInstalled();
    std::lock_guard<std::mutex> lk(g_mtx);
    s.state = g_state;
    s.error = g_error;
    s.ref = Settings::GetString("cloud", "ref", kDefaultRef);
    s.items = g_items;
    for (Item& it : s.items) {
        for (const Installed& i : inst) {
            if (i.id == it.id) {
                it.installed = true;
                it.installedVersion = i.version;
                it.updateAvailable = (i.version != it.version);
            }
        }
        auto st = g_installState.find(it.id);
        if (st != g_installState.end()) { it.state = st->second.first; it.error = st->second.second; }
    }
    return s;
}

// ------------------------------------------------------------------ Script Updates for local scripts
static std::map<std::string, ScriptUpdateItem> g_scriptUpdates;
static std::atomic<bool> g_scriptChecking{ false };

bool IsScriptIgnored(const std::string& scriptId) {
    return Settings::GetString("scripts_ignore_update", scriptId, "0") == "1";
}

void SetScriptIgnored(const std::string& scriptId, bool ignored) {
    Settings::SetString("scripts_ignore_update", scriptId, ignored ? "1" : "0");
    std::lock_guard<std::mutex> lk(g_mtx);
    if (ignored) {
        g_scriptUpdates[scriptId] = { scriptId, "ignored", "" };
    } else {
        g_scriptUpdates.erase(scriptId);
    }
}

void CheckScriptUpdatesAsync(const std::string& singleScript) {
    if (g_scriptChecking.exchange(true)) return;
    std::thread([exeDir = g_exeDir, singleScript]() {
        std::wstring scriptsDir = exeDir + L"\\scripts";
        std::vector<std::string> names;
        if (!singleScript.empty()) {
            names.push_back(singleScript);
        } else {
            WIN32_FIND_DATAW fd;
            HANDLE h = FindFirstFileW((scriptsDir + L"\\*.lua").c_str(), &fd);
            if (h != INVALID_HANDLE_VALUE) {
                do {
                    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                        std::string fn = Logger::ToNarrow(fd.cFileName);
                        if (fn.size() > 4 && fn.substr(fn.size() - 4) == ".lua") {
                            names.push_back(fn.substr(0, fn.size() - 4));
                        }
                    }
                } while (FindNextFileW(h, &fd));
                FindClose(h);
            }
        }

        {
            std::lock_guard<std::mutex> lk(g_mtx);
            for (const auto& name : names) {
                if (IsScriptIgnored(name)) {
                    g_scriptUpdates[name] = { name, "ignored", "" };
                } else {
                    g_scriptUpdates[name] = { name, "checking", "" };
                }
            }
        }

        for (const auto& name : names) {
            if (g_stop.load(std::memory_order_acquire)) break;
            if (IsScriptIgnored(name)) continue;

            std::string localContent;
            std::ifstream in(scriptsDir + L"\\" + Logger::ToWide(name) + L".lua", std::ios::binary);
            if (in.is_open()) {
                localContent.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            }

            std::vector<uint8_t> remoteBytes;
            DWORD status = 0;
            const std::string repoPath = "scripts/" + name + ".lua";
            std::string oldRef = Settings::GetString("cloud", "ref", kDefaultRef);
            Settings::SetString("cloud", "ref", "main");
            bool ok = Fetch(repoPath, kMaxFileBytes, remoteBytes, status);
            Settings::SetString("cloud", "ref", oldRef);

            std::lock_guard<std::mutex> lk(g_mtx);
            if (status == 404) {
                g_scriptUpdates[name] = { name, "custom", "" };
            } else if (ok && status == 200) {
                std::string remoteStr(remoteBytes.begin(), remoteBytes.end());
                auto normalize = [](std::string s) {
                    s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());
                    return s;
                };
                if (normalize(localContent) != normalize(remoteStr)) {
                    g_scriptUpdates[name] = { name, "update_available", "" };
                } else {
                    g_scriptUpdates[name] = { name, "up_to_date", "" };
                }
            } else {
                g_scriptUpdates[name] = { name, "error", HttpErr(status) };
            }
        }
        g_scriptChecking = false;
    }).detach();
}

void UpdateScriptAsync(const std::string& scriptId) {
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_scriptUpdates[scriptId] = { scriptId, "updating", "" };
    }
    std::thread([exeDir = g_exeDir, scriptId]() {
        std::vector<uint8_t> remoteBytes;
        DWORD status = 0;
        const std::string repoPath = "scripts/" + scriptId + ".lua";
        std::string oldRef = Settings::GetString("cloud", "ref", kDefaultRef);
        Settings::SetString("cloud", "ref", "main");
        bool ok = Fetch(repoPath, kMaxFileBytes, remoteBytes, status);
        Settings::SetString("cloud", "ref", oldRef);

        if (ok && status == 200) {
            std::wstring dest = exeDir + L"\\scripts\\" + Logger::ToWide(scriptId) + L".lua";
            std::ofstream out(dest, std::ios::binary);
            if (out.is_open()) {
                out.write(reinterpret_cast<const char*>(remoteBytes.data()), remoteBytes.size());
                out.close();
            }
            std::wstring devDest = exeDir + L"\\Urushi\\scripts\\" + Logger::ToWide(scriptId) + L".lua";
            if (GetFileAttributesW(devDest.c_str()) != INVALID_FILE_ATTRIBUTES) {
                std::ofstream devOut(devDest, std::ios::binary);
                if (devOut.is_open()) {
                    devOut.write(reinterpret_cast<const char*>(remoteBytes.data()), remoteBytes.size());
                    devOut.close();
                }
            }
            std::lock_guard<std::mutex> lk(g_mtx);
            g_scriptUpdates[scriptId] = { scriptId, "updated", "" };
            Logger::Info("Script '" + scriptId + "' successfully updated from GitHub.");
        } else {
            std::lock_guard<std::mutex> lk(g_mtx);
            g_scriptUpdates[scriptId] = { scriptId, "error", HttpErr(status) };
            Logger::Error("Failed to update script '" + scriptId + "': " + HttpErr(status));
        }
    }).detach();
}

ScriptUpdatesSnapshot GetScriptUpdatesSnapshot() {
    std::lock_guard<std::mutex> lk(g_mtx);
    ScriptUpdatesSnapshot snap;
    snap.checking = g_scriptChecking.load();
    for (const auto& kv : g_scriptUpdates) {
        snap.items.push_back(kv.second);
    }
    return snap;
}

} // namespace CloudScripts
