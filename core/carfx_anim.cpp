#include "carfx_anim.h"
#include "carfx_internal.h"
#include "logger.h"

#include <windows.h>
#include <wincodec.h>
#include <shlwapi.h>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cmath>
#include <thread>
#include <mutex>
#include <atomic>
#include <vector>

namespace CarFX {
namespace Anim {

namespace {

struct DecodedFrame {
    std::vector<uint8_t> rgbaPixels;
    float durationSec = 0.04167f;
};

struct DecodedClip {
    UINT width = 0;
    UINT height = 0;
    float fps = kDefaultSequenceFps;
    float totalDurationSec = 0.0f;
    std::vector<DecodedFrame> frames;
};

struct GpuFrame {
    ID3D11Texture2D* texture = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    float durationSec = 0.04167f;
};

struct SlotClip {
    std::string path;
    State state = State::None;
    std::string errorMessage;

    UINT width = 0;
    UINT height = 0;
    float fps = kDefaultSequenceFps;
    float totalDurationSec = 0.0f;
    std::vector<GpuFrame> frames;

    // Playback state
    bool playing = true;
    float speed = 1.0f;
    LoopMode mode = LoopMode::Loop;
    float currentTime = 0.0f;
    int currentFrame = 0;

    // Worker synchronization
    uint64_t generation = 0;
    std::thread workerThread;
    std::atomic<bool> workerRunning{ false };
    std::atomic<bool> workerCancel{ false };

    // Pending upload ready from worker thread
    bool hasPendingUpload = false;
    DecodedClip pendingClip;
};

std::mutex g_animMutex;
SlotClip g_slotClips[kTargetCount][kTextures];
std::atomic<bool> g_animInitialized{ false };

// Downscale helper using WIC
bool DownscalePixels(IWICImagingFactory* factory, const uint32_t* srcPixels, UINT srcW, UINT srcH,
                     UINT dstW, UINT dstH, std::vector<uint8_t>& outPixels) {
    if (!factory || !srcPixels || srcW == 0 || srcH == 0 || dstW == 0 || dstH == 0) return false;
    if (srcW == dstW && srcH == dstH) {
        outPixels.resize(static_cast<size_t>(dstW) * dstH * 4);
        memcpy(outPixels.data(), srcPixels, outPixels.size());
        return true;
    }

    IWICBitmap* wicBitmap = nullptr;
    HRESULT hr = factory->CreateBitmapFromMemory(
        srcW, srcH, GUID_WICPixelFormat32bppRGBA, srcW * 4,
        srcW * srcH * 4, reinterpret_cast<BYTE*>(const_cast<uint32_t*>(srcPixels)), &wicBitmap);
    if (FAILED(hr) || !wicBitmap) return false;

    IWICBitmapScaler* scaler = nullptr;
    hr = factory->CreateBitmapScaler(&scaler);
    if (FAILED(hr) || !scaler) {
        wicBitmap->Release();
        return false;
    }

    hr = scaler->Initialize(wicBitmap, dstW, dstH, WICBitmapInterpolationModeFant);
    if (FAILED(hr)) {
        scaler->Release();
        wicBitmap->Release();
        return false;
    }

    outPixels.resize(static_cast<size_t>(dstW) * dstH * 4);
    hr = scaler->CopyPixels(nullptr, dstW * 4, static_cast<UINT>(outPixels.size()), outPixels.data());

    scaler->Release();
    wicBitmap->Release();
    return SUCCEEDED(hr);
}

// GIF decoding with full GIF89a composition (disposal methods 0..3, per-frame rects & delays)
bool DecodeGif(const std::wstring& fullPathW, UINT maxDim, std::atomic<bool>& cancelFlag,
               DecodedClip& outClip, std::string& outError) {
    HRESULT hrCom = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool needUninit = SUCCEEDED(hrCom);

    auto cleanup = [&](const char* err) -> bool {
        if (needUninit) CoUninitialize();
        if (err) outError = err;
        return false;
    };

    IWICImagingFactory* factory = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  __uuidof(IWICImagingFactory), reinterpret_cast<void**>(&factory));
    if (FAILED(hr) || !factory) {
        return cleanup("WIC factory creation failed");
    }

    IWICBitmapDecoder* decoder = nullptr;
    hr = factory->CreateDecoderFromFilename(fullPathW.c_str(), nullptr, GENERIC_READ,
                                            WICDecodeMetadataCacheOnLoad, &decoder);
    if (FAILED(hr) || !decoder) {
        factory->Release();
        return cleanup("Failed to open GIF file");
    }

    UINT frameCount = 0;
    if (FAILED(decoder->GetFrameCount(&frameCount)) || frameCount == 0) {
        decoder->Release();
        factory->Release();
        return cleanup("GIF contains no frames");
    }

    // Determine logical canvas dimensions
    UINT canvasW = 0, canvasH = 0;
    IWICMetadataQueryReader* globalMeta = nullptr;
    if (SUCCEEDED(decoder->GetMetadataQueryReader(&globalMeta)) && globalMeta) {
        PROPVARIANT prop;
        PropVariantInit(&prop);
        if (SUCCEEDED(globalMeta->GetMetadataByName(L"/logscrdesc/Width", &prop)) &&
            (prop.vt == VT_UI2 || prop.vt == VT_UI4)) {
            canvasW = prop.uiVal;
        }
        PropVariantClear(&prop);
        if (SUCCEEDED(globalMeta->GetMetadataByName(L"/logscrdesc/Height", &prop)) &&
            (prop.vt == VT_UI2 || prop.vt == VT_UI4)) {
            canvasH = prop.uiVal;
        }
        PropVariantClear(&prop);
        globalMeta->Release();
    }

    if (canvasW == 0 || canvasH == 0) {
        IWICBitmapFrameDecode* frame0 = nullptr;
        if (SUCCEEDED(decoder->GetFrame(0, &frame0)) && frame0) {
            frame0->GetSize(&canvasW, &canvasH);
            frame0->Release();
        }
    }

    if (canvasW == 0 || canvasH == 0 || canvasW > Internal::kMaxTexSide || canvasH > Internal::kMaxTexSide) {
        decoder->Release();
        factory->Release();
        return cleanup("Invalid GIF canvas dimensions");
    }

    // Compute target dimensions constrained by budget
    UINT targetW = canvasW;
    UINT targetH = canvasH;
    const UINT maxAllowed = (std::min)(maxDim, kHardMaxDimension);
    const UINT maxSide = (std::max)(canvasW, canvasH);
    if (maxSide > maxAllowed) {
        float scale = static_cast<float>(maxAllowed) / static_cast<float>(maxSide);
        targetW = (std::max)(1u, static_cast<UINT>(canvasW * scale));
        targetH = (std::max)(1u, static_cast<UINT>(canvasH * scale));
    }

    // Check frame count budget
    UINT frameStep = 1;
    if (frameCount > kMaxAnimFrameCount) {
        frameStep = (frameCount + kMaxAnimFrameCount - 1) / kMaxAnimFrameCount;
        Logger::Warn("CarFX GIF frame count (" + std::to_string(frameCount) +
                     ") exceeds budget; subsampling step=" + std::to_string(frameStep));
    }

    // Check memory budget
    UINT estimatedFrames = (frameCount + frameStep - 1) / frameStep;
    size_t estMemory = static_cast<size_t>(estimatedFrames) * targetW * targetH * 4;
    if (estMemory > kMaxAnimMemoryBytes) {
        // Downscale target dimensions by 2 if over budget
        targetW = (std::max)(1u, targetW / 2);
        targetH = (std::max)(1u, targetH / 2);
        estMemory = static_cast<size_t>(estimatedFrames) * targetW * targetH * 4;
        Logger::Warn("CarFX GIF memory estimate exceeded 256MB budget; downscaling to " +
                     std::to_string(targetW) + "x" + std::to_string(targetH));
    }

    std::vector<uint32_t> canvas(static_cast<size_t>(canvasW) * canvasH, 0); // 32bpp RGBA transparent black
    std::vector<uint32_t> prevCanvas = canvas;

    outClip.width = targetW;
    outClip.height = targetH;
    outClip.frames.clear();
    outClip.frames.reserve(estimatedFrames);
    float totalDuration = 0.0f;
    float accumulatedDuration = 0.0f;

    for (UINT i = 0; i < frameCount; ++i) {
        if (cancelFlag.load(std::memory_order_relaxed)) {
            decoder->Release();
            factory->Release();
            return cleanup("GIF decoding cancelled");
        }

        IWICBitmapFrameDecode* frame = nullptr;
        if (FAILED(decoder->GetFrame(i, &frame)) || !frame) {
            continue;
        }

        UINT frameW = 0, frameH = 0;
        frame->GetSize(&frameW, &frameH);
        UINT left = 0, top = 0;
        UINT delayCentis = 10; // Default 100ms
        UINT disposal = 1;     // Default 1: do not dispose

        IWICMetadataQueryReader* meta = nullptr;
        if (SUCCEEDED(frame->GetMetadataQueryReader(&meta)) && meta) {
            PROPVARIANT prop;
            PropVariantInit(&prop);
            if (SUCCEEDED(meta->GetMetadataByName(L"/imgdesc/Left", &prop)) &&
                (prop.vt == VT_UI2 || prop.vt == VT_UI4)) {
                left = prop.uiVal;
            }
            PropVariantClear(&prop);
            if (SUCCEEDED(meta->GetMetadataByName(L"/imgdesc/Top", &prop)) &&
                (prop.vt == VT_UI2 || prop.vt == VT_UI4)) {
                top = prop.uiVal;
            }
            PropVariantClear(&prop);
            if (SUCCEEDED(meta->GetMetadataByName(L"/grctlext/Delay", &prop)) &&
                (prop.vt == VT_UI2 || prop.vt == VT_UI4)) {
                delayCentis = prop.uiVal;
            }
            PropVariantClear(&prop);
            if (SUCCEEDED(meta->GetMetadataByName(L"/grctlext/Disposal", &prop)) &&
                (prop.vt == VT_UI1 || prop.vt == VT_UI2)) {
                disposal = prop.bVal;
            }
            PropVariantClear(&prop);
            meta->Release();
        }

        // Browser standard: treat <= 1 centisecond (<= 10ms) as 10 centiseconds (100ms)
        if (delayCentis <= 1) delayCentis = 10;
        const float frameDurationSec = delayCentis * 0.01f;
        accumulatedDuration += frameDurationSec;

        // If disposal mode 3 (restore to previous), save canvas state BEFORE applying this frame
        if (disposal == 3) {
            prevCanvas = canvas;
        }

        // Decode frame rect to 32bpp RGBA
        IWICFormatConverter* conv = nullptr;
        hr = factory->CreateFormatConverter(&conv);
        if (SUCCEEDED(hr) && conv) {
            hr = conv->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone,
                                  nullptr, 0.0, WICBitmapPaletteTypeCustom);
        }

        if (SUCCEEDED(hr) && conv) {
            std::vector<uint32_t> subPixels(static_cast<size_t>(frameW) * frameH);
            hr = conv->CopyPixels(nullptr, frameW * 4, static_cast<UINT>(subPixels.size() * 4),
                                  reinterpret_cast<BYTE*>(subPixels.data()));
            conv->Release();

            if (SUCCEEDED(hr)) {
                // Compose subPixels onto canvas
                for (UINT y = 0; y < frameH; ++y) {
                    const UINT cy = top + y;
                    if (cy >= canvasH) break;
                    for (UINT x = 0; x < frameW; ++x) {
                        const UINT cx = left + x;
                        if (cx >= canvasW) break;
                        const uint32_t p = subPixels[y * frameW + x];
                        // High byte is alpha in little-endian 32bpp RGBA
                        if ((p & 0xFF000000u) != 0) {
                            canvas[cy * canvasW + cx] = p;
                        }
                    }
                }
            }
        }
        frame->Release();

        // Output composed frame if matching subsampling step or last frame
        const bool shouldSave = ((i % frameStep) == 0) || (i == frameCount - 1);
        if (shouldSave) {
            DecodedFrame df;
            df.durationSec = accumulatedDuration;
            accumulatedDuration = 0.0f;
            totalDuration += df.durationSec;

            if (!DownscalePixels(factory, canvas.data(), canvasW, canvasH, targetW, targetH, df.rgbaPixels)) {
                decoder->Release();
                factory->Release();
                return cleanup("Failed to downscale composed frame");
            }
            outClip.frames.push_back(std::move(df));
        }

        // Apply disposal method for NEXT frame
        if (disposal == 2) {
            // Restore to background (transparent black)
            for (UINT y = 0; y < frameH; ++y) {
                const UINT cy = top + y;
                if (cy >= canvasH) break;
                for (UINT x = 0; x < frameW; ++x) {
                    const UINT cx = left + x;
                    if (cx >= canvasW) break;
                    canvas[cy * canvasW + cx] = 0;
                }
            }
        } else if (disposal == 3) {
            // Restore to previous
            canvas = prevCanvas;
        }
        // disposal == 0 or 1: do not dispose (leave canvas as is)
    }

    decoder->Release();
    factory->Release();
    if (needUninit) CoUninitialize();

    if (outClip.frames.empty()) {
        outError = "Failed to produce any decoded frames";
        return false;
    }

    outClip.totalDurationSec = totalDuration;
    outClip.fps = (totalDuration > 0.0f) ? (static_cast<float>(outClip.frames.size()) / totalDuration) : 24.0f;
    return true;
}

// Image sequence folder decoding
bool DecodeSequence(const std::wstring& dirPathW, float requestedFps, UINT maxDim,
                    std::atomic<bool>& cancelFlag, DecodedClip& outClip, std::string& outError) {
    HRESULT hrCom = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool needUninit = SUCCEEDED(hrCom);

    auto cleanup = [&](const char* err) -> bool {
        if (needUninit) CoUninitialize();
        if (err) outError = err;
        return false;
    };

    // Optional fps.txt in directory
    float effectiveFps = (requestedFps > 0.1f) ? requestedFps : kDefaultSequenceFps;
    std::wstring fpsTxtPath = dirPathW + L"\\fps.txt";
    std::ifstream fpsFile(fpsTxtPath);
    if (fpsFile.is_open()) {
        float fval = 0.0f;
        if (fpsFile >> fval && fval >= 0.5f && fval <= 240.0f) {
            effectiveFps = fval;
        }
        fpsFile.close();
    }

    // Collect image files
    std::vector<std::wstring> fileList;
    WIN32_FIND_DATAW fd{};
    HANDLE hFind = FindFirstFileW((dirPathW + L"\\*").c_str(), &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                const wchar_t* ext = wcsrchr(fd.cFileName, L'.');
                if (ext) {
                    if (_wcsicmp(ext, L".png") == 0 || _wcsicmp(ext, L".jpg") == 0 ||
                        _wcsicmp(ext, L".jpeg") == 0 || _wcsicmp(ext, L".bmp") == 0 ||
                        _wcsicmp(ext, L".tga") == 0) {
                        fileList.push_back(dirPathW + L"\\" + fd.cFileName);
                    }
                }
            }
        } while (FindNextFileW(hFind, &fd));
        FindClose(hFind);
    }

    if (fileList.empty()) {
        return cleanup("No supported image files (.png/.jpg) found in sequence directory");
    }

    // Natural sort: frame1.png, frame2.png, ..., frame10.png
    std::sort(fileList.begin(), fileList.end(), [](const std::wstring& a, const std::wstring& b) {
        return StrCmpLogicalW(a.c_str(), b.c_str()) < 0;
    });

    IWICImagingFactory* factory = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  __uuidof(IWICImagingFactory), reinterpret_cast<void**>(&factory));
    if (FAILED(hr) || !factory) {
        return cleanup("WIC factory creation failed");
    }

    // Subsample if file count exceeds frame budget
    UINT frameStep = 1;
    if (fileList.size() > kMaxAnimFrameCount) {
        frameStep = static_cast<UINT>((fileList.size() + kMaxAnimFrameCount - 1) / kMaxAnimFrameCount);
        Logger::Warn("CarFX image sequence frame count (" + std::to_string(fileList.size()) +
                     ") exceeds budget; subsampling step=" + std::to_string(frameStep));
    }

    UINT targetW = 0, targetH = 0;
    const UINT maxAllowed = (std::min)(maxDim, kHardMaxDimension);

    outClip.frames.clear();
    outClip.frames.reserve(fileList.size() / frameStep + 1);

    for (size_t i = 0; i < fileList.size(); i += frameStep) {
        if (cancelFlag.load(std::memory_order_relaxed)) {
            factory->Release();
            return cleanup("Sequence decoding cancelled");
        }

        IWICBitmapDecoder* decoder = nullptr;
        if (FAILED(factory->CreateDecoderFromFilename(fileList[i].c_str(), nullptr, GENERIC_READ,
                                                      WICDecodeMetadataCacheOnLoad, &decoder)) || !decoder) {
            continue;
        }

        IWICBitmapFrameDecode* frame = nullptr;
        if (FAILED(decoder->GetFrame(0, &frame)) || !frame) {
            decoder->Release();
            continue;
        }

        UINT fw = 0, fh = 0;
        frame->GetSize(&fw, &fh);
        if (fw == 0 || fh == 0) {
            frame->Release();
            decoder->Release();
            continue;
        }

        if (targetW == 0 || targetH == 0) {
            targetW = fw;
            targetH = fh;
            const UINT maxSide = (std::max)(fw, fh);
            if (maxSide > maxAllowed) {
                float scale = static_cast<float>(maxAllowed) / static_cast<float>(maxSide);
                targetW = (std::max)(1u, static_cast<UINT>(fw * scale));
                targetH = (std::max)(1u, static_cast<UINT>(fh * scale));
            }
        }

        // Memory budget check
        const size_t estTotalMemory = fileList.size() / frameStep * targetW * targetH * 4;
        if (estTotalMemory > kMaxAnimMemoryBytes && targetW > 64 && targetH > 64) {
            targetW = (std::max)(1u, targetW / 2);
            targetH = (std::max)(1u, targetH / 2);
            Logger::Warn("CarFX sequence memory exceeded budget; downscaled to " +
                         std::to_string(targetW) + "x" + std::to_string(targetH));
        }

        IWICBitmapSource* srcToConvert = frame;
        IWICBitmapScaler* scaler = nullptr;
        if (fw != targetW || fh != targetH) {
            if (SUCCEEDED(factory->CreateBitmapScaler(&scaler)) && scaler) {
                if (SUCCEEDED(scaler->Initialize(frame, targetW, targetH, WICBitmapInterpolationModeFant))) {
                    srcToConvert = scaler;
                }
            }
        }

        IWICFormatConverter* conv = nullptr;
        hr = factory->CreateFormatConverter(&conv);
        if (SUCCEEDED(hr) && conv) {
            hr = conv->Initialize(srcToConvert, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone,
                                  nullptr, 0.0, WICBitmapPaletteTypeCustom);
        }

        if (SUCCEEDED(hr) && conv) {
            DecodedFrame df;
            df.rgbaPixels.resize(static_cast<size_t>(targetW) * targetH * 4);
            df.durationSec = 1.0f / (effectiveFps / static_cast<float>(frameStep));
            hr = conv->CopyPixels(nullptr, targetW * 4, static_cast<UINT>(df.rgbaPixels.size()), df.rgbaPixels.data());
            conv->Release();
            if (SUCCEEDED(hr)) {
                outClip.frames.push_back(std::move(df));
            }
        }

        if (scaler) scaler->Release();
        frame->Release();
        decoder->Release();
    }

    factory->Release();
    if (needUninit) CoUninitialize();

    if (outClip.frames.empty()) {
        return cleanup("Failed to decode any frames from image sequence");
    }

    outClip.width = targetW;
    outClip.height = targetH;
    outClip.fps = effectiveFps / static_cast<float>(frameStep);
    float totalDur = 0.0f;
    for (const auto& f : outClip.frames) totalDur += f.durationSec;
    outClip.totalDurationSec = totalDur;
    return true;
}

void FreeGpuFrames(std::vector<GpuFrame>& frames) {
    for (auto& f : frames) {
        if (f.srv) Internal::Retire(f.srv);
        if (f.texture) Internal::Retire(f.texture);
        f = {};
    }
    frames.clear();
}

} // namespace

void Init() {
    g_animInitialized.store(true, std::memory_order_release);
}

void Shutdown() {
    g_animInitialized.store(false, std::memory_order_release);
    for (int t = 0; t < kTargetCount; ++t) {
        StopAndClearTarget(t);
    }
}

void OnSceneTransition() {
    // Retain body animation, clear ghost animations on scene change
    for (int t = kTargetGhostBase; t < kTargetCount; ++t) {
        StopAndClearTarget(t);
    }
}

bool StartLoad(int target, int slot, const std::string& pathUtf8, const std::wstring& fullPathW,
               bool isDirectory, std::string* outError) {
    if (!Internal::ValidTarget(target) || slot < 0 || slot >= kTextures) {
        if (outError) *outError = "Invalid target or slot index";
        return false;
    }

    std::thread oldThread;
    uint64_t currentGen = 0;

    {
        std::lock_guard<std::mutex> lk(g_animMutex);
        SlotClip& sc = g_slotClips[target][slot];
        sc.workerCancel.store(true, std::memory_order_relaxed);
        if (sc.workerThread.joinable()) {
            oldThread = std::move(sc.workerThread);
        }

        sc.path = pathUtf8;
        sc.state = State::Loading;
        sc.errorMessage.clear();
        sc.hasPendingUpload = false;
        sc.pendingClip = {};
        sc.generation++;
        currentGen = sc.generation;
        sc.workerRunning.store(true, std::memory_order_release);
        sc.workerCancel.store(false, std::memory_order_release);

        // Record path in target state
        {
            std::lock_guard<std::mutex> lkPub(Internal::g_targets[target].publishMtx);
            Internal::g_targets[target].userTexPaths[slot] = pathUtf8;
        }

        // Spawn worker thread for decoding
        sc.workerThread = std::thread([target, slot, pathUtf8, fullPathW, isDirectory, currentGen]() {
            DecodedClip decoded;
            std::string err;
            std::atomic<bool> dummyCancel{ false };

            SlotClip* pSlot = nullptr;
            {
                std::lock_guard<std::mutex> lk(g_animMutex);
                pSlot = &g_slotClips[target][slot];
            }

            bool ok = false;
            if (isDirectory) {
                ok = DecodeSequence(fullPathW, kDefaultSequenceFps, kDefaultMaxDimension,
                                    pSlot->workerCancel, decoded, err);
            } else {
                ok = DecodeGif(fullPathW, kDefaultMaxDimension,
                               pSlot->workerCancel, decoded, err);
            }

            std::lock_guard<std::mutex> lk(g_animMutex);
            if (pSlot->generation == currentGen) {
                pSlot->workerRunning.store(false, std::memory_order_release);
                if (ok) {
                    pSlot->pendingClip = std::move(decoded);
                    pSlot->hasPendingUpload = true;
                    Logger::Info("CarFX animation decoded [" + std::to_string(slot) + "]: " +
                                 pathUtf8 + " (" + std::to_string(pSlot->pendingClip.frames.size()) +
                                 " frames, " + std::to_string(pSlot->pendingClip.width) + "x" +
                                 std::to_string(pSlot->pendingClip.height) + ")");
                } else {
                    pSlot->state = State::Error;
                    pSlot->errorMessage = err;
                    Logger::Warn("CarFX animation decode failed [" + std::to_string(slot) + "]: " + err);
                }
            }
        });
    }

    if (oldThread.joinable()) {
        oldThread.detach(); // Detach old aborted thread safely
    }

    if (outError) outError->clear();
    return true;
}

void StopAndClearSlot(int target, int slot) {
    if (!Internal::ValidTarget(target) || slot < 0 || slot >= kTextures) return;

    std::thread oldThread;
    std::vector<GpuFrame> framesToFree;

    {
        std::lock_guard<std::mutex> lk(g_animMutex);
        SlotClip& sc = g_slotClips[target][slot];
        sc.workerCancel.store(true, std::memory_order_relaxed);
        if (sc.workerThread.joinable()) {
            oldThread = std::move(sc.workerThread);
        }
        sc.generation++;
        sc.state = State::None;
        sc.path.clear();
        sc.errorMessage.clear();
        sc.hasPendingUpload = false;
        sc.pendingClip = {};
        framesToFree = std::move(sc.frames);
        sc.frames.clear();
        sc.width = 0;
        sc.height = 0;
        sc.totalDurationSec = 0.0f;
        sc.currentTime = 0.0f;
        sc.currentFrame = 0;
    }

    if (oldThread.joinable()) {
        oldThread.detach();
    }
    FreeGpuFrames(framesToFree);
}

void StopAndClearTarget(int target) {
    if (!Internal::ValidTarget(target)) return;
    for (int s = 0; s < kTextures; ++s) {
        StopAndClearSlot(target, s);
    }
}

void OnNewFrame(ID3D11Device* device, double dtSec) {
    if (!g_animInitialized.load(std::memory_order_acquire) || !device) return;

    std::lock_guard<std::mutex> lk(g_animMutex);

    for (int t = 0; t < kTargetCount; ++t) {
        for (int s = 0; s < kTextures; ++s) {
            SlotClip& sc = g_slotClips[t][s];

            // 1. Process pending GPU upload from worker thread
            if (sc.hasPendingUpload) {
                sc.hasPendingUpload = false;
                DecodedClip clip = std::move(sc.pendingClip);
                sc.pendingClip = {};

                // Retire existing frames
                FreeGpuFrames(sc.frames);

                sc.width = clip.width;
                sc.height = clip.height;
                sc.fps = clip.fps;
                sc.totalDurationSec = clip.totalDurationSec;
                sc.frames.clear();
                sc.frames.reserve(clip.frames.size());

                bool uploadOk = true;
                for (const auto& df : clip.frames) {
                    D3D11_TEXTURE2D_DESC td = {};
                    td.Width = clip.width;
                    td.Height = clip.height;
                    td.MipLevels = 1;
                    td.ArraySize = 1;
                    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                    td.SampleDesc.Count = 1;
                    td.Usage = D3D11_USAGE_IMMUTABLE;
                    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

                    D3D11_SUBRESOURCE_DATA initData = {};
                    initData.pSysMem = df.rgbaPixels.data();
                    initData.SysMemPitch = clip.width * 4;

                    ID3D11Texture2D* tex = nullptr;
                    if (FAILED(device->CreateTexture2D(&td, &initData, &tex)) || !tex) {
                        uploadOk = false;
                        break;
                    }

                    ID3D11ShaderResourceView* srv = nullptr;
                    if (FAILED(device->CreateShaderResourceView(tex, nullptr, &srv)) || !srv) {
                        tex->Release();
                        uploadOk = false;
                        break;
                    }

                    GpuFrame gf;
                    gf.texture = tex;
                    gf.srv = srv;
                    gf.durationSec = df.durationSec;
                    sc.frames.push_back(gf);
                }

                if (uploadOk && !sc.frames.empty()) {
                    sc.state = State::Ready;
                    sc.currentFrame = 0;
                    sc.currentTime = 0.0f;
                    ID3D11ShaderResourceView* oldSrv =
                        Internal::g_targets[t].userTex[s].exchange(sc.frames[0].srv, std::memory_order_release);
                    if (oldSrv) Internal::Retire(oldSrv);
                } else {
                    sc.state = State::Error;
                    sc.errorMessage = "Failed to create GPU textures for animation";
                    FreeGpuFrames(sc.frames);
                }
            }

            // 2. Advance playback timing for ready animations
            if (sc.state == State::Ready && !sc.frames.empty()) {
                if (sc.playing && sc.frames.size() > 1 && sc.totalDurationSec > 0.0f) {
                    sc.currentTime += static_cast<float>(dtSec) * sc.speed;

                    float effectiveTime = sc.currentTime;
                    if (sc.mode == LoopMode::Loop) {
                        if (effectiveTime >= sc.totalDurationSec) {
                            effectiveTime = std::fmod(effectiveTime, sc.totalDurationSec);
                            sc.currentTime = effectiveTime;
                        } else if (effectiveTime < 0.0f) {
                            effectiveTime = sc.totalDurationSec + std::fmod(effectiveTime, sc.totalDurationSec);
                            sc.currentTime = effectiveTime;
                        }
                    } else if (sc.mode == LoopMode::Once) {
                        if (effectiveTime >= sc.totalDurationSec) {
                            effectiveTime = sc.totalDurationSec - 0.0001f;
                            sc.currentTime = sc.totalDurationSec;
                            sc.playing = false;
                        } else if (effectiveTime < 0.0f) {
                            effectiveTime = 0.0f;
                            sc.currentTime = 0.0f;
                        }
                    } else if (sc.mode == LoopMode::PingPong) {
                        const float cycle = sc.totalDurationSec * 2.0f;
                        float phase = std::fmod(effectiveTime, cycle);
                        if (phase < 0.0f) phase += cycle;
                        if (phase > sc.totalDurationSec) {
                            effectiveTime = cycle - phase; // reverse phase
                        } else {
                            effectiveTime = phase;
                        }
                    }

                    // Map effectiveTime to frame index
                    effectiveTime = (std::max)(0.0f, (std::min)(effectiveTime, sc.totalDurationSec));
                    float acc = 0.0f;
                    int newFrame = static_cast<int>(sc.frames.size()) - 1;
                    for (size_t i = 0; i < sc.frames.size(); ++i) {
                        acc += sc.frames[i].durationSec;
                        if (effectiveTime < acc) {
                            newFrame = static_cast<int>(i);
                            break;
                        }
                    }

                    if (newFrame != sc.currentFrame) {
                        sc.currentFrame = newFrame;
                        Internal::g_targets[t].userTex[s].store(sc.frames[newFrame].srv, std::memory_order_release);
                    }
                }
            }
        }
    }
}

bool SetPlayback(int target, int slot, const PlaybackConfig& cfg) {
    if (!Internal::ValidTarget(target) || slot < 0 || slot >= kTextures) return false;
    std::lock_guard<std::mutex> lk(g_animMutex);
    SlotClip& sc = g_slotClips[target][slot];

    if (cfg.hasPlaying) sc.playing = cfg.playing;
    if (cfg.hasSpeed) sc.speed = std::clamp(cfg.speed, -10.0f, 10.0f);
    if (cfg.hasMode) sc.mode = cfg.mode;
    if (cfg.hasFps && cfg.fps > 0.1f && !sc.frames.empty()) {
        sc.fps = cfg.fps;
        const float dur = 1.0f / cfg.fps;
        sc.totalDurationSec = dur * static_cast<float>(sc.frames.size());
        for (auto& f : sc.frames) f.durationSec = dur;
    }
    if (cfg.hasFrame && !sc.frames.empty()) {
        int f = std::clamp(cfg.frame, 0, static_cast<int>(sc.frames.size()) - 1);
        sc.currentFrame = f;
        float acc = 0.0f;
        for (int i = 0; i < f; ++i) acc += sc.frames[i].durationSec;
        sc.currentTime = acc;
        Internal::g_targets[target].userTex[slot].store(sc.frames[f].srv, std::memory_order_release);
    }
    return true;
}

SlotInfo GetInfo(int target, int slot) {
    SlotInfo info{};
    if (!Internal::ValidTarget(target) || slot < 0 || slot >= kTextures) return info;

    std::lock_guard<std::mutex> lk(g_animMutex);
    const SlotClip& sc = g_slotClips[target][slot];

    if (sc.state != State::None) {
        info.file = sc.path;
        info.animated = (sc.state == State::Ready || sc.state == State::Loading);
        info.frames = static_cast<int>(sc.frames.size());
        info.fps = sc.fps;
        info.duration = sc.totalDurationSec;
        info.width = static_cast<int>(sc.width);
        info.height = static_cast<int>(sc.height);
        info.playing = sc.playing;
        info.speed = sc.speed;
        info.mode = (sc.mode == LoopMode::PingPong) ? "pingpong"
                  : ((sc.mode == LoopMode::Once) ? "once" : "loop");
        info.frame = sc.currentFrame;
        info.state = (sc.state == State::Loading) ? "loading"
                   : ((sc.state == State::Error) ? "error" : "ready");
        info.error = sc.errorMessage;
    } else {
        // Check if a static user texture is active in target slot
        std::string staticPath;
        {
            std::lock_guard<std::mutex> lkPub(Internal::g_targets[target].publishMtx);
            staticPath = Internal::g_targets[target].userTexPaths[slot];
        }
        info.file = staticPath;
        info.animated = false;
        info.frames = staticPath.empty() ? 0 : 1;
        info.fps = 0.0f;
        info.duration = 0.0f;
        info.width = 0;
        info.height = 0;
        info.playing = false;
        info.speed = 1.0f;
        info.mode = "loop";
        info.frame = 0;
        info.state = "ready";
    }

    return info;
}

bool IsSlotAnimated(int target, int slot) {
    if (!Internal::ValidTarget(target) || slot < 0 || slot >= kTextures) return false;
    std::lock_guard<std::mutex> lk(g_animMutex);
    return g_slotClips[target][slot].state == State::Ready;
}

void GetConstantData(int target, float outData[kTextures][4]) {
    memset(outData, 0, sizeof(float) * kTextures * 4);
    if (!Internal::ValidTarget(target)) return;

    std::lock_guard<std::mutex> lk(g_animMutex);
    for (int s = 0; s < kTextures; ++s) {
        const SlotClip& sc = g_slotClips[target][s];
        if (sc.state == State::Ready && !sc.frames.empty()) {
            float progress = (sc.totalDurationSec > 0.0f)
                ? std::clamp(sc.currentTime / sc.totalDurationSec, 0.0f, 1.0f) : 0.0f;
            outData[s][0] = progress;
            outData[s][1] = static_cast<float>(sc.currentFrame);
            outData[s][2] = static_cast<float>(sc.frames.size());
            outData[s][3] = sc.playing ? 1.0f : 0.0f;
        } else {
            ID3D11ShaderResourceView* v = Internal::g_targets[target].userTex[s].load(std::memory_order_relaxed);
            outData[s][0] = 0.0f;
            outData[s][1] = 0.0f;
            outData[s][2] = v ? 1.0f : 0.0f;
            outData[s][3] = 0.0f;
        }
    }
}

std::vector<std::string> ListCandidateTextures() {
    std::vector<std::string> out;
    if (Internal::g_shadersDir.empty()) return out;

    // 1. Files in shaders/
    WIN32_FIND_DATAW fd{};
    HANDLE hFind = FindFirstFileW((Internal::g_shadersDir + L"\\*").c_str(), &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (fd.cFileName[0] == L'.') continue;
                if (_wcsicmp(fd.cFileName, L"cloud") == 0) continue;
                // Check if directory contains sequence image files
                std::wstring subDir = Internal::g_shadersDir + L"\\" + fd.cFileName;
                WIN32_FIND_DATAW subFd{};
                HANDLE hSub = FindFirstFileW((subDir + L"\\*").c_str(), &subFd);
                bool hasImages = false;
                if (hSub != INVALID_HANDLE_VALUE) {
                    do {
                        if (!(subFd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                            const wchar_t* ext = wcsrchr(subFd.cFileName, L'.');
                            if (ext && (_wcsicmp(ext, L".png") == 0 || _wcsicmp(ext, L".jpg") == 0 ||
                                        _wcsicmp(ext, L".jpeg") == 0 || _wcsicmp(ext, L".bmp") == 0)) {
                                hasImages = true;
                                break;
                            }
                        }
                    } while (FindNextFileW(hSub, &subFd));
                    FindClose(hSub);
                }
                if (hasImages) {
                    out.push_back(Internal::WideToUtf8(fd.cFileName) + "/");
                }
            } else {
                const wchar_t* ext = wcsrchr(fd.cFileName, L'.');
                if (ext && (_wcsicmp(ext, L".gif") == 0 || _wcsicmp(ext, L".png") == 0 ||
                            _wcsicmp(ext, L".jpg") == 0 || _wcsicmp(ext, L".jpeg") == 0 ||
                            _wcsicmp(ext, L".bmp") == 0 || _wcsicmp(ext, L".tga") == 0)) {
                    out.push_back(Internal::WideToUtf8(fd.cFileName));
                }
            }
        } while (FindNextFileW(hFind, &fd));
        FindClose(hFind);
    }

    std::sort(out.begin(), out.end());
    return out;
}

} // namespace Anim
} // namespace CarFX
