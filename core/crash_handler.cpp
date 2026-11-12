#include "crash_handler.h"
#include "logger.h"
#include <dbghelp.h>   // types only; dbghelp.dll itself is loaded dynamically

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <exception>
#include <fstream>
#include <sstream>

namespace CrashHandler {

namespace {

typedef BOOL(WINAPI* t_MiniDumpWriteDump)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                          PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);

HMODULE g_self = nullptr;
std::wstring g_exeDir;
std::wstring g_crashDir;
std::wstring g_markerPath;
LPTOP_LEVEL_EXCEPTION_FILTER g_prevFilter = nullptr;
std::atomic<bool> g_inCrash{ false };
std::atomic<const char*> g_luaContext{ "none" };
std::atomic<const char*> g_modeName{ "booting" };
PreviousCrash g_prev;
bool g_prevConsumed = false;
std::terminate_handler g_prevTerminate = nullptr;

const char* ExceptionName(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: return "ACCESS_VIOLATION";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "ARRAY_BOUNDS_EXCEEDED";
    case EXCEPTION_DATATYPE_MISALIGNMENT: return "DATATYPE_MISALIGNMENT";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO: return "FLT_DIVIDE_BY_ZERO";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "ILLEGAL_INSTRUCTION";
    case EXCEPTION_IN_PAGE_ERROR: return "IN_PAGE_ERROR";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "INT_DIVIDE_BY_ZERO";
    case EXCEPTION_PRIV_INSTRUCTION: return "PRIV_INSTRUCTION";
    case EXCEPTION_STACK_OVERFLOW: return "STACK_OVERFLOW";
    case EXCEPTION_BREAKPOINT: return "BREAKPOINT";
    case 0xC0000409: return "FAST_FAIL (stack cookie / abort)";
    case 0xE06D7363: return "C++ exception";
    case 0xE0000001: return "std::terminate";
    default: return "UNKNOWN";
    }
}

// "module.dll+0x1234" for an address; returns true when the address is inside urushi.dll
bool DescribeAddress(DWORD64 addr, char* out, size_t outSize) {
    HMODULE hm = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(addr), &hm) && hm) {
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(hm, path, MAX_PATH);
        const wchar_t* base = wcsrchr(path, L'\\');
        base = base ? base + 1 : path;
        char narrow[MAX_PATH] = {};
        WideCharToMultiByte(CP_UTF8, 0, base, -1, narrow, sizeof(narrow) - 1, nullptr, nullptr);
        snprintf(out, outSize, "%s+0x%llX", narrow, static_cast<unsigned long long>(addr - reinterpret_cast<DWORD64>(hm)));
        return hm == g_self;
    }
    snprintf(out, outSize, "0x%llX", static_cast<unsigned long long>(addr));
    return false;
}

std::wstring Stamp() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t buf[64];
    swprintf(buf, 64, L"%04d%02d%02d_%02d%02d%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

void WriteDump(EXCEPTION_POINTERS* ep, const std::wstring& path) {
    HMODULE dbg = LoadLibraryW(L"dbghelp.dll");
    if (!dbg) return;
    auto fn = reinterpret_cast<t_MiniDumpWriteDump>(GetProcAddress(dbg, "MiniDumpWriteDump"));
    if (!fn) return;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;

    MINIDUMP_EXCEPTION_INFORMATION info{};
    info.ThreadId = GetCurrentThreadId();
    info.ExceptionPointers = ep;
    info.ClientPointers = FALSE;
    const MINIDUMP_TYPE type = static_cast<MINIDUMP_TYPE>(MiniDumpNormal | MiniDumpWithThreadInfo |
                                                         MiniDumpWithUnloadedModules | MiniDumpWithIndirectlyReferencedMemory);
    fn(GetCurrentProcess(), GetCurrentProcessId(), f, type, ep ? &info : nullptr, nullptr, nullptr);
    CloseHandle(f);
}

// Core report writer. Intentionally uses only stack buffers + WinAPI I/O where possible
// because the heap may be corrupted at this point.
void WriteReport(EXCEPTION_POINTERS* ep, const char* originOverride) {
    if (g_inCrash.exchange(true)) return;

    const std::wstring stamp = Stamp();
    CreateDirectoryW(g_crashDir.c_str(), nullptr);
    const std::wstring txtPath = g_crashDir + L"\\crash_" + stamp + L".txt";
    const std::wstring dmpPath = g_crashDir + L"\\crash_" + stamp + L".dmp";
    const std::wstring logPath = g_crashDir + L"\\crash_" + stamp + L".log";

    char text[16384];
    size_t n = 0;
    auto add = [&](const char* fmt, ...) {
        if (n >= sizeof(text) - 1) return;
        va_list ap;
        va_start(ap, fmt);
        int w = vsnprintf(text + n, sizeof(text) - n, fmt, ap);
        va_end(ap);
        if (w > 0) n += static_cast<size_t>(w);
        if (n > sizeof(text) - 1) n = sizeof(text) - 1;
    };

    SYSTEMTIME st;
    GetLocalTime(&st);
    const DWORD code = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0xE0000001;
    add("URUSHI crash report\r\n");
    add("Time:        %04d-%02d-%02d %02d:%02d:%02d\r\n", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    add("Origin:      %s\r\n", originOverride ? originOverride : "unhandled SEH exception");
    add("Exception:   0x%08lX (%s)\r\n", code, ExceptionName(code));
    add("Thread:      %lu\r\n", GetCurrentThreadId());
    add("URUSHI mode: %s\r\n", g_modeName.load());
    add("Lua context: %s   (last Lua entry point being executed; 'none' = not inside Lua)\r\n", g_luaContext.load());

    bool inUrushi = false;
    char summary[256] = {};
    char fault[160] = {};

    if (ep && ep->ExceptionRecord && ep->ContextRecord) {
        const EXCEPTION_RECORD* er = ep->ExceptionRecord;
        const DWORD64 faultAddr = reinterpret_cast<DWORD64>(er->ExceptionAddress);
        const bool faultInUrushi = DescribeAddress(faultAddr, fault, sizeof(fault));
        add("Fault addr:  %s\r\n", fault);
        if (code == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) {
            const char* kind = er->ExceptionInformation[0] == 0 ? "read" : (er->ExceptionInformation[0] == 1 ? "write" : "execute");
            add("Access:      %s of 0x%llX\r\n", kind, static_cast<unsigned long long>(er->ExceptionInformation[1]));
        }
        const CONTEXT* c = ep->ContextRecord;
        add("\r\nRegisters:\r\n");
        add("  RIP=%016llX RSP=%016llX RBP=%016llX\r\n", c->Rip, c->Rsp, c->Rbp);
        add("  RAX=%016llX RBX=%016llX RCX=%016llX RDX=%016llX\r\n", c->Rax, c->Rbx, c->Rcx, c->Rdx);
        add("  RSI=%016llX RDI=%016llX R8 =%016llX R9 =%016llX\r\n", c->Rsi, c->Rdi, c->R8, c->R9);

        add("\r\nStack (x64 unwind):\r\n");
        CONTEXT ctx = *c;
        inUrushi = faultInUrushi;
        for (int i = 0; i < 40; ++i) {
            const DWORD64 pc = ctx.Rip;
            if (!pc) break;
            char desc[160];
            const bool mine = DescribeAddress(pc, desc, sizeof(desc));
            if (mine && i < 16) inUrushi = true;
            add("  #%02d %s%s\r\n", i, desc, mine ? "   <== urushi" : "");

            DWORD64 imageBase = 0;
            PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(pc, &imageBase, nullptr);
            if (rf) {
                PVOID handlerData = nullptr;
                DWORD64 establisher = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, pc, rf, &ctx, &handlerData, &establisher, nullptr);
            } else {
                // leaf function without unwind info
                if (!ctx.Rsp) break;
                ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);
                ctx.Rsp += 8;
            }
        }
    }

    add("\r\nCrash in URUSHI code: %s\r\n", inUrushi ? "YES (top frames are inside urushi.dll)" : "no (game / driver / other module)");
    add("Files: crash_%ls.dmp (minidump), crash_%ls.log (urushi.log at crash time)\r\n", stamp.c_str(), stamp.c_str());

    snprintf(summary, sizeof(summary), "%s at %s", ExceptionName(code), fault[0] ? fault : "unknown");

    // report
    HANDLE f = CreateFileW(txtPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD wr = 0;
        WriteFile(f, text, static_cast<DWORD>(n), &wr, nullptr);
        CloseHandle(f);
    }

    // marker for next launch
    char marker[600];
    char reportA[128] = {};
    WideCharToMultiByte(CP_UTF8, 0, (L"crash_" + stamp + L".txt").c_str(), -1, reportA, sizeof(reportA) - 1, nullptr, nullptr);
    int mn = snprintf(marker, sizeof(marker), "report=%s\r\nin_urushi=%d\r\nsummary=%s\r\n", reportA, inUrushi ? 1 : 0, summary);
    HANDLE m = CreateFileW(g_markerPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (m != INVALID_HANDLE_VALUE) {
        DWORD wr = 0;
        WriteFile(m, marker, static_cast<DWORD>(mn > 0 ? mn : 0), &wr, nullptr);
        CloseHandle(m);
    }
    // log copy, dump
    CopyFileW((g_exeDir + L"\\urushi.log").c_str(), logPath.c_str(), FALSE);
    WriteDump(ep, dmpPath);
}

LONG WINAPI Filter(EXCEPTION_POINTERS* ep) {
    WriteReport(ep, nullptr);
    if (g_prevFilter) return g_prevFilter(ep);
    return EXCEPTION_CONTINUE_SEARCH;
}

void TerminateHandler() {
    CONTEXT ctx;
    RtlCaptureContext(&ctx);
    EXCEPTION_RECORD er = {};
    er.ExceptionCode = 0xE0000001;
    er.ExceptionAddress = reinterpret_cast<PVOID>(ctx.Rip);
    EXCEPTION_POINTERS ep{ &er, &ctx };
    WriteReport(&ep, "std::terminate (uncaught C++ exception)");
    if (g_prevTerminate) g_prevTerminate();
    abort();
}

} // namespace

void Init(const std::wstring& exeDir, HMODULE selfModule) {
    g_self = selfModule;
    g_exeDir = exeDir;
    g_crashDir = exeDir + L"\\urushi_crash";
    g_markerPath = g_crashDir + L"\\last_crash.marker";
    CreateDirectoryW(g_crashDir.c_str(), nullptr);

    g_prevFilter = SetUnhandledExceptionFilter(Filter);
    g_prevTerminate = std::set_terminate(TerminateHandler);
    Logger::Info("CrashHandler installed (reports -> urushi_crash\\).");
}

void Tick() {
    // Unity (or another overlay) may have replaced the filter after us; take it back and chain to it.
    LPTOP_LEVEL_EXCEPTION_FILTER cur = SetUnhandledExceptionFilter(Filter);
    if (cur && cur != Filter) g_prevFilter = cur;
}

void Shutdown() {
    // Only restore if we are still the active filter.
    LPTOP_LEVEL_EXCEPTION_FILTER cur = SetUnhandledExceptionFilter(g_prevFilter);
    if (cur && cur != Filter) SetUnhandledExceptionFilter(cur);
    if (g_prevTerminate) std::set_terminate(g_prevTerminate);
}

PreviousCrash ConsumePreviousCrash() {
    if (g_prevConsumed) return g_prev;
    g_prevConsumed = true;
    std::ifstream in(g_markerPath, std::ios::binary);
    if (!in) return g_prev;
    std::string line;
    g_prev.crashed = true;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (line.rfind("report=", 0) == 0) g_prev.report = line.substr(7);
        else if (line.rfind("in_urushi=", 0) == 0) g_prev.inUrushi = line.substr(10) == "1";
        else if (line.rfind("summary=", 0) == 0) g_prev.summary = line.substr(8);
    }
    in.close();
    DeleteFileW(g_markerPath.c_str());
    return g_prev;
}

const PreviousCrash& GetPreviousCrash() { return g_prev; }

void SetLuaContext(const char* label) { g_luaContext.store(label ? label : "none", std::memory_order_relaxed); }
const char* GetLuaContext() { return g_luaContext.load(std::memory_order_relaxed); }
void SetModeName(const char* mode) { g_modeName.store(mode ? mode : "unknown", std::memory_order_relaxed); }
std::wstring GetCrashDir() { return g_crashDir; }

} // namespace CrashHandler
