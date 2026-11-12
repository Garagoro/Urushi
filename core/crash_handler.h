#pragma once
#include <string>
#include <windows.h>

// Crash capture: unhandled SEH exceptions and std::terminate are turned into
//   <gameDir>\urushi_crash\crash_<stamp>.txt  (human readable report: exception, registers, stack, last Lua context)
//   <gameDir>\urushi_crash\crash_<stamp>.dmp  (minidump)
//   <gameDir>\urushi_crash\crash_<stamp>.log  (copy of urushi.log at the moment of the crash)
// and a marker file that the NEXT launch uses to offer Safe mode.
namespace CrashHandler {

struct PreviousCrash {
    bool crashed = false;     // previous session left a crash marker
    bool inUrushi = false;    // top frames of the crash were inside urushi.dll
    std::string report;       // file name of the report (inside the crash dir)
    std::string summary;      // one line: exception + faulting module
};

void Init(const std::wstring& exeDir, HMODULE selfModule);
void Shutdown();

// Re-installs the filter if somebody (Unity) replaced it. Cheap; call about once per second.
void Tick();

// Reads and clears the marker left by the previous session. Result is cached for GetPreviousCrash().
PreviousCrash ConsumePreviousCrash();
const PreviousCrash& GetPreviousCrash();

// Free-form context that is written into the report (static-lifetime strings only!).
void SetLuaContext(const char* label);
const char* GetLuaContext();
void SetModeName(const char* mode);

std::wstring GetCrashDir();

} // namespace CrashHandler
