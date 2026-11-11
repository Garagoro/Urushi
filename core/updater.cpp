#include "updater.h"
#include "urushi_ui.h"
#include "logger.h"

#include <winhttp.h>
#include <bcrypt.h>
#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")

namespace Updater {
namespace {

constexpr wchar_t kRepoOwner[] = L"Garagoro";
constexpr wchar_t kRepoName[] = L"Urushi";
constexpr wchar_t kRepoBranch[] = L"main";
constexpr uint32_t kSteamAppId = 1826420;

constexpr DWORD kResolveMs = 10000;
constexpr DWORD kConnectMs = 10000;
constexpr DWORD kIoMs = 30000;
constexpr DWORD kHttpChunk = 64 * 1024;
constexpr size_t kMaxTreeBytes = 4u * 1024u * 1024u;
constexpr size_t kMaxEntries = 5000u;
constexpr uint64_t kMaxFileBytes = 64ull * 1024ull * 1024ull;
constexpr uint64_t kMaxTotalBytes = 256ull * 1024ull * 1024ull;

struct TreeEntry {
    std::string path;
    std::string sha;
    uint64_t size = 0;
};

struct PlannedFile {
    TreeEntry entry;
    std::wstring destPath;
    bool isProxy = false;
    bool isCoreDll = false;
};

struct WinHttpHandle {
    HINTERNET h = nullptr;
    explicit WinHttpHandle(HINTERNET v = nullptr) : h(v) {}
    ~WinHttpHandle() { if (h) WinHttpCloseHandle(h); }
    WinHttpHandle(const WinHttpHandle&) = delete;
    WinHttpHandle& operator=(const WinHttpHandle&) = delete;
};

std::mutex s_mtx;
std::thread s_worker;
std::atomic<bool> s_stop{ false };
std::atomic<bool> s_workerBusy{ false };

std::wstring s_exeDir;
HMODULE s_selfModule = nullptr;
uint32_t s_localLinkTime = 0;

State s_state = State::Idle;
std::string s_remoteVersion;
std::string s_releaseNotes;
std::string s_statusText;
std::string s_currentFile;
uint32_t s_remoteLinkTime = 0;
bool s_localBuildNewer = false;
bool s_proxyNeedsRestart = false;
bool s_bannerDismissed = false;
uint32_t s_filesTotal = 0;
uint32_t s_filesDone = 0;
uint64_t s_bytesTotal = 0;
uint64_t s_bytesDone = 0;
std::vector<PlannedFile> s_planned;

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
    return s;
}

uint32_t ReadPeTimestampFromMemory(const void* moduleBase) {
    if (!moduleBase) return 0;
    __try {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(moduleBase);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 0x1000) return 0;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
            reinterpret_cast<const uint8_t*>(moduleBase) + dos->e_lfanew);
        return (nt->Signature == IMAGE_NT_SIGNATURE) ? nt->FileHeader.TimeDateStamp : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

bool HttpGet(const wchar_t* host, const std::wstring& path, const std::wstring& extraHeaders,
             uint64_t maxBytes, std::vector<uint8_t>& out, DWORD& status) {
    out.clear();
    status = 0;
    if (s_stop.load(std::memory_order_acquire)) return false;

    WinHttpHandle session(WinHttpOpen(L"URUSHI-Updater/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.h) return false;
    WinHttpSetTimeouts(session.h, kResolveMs, kConnectMs, kIoMs, kIoMs);

    WinHttpHandle conn(WinHttpConnect(session.h, host, INTERNET_DEFAULT_HTTPS_PORT, 0));
    if (!conn.h) return false;
    WinHttpHandle req(WinHttpOpenRequest(conn.h, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
    if (!req.h) return false;

    const wchar_t* hdr = extraHeaders.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : extraHeaders.c_str();
    const DWORD hdrLen = extraHeaders.empty() ? 0 : static_cast<DWORD>(-1L);
    if (!WinHttpSendRequest(req.h, hdr, hdrLen, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
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

    std::vector<uint8_t> buf(kHttpChunk);
    for (;;) {
        if (s_stop.load(std::memory_order_acquire)) {
            out.clear();
            return false;
        }
        DWORD got = 0;
        if (!WinHttpReadData(req.h, buf.data(), kHttpChunk, &got)) {
            out.clear();
            return false;
        }
        if (got == 0) break;
        if (out.size() + got > maxBytes) {
            out.clear();
            return false;
        }
        out.insert(out.end(), buf.begin(), buf.begin() + got);
    }
    return true;
}

bool GitHubApiGet(const std::wstring& path, const wchar_t* accept,
                  uint64_t maxBytes, std::vector<uint8_t>& out, DWORD& status) {
    std::wstring headers = L"Accept: ";
    headers += accept;
    headers += L"\r\nX-GitHub-Api-Version: 2022-11-28\r\nUser-Agent: URUSHI-Updater\r\n";
    return HttpGet(L"api.github.com", path, headers, maxBytes, out, status);
}

// Git blob object hash: SHA-1("blob <size>\0" + bytes), 40 lowercase hex chars.
std::string ComputeGitBlobSha(const uint8_t* data, size_t size) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA1_ALGORITHM, nullptr, 0) < 0) return {};

    std::string hex;
    if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) >= 0) {
        char hdr[48] = {};
        const int n = snprintf(hdr, sizeof(hdr), "blob %llu", static_cast<unsigned long long>(size));
        UCHAR digest[20]{};
        if (n > 0 && BCryptHashData(hash, reinterpret_cast<PUCHAR>(hdr), static_cast<ULONG>(n + 1), 0) >= 0 &&
            (size == 0 || BCryptHashData(hash, const_cast<PUCHAR>(data), static_cast<ULONG>(size), 0) >= 0) &&
            BCryptFinishHash(hash, digest, sizeof(digest), 0) >= 0) {
            static const char kHex[] = "0123456789abcdef";
            hex.reserve(40);
            for (UCHAR b : digest) {
                hex += kHex[b >> 4];
                hex += kHex[b & 0x0F];
            }
        }
        BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return hex;
}

// ---------------- Minimal Strict JSON Parser for GitHub git/trees ----------------

struct JsonCursor {
    const char* p;
    const char* end;
    void Ws() { while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) ++p; }
    bool Eat(char c) { Ws(); if (p < end && *p == c) { ++p; return true; } return false; }
};

void AppendUtf8(std::string& s, unsigned cp) {
    if (cp < 0x80) s += static_cast<char>(cp);
    else if (cp < 0x800) { s += static_cast<char>(0xC0 | (cp >> 6)); s += static_cast<char>(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) {
        s += static_cast<char>(0xE0 | (cp >> 12));
        s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        s += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        s += static_cast<char>(0xF0 | (cp >> 18));
        s += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        s += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

bool ParseHex4(JsonCursor& c, unsigned& v) {
    if (c.end - c.p < 4) return false;
    v = 0;
    for (int i = 0; i < 4; ++i) {
        const char ch = *c.p++;
        v <<= 4;
        if (ch >= '0' && ch <= '9') v |= static_cast<unsigned>(ch - '0');
        else if (ch >= 'a' && ch <= 'f') v |= static_cast<unsigned>(ch - 'a' + 10);
        else if (ch >= 'A' && ch <= 'F') v |= static_cast<unsigned>(ch - 'A' + 10);
        else return false;
    }
    return true;
}

bool ParseJsonString(JsonCursor& c, std::string& out) {
    out.clear();
    if (!c.Eat('"')) return false;
    while (c.p < c.end) {
        const char ch = *c.p++;
        if (ch == '"') return true;
        if (static_cast<unsigned char>(ch) < 0x20) return false;
        if (ch != '\\') { out += ch; continue; }
        if (c.p >= c.end) return false;
        const char e = *c.p++;
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
            unsigned cp = 0;
            if (!ParseHex4(c, cp)) return false;
            if (cp >= 0xD800 && cp < 0xDC00) {
                unsigned lo = 0;
                if (c.end - c.p < 2 || c.p[0] != '\\' || c.p[1] != 'u') return false;
                c.p += 2;
                if (!ParseHex4(c, lo) || lo < 0xDC00 || lo >= 0xE000) return false;
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            }
            AppendUtf8(out, cp);
            break;
        }
        default: return false;
        }
    }
    return false;
}

bool ParseJsonBare(JsonCursor& c, std::string& out) {
    c.Ws();
    const char* b = c.p;
    while (c.p < c.end && *c.p != ',' && *c.p != '}' && *c.p != ']' &&
           *c.p != ' ' && *c.p != '\n' && *c.p != '\r' && *c.p != '\t') {
        ++c.p;
    }
    out.assign(b, c.p);
    return !out.empty();
}

bool ParseJsonValue(JsonCursor& c, std::string& out, bool& isString) {
    c.Ws();
    if (c.p >= c.end) return false;
    isString = (*c.p == '"');
    return isString ? ParseJsonString(c, out) : ParseJsonBare(c, out);
}

bool ParseGitTreeJson(const std::vector<uint8_t>& body, std::vector<TreeEntry>& out, bool& truncated) {
    out.clear();
    truncated = true;
    JsonCursor c{ reinterpret_cast<const char*>(body.data()), reinterpret_cast<const char*>(body.data() + body.size()) };
    if (!c.Eat('{')) return false;

    bool sawTree = false;
    bool sawTruncated = false;
    std::string key, val;
    bool isStr = false;
    for (;;) {
        if (!ParseJsonString(c, key) || !c.Eat(':')) return false;
        if (key == "tree") {
            if (!c.Eat('[')) return false;
            sawTree = true;
            if (!c.Eat(']')) {
                for (;;) {
                    if (out.size() >= kMaxEntries) return false;
                    if (!c.Eat('{')) return false;
                    TreeEntry e;
                    std::string type;
                    for (;;) {
                        if (!ParseJsonString(c, key) || !c.Eat(':') || !ParseJsonValue(c, val, isStr)) return false;
                        if (key == "path" && isStr) e.path = val;
                        else if (key == "sha" && isStr) e.sha = val;
                        else if (key == "type" && isStr) type = val;
                        else if (key == "size" && !isStr) e.size = _strtoui64(val.c_str(), nullptr, 10);
                        if (c.Eat(',')) continue;
                        if (c.Eat('}')) break;
                        return false;
                    }
                    if (type == "blob" && e.sha.size() == 40) out.push_back(std::move(e));
                    if (c.Eat(',')) continue;
                    if (c.Eat(']')) break;
                    return false;
                }
            }
        } else {
            if (!ParseJsonValue(c, val, isStr)) return false;
            if (key == "truncated") {
                sawTruncated = true;
                truncated = (val != "false");
            }
        }
        if (c.Eat(',')) continue;
        if (c.Eat('}')) break;
        return false;
    }
    return sawTree && sawTruncated;
}

// ---------------- Safe File Operations & Manifest Helpers ----------------

std::wstring ResolveSafeLocalPath(const std::wstring& root, const std::string& repoRelPath) {
    if (repoRelPath.empty()) return {};
    size_t start = 0;
    while (start <= repoRelPath.size()) {
        size_t slash = repoRelPath.find('/', start);
        if (slash == std::string::npos) slash = repoRelPath.size();
        const std::string part = repoRelPath.substr(start, slash - start);
        if (part.empty() || part == "." || part == "..") return {};
        for (unsigned char ch : part) {
            if (ch < 0x20 || strchr("\\:*?\"<>|", static_cast<char>(ch))) return {};
        }
        if (part.back() == '.' || part.back() == ' ') return {};
        start = slash + 1;
    }
    std::wstring w = Utf8ToWide(repoRelPath);
    if (w.empty()) return {};
    for (wchar_t& ch : w) {
        if (ch == L'/') ch = L'\\';
    }
    std::wstring full = root;
    if (!full.empty() && full.back() != L'\\' && full.back() != L'/') full += L'\\';
    return full + w;
}

bool EnsureParentDirs(const std::wstring& filePath) {
    for (size_t i = 3; i < filePath.size(); ++i) {
        if (filePath[i] != L'\\') continue;
        const std::wstring dir = filePath.substr(0, i);
        if (!CreateDirectoryW(dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
            return false;
        }
    }
    return true;
}

bool ReadFileExact(const std::wstring& path, uint64_t expectSize, std::vector<uint8_t>& out) {
    out.clear();
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    bool ok = GetFileSizeEx(h, &sz) && static_cast<uint64_t>(sz.QuadPart) == expectSize && expectSize <= kMaxFileBytes;
    if (ok && expectSize > 0) {
        out.resize(static_cast<size_t>(expectSize));
        DWORD got = 0;
        ok = ReadFile(h, out.data(), static_cast<DWORD>(expectSize), &got, nullptr) && got == expectSize;
    }
    CloseHandle(h);
    if (!ok) out.clear();
    return ok;
}

bool LocalFileMatchesGitBlob(const std::wstring& path, const TreeEntry& e) {
    std::vector<uint8_t> bytes;
    if (!ReadFileExact(path, e.size, bytes)) return false;
    return ComputeGitBlobSha(bytes.data(), bytes.size()) == e.sha;
}

bool WriteReplaceFile(const std::wstring& path, const std::vector<uint8_t>& data, bool moveExistingAsideFirst) {
    if (!EnsureParentDirs(path)) return false;
    const std::wstring part = path + L".part";
    HANDLE h = CreateFileW(part.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    DWORD put = 0;
    const bool wrote = data.empty() ||
                       (WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &put, nullptr) && put == data.size());
    CloseHandle(h);
    if (!wrote) {
        DeleteFileW(part.c_str());
        return false;
    }

    if (moveExistingAsideFirst && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
        const std::wstring oldPath = path + L".old";
        MoveFileExW(path.c_str(), oldPath.c_str(), MOVEFILE_REPLACE_EXISTING);
    }

    if (MoveFileExW(part.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return true;
    }
    DeleteFileW(part.c_str());
    return false;
}

bool FetchGitBlob(const TreeEntry& e, std::vector<uint8_t>& out, DWORD& status) {
    if (e.size > kMaxFileBytes) {
        out.clear();
        status = 0;
        return false;
    }
    const std::wstring apiPath = std::wstring(L"/repos/") + kRepoOwner + L"/" + kRepoName + L"/git/blobs/" + Utf8ToWide(e.sha);
    if (!GitHubApiGet(apiPath, L"application/vnd.github.raw+json", e.size, out, status)) {
        return false;
    }
    if (out.size() != e.size || ComputeGitBlobSha(out.data(), out.size()) != e.sha) {
        out.clear();
        return false;
    }
    return true;
}

void ParseVersionManifest(const std::vector<uint8_t>& bytes, std::string& outVer, uint32_t& outLinkTime, std::string& outNotes) {
    outVer.clear();
    outLinkTime = 0;
    outNotes.clear();
    std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t eq = line.find('=');
        if (eq != std::string::npos) {
            std::string k = line.substr(0, eq);
            std::string v = line.substr(eq + 1);
            if (k == "version") outVer = v;
            else if (k == "link_time") outLinkTime = strtoul(v.c_str(), nullptr, 10);
            else if (k == "notes") outNotes = v;
        }
        pos = end + 1;
    }
}

inline const char* L(const char* ru, const char* en) {
    return UrushiUI::IsEnglish() ? en : ru;
}

void CheckWorker(bool userInitiated) {
    s_workerBusy.store(true, std::memory_order_release);

    std::wstring exeDir;
    uint32_t localTs = 0;
    {
        std::lock_guard<std::mutex> lk(s_mtx);
        exeDir = s_exeDir;
        localTs = s_localLinkTime;
        s_state = State::Checking;
        s_statusText = L("Проверка обновлений на GitHub...", "Checking GitHub for updates...");
        s_planned.clear();
        s_localBuildNewer = false;
        s_remoteVersion.clear();
        s_releaseNotes.clear();
        s_remoteLinkTime = 0;
        if (userInitiated) s_bannerDismissed = false;
    }

    std::vector<uint8_t> treeBody;
    DWORD status = 0;
    const std::wstring treePath = std::wstring(L"/repos/") + kRepoOwner + L"/" + kRepoName +
                                  L"/git/trees/" + kRepoBranch + L"?recursive=1";

    if (!GitHubApiGet(treePath, L"application/vnd.github+json", kMaxTreeBytes, treeBody, status)) {
        std::lock_guard<std::mutex> lk(s_mtx);
        s_state = State::Error;
        if (status == 0) s_statusText = L("Нет подключения к api.github.com", "Cannot connect to api.github.com");
        else if (status == 401 || status == 404) {
            s_statusText = std::string(L("Репозиторий недоступен (HTTP ", "Repository unavailable (HTTP ")) + std::to_string(status) + ")";
        } else if (status == 403 || status == 429) {
            s_statusText = "GitHub rate limit (HTTP " + std::to_string(status) + ")";
        } else {
            s_statusText = std::string(L("Ошибка GitHub API: HTTP ", "GitHub API error: HTTP ")) + std::to_string(status);
        }
        Logger::Warn("Updater check failed: " + s_statusText);
        s_workerBusy.store(false, std::memory_order_release);
        return;
    }

    std::vector<TreeEntry> tree;
    bool truncated = true;
    if (!ParseGitTreeJson(treeBody, tree, truncated) || truncated) {
        std::lock_guard<std::mutex> lk(s_mtx);
        s_state = State::Error;
        s_statusText = truncated
            ? L("Список файлов GitHub усечён", "GitHub file tree truncated")
            : L("Ошибка разбора ответа GitHub", "Failed to parse GitHub response");
        s_workerBusy.store(false, std::memory_order_release);
        return;
    }

    // Optional dist/version.txt manifest (tiny fetch only when present)
    std::string remoteVer;
    std::string releaseNotes;
    uint32_t remoteTs = 0;
    for (const TreeEntry& e : tree) {
        if (e.path == "dist/version.txt" && e.size <= 32768) {
            std::vector<uint8_t> mfBytes;
            DWORD mfStatus = 0;
            if (FetchGitBlob(e, mfBytes, mfStatus)) {
                ParseVersionManifest(mfBytes, remoteVer, remoteTs, releaseNotes);
            }
            break;
        }
    }

    const bool localNewerThanRepo = (remoteTs > 0 && localTs > 0 && remoteTs < localTs);
    std::vector<PlannedFile> planned;
    uint64_t totalBytes = 0;

    if (!localNewerThanRepo) {
        for (const TreeEntry& e : tree) {
            if (s_stop.load(std::memory_order_acquire)) break;

            if (e.path.compare(0, 8, "scripts/") == 0 || e.path.compare(0, 8, "shaders/") == 0) {
                if (e.path == "scripts/runner_state.json") continue;
                std::wstring dest = ResolveSafeLocalPath(exeDir, e.path);
                if (dest.empty()) continue;
                if (!LocalFileMatchesGitBlob(dest, e)) {
                    planned.push_back({ e, std::move(dest), false, false });
                    totalBytes += e.size;
                }
            } else if (e.path == "dist/CarX2ScriptHook.dll") {
                std::wstring dest = exeDir + L"\\CarX2ScriptHook.dll";
                if (!LocalFileMatchesGitBlob(dest, e)) {
                    planned.push_back({ e, std::move(dest), false, true });
                    totalBytes += e.size;
                }
            } else if (e.path == "dist/version.dll") {
                std::wstring dest = exeDir + L"\\version.dll";
                if (!LocalFileMatchesGitBlob(dest, e)) {
                    planned.push_back({ e, std::move(dest), true, false });
                    totalBytes += e.size;
                }
            }
        }
    }

    if (s_stop.load(std::memory_order_acquire)) {
        s_workerBusy.store(false, std::memory_order_release);
        return;
    }

    std::lock_guard<std::mutex> lk(s_mtx);
    s_remoteVersion = remoteVer;
    s_remoteLinkTime = remoteTs;
    s_releaseNotes = releaseNotes;
    s_localBuildNewer = localNewerThanRepo;
    s_planned = std::move(planned);
    s_filesTotal = static_cast<uint32_t>(s_planned.size());
    s_filesDone = 0;
    s_bytesTotal = totalBytes;
    s_bytesDone = 0;

    if (s_planned.empty()) {
        s_state = State::UpToDate;
        s_statusText = localNewerThanRepo
            ? L("Локальная сборка новее репо (dev)", "Local build is newer than repo (dev)")
            : L("Установлена актуальная версия URUSHI", "URUSHI is up to date");
        Logger::Info("Updater check: up to date (" + s_statusText + ")");
    } else {
        s_state = State::UpdateAvailable;
        s_bannerDismissed = false;
        s_statusText = std::string(L("Доступно обновление (", "Update available (")) + std::to_string(s_planned.size()) + L(" ф.)", " files)");
        Logger::Info("Updater check: found " + std::to_string(s_planned.size()) +
                     " changed file(s), remoteVer=" + (remoteVer.empty() ? "n/a" : remoteVer));
    }
    s_workerBusy.store(false, std::memory_order_release);
}

void InstallWorker() {
    s_workerBusy.store(true, std::memory_order_release);

    std::vector<PlannedFile> files;
    {
        std::lock_guard<std::mutex> lk(s_mtx);
        files = s_planned;
        s_state = State::Downloading;
        s_filesTotal = static_cast<uint32_t>(files.size());
        s_filesDone = 0;
        s_bytesDone = 0;
        s_proxyNeedsRestart = false;
        s_statusText = L("Подготовка к загрузке...", "Preparing download...");
    }

    uint64_t writtenBytes = 0;
    uint32_t writtenFiles = 0;
    bool touchedProxy = false;
    bool failed = false;
    std::string failMsg;

    for (size_t i = 0; i < files.size(); ++i) {
        if (s_stop.load(std::memory_order_acquire) || writtenBytes + files[i].entry.size > kMaxTotalBytes) {
            failed = true;
            failMsg = s_stop.load(std::memory_order_acquire)
                ? L("Отменено", "Cancelled")
                : L("Превышен лимит объёма", "Download size limit exceeded");
            break;
        }
        {
            std::lock_guard<std::mutex> lk(s_mtx);
            s_currentFile = files[i].entry.path;
            s_statusText = std::string(L("Скачивание ", "Downloading ")) + std::to_string(i + 1) + "/" + std::to_string(files.size()) + ": " + files[i].entry.path;
        }

        std::vector<uint8_t> blob;
        DWORD status = 0;
        if (!FetchGitBlob(files[i].entry, blob, status)) {
            failed = true;
            failMsg = std::string(L("Не удалось скачать ", "Failed to download ")) + files[i].entry.path + " (HTTP " + std::to_string(status) + ")";
            break;
        }
        if (!WriteReplaceFile(files[i].destPath, blob, files[i].isProxy)) {
            failed = true;
            failMsg = std::string(L("Ошибка записи ", "Failed to write ")) + files[i].entry.path + " (err " + std::to_string(GetLastError()) + ")";
            break;
        }

        if (files[i].isProxy) touchedProxy = true;
        writtenBytes += files[i].entry.size;
        writtenFiles++;

        std::lock_guard<std::mutex> lk(s_mtx);
        s_filesDone = writtenFiles;
        s_bytesDone = writtenBytes;
    }

    if (failed) {
        std::lock_guard<std::mutex> lk(s_mtx);
        s_state = State::Error;
        s_statusText = failMsg;
        Logger::Error("Updater install failed: " + failMsg);
        s_workerBusy.store(false, std::memory_order_release);
        return;
    }

    Logger::Info("Updater downloaded " + std::to_string(writtenFiles) + " file(s) (" +
                 std::to_string(writtenBytes / 1024) + " KB). Triggering hot-reinject...");

    wchar_t evName[64] = {};
    wsprintfW(evName, L"Local\\UrushiReload_%lu", GetCurrentProcessId());
    HANDLE hReload = OpenEventW(EVENT_MODIFY_STATE, FALSE, evName);

    if (hReload && !touchedProxy) {
        {
            std::lock_guard<std::mutex> lk(s_mtx);
            s_state = State::Reinjecting;
            s_statusText = L("Перезагрузка модулей URUSHI...", "Reloading URUSHI modules...");
        }
        s_workerBusy.store(false, std::memory_order_release);
        SetEvent(hReload);
        CloseHandle(hReload);
        return;
    }

    if (hReload) {
        // Core files can still hot-reinject, while version.dll needs a full restart to load the new proxy.
        CloseHandle(hReload);
    }

    std::lock_guard<std::mutex> lk(s_mtx);
    s_proxyNeedsRestart = touchedProxy;
    s_state = State::RestartRequired;
    s_statusText = touchedProxy
        ? L("Файлы обновлены! Для новой version.dll нужен перезапуск игры.", "Files updated! Restart the game to load the new version.dll.")
        : L("Обновление скачано! Нужен перезапуск игры (прокси не обнаружен).", "Update downloaded! Game restart required.");
    s_workerBusy.store(false, std::memory_order_release);
}

void JoinFinishedWorkerLocked() {
    if (s_worker.joinable()) {
        s_worker.join();
    }
}

} // namespace

void Init(const std::wstring& exeDir, HMODULE selfModule) {
    {
        std::lock_guard<std::mutex> lk(s_mtx);
        s_stop.store(false, std::memory_order_release);
        s_exeDir = exeDir;
        s_selfModule = selfModule;
        s_localLinkTime = ReadPeTimestampFromMemory(selfModule);
    }
    CheckAsync(false);
}

void Shutdown() {
    s_stop.store(true, std::memory_order_release);
    if (s_worker.joinable()) {
        s_worker.join();
    }
}

void CheckAsync(bool userInitiated) {
    if (s_workerBusy.load(std::memory_order_acquire) || s_stop.load(std::memory_order_acquire)) return;
    JoinFinishedWorkerLocked();
    s_worker = std::thread([userInitiated]() { CheckWorker(userInitiated); });
}

void StartInstallAsync() {
    if (s_workerBusy.load(std::memory_order_acquire) || s_stop.load(std::memory_order_acquire)) return;
    {
        std::lock_guard<std::mutex> lk(s_mtx);
        if (s_planned.empty()) return;
    }
    JoinFinishedWorkerLocked();
    s_worker = std::thread([]() { InstallWorker(); });
}

void RestartGame() {
    wchar_t cmdLine[256] = {};
    wsprintfW(cmdLine, L"cmd.exe /c ping 127.0.0.1 -n 3 >nul & start \"\" \"steam://rungameid/%lu\"", kSteamAppId);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, cmdLine, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        ExitProcess(0);
    }
}

void SetDismissed(bool dismissed) {
    std::lock_guard<std::mutex> lk(s_mtx);
    s_bannerDismissed = dismissed;
}

StatusSnapshot GetStatus() {
    std::lock_guard<std::mutex> lk(s_mtx);
    StatusSnapshot st;
    st.state = s_state;
    st.localVersion = URUSHI_VERSION;
    st.remoteVersion = s_remoteVersion;
    st.releaseNotes = s_releaseNotes;
    st.statusText = s_statusText;
    st.currentFile = s_currentFile;
    st.localLinkTime = s_localLinkTime;
    st.remoteLinkTime = s_remoteLinkTime;
    st.localBuildNewer = s_localBuildNewer;
    st.proxyNeedsRestart = s_proxyNeedsRestart;
    st.bannerDismissed = s_bannerDismissed;
    st.filesTotal = s_filesTotal;
    st.filesDone = s_filesDone;
    st.bytesTotal = s_bytesTotal;
    st.bytesDone = s_bytesDone;
    st.files.reserve(s_planned.size());
    for (const auto& p : s_planned) {
        st.files.push_back({ p.entry.path, p.entry.size, p.isProxy });
    }
    return st;
}

void RenderUI(bool menuOpen) {
    StatusSnapshot st = GetStatus();
    if (st.bannerDismissed) return;

    const bool showBanner = (st.state == State::UpdateAvailable ||
                             st.state == State::Downloading ||
                             st.state == State::Reinjecting ||
                             st.state == State::RestartRequired);
    if (!showBanner) return;

    const float S = UrushiUI::GetUiScale();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float pad = 18.0f * S;
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - pad, vp->WorkPos.y + pad),
                            ImGuiCond_Always, ImVec2(1.0f, 0.0f));

    if (!menuOpen) {
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.045f, 0.045f, 0.060f, 0.86f));
        ImGui::PushStyleColor(ImGuiCol_Border, UrushiUI::GetAccentVec4(0.45f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f * S);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f * S, 8.0f * S));
        if (ImGui::Begin("##urushi_update_toast", nullptr,
                         ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                         ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
            UrushiUI::Badge(L("ОБНОВЛЕНИЕ", "UPDATE"), UrushiUI::Tone::Accent);
            ImGui::SameLine(0.0f, 8.0f * S);
            ImGui::TextColored(ImVec4(0.92f, 0.92f, 0.95f, 1.0f),
                               L("%u ф. — нажмите F4", "%u file(s) — press F4"), st.filesTotal);
        }
        ImGui::End();
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(2);
        return;
    }

    const float winW = (std::min)(400.0f * S, (std::max)(240.0f * S, vp->WorkSize.x - pad * 2.0f));
    ImGui::SetNextWindowSize(ImVec2(winW, 0.0f), ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.050f, 0.050f, 0.068f, 0.96f));
    ImGui::PushStyleColor(ImGuiCol_Border, UrushiUI::GetAccentVec4(0.40f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f * S);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f * S, 12.0f * S));
    const bool opened = ImGui::Begin("URUSHI // Update##urushi_updater_banner", nullptr,
                                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize |
                                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);
    if (!opened) {
        ImGui::End();
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(2);
        return;
    }

    UrushiUI::Badge(L("ОБНОВЛЕНИЕ", "UPDATE"), UrushiUI::Tone::Accent);
    ImGui::SameLine(0.0f, 8.0f * S);
    UrushiUI::Text(L("Обновление URUSHI", "URUSHI Update"));
    UrushiUI::Spacing(6.0f * S);

    if (st.state == State::UpdateAvailable) {
        const std::string verStr = st.remoteVersion.empty()
            ? (std::string(L("файлов: ", "files: ")) + std::to_string(st.filesTotal))
            : ("v" + st.localVersion + " -> v" + st.remoteVersion);

        const double kb = static_cast<double>(st.bytesTotal) / 1024.0;
        char sizeBuf[48];
        snprintf(sizeBuf, sizeof(sizeBuf), L("%u (%.1f КБ)", "%u (%.1f KB)"), st.filesTotal, kb);

        UrushiUI::BeginCard(L("Доступна новая версия", "New version available"), verStr);
        UrushiUI::StatRow({
            { L("Файлов", "Files"), sizeBuf, UrushiUI::Tone::Accent },
            { L("Статус", "Status"), L("Готово", "Ready"), UrushiUI::Tone::Green }
        });

        if (!st.releaseNotes.empty()) {
            UrushiUI::Spacing(4.0f * S);
            UrushiUI::TextWrapped(st.releaseNotes, UrushiUI::Tone::Muted);
        }

        if (!st.files.empty() && UrushiUI::CollapsingSection(L("Изменённые файлы", "Changed files"), false)) {
            for (const auto& f : st.files) {
                char lineBuf[320];
                snprintf(lineBuf, sizeof(lineBuf), "- %s (%.1f KB)", f.repoPath.c_str(), static_cast<double>(f.size) / 1024.0);
                UrushiUI::Text(lineBuf, UrushiUI::Tone::Muted);
            }
        }

        UrushiUI::Spacing(6.0f * S);
        const float gap = 8.0f * S;
        const float cw = UrushiUI::CurrentContentWidth();
        const float bw1 = (cw - gap) * 0.62f;
        const float bw2 = (cw - gap) * 0.38f;
        if (UrushiUI::Button(L("Обновить", "Update"), bw1, 30.0f * S, UrushiUI::ButtonStyle::Primary)) {
            StartInstallAsync();
        }
        ImGui::SameLine(0.0f, gap);
        if (UrushiUI::Button(L("Позже", "Later"), bw2, 30.0f * S, UrushiUI::ButtonStyle::Ghost)) {
            SetDismissed(true);
        }
        UrushiUI::EndCard();
    } else if (st.state == State::Downloading) {
        UrushiUI::BeginCard(L("Загрузка файлов", "Downloading files"), st.statusText);
        float frac = (st.filesTotal > 0) ? (static_cast<float>(st.filesDone) / static_cast<float>(st.filesTotal)) : 0.0f;
        char progBuf[48];
        snprintf(progBuf, sizeof(progBuf), "%u / %u", st.filesDone, st.filesTotal);
        UrushiUI::ProgressBar(frac, -1.0f, 14.0f * S, progBuf);
        UrushiUI::EndCard();
    } else if (st.state == State::Reinjecting) {
        UrushiUI::BeginCard(L("Установка", "Installing"), L("Файлы скачаны", "Download complete"));
        UrushiUI::TextWrapped(L("Выполняется реинжект URUSHI...", "Reloading URUSHI..."), UrushiUI::Tone::Green);
        UrushiUI::EndCard();
    } else if (st.state == State::RestartRequired) {
        UrushiUI::BeginCard(L("Требуется перезапуск", "Restart required"), L("Обновление ядра", "Core update"));
        UrushiUI::TextWrapped(st.statusText, UrushiUI::Tone::Yellow);
        UrushiUI::Spacing(6.0f * S);
        const float gap = 8.0f * S;
        const float cw = UrushiUI::CurrentContentWidth();
        const float bw1 = (cw - gap) * 0.62f;
        const float bw2 = (cw - gap) * 0.38f;
        if (UrushiUI::Button(L("Перезапустить игру", "Restart game"), bw1, 30.0f * S, UrushiUI::ButtonStyle::Primary)) {
            RestartGame();
        }
        ImGui::SameLine(0.0f, gap);
        if (UrushiUI::Button(L("Закрыть", "Close"), bw2, 30.0f * S, UrushiUI::ButtonStyle::Ghost)) {
            SetDismissed(true);
        }
        UrushiUI::EndCard();
    }

    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
}

} // namespace Updater
