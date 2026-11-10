#pragma once
#include <string>
#include <vector>
#include <array>
#include <mutex>
#include <cstdint>
#include "native_carx.h"

#pragma pack(push, 1)
struct ReplayHeader {
    char magic[4] = { 'C', 'X', 'R', '2' };
    uint32_t version = 3;
    uint32_t frameCount = 0;
    float duration = 0.0f;
    char trackName[64] = { 0 };
    char carName[64] = { 0 };
    uint64_t recordedTimestamp = 0;
};

struct ReplayTrackHeader {
    char carName[64] = { 0 };
    uint8_t isLocalPlayer = 0;
    uint8_t reserved[3] = { 0 };
    uint32_t frameCount = 0;
};

struct ReplayFrame {
    float time = 0.0f;
    float transformMatrix[16] = { 0 };
    float speedKmh = 0.0f;
    float rpm = 0.0f;
    int32_t gear = 0;
    float steerAngle = 0.0f;
    float wheelMatrices[4][16] = { 0 };
    float wheelSmoke[4] = { 0 };
    uint32_t flags = 0; // bit 0: brake, bit 1: handbrake, bit 2: accel
    float caliperMatrices[4][16] = { 0 };
};
#pragma pack(pop)

struct ReplayTrack {
    std::string carName;
    bool isLocalPlayer = false;
    void* sourceCarPtr = nullptr;
    std::vector<ReplayFrame> frames;
};

struct RecordTarget {
    void* carPtr = nullptr;
    std::string carName;
    bool isLocalPlayer = false;
};

struct ReplayInfo {
    std::string filename;
    std::string trackName;
    std::string carName;
    float duration = 0.0f;
    uint32_t frameCount = 0;
    uint32_t trackCount = 1;
    std::string recordedDate;
};

class ReplayEngine {
public:
    static void Init(const std::wstring& replaysDir);

    // Recording (single-car and multi-car Player List)
    static bool StartRecording(const std::string& trackName = "Unknown", const std::string& carName = "Current");
    static bool StartRecordingMulti(const std::string& trackName, const std::vector<RecordTarget>& targets);
    static void RecordFrame(const CarTelemetry& tele, float currentTime);
    static void RecordMultiFrame(const std::vector<CarTelemetry>& allCars, float currentTime);
    static bool StopRecording(const std::string& customFilename = "");
    static bool IsRecording();
    static float GetCurrentRecordDuration();
    static uint32_t GetCurrentRecordFrameCount();
    static uint32_t GetCurrentRecordTrackCount();

    // Playback
    static bool LoadReplay(const std::string& filename);
    static void UnloadReplay();
    static bool IsReplayLoaded();
    static float GetReplayDuration();
    static uint32_t GetReplayFrameCount();
    static uint32_t GetReplayTrackCount();
    static std::string GetReplayTrackName(int trackIdx);
    static bool IsReplayTrackLocal(int trackIdx);
    static uint32_t GetReplayTrackFrameCount(int trackIdx);
    static bool GetInterpolatedFrame(float targetTime, ReplayFrame& outFrame);
    static bool GetInterpolatedTrackFrame(int trackIdx, float targetTime, ReplayFrame& outFrame);

    // File Management
    static std::vector<ReplayInfo> ListSavedReplays();
    static std::wstring GetReplaysDirectory();

private:
    static ReplayFrame BuildFrameFromTelemetry(const CarTelemetry& tele, float relTime);
    static void InterpolateMatrix(const float* m0, const float* m1, float factor, float* outMatrix);
    static void InterpolateFrames(const ReplayFrame& f0, const ReplayFrame& f1, float factor, float targetTime, ReplayFrame& outFrame);

    static std::recursive_mutex s_mutex;
    static std::wstring s_replaysDir;

    static bool s_isRecording;
    static float s_recordStartTime;
    static std::vector<ReplayTrack> s_recordedTracks;
    static ReplayHeader s_currentHeader;

    static bool s_isLoaded;
    static std::vector<ReplayTrack> s_loadedTracks;
    static ReplayHeader s_loadedHeader;
};
