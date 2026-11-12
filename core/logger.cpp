#include "logger.h"
#include <iostream>
#include <chrono>
#include <iomanip>
#include <sstream>

std::ofstream Logger::s_file;
std::recursive_mutex Logger::s_mutex;
bool Logger::s_hasConsole = false;

void Logger::Init(const std::wstring& logFilePath) {
    {
        std::lock_guard<std::recursive_mutex> lock(s_mutex);
        // Keep the previous session's log (it is truncated below) so a crash can be investigated afterwards
        {
            std::wstring prev = logFilePath;
            const size_t dot = prev.rfind(L'.');
            prev = (dot == std::wstring::npos ? prev : prev.substr(0, dot)) + L".prev.log";
            CopyFileW(logFilePath.c_str(), prev.c_str(), FALSE);
        }
        s_file.open(logFilePath, std::ios::out | std::ios::trunc);

        // Create debug console window if needed
#ifdef _DEBUG
        if (AllocConsole()) {
            FILE* fp;
            freopen_s(&fp, "CONOUT$", "w", stdout);
            freopen_s(&fp, "CONOUT$", "w", stderr);
            SetConsoleTitleA("URUSHI Debug Console");
            s_hasConsole = true;
        }
#endif
    }

    Info("=== URUSHI Logger Initialized ===");
}

void Logger::Shutdown() {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    if (s_file.is_open()) {
        s_file.flush();
        s_file.close();
    }
    if (s_hasConsole) {
        FreeConsole();
        s_hasConsole = false;
    }
}

void Logger::Info(const std::string& message) {
    Log("INFO", message);
}

void Logger::Warn(const std::string& message) {
    Log("WARN", message);
}

void Logger::Error(const std::string& message) {
    Log("ERROR", message);
}

std::string Logger::ToNarrow(const std::wstring& wstr) {
    if (wstr.empty()) return "";
    int size = WideCharToMultiByte(CP_UTF8, 0, wstr.data(), (int)wstr.size(), nullptr, 0, nullptr, nullptr);
    std::string result(size, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.data(), (int)wstr.size(), &result[0], size, nullptr, nullptr);
    return result;
}

std::wstring Logger::ToWide(const std::string& str) {
    if (str.empty()) return L"";
    int size = MultiByteToWideChar(CP_UTF8, 0, str.data(), (int)str.size(), nullptr, 0);
    std::wstring result(size, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.data(), (int)str.size(), &result[0], size);
    return result;
}

void Logger::Log(const std::string& level, const std::string& message) {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);

    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

    std::tm timeInfo;
    localtime_s(&timeInfo, &in_time_t);

    std::ostringstream ss;
    ss << "[" << std::put_time(&timeInfo, "%H:%M:%S") << "."
       << std::setfill('0') << std::setw(3) << ms.count() << "] "
       << "[" << level << "] " << message << "\n";

    std::string formatted = ss.str();

    if (s_file.is_open()) {
        s_file << formatted;
        s_file.flush();
    }

    if (s_hasConsole) {
        std::cout << formatted;
    }
    OutputDebugStringA(formatted.c_str());
}
