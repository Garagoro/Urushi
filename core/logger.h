#pragma once
#include <string>
#include <fstream>
#include <mutex>
#include <windows.h>

class Logger {
public:
    static void Init(const std::wstring& logFilePath);
    static void Shutdown();

    static void Info(const std::string& message);
    static void Warn(const std::string& message);
    static void Error(const std::string& message);

    static std::string ToNarrow(const std::wstring& wstr);
    static std::wstring ToWide(const std::string& str);

private:
    static void Log(const std::string& level, const std::string& message);

    static std::ofstream s_file;
    static std::recursive_mutex s_mutex;
    static bool s_hasConsole;
};
