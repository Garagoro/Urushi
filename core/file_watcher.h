#pragma once
#include <string>
#include <functional>
#include <thread>
#include <atomic>
#include <unordered_map>
#include <windows.h>

class FileWatcher {
public:
    typedef std::function<void(const std::string& filename)> Callback;

    static void Start(const std::wstring& directory, Callback onFileChanged);
    static void Stop();

private:
    static void WatcherThread();

    static std::wstring s_directory;
    static Callback s_callback;
    static std::thread s_thread;
    static std::atomic<bool> s_running;
    static std::unordered_map<std::wstring, FILETIME> s_fileTimes;
};
