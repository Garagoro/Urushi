#include "audio_capture.h"
#include "logger.h"
#include "safe_mode.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <shlwapi.h>

#include <cmath>
#include <atomic>
#include <vector>
#include <string>
#include <algorithm>
#include <memory>

#ifndef VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK
#define VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK L"VAD\\Process_Loopback"
#endif

#ifndef AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK
enum AUDIOCLIENT_ACTIVATION_TYPE {
    AUDIOCLIENT_ACTIVATION_TYPE_DEFAULT = 0,
    AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK = 1
};
enum PROCESS_LOOPBACK_MODE {
    PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE = 0,
    PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE = 1
};
struct AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS {
    DWORD TargetProcessId;
    PROCESS_LOOPBACK_MODE ProcessLoopbackMode;
};
struct AUDIOCLIENT_ACTIVATION_PARAMS {
    AUDIOCLIENT_ACTIVATION_TYPE ActivationType;
    union {
        AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS ProcessLoopbackParams;
    };
};
#endif

typedef HRESULT (WINAPI *PFN_ActivateAudioInterfaceAsync)(
    LPCWSTR deviceInterfacePath,
    REFIID riid,
    PROPVARIANT *activationParams,
    IActivateAudioInterfaceCompletionHandler *completionHandler,
    IActivateAudioInterfaceAsyncOperation **activationOperation
);

namespace AudioCapture {

namespace {

constexpr int kFftSize = 1024;
constexpr int kFftHop = 512;
constexpr float kPi = 3.14159265358979323846f;

struct FrameSnapshot {
    Levels levels{};
    float spectrum[32] = {};
    bool active = false;
};

// Global state
std::atomic<bool> g_initialized{ false };
std::atomic<bool> g_enabled{ false };
std::atomic<bool> g_threadRunning{ false };
HANDLE g_hThread = nullptr;
HANDLE g_hWakeEvent = nullptr;

SRWLOCK g_sourceLock = SRWLOCK_INIT;
std::string g_targetSource = "system";

SRWLOCK g_statusLock = SRWLOCK_INIT;
std::string g_statusState = "off";
std::string g_statusMessage = "Capture is disabled";

SRWLOCK g_dataLock = SRWLOCK_INIT;
FrameSnapshot g_snapshot{};

std::atomic<float> g_gain{ 1.0f };
std::atomic<float> g_attack{ 0.65f };
std::atomic<float> g_release{ 0.18f };

// Activation completion handler for ActivateAudioInterfaceAsync
class AudioActivationHandler : public IActivateAudioInterfaceCompletionHandler {
public:
    AudioActivationHandler() : m_ref(1), m_hEvent(CreateEventW(nullptr, FALSE, FALSE, nullptr)) {}
    ~AudioActivationHandler() {
        if (m_hEvent) CloseHandle(m_hEvent);
        if (m_client) { m_client->Release(); m_client = nullptr; }
    }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IActivateAudioInterfaceCompletionHandler)) {
            *ppv = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override {
        return InterlockedIncrement(&m_ref);
    }

    STDMETHODIMP_(ULONG) Release() override {
        ULONG r = InterlockedDecrement(&m_ref);
        if (r == 0) delete this;
        return r;
    }

    STDMETHODIMP ActivateCompleted(IActivateAudioInterfaceAsyncOperation* operation) override {
        HRESULT hrActivate = E_FAIL;
        IUnknown* punk = nullptr;
        if (operation) {
            HRESULT hr = operation->GetActivateResult(&hrActivate, &punk);
            if (SUCCEEDED(hr) && SUCCEEDED(hrActivate) && punk) {
                punk->QueryInterface(IID_PPV_ARGS(&m_client));
                punk->Release();
            }
        }
        m_hr = SUCCEEDED(hrActivate) ? S_OK : hrActivate;
        SetEvent(m_hEvent);
        return S_OK;
    }

    HANDLE m_hEvent = nullptr;
    IAudioClient* m_client = nullptr;
    HRESULT m_hr = E_FAIL;
    volatile LONG m_ref = 1;
};

// Helper: case-insensitive string equality
bool EqualsIgnoreCase(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (tolower(static_cast<unsigned char>(a[i])) != tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

std::string ToLower(const std::string& str) {
    std::string res = str;
    for (char& c : res) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return res;
}

PFN_ActivateAudioInterfaceAsync GetActivateAudioInterfaceAsync() {
    static PFN_ActivateAudioInterfaceAsync s_pfn = nullptr;
    static bool s_checked = false;
    if (!s_checked) {
        HMODULE hMod = GetModuleHandleW(L"mmdevapi.dll");
        if (!hMod) hMod = LoadLibraryW(L"mmdevapi.dll");
        if (hMod) {
            s_pfn = reinterpret_cast<PFN_ActivateAudioInterfaceAsync>(GetProcAddress(hMod, "ActivateAudioInterfaceAsync"));
        }
        s_checked = true;
    }
    return s_pfn;
}

// In-place Radix-2 Cooley-Tukey FFT (N = 1024)
void FFT(float* re, float* im, int n) {
    // Bit reversal
    int j = 0;
    for (int i = 0; i < n - 1; ++i) {
        if (i < j) {
            std::swap(re[i], re[j]);
            std::swap(im[i], im[j]);
        }
        int k = n >> 1;
        while (k <= j) {
            j -= k;
            k >>= 1;
        }
        j += k;
    }

    // Butterfly stages
    for (int len = 2; len <= n; len <<= 1) {
        float angle = -2.0f * kPi / len;
        float wlen_r = cosf(angle);
        float wlen_i = sinf(angle);
        int halfLen = len >> 1;
        for (int i = 0; i < n; i += len) {
            float w_r = 1.0f;
            float w_i = 0.0f;
            for (int k = 0; k < halfLen; ++k) {
                float u_r = re[i + k];
                float u_i = im[i + k];
                float v_r = re[i + k + halfLen] * w_r - im[i + k + halfLen] * w_i;
                float v_i = re[i + k + halfLen] * w_i + im[i + k + halfLen] * w_r;
                re[i + k] = u_r + v_r;
                im[i + k] = u_i + v_i;
                re[i + k + halfLen] = u_r - v_r;
                im[i + k + halfLen] = u_i - v_i;
                float next_w_r = w_r * wlen_r - w_i * wlen_i;
                w_i = w_r * wlen_i + w_i * wlen_r;
                w_r = next_w_r;
            }
        }
    }
}

// Extract exe name from PID
std::string GetExeNameFromPid(DWORD pid) {
    if (pid == 0) return "System";
    std::string result;
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (hProcess) {
        wchar_t fullPath[MAX_PATH] = {};
        DWORD size = MAX_PATH;
        if (QueryFullProcessImageNameW(hProcess, 0, fullPath, &size)) {
            wchar_t* fname = PathFindFileNameW(fullPath);
            if (fname) {
                char buf[MAX_PATH] = {};
                WideCharToMultiByte(CP_UTF8, 0, fname, -1, buf, sizeof(buf), nullptr, nullptr);
                result = buf;
            }
        }
        CloseHandle(hProcess);
    }
    return result;
}

// Find PID for executable name
DWORD FindPidForExe(const std::string& targetExe) {
    if (targetExe.empty() || EqualsIgnoreCase(targetExe, "system")) return 0;

    std::string normTarget = ToLower(targetExe);
    if (normTarget.size() < 4 || normTarget.substr(normTarget.size() - 4) != ".exe") {
        normTarget += ".exe";
    }

    // 1. First check active audio sessions
    IMMDeviceEnumerator* pEnum = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&pEnum)))) {
        IMMDevice* pDev = nullptr;
        if (SUCCEEDED(pEnum->GetDefaultAudioEndpoint(eRender, eMultimedia, &pDev))) {
            IAudioSessionManager2* pMgr = nullptr;
            if (SUCCEEDED(pDev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&pMgr)))) {
                IAudioSessionEnumerator* pSessions = nullptr;
                if (SUCCEEDED(pMgr->GetSessionEnumerator(&pSessions))) {
                    int count = 0;
                    pSessions->GetCount(&count);
                    for (int i = 0; i < count; ++i) {
                        IAudioSessionControl* pCtrl = nullptr;
                        if (SUCCEEDED(pSessions->GetSession(i, &pCtrl))) {
                            IAudioSessionControl2* pCtrl2 = nullptr;
                            if (SUCCEEDED(pCtrl->QueryInterface(IID_PPV_ARGS(&pCtrl2)))) {
                                DWORD pid = 0;
                                pCtrl2->GetProcessId(&pid);
                                if (pid > 0) {
                                    std::string exe = GetExeNameFromPid(pid);
                                    if (EqualsIgnoreCase(ToLower(exe), normTarget)) {
                                        pCtrl2->Release();
                                        pCtrl->Release();
                                        pSessions->Release();
                                        pMgr->Release();
                                        pDev->Release();
                                        pEnum->Release();
                                        return pid;
                                    }
                                }
                                pCtrl2->Release();
                            }
                            pCtrl->Release();
                        }
                    }
                    pSessions->Release();
                }
                pMgr->Release();
            }
            pDev->Release();
        }
        pEnum->Release();
    }

    // 2. Fallback: check running processes snapshot
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe = {};
        pe.dwSize = sizeof(pe);
        if (Process32FirstW(hSnap, &pe)) {
            do {
                char exeName[MAX_PATH] = {};
                WideCharToMultiByte(CP_UTF8, 0, pe.szExeFile, -1, exeName, sizeof(exeName), nullptr, nullptr);
                if (EqualsIgnoreCase(ToLower(exeName), normTarget)) {
                    CloseHandle(hSnap);
                    return pe.th32ProcessID;
                }
            } while (Process32NextW(hSnap, &pe));
        }
        CloseHandle(hSnap);
    }

    return 0;
}

// Convert arbitrary audio format buffer to mono float samples [-1.0f, 1.0f]
void DownmixToMono(const BYTE* pData, UINT32 numFrames, const WAVEFORMATEX* pwfx, std::vector<float>& outSamples) {
    if (!pData || numFrames == 0 || !pwfx) return;

    const WORD channels = pwfx->nChannels;
    if (channels == 0) return;

    outSamples.resize(numFrames);

    bool isFloat = false;
    WORD bitsPerSample = pwfx->wBitsPerSample;

    if (pwfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        isFloat = true;
    } else if (pwfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const auto* pExt = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(pwfx);
        if (pExt->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) {
            isFloat = true;
        }
    }

    const UINT bytesPerSample = bitsPerSample / 8;
    const UINT bytesPerFrame = pwfx->nBlockAlign;

    for (UINT32 i = 0; i < numFrames; ++i) {
        const BYTE* framePtr = pData + i * bytesPerFrame;
        float sum = 0.0f;

        for (WORD c = 0; c < channels; ++c) {
            const BYTE* samplePtr = framePtr + c * bytesPerSample;
            float s = 0.0f;

            if (isFloat && bitsPerSample == 32) {
                s = *reinterpret_cast<const float*>(samplePtr);
            } else if (!isFloat && bitsPerSample == 16) {
                int16_t val = *reinterpret_cast<const int16_t*>(samplePtr);
                s = val / 32768.0f;
            } else if (!isFloat && bitsPerSample == 24) {
                int32_t val = (static_cast<int32_t>(samplePtr[0]) << 8) |
                              (static_cast<int32_t>(samplePtr[1]) << 16) |
                              (static_cast<int32_t>(static_cast<int8_t>(samplePtr[2])) << 24);
                s = val / 2147483648.0f;
            } else if (!isFloat && bitsPerSample == 32) {
                int32_t val = *reinterpret_cast<const int32_t*>(samplePtr);
                s = val / 2147483648.0f;
            }

            sum += s;
        }

        outSamples[i] = sum / static_cast<float>(channels);
    }
}

void UpdateStatus(const std::string& state, const std::string& message) {
    AcquireSRWLockExclusive(&g_statusLock);
    g_statusState = state;
    g_statusMessage = message;
    ReleaseSRWLockExclusive(&g_statusLock);
}

// Background capture worker thread
DWORD WINAPI AudioCaptureThread(LPVOID) {
    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    std::vector<float> ringBuffer(kFftSize * 4, 0.0f);
    int ringHead = 0;
    int ringCount = 0;
    int samplesSinceFft = 0;

    float smoothedLevel = 0.0f;
    float smoothedBass = 0.0f;
    float smoothedLowMid = 0.0f;
    float smoothedHighMid = 0.0f;
    float smoothedMid = 0.0f;
    float smoothedTreble = 0.0f;
    float smoothedSpectrum[32] = {};
    float beatPulse = 0.0f;
    float fluxAvg = 0.05f;
    float prevBass = 0.0f;
    ULONGLONG lastBeatTick = 0;

    float peakLevel = 0.05f;

    ULONGLONG lastPacketTick = GetTickCount64();

    while (g_threadRunning.load(std::memory_order_relaxed)) {
        if (!g_enabled.load(std::memory_order_relaxed)) {
            UpdateStatus("off", "Audio capture is disabled");
            WaitForSingleObject(g_hWakeEvent, 200);
            continue;
        }

        std::string curSource;
        {
            AcquireSRWLockShared(&g_sourceLock);
            curSource = g_targetSource;
            ReleaseSRWLockShared(&g_sourceLock);
        }

        const bool isSystem = EqualsIgnoreCase(curSource, "system");
        DWORD targetPid = 0;

        if (!isSystem) {
            targetPid = FindPidForExe(curSource);
            if (targetPid == 0) {
                UpdateStatus("waiting", "Waiting for " + curSource + "...");
                // Decay active audio values while waiting
                {
                    AcquireSRWLockExclusive(&g_dataLock);
                    g_snapshot = FrameSnapshot{};
                    ReleaseSRWLockExclusive(&g_dataLock);
                }
                WaitForSingleObject(g_hWakeEvent, 1500);
                continue;
            }
        }

        // Initialize capture interface
        IAudioClient* pAudioClient = nullptr;
        HANDLE hTargetProcess = nullptr;

        if (isSystem) {
            IMMDeviceEnumerator* pEnum = nullptr;
            if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&pEnum)))) {
                IMMDevice* pDev = nullptr;
                if (SUCCEEDED(pEnum->GetDefaultAudioEndpoint(eRender, eMultimedia, &pDev))) {
                    pDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&pAudioClient));
                    pDev->Release();
                }
                pEnum->Release();
            }
        } else {
            PFN_ActivateAudioInterfaceAsync pfnActivate = GetActivateAudioInterfaceAsync();
            if (!pfnActivate) {
                UpdateStatus("error", "Process loopback API not available on this Windows build");
                WaitForSingleObject(g_hWakeEvent, 2000);
                continue;
            }

            hTargetProcess = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, targetPid);

            auto* handler = new AudioActivationHandler();
            PROPVARIANT activateParams = {};
            activateParams.vt = VT_BLOB;
            AUDIOCLIENT_ACTIVATION_PARAMS params = {};
            params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
            params.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
            params.ProcessLoopbackParams.TargetProcessId = targetPid;
            activateParams.blob.cbSize = sizeof(params);
            activateParams.blob.pBlobData = reinterpret_cast<BYTE*>(&params);

            IActivateAudioInterfaceAsyncOperation* pAsyncOp = nullptr;
            HRESULT hrAct = pfnActivate(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), &activateParams, handler, &pAsyncOp);
            if (SUCCEEDED(hrAct)) {
                WaitForSingleObject(handler->m_hEvent, 2000);
                if (SUCCEEDED(handler->m_hr) && handler->m_client) {
                    pAudioClient = handler->m_client;
                    pAudioClient->AddRef();
                }
            }

            if (pAsyncOp) pAsyncOp->Release();
            handler->Release();
        }

        if (!pAudioClient) {
            if (hTargetProcess) CloseHandle(hTargetProcess);
            UpdateStatus("waiting", isSystem ? "Failed to access default render device" : ("Waiting to attach to " + curSource + "..."));
            WaitForSingleObject(g_hWakeEvent, 1500);
            continue;
        }

        WAVEFORMATEX* pwfx = nullptr;
        HRESULT hr = pAudioClient->GetMixFormat(&pwfx);
        if (FAILED(hr) || !pwfx) {
            pAudioClient->Release();
            if (hTargetProcess) CloseHandle(hTargetProcess);
            UpdateStatus("error", "Failed to retrieve audio format");
            WaitForSingleObject(g_hWakeEvent, 1000);
            continue;
        }

        const DWORD sampleRate = pwfx->nSamplesPerSec > 0 ? pwfx->nSamplesPerSec : 48000;
        constexpr REFERENCE_TIME kBufferDuration = 1000000; // 100 ms

        hr = pAudioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, kBufferDuration, 0, pwfx, nullptr);
        if (FAILED(hr)) {
            CoTaskMemFree(pwfx);
            pAudioClient->Release();
            if (hTargetProcess) CloseHandle(hTargetProcess);
            UpdateStatus("error", "Failed to initialize audio client");
            WaitForSingleObject(g_hWakeEvent, 1000);
            continue;
        }

        IAudioCaptureClient* pCapture = nullptr;
        hr = pAudioClient->GetService(IID_PPV_ARGS(&pCapture));
        if (FAILED(hr) || !pCapture) {
            CoTaskMemFree(pwfx);
            pAudioClient->Release();
            if (hTargetProcess) CloseHandle(hTargetProcess);
            UpdateStatus("error", "Failed to obtain capture client");
            WaitForSingleObject(g_hWakeEvent, 1000);
            continue;
        }

        hr = pAudioClient->Start();
        if (FAILED(hr)) {
            pCapture->Release();
            CoTaskMemFree(pwfx);
            pAudioClient->Release();
            if (hTargetProcess) CloseHandle(hTargetProcess);
            UpdateStatus("error", "Failed to start capture stream");
            WaitForSingleObject(g_hWakeEvent, 1000);
            continue;
        }

        if (isSystem) {
            UpdateStatus("capturing", "Capturing whole system audio");
        } else {
            UpdateStatus("capturing", "Capturing " + curSource + " (PID " + std::to_string(targetPid) + ")");
        }

        std::vector<float> monoChunk;
        bool streamActive = true;

        while (streamActive && g_threadRunning.load(std::memory_order_relaxed) && g_enabled.load(std::memory_order_relaxed)) {
            // Check if source selection changed
            {
                AcquireSRWLockShared(&g_sourceLock);
                if (!EqualsIgnoreCase(g_targetSource, curSource)) {
                    streamActive = false;
                }
                ReleaseSRWLockShared(&g_sourceLock);
            }
            if (!streamActive) break;

            // Check if target process terminated
            if (hTargetProcess && WaitForSingleObject(hTargetProcess, 0) == WAIT_OBJECT_0) {
                Logger::Info("AudioCapture: target process exited, re-attaching...");
                streamActive = false;
                break;
            }

            UINT32 packetFrames = 0;
            hr = pCapture->GetNextPacketSize(&packetFrames);
            if (FAILED(hr)) {
                // Device invalidated or audio service stopped
                streamActive = false;
                break;
            }

            bool receivedData = false;
            while (packetFrames > 0) {
                BYTE* pData = nullptr;
                UINT32 numFrames = 0;
                DWORD flags = 0;
                hr = pCapture->GetBuffer(&pData, &numFrames, &flags, nullptr, nullptr);
                if (SUCCEEDED(hr)) {
                    if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                        monoChunk.assign(numFrames, 0.0f);
                    } else {
                        DownmixToMono(pData, numFrames, pwfx, monoChunk);
                    }

                    for (float s : monoChunk) {
                        ringBuffer[ringHead] = s;
                        ringHead = (ringHead + 1) % ringBuffer.size();
                        if (ringCount < static_cast<int>(ringBuffer.size())) ringCount++;
                        samplesSinceFft++;
                    }

                    pCapture->ReleaseBuffer(numFrames);
                    receivedData = true;
                    lastPacketTick = GetTickCount64();
                }
                pCapture->GetNextPacketSize(&packetFrames);
            }

            // If silence for > 60 ms, slowly decay levels to 0
            const ULONGLONG now = GetTickCount64();
            if (!receivedData && (now - lastPacketTick > 60)) {
                smoothedLevel *= 0.85f;
                smoothedBass *= 0.85f;
                smoothedLowMid *= 0.85f;
                smoothedHighMid *= 0.85f;
                smoothedMid *= 0.85f;
                smoothedTreble *= 0.85f;
                beatPulse *= 0.80f;
                for (float& b : smoothedSpectrum) b *= 0.85f;

                AcquireSRWLockExclusive(&g_dataLock);
                g_snapshot.levels.level = smoothedLevel;
                g_snapshot.levels.bass = smoothedBass;
                g_snapshot.levels.lowMid = smoothedLowMid;
                g_snapshot.levels.highMid = smoothedHighMid;
                g_snapshot.levels.mid = smoothedMid;
                g_snapshot.levels.treble = smoothedTreble;
                g_snapshot.levels.beat = beatPulse;
                memcpy(g_snapshot.spectrum, smoothedSpectrum, sizeof(smoothedSpectrum));
                g_snapshot.active = true;
                ReleaseSRWLockExclusive(&g_dataLock);
            }

            // Run FFT whenever we have accumulated enough new samples
            if (ringCount >= kFftSize && samplesSinceFft >= kFftHop) {
                samplesSinceFft = 0;

                float fftRe[kFftSize];
                float fftIm[kFftSize] = {};

                // Copy latest 1024 samples from ring buffer with Hann window
                const int readStart = (ringHead - kFftSize + static_cast<int>(ringBuffer.size())) % ringBuffer.size();
                float sumSq = 0.0f;
                for (int i = 0; i < kFftSize; ++i) {
                    float s = ringBuffer[(readStart + i) % ringBuffer.size()];
                    sumSq += s * s;
                    float w = 0.5f * (1.0f - cosf(2.0f * kPi * i / (kFftSize - 1)));
                    fftRe[i] = s * w;
                }

                float rms = sqrtf(sumSq / static_cast<float>(kFftSize));

                // Execute FFT
                FFT(fftRe, fftIm, kFftSize);

                // Compute magnitudes
                float mag[kFftSize / 2 + 1];
                for (int i = 0; i <= kFftSize / 2; ++i) {
                    mag[i] = (2.0f / static_cast<float>(kFftSize)) * sqrtf(fftRe[i] * fftRe[i] + fftIm[i] * fftIm[i]);
                }

                const float binWidth = static_cast<float>(sampleRate) / static_cast<float>(kFftSize);

                // Calculate raw energy bands
                auto computeBandEnergy = [&](float minF, float maxF) -> float {
                    int kMin = (std::max)(1, static_cast<int>(minF / binWidth));
                    int kMax = (std::min)(kFftSize / 2, static_cast<int>(maxF / binWidth));
                    if (kMin > kMax) kMax = kMin;
                    float sum = 0.0f;
                    int cnt = 0;
                    for (int k = kMin; k <= kMax; ++k) {
                        sum += mag[k];
                        cnt++;
                    }
                    return cnt > 0 ? (sum / cnt) : 0.0f;
                };

                float rawBass = computeBandEnergy(20.0f, 250.0f);
                float rawLowMid = computeBandEnergy(250.0f, 1000.0f);
                float rawHighMid = computeBandEnergy(1000.0f, 4000.0f);
                float rawMid = computeBandEnergy(250.0f, 4000.0f);
                float rawTreble = computeBandEnergy(4000.0f, 16000.0f);

                // Compute 32 log-spaced spectrum bands (40 Hz to 16 kHz)
                constexpr float fMin = 40.0f;
                constexpr float fMax = 16000.0f;
                const float logRatio = logf(fMax / fMin);
                float rawSpectrum[32] = {};

                for (int b = 0; b < 32; ++b) {
                    float bStartF = fMin * expf(logRatio * (static_cast<float>(b) / 32.0f));
                    float bEndF = fMin * expf(logRatio * (static_cast<float>(b + 1) / 32.0f));
                    rawSpectrum[b] = computeBandEnergy(bStartF, bEndF);
                }

                // Auto-gain control (AGC)
                if (rms > peakLevel) {
                    peakLevel = rms;
                } else {
                    peakLevel = peakLevel * 0.999f + 0.001f * 0.02f;
                }
                if (peakLevel < 0.02f) peakLevel = 0.02f;

                const float userGain = g_gain.load(std::memory_order_relaxed);
                const float agcFactor = (1.0f / peakLevel) * userGain;

                auto normalizeVal = [&](float val) -> float {
                    if (rms < 0.002f) return 0.0f;
                    float v = val * agcFactor;
                    return (std::min)(1.0f, (std::max)(0.0f, v));
                };

                float targetLevel = normalizeVal(rms * 1.5f);
                float targetBass = normalizeVal(rawBass * 2.2f);
                float targetLowMid = normalizeVal(rawLowMid * 2.5f);
                float targetHighMid = normalizeVal(rawHighMid * 3.0f);
                float targetMid = normalizeVal(rawMid * 2.6f);
                float targetTreble = normalizeVal(rawTreble * 3.8f);

                // Beat pulse detection (energy-flux onset detector on bass band)
                float deltaBass = (std::max)(0.0f, targetBass - prevBass);
                prevBass = targetBass;
                fluxAvg = fluxAvg * 0.90f + deltaBass * 0.10f;

                const float beatThreshold = (std::max)(fluxAvg * 1.35f, 0.07f);
                if (deltaBass > beatThreshold && targetBass > 0.14f && (now - lastBeatTick > 150)) {
                    beatPulse = 1.0f;
                    lastBeatTick = now;
                } else {
                    beatPulse *= 0.88f;
                    if (beatPulse < 0.01f) beatPulse = 0.0f;
                }

                // Attack / release smoothing
                const float att = g_attack.load(std::memory_order_relaxed);
                const float rel = g_release.load(std::memory_order_relaxed);

                auto smooth = [&](float cur, float tgt) -> float {
                    float factor = (tgt > cur) ? att : rel;
                    float res = cur + factor * (tgt - cur);
                    if (res < 0.001f) res = 0.0f;
                    return (std::min)(1.0f, (std::max)(0.0f, res));
                };

                smoothedLevel = smooth(smoothedLevel, targetLevel);
                smoothedBass = smooth(smoothedBass, targetBass);
                smoothedLowMid = smooth(smoothedLowMid, targetLowMid);
                smoothedHighMid = smooth(smoothedHighMid, targetHighMid);
                smoothedMid = smooth(smoothedMid, targetMid);
                smoothedTreble = smooth(smoothedTreble, targetTreble);

                for (int b = 0; b < 32; ++b) {
                    float tgt = normalizeVal(rawSpectrum[b] * 2.5f);
                    smoothedSpectrum[b] = smooth(smoothedSpectrum[b], tgt);
                }

                // Publish snapshot for readers
                AcquireSRWLockExclusive(&g_dataLock);
                g_snapshot.levels.level = smoothedLevel;
                g_snapshot.levels.bass = smoothedBass;
                g_snapshot.levels.lowMid = smoothedLowMid;
                g_snapshot.levels.highMid = smoothedHighMid;
                g_snapshot.levels.mid = smoothedMid;
                g_snapshot.levels.treble = smoothedTreble;
                g_snapshot.levels.beat = beatPulse;
                memcpy(g_snapshot.spectrum, smoothedSpectrum, sizeof(smoothedSpectrum));
                g_snapshot.active = true;
                ReleaseSRWLockExclusive(&g_dataLock);
            }

            Sleep(5);
        }

        // Clean up current audio stream
        pAudioClient->Stop();
        pCapture->Release();
        CoTaskMemFree(pwfx);
        pAudioClient->Release();
        if (hTargetProcess) {
            CloseHandle(hTargetProcess);
            hTargetProcess = nullptr;
        }

        ringHead = 0;
        ringCount = 0;
        samplesSinceFft = 0;

        // Zero out data when stream ends
        AcquireSRWLockExclusive(&g_dataLock);
        g_snapshot = FrameSnapshot{};
        ReleaseSRWLockExclusive(&g_dataLock);

        if (g_enabled.load(std::memory_order_relaxed)) {
            WaitForSingleObject(g_hWakeEvent, 1000);
        }
    }

    if (SUCCEEDED(hrCo)) CoUninitialize();
    return 0;
}

} // namespace

void Init() {
    if (g_initialized.exchange(true)) return;

    if (!SafeMode::IsFull()) {
        Logger::Info("AudioCapture: Safe mode active, capture disabled.");
        return;
    }

    g_hWakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_threadRunning.store(true);

    g_hThread = CreateThread(nullptr, 0, AudioCaptureThread, nullptr, 0, nullptr);
    if (!g_hThread) {
        Logger::Error("AudioCapture: failed to spawn capture thread.");
        g_threadRunning.store(false);
        return;
    }

    Logger::Info("AudioCapture: initialized (capture OFF by default).");
}

void Shutdown() {
    if (!g_initialized.exchange(false)) return;

    g_enabled.store(false);
    g_threadRunning.store(false);

    if (g_hWakeEvent) {
        SetEvent(g_hWakeEvent);
    }

    if (g_hThread) {
        WaitForSingleObject(g_hThread, 2500);
        CloseHandle(g_hThread);
        g_hThread = nullptr;
    }

    if (g_hWakeEvent) {
        CloseHandle(g_hWakeEvent);
        g_hWakeEvent = nullptr;
    }

    AcquireSRWLockExclusive(&g_dataLock);
    g_snapshot = FrameSnapshot{};
    ReleaseSRWLockExclusive(&g_dataLock);

    UpdateStatus("off", "Capture shut down");
    Logger::Info("AudioCapture: shut down cleanly.");
}

bool IsProcessLoopbackSupported() {
    // Requires Windows 10 2004+ (Build >= 19041) and ActivateAudioInterfaceAsync
    PFN_ActivateAudioInterfaceAsync pfn = GetActivateAudioInterfaceAsync();
    return pfn != nullptr;
}

std::vector<SourceInfo> ListSources() {
    std::vector<SourceInfo> result;
    const DWORD currentPid = GetCurrentProcessId();

    struct ComScope {
        bool didInit = false;
        ComScope() {
            HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            didInit = (hr == S_OK || hr == S_FALSE);
        }
        ~ComScope() {
            if (didInit) CoUninitialize();
        }
    } comScope;

    IMMDeviceEnumerator* pEnum = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&pEnum)))) {
        return result;
    }

    IMMDevice* pDev = nullptr;
    if (FAILED(pEnum->GetDefaultAudioEndpoint(eRender, eMultimedia, &pDev))) {
        pEnum->Release();
        return result;
    }

    IAudioSessionManager2* pMgr = nullptr;
    if (FAILED(pDev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&pMgr)))) {
        pDev->Release();
        pEnum->Release();
        return result;
    }

    IAudioSessionEnumerator* pSessions = nullptr;
    if (SUCCEEDED(pMgr->GetSessionEnumerator(&pSessions))) {
        int count = 0;
        pSessions->GetCount(&count);

        for (int i = 0; i < count; ++i) {
            IAudioSessionControl* pCtrl = nullptr;
            if (SUCCEEDED(pSessions->GetSession(i, &pCtrl))) {
                IAudioSessionControl2* pCtrl2 = nullptr;
                if (SUCCEEDED(pCtrl->QueryInterface(IID_PPV_ARGS(&pCtrl2)))) {
                    DWORD pid = 0;
                    pCtrl2->GetProcessId(&pid);

                    if (pid > 0) {
                        std::string exe = GetExeNameFromPid(pid);
                        if (!exe.empty()) {
                            AudioSessionState state;
                            pCtrl->GetState(&state);

                            LPWSTR wDisp = nullptr;
                            pCtrl->GetDisplayName(&wDisp);
                            std::string disp;
                            if (wDisp && wDisp[0] != L'\0') {
                                char buf[MAX_PATH] = {};
                                WideCharToMultiByte(CP_UTF8, 0, wDisp, -1, buf, sizeof(buf), nullptr, nullptr);
                                disp = buf;
                            } else {
                                disp = exe;
                                // Strip .exe extension if present for cleaner display
                                if (disp.size() > 4 && EqualsIgnoreCase(disp.substr(disp.size() - 4), ".exe")) {
                                    disp = disp.substr(0, disp.size() - 4);
                                }
                            }
                            if (wDisp) CoTaskMemFree(wDisp);

                            const bool isActive = (state == AudioSessionStateActive);
                            const bool isGame = (pid == currentPid);

                            // Check if already listed
                            bool found = false;
                            for (auto& s : result) {
                                if (EqualsIgnoreCase(s.exeName, exe)) {
                                    if (isActive) s.isActive = true;
                                    found = true;
                                    break;
                                }
                            }

                            if (!found) {
                                SourceInfo info;
                                info.pid = pid;
                                info.exeName = exe;
                                info.displayName = disp;
                                info.isActive = isActive;
                                info.isGame = isGame;
                                result.push_back(info);
                            }
                        }
                    }
                    pCtrl2->Release();
                }
                pCtrl->Release();
            }
        }
        pSessions->Release();
    }

    pMgr->Release();
    pDev->Release();
    pEnum->Release();

    // Sort: active first, then game, then alphabetically
    std::sort(result.begin(), result.end(), [](const SourceInfo& a, const SourceInfo& b) {
        if (a.isActive != b.isActive) return a.isActive > b.isActive;
        if (a.isGame != b.isGame) return a.isGame > b.isGame;
        return a.displayName < b.displayName;
    });

    return result;
}

bool SetSource(const std::string& sourceName) {
    std::string s = sourceName;
    if (s.empty()) s = "system";

    AcquireSRWLockExclusive(&g_sourceLock);
    g_targetSource = s;
    ReleaseSRWLockExclusive(&g_sourceLock);

    if (g_hWakeEvent) SetEvent(g_hWakeEvent);
    return true;
}

std::string GetSource() {
    AcquireSRWLockShared(&g_sourceLock);
    std::string s = g_targetSource;
    ReleaseSRWLockShared(&g_sourceLock);
    return s;
}

void SetEnabled(bool enabled) {
    if (!SafeMode::IsFull() && enabled) return;

    g_enabled.store(enabled, std::memory_order_release);
    if (!enabled) {
        AcquireSRWLockExclusive(&g_dataLock);
        g_snapshot = FrameSnapshot{};
        ReleaseSRWLockExclusive(&g_dataLock);
        UpdateStatus("off", "Capture is disabled");
    }
    if (g_hWakeEvent) SetEvent(g_hWakeEvent);
}

bool IsEnabled() {
    return g_enabled.load(std::memory_order_acquire);
}

void GetStatus(std::string* outState, std::string* outMessage) {
    AcquireSRWLockShared(&g_statusLock);
    if (outState) *outState = g_statusState;
    if (outMessage) *outMessage = g_statusMessage;
    ReleaseSRWLockShared(&g_statusLock);
}

Levels GetLevels() {
    AcquireSRWLockShared(&g_dataLock);
    Levels l = g_snapshot.levels;
    ReleaseSRWLockShared(&g_dataLock);
    return l;
}

void GetSpectrum(float outSpectrum[32]) {
    if (!outSpectrum) return;
    AcquireSRWLockShared(&g_dataLock);
    memcpy(outSpectrum, g_snapshot.spectrum, sizeof(float) * 32);
    ReleaseSRWLockShared(&g_dataLock);
}

void SetGain(float gain) {
    g_gain.store((std::max)(0.05f, (std::min)(10.0f, gain)), std::memory_order_relaxed);
}

float GetGain() {
    return g_gain.load(std::memory_order_relaxed);
}

void SetSmoothing(float attack, float release) {
    g_attack.store((std::max)(0.01f, (std::min)(1.0f, attack)), std::memory_order_relaxed);
    g_release.store((std::max)(0.01f, (std::min)(1.0f, release)), std::memory_order_relaxed);
}

void GetSmoothing(float* outAttack, float* outRelease) {
    if (outAttack) *outAttack = g_attack.load(std::memory_order_relaxed);
    if (outRelease) *outRelease = g_release.load(std::memory_order_relaxed);
}

void GetShaderAudioData(float audio[4], float audio2[4], float* spectrum32) {
    if (!g_enabled.load(std::memory_order_relaxed)) {
        if (audio) memset(audio, 0, sizeof(float) * 4);
        if (audio2) memset(audio2, 0, sizeof(float) * 4);
        if (spectrum32) memset(spectrum32, 0, sizeof(float) * 32);
        return;
    }

    AcquireSRWLockShared(&g_dataLock);
    if (audio) {
        audio[0] = g_snapshot.levels.level;
        audio[1] = g_snapshot.levels.bass;
        audio[2] = g_snapshot.levels.mid;
        audio[3] = g_snapshot.levels.treble;
    }
    if (audio2) {
        audio2[0] = g_snapshot.levels.beat;
        audio2[1] = g_snapshot.levels.lowMid;
        audio2[2] = g_snapshot.levels.highMid;
        audio2[3] = 1.0f; // enabled flag
    }
    if (spectrum32) {
        memcpy(spectrum32, g_snapshot.spectrum, sizeof(float) * 32);
    }
    ReleaseSRWLockShared(&g_dataLock);
}

} // namespace AudioCapture
