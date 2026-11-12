#include "settings.h"
#include "logger.h"
#include <windows.h>
#include <mutex>

namespace Settings {

static std::wstring s_path;
static std::mutex s_mtx;
static std::wstring s_dir;

std::wstring GameDir() {
    std::lock_guard<std::mutex> lk(s_mtx);
    return s_dir;
}

void Init(const std::wstring& exeDir) {
    std::lock_guard<std::mutex> lk(s_mtx);
    s_dir = exeDir;
    s_path = exeDir + L"\\urushi_settings.ini";
}

std::string GetString(const std::string& section, const std::string& key, const std::string& def) {
    std::lock_guard<std::mutex> lk(s_mtx);
    if (s_path.empty()) return def;
    wchar_t buf[1024] = {};
    GetPrivateProfileStringW(Logger::ToWide(section).c_str(), Logger::ToWide(key).c_str(),
                             Logger::ToWide(def).c_str(), buf, 1024, s_path.c_str());
    return Logger::ToNarrow(buf);
}

void SetString(const std::string& section, const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lk(s_mtx);
    if (s_path.empty()) return;
    WritePrivateProfileStringW(Logger::ToWide(section).c_str(), Logger::ToWide(key).c_str(),
                               Logger::ToWide(value).c_str(), s_path.c_str());
}

bool GetBool(const std::string& section, const std::string& key, bool def) {
    std::string v = GetString(section, key, def ? "1" : "0");
    return !(v == "0" || v == "false" || v == "no" || v == "off" || v.empty());
}

void SetBool(const std::string& section, const std::string& key, bool value) {
    SetString(section, key, value ? "1" : "0");
}

CrashPolicy GetCrashPolicy() {
    std::string v = GetString("stability", "crash_policy", "ask");
    if (v == "safe") return CrashPolicy::Safe;
    if (v == "normal") return CrashPolicy::Normal;
    return CrashPolicy::Ask;
}

const char* CrashPolicyName(CrashPolicy p) {
    switch (p) {
    case CrashPolicy::Safe: return "safe";
    case CrashPolicy::Normal: return "normal";
    default: return "ask";
    }
}

void SetCrashPolicy(CrashPolicy p) {
    SetString("stability", "crash_policy", CrashPolicyName(p));
}

} // namespace Settings
