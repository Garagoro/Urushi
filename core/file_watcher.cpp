#include "file_watcher.h"
#include "logger.h"
#include <chrono>

std::wstring FileWatcher::s_directory;
FileWatcher::Callback FileWatcher::s_callback;
std::thread FileWatcher::s_thread;
std::atomic<bool> FileWatcher::s_running(false);
std::unordered_map<std::wstring, FILETIME> FileWatcher::s_fileTimes;

void FileWatcher::Start(const std::wstring& directory, Callback onFileChanged) {
    if (s_running) return;

    s_directory = directory;
    s_callback = onFileChanged;
    s_running = true;
    s_fileTimes.clear();

    s_thread = std::thread(WatcherThread);
    Logger::Info("FileWatcher started on directory: " + Logger::ToNarrow(directory));
}

void FileWatcher::Stop() {
    if (!s_running) return;

    s_running = false;
    if (s_thread.joinable()) {
        s_thread.join();
    }
    s_fileTimes.clear();
    Logger::Info("FileWatcher stopped.");
}

void FileWatcher::WatcherThread() {
    while (s_running) {
        std::wstring pattern = s_directory + L"\\*.lua";
        WIN32_FIND_DATAW fd;
        HANDLE hFind = FindFirstFileW(pattern.c_str(), &fd);

        if (hFind != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                    std::wstring filename = fd.cFileName;
                    auto it = s_fileTimes.find(filename);

                    if (it != s_fileTimes.end()) {
                        if (CompareFileTime(&it->second, &fd.ftLastWriteTime) != 0) {
                            it->second = fd.ftLastWriteTime;
                            if (s_callback) {
                                std::string narrowName = Logger::ToNarrow(filename);
                                Logger::Info("Script file modified detected: " + narrowName);
                                s_callback(narrowName);
                            }
                        }
                    } else {
                        // First time seeing this file
                        s_fileTimes[filename] = fd.ftLastWriteTime;
                    }
                }
            } while (FindNextFileW(hFind, &fd));
            FindClose(hFind);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}
