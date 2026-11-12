#pragma once
#include <string>

// Tiny persistent key/value store backed by <gameDir>\urushi_settings.ini.
// Used for stability options (crash policy, safe mode) and per-script enable flags.
namespace Settings {

void Init(const std::wstring& exeDir);

std::string GetString(const std::string& section, const std::string& key, const std::string& def = "");
void SetString(const std::string& section, const std::string& key, const std::string& value);

bool GetBool(const std::string& section, const std::string& key, bool def);
void SetBool(const std::string& section, const std::string& key, bool value);

// Crash policy: what to do after the previous session crashed.
enum class CrashPolicy { Ask, Safe, Normal };
CrashPolicy GetCrashPolicy();
void SetCrashPolicy(CrashPolicy p);
const char* CrashPolicyName(CrashPolicy p);

} // namespace Settings
