#include "audio_player.h"
#include "logger.h"

#include <windows.h>
#include <shlwapi.h>
#include <xaudio2.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>
#include <string>
#include <array>
#include <unordered_map>
#include <algorithm>
#include <cwchar>

namespace AudioPlayer {

namespace {

constexpr int kMaxVoices = 8;

enum class VoiceState {
    Stopped = 0,
    Loading = 1,
    Playing = 2,
};

struct DecodedAudioBuffer {
    WAVEFORMATEXTENSIBLE wfx = {};
    std::vector<uint8_t> pcmData;
};

struct Voice {
    uint32_t id = 0;
    std::atomic<VoiceState> state{ VoiceState::Stopped };
    std::string filename;
    bool loop = false;
    std::atomic<float> volume{ 1.0f };
    IXAudio2SourceVoice* sourceVoice = nullptr;
    std::shared_ptr<DecodedAudioBuffer> buffer;
    ULONGLONG startedTick = 0;
};

static IXAudio2* s_xaudio2 = nullptr;
static IXAudio2MasteringVoice* s_masteringVoice = nullptr;
static bool s_mfStarted = false;
static bool s_comInit = false;

static std::array<Voice, kMaxVoices> s_voices{};
static std::unordered_map<std::wstring, std::shared_ptr<DecodedAudioBuffer>> s_bufferCache;
static std::mutex s_mutex;
static std::atomic<uint32_t> s_nextVoiceId{ 1 };
static std::atomic<int> s_activeDecodes{ 0 };
static std::atomic<bool> s_shuttingDown{ false };

static std::wstring GetExeDirectory() {
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    PathRemoveFileSpecW(buf);
    return std::wstring(buf);
}

static std::wstring GetAudioDirectory() {
    return GetExeDirectory() + L"\\urushi_data\\audio\\";
}

static bool EqualNoCase(const std::wstring& a, const std::wstring& b) {
    return a.size() == b.size() && _wcsicmp(a.c_str(), b.c_str()) == 0;
}

static bool HasPrefixNoCase(const std::wstring& s, const std::wstring& prefix) {
    return s.size() >= prefix.size() && _wcsnicmp(s.c_str(), prefix.c_str(), prefix.size()) == 0;
}

static bool AllowedAudioExt(const std::wstring& name) {
    const size_t dot = name.rfind(L'.');
    if (dot == std::wstring::npos) return false;
    const std::wstring ext = name.substr(dot);
    return EqualNoCase(ext, L".wav") || EqualNoCase(ext, L".mp3") ||
           EqualNoCase(ext, L".ogg") || EqualNoCase(ext, L".flac") ||
           EqualNoCase(ext, L".m4a");
}

static bool ValidAudioComponent(const std::wstring& c) {
    if (c.empty() || c.size() > 128) return false;
    if (c[0] == L'.' || c.back() == L'.' || c.back() == L' ') return false;
    for (wchar_t ch : c) {
        if (ch < 0x20 || wcschr(L"<>\"|?*", ch)) return false;
    }
    std::wstring stem = c.substr(0, c.find(L'.'));
    while (!stem.empty() && stem.back() == L' ') stem.pop_back();
    static const wchar_t* kReserved[] = { L"CON", L"PRN", L"AUX", L"NUL" };
    for (const wchar_t* r : kReserved) if (EqualNoCase(stem, r)) return false;
    if (stem.size() == 4 && (HasPrefixNoCase(stem, L"COM") || HasPrefixNoCase(stem, L"LPT")) && stem[3] >= L'0' && stem[3] <= L'9') return false;
    return true;
}

static bool ResolveAudioPath(const std::string& pathUtf8, std::wstring* outFull) {
    if (pathUtf8.empty()) return false;
    std::wstring audioRoot = GetAudioDirectory();

    int len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, pathUtf8.c_str(), -1, nullptr, 0);
    if (len <= 1 || len > 260) return false;
    std::wstring w(static_cast<size_t>(len - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, pathUtf8.c_str(), -1, &w[0], len);

    for (wchar_t ch : w) {
        if (ch < 0x20 || wcschr(L"<>\"|?*", ch)) return false;
    }
    if (w.size() >= 2 && ((w[0] == L'\\' && w[1] == L'\\') || (w[0] == L'/' && w[1] == L'/'))) return false;
    if (w.find(L':') != std::wstring::npos) return false;
    if (w.find(L"..") != std::wstring::npos) return false;

    for (wchar_t& c : w) if (c == L'/') c = L'\\';

    std::wstring combined = audioRoot + w;
    wchar_t full[MAX_PATH * 2] = {};
    const DWORD fl = GetFullPathNameW(combined.c_str(), static_cast<DWORD>(sizeof(full) / sizeof(full[0])), full, nullptr);
    if (fl == 0 || fl >= sizeof(full) / sizeof(full[0])) return false;

    std::wstring f(full);
    if (!HasPrefixNoCase(f, audioRoot)) return false;

    std::wstring rest = f.substr(audioRoot.size());
    size_t start = 0;
    std::wstring last;
    int comps = 0;
    while (start <= rest.size()) {
        size_t slash = rest.find(L'\\', start);
        if (slash == std::wstring::npos) slash = rest.size();
        last = rest.substr(start, slash - start);
        if (!ValidAudioComponent(last) || ++comps > 4) return false;
        start = slash + 1;
    }

    if (!AllowedAudioExt(last)) return false;

    const DWORD attr = GetFileAttributesW(f.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) return false;
    if (attr & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) return false;

    if (outFull) *outFull = f;
    return true;
}

static std::shared_ptr<DecodedAudioBuffer> DecodeWithMediaFoundation(const std::wstring& filePath) {
    IMFSourceReader* pReader = nullptr;
    IMFAttributes* pAttributes = nullptr;
    HRESULT hr = MFCreateAttributes(&pAttributes, 1);
    if (SUCCEEDED(hr) && pAttributes) {
        pAttributes->SetUINT32(MF_LOW_LATENCY, TRUE);
    }
    hr = MFCreateSourceReaderFromURL(filePath.c_str(), pAttributes, &pReader);
    if (pAttributes) pAttributes->Release();
    if (FAILED(hr) || !pReader) {
        return nullptr;
    }

    pReader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    pReader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), TRUE);

    IMFMediaType* pPartialType = nullptr;
    MFCreateMediaType(&pPartialType);
    if (pPartialType) {
        pPartialType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        pPartialType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        hr = pReader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), nullptr, pPartialType);
        pPartialType->Release();
    }
    if (FAILED(hr)) {
        pReader->Release();
        return nullptr;
    }

    IMFMediaType* pUncompressedType = nullptr;
    hr = pReader->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), &pUncompressedType);
    if (FAILED(hr) || !pUncompressedType) {
        pReader->Release();
        return nullptr;
    }

    WAVEFORMATEX* pWfx = nullptr;
    UINT32 cbWfx = 0;
    hr = MFCreateWaveFormatExFromMFMediaType(pUncompressedType, &pWfx, &cbWfx);
    pUncompressedType->Release();
    if (FAILED(hr) || !pWfx) {
        pReader->Release();
        return nullptr;
    }

    auto result = std::make_shared<DecodedAudioBuffer>();
    memset(&result->wfx, 0, sizeof(result->wfx));
    memcpy(&result->wfx, pWfx, std::min<size_t>(cbWfx, sizeof(result->wfx)));
    CoTaskMemFree(pWfx);

    // Read all audio samples into PCM buffer
    for (;;) {
        DWORD flags = 0;
        IMFSample* pSample = nullptr;
        LONGLONG llTimeStamp = 0;
        hr = pReader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), 0, nullptr, &flags, &llTimeStamp, &pSample);
        if (FAILED(hr) || (flags & MF_SOURCE_READERF_ENDOFSTREAM)) {
            if (pSample) pSample->Release();
            break;
        }
        if (pSample) {
            IMFMediaBuffer* pBuffer = nullptr;
            if (SUCCEEDED(pSample->ConvertToContiguousBuffer(&pBuffer)) && pBuffer) {
                BYTE* pAudioBytes = nullptr;
                DWORD cbLen = 0;
                if (SUCCEEDED(pBuffer->Lock(&pAudioBytes, nullptr, &cbLen)) && pAudioBytes && cbLen > 0) {
                    const size_t cur = result->pcmData.size();
                    result->pcmData.resize(cur + cbLen);
                    memcpy(result->pcmData.data() + cur, pAudioBytes, cbLen);
                    pBuffer->Unlock();
                }
                pBuffer->Release();
            }
            pSample->Release();
        }
    }
    pReader->Release();

    if (result->pcmData.empty()) {
        return nullptr;
    }
    return result;
}

static Voice* FindVoice(uint32_t id) {
    if (id == 0) return nullptr;
    for (auto& v : s_voices) {
        if (v.id == id) return &v;
    }
    return nullptr;
}

static void CleanVoice(Voice& v) {
    v.state.store(VoiceState::Stopped);
    if (v.sourceVoice) {
        v.sourceVoice->Stop(0);
        v.sourceVoice->FlushSourceBuffers();
        v.sourceVoice->DestroyVoice();
        v.sourceVoice = nullptr;
    }
    v.buffer.reset();
}

} // namespace

bool Init() {
    std::lock_guard<std::mutex> lock(s_mutex);
    if (s_xaudio2) return true;

    s_shuttingDown.store(false);

    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    s_comInit = SUCCEEDED(hrCo);

    HRESULT hrMf = MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
    s_mfStarted = SUCCEEDED(hrMf);

    HRESULT hrX = XAudio2Create(&s_xaudio2, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hrX) || !s_xaudio2) {
        Logger::Error("AudioPlayer: Failed to initialize XAudio2 (hr=0x" + std::to_string(hrX) + ")");
        return false;
    }

    HRESULT hrM = s_xaudio2->CreateMasteringVoice(&s_masteringVoice);
    if (FAILED(hrM) || !s_masteringVoice) {
        Logger::Error("AudioPlayer: Failed to create XAudio2 mastering voice");
        s_xaudio2->Release();
        s_xaudio2 = nullptr;
        return false;
    }

    std::wstring dir = GetAudioDirectory();
    CreateDirectoryW(dir.c_str(), nullptr);

    Logger::Info("AudioPlayer initialized (XAudio2 + Media Foundation).");
    return true;
}

void Shutdown() {
    s_shuttingDown.store(true);
    StopAll();

    // Wait for any active decode background tasks to finish
    int spin = 0;
    while (s_activeDecodes.load() > 0 && spin < 100) {
        Sleep(20);
        spin++;
    }

    std::lock_guard<std::mutex> lock(s_mutex);
    for (auto& v : s_voices) {
        CleanVoice(v);
        v.id = 0;
    }
    s_bufferCache.clear();

    if (s_masteringVoice) {
        s_masteringVoice->DestroyVoice();
        s_masteringVoice = nullptr;
    }

    if (s_xaudio2) {
        s_xaudio2->Release();
        s_xaudio2 = nullptr;
    }

    if (s_mfStarted) {
        MFShutdown();
        s_mfStarted = false;
    }

    if (s_comInit) {
        CoUninitialize();
        s_comInit = false;
    }

    Logger::Info("AudioPlayer shut down.");
}

void OnLuaReload() {
    StopAll();
}

std::vector<std::string> ListAudioFiles() {
    std::vector<std::string> out;
    std::wstring dir = GetAudioDirectory();
    CreateDirectoryW(dir.c_str(), nullptr);

    std::wstring pat = dir + L"*";
    WIN32_FIND_DATAW fd = {};
    HANDLE hf = FindFirstFileW(pat.c_str(), &fd);
    if (hf != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) continue;
            std::wstring name(fd.cFileName);
            if (AllowedAudioExt(name)) {
                int len = WideCharToMultiByte(CP_UTF8, 0, name.c_str(), -1, nullptr, 0, nullptr, nullptr);
                if (len > 1) {
                    std::string narrow(len - 1, '\0');
                    WideCharToMultiByte(CP_UTF8, 0, name.c_str(), -1, &narrow[0], len, nullptr, nullptr);
                    out.push_back(std::move(narrow));
                }
            }
        } while (FindNextFileW(hf, &fd));
        FindClose(hf);
    }

    std::sort(out.begin(), out.end());
    return out;
}

uint32_t Play(const std::string& filenameUtf8, const PlayOptions& options, std::string* outError) {
    if (s_shuttingDown.load()) {
        if (outError) *outError = "Audio system is shutting down";
        return 0;
    }

    std::wstring fullPath;
    if (!ResolveAudioPath(filenameUtf8, &fullPath)) {
        if (outError) *outError = "File not allowed or not found in urushi_data/audio/";
        return 0;
    }

    std::lock_guard<std::mutex> lock(s_mutex);
    if (!s_xaudio2) {
        if (!Init()) {
            if (outError) *outError = "XAudio2 engine not available";
            return 0;
        }
    }

    // 1. Sweep finished voices to free slots
    for (auto& v : s_voices) {
        if (v.state.load() == VoiceState::Playing && v.sourceVoice) {
            XAUDIO2_VOICE_STATE st = {};
            v.sourceVoice->GetState(&st);
            if (st.BuffersQueued == 0 && !v.loop) {
                CleanVoice(v);
            }
        }
    }

    // 2. Find an available slot
    Voice* slot = nullptr;
    for (auto& v : s_voices) {
        if (v.state.load() == VoiceState::Stopped) {
            slot = &v;
            break;
        }
    }

    // If all slots are occupied, reclaim the oldest active voice
    if (!slot) {
        Voice* oldest = &s_voices[0];
        for (size_t i = 1; i < s_voices.size(); ++i) {
            if (s_voices[i].startedTick < oldest->startedTick) {
                oldest = &s_voices[i];
            }
        }
        CleanVoice(*oldest);
        slot = oldest;
    }

    const uint32_t voiceId = s_nextVoiceId.fetch_add(1);
    slot->id = voiceId;
    slot->filename = filenameUtf8;
    slot->loop = options.loop;
    slot->volume.store(std::clamp(options.volume, 0.0f, 1.0f));
    slot->startedTick = GetTickCount64();

    // 3. Fast path: buffer already decoded and cached in memory
    auto it = s_bufferCache.find(fullPath);
    if (it != s_bufferCache.end() && it->second && !it->second->pcmData.empty()) {
        slot->buffer = it->second;
        HRESULT hr = s_xaudio2->CreateSourceVoice(&slot->sourceVoice, reinterpret_cast<const WAVEFORMATEX*>(&slot->buffer->wfx));
        if (SUCCEEDED(hr) && slot->sourceVoice) {
            XAUDIO2_BUFFER xbuf = {};
            xbuf.AudioBytes = static_cast<UINT32>(slot->buffer->pcmData.size());
            xbuf.pAudioData = slot->buffer->pcmData.data();
            xbuf.Flags = XAUDIO2_END_OF_STREAM;
            if (slot->loop) xbuf.LoopCount = XAUDIO2_LOOP_INFINITE;
            slot->sourceVoice->SubmitSourceBuffer(&xbuf);
            slot->sourceVoice->SetVolume(slot->volume.load());
            slot->sourceVoice->Start(0);
            slot->state.store(VoiceState::Playing);
            return voiceId;
        } else {
            CleanVoice(*slot);
            if (outError) *outError = "Failed to create XAudio2 source voice";
            return 0;
        }
    }

    // 4. Background decoding path (Media Foundation runs off the render thread)
    slot->state.store(VoiceState::Loading);
    s_activeDecodes.fetch_add(1);

    std::thread([fullPath, voiceId]() {
        struct DecodeGuard {
            ~DecodeGuard() { s_activeDecodes.fetch_sub(1); }
        } guard;

        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        auto decoded = DecodeWithMediaFoundation(fullPath);
        CoUninitialize();

        std::lock_guard<std::mutex> lk(s_mutex);
        if (!s_xaudio2 || s_shuttingDown.load()) return;

        if (decoded && !decoded->pcmData.empty()) {
            s_bufferCache[fullPath] = decoded;
        }

        Voice* v = FindVoice(voiceId);
        if (!v || v->state.load() != VoiceState::Loading) {
            return; // Stopped or reused while decoding
        }

        if (!decoded || decoded->pcmData.empty()) {
            CleanVoice(*v);
            Logger::Error("AudioPlayer: Failed to decode audio file: " + Logger::ToNarrow(fullPath));
            return;
        }

        v->buffer = decoded;
        HRESULT hr = s_xaudio2->CreateSourceVoice(&v->sourceVoice, reinterpret_cast<const WAVEFORMATEX*>(&v->buffer->wfx));
        if (SUCCEEDED(hr) && v->sourceVoice) {
            XAUDIO2_BUFFER xbuf = {};
            xbuf.AudioBytes = static_cast<UINT32>(v->buffer->pcmData.size());
            xbuf.pAudioData = v->buffer->pcmData.data();
            xbuf.Flags = XAUDIO2_END_OF_STREAM;
            if (v->loop) xbuf.LoopCount = XAUDIO2_LOOP_INFINITE;
            v->sourceVoice->SubmitSourceBuffer(&xbuf);
            v->sourceVoice->SetVolume(v->volume.load());
            v->sourceVoice->Start(0);
            v->state.store(VoiceState::Playing);
        } else {
            CleanVoice(*v);
            Logger::Error("AudioPlayer: Failed to create source voice after async decode");
        }
    }).detach();

    return voiceId;
}

bool Stop(uint32_t voiceId) {
    std::lock_guard<std::mutex> lock(s_mutex);
    Voice* v = FindVoice(voiceId);
    if (!v || v->state.load() == VoiceState::Stopped) return false;
    CleanVoice(*v);
    return true;
}

void StopAll() {
    std::lock_guard<std::mutex> lock(s_mutex);
    for (auto& v : s_voices) {
        if (v.state.load() != VoiceState::Stopped) {
            CleanVoice(v);
        }
    }
}

bool SetVolume(uint32_t voiceId, float volume) {
    const float cl = std::clamp(volume, 0.0f, 1.0f);
    std::lock_guard<std::mutex> lock(s_mutex);
    Voice* v = FindVoice(voiceId);
    if (!v || v->state.load() == VoiceState::Stopped) return false;
    v->volume.store(cl);
    if (v->sourceVoice) {
        // XAudio2 smoothly interpolates volume changes per audio quantum: click-free!
        v->sourceVoice->SetVolume(cl);
    }
    return true;
}

bool IsPlaying(uint32_t voiceId) {
    std::lock_guard<std::mutex> lock(s_mutex);
    Voice* v = FindVoice(voiceId);
    if (!v) return false;
    if (v->state.load() == VoiceState::Loading) return true;
    if (v->state.load() == VoiceState::Playing) {
        if (v->sourceVoice) {
            XAUDIO2_VOICE_STATE st = {};
            v->sourceVoice->GetState(&st);
            if (st.BuffersQueued > 0) return true;
            CleanVoice(*v);
        } else {
            v->state.store(VoiceState::Stopped);
        }
    }
    return false;
}

} // namespace AudioPlayer
