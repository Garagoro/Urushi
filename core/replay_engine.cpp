#include "replay_engine.h"
#include "logger.h"
#include <fstream>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <cmath>
#include <algorithm>
#include <shlwapi.h>

std::recursive_mutex ReplayEngine::s_mutex;
std::wstring ReplayEngine::s_replaysDir;

bool ReplayEngine::s_isRecording = false;
float ReplayEngine::s_recordStartTime = 0.0f;
std::vector<ReplayTrack> ReplayEngine::s_recordedTracks;
ReplayHeader ReplayEngine::s_currentHeader;
int ReplayEngine::s_currentCarFxTrackIdx = 0;
CarFX::ReplayShaderConfig ReplayEngine::s_currentCarFx{};

bool ReplayEngine::s_isLoaded = false;
std::vector<ReplayTrack> ReplayEngine::s_loadedTracks;
ReplayHeader ReplayEngine::s_loadedHeader;
int ReplayEngine::s_loadedCarFxTrackIdx = 0;
CarFX::ReplayShaderConfig ReplayEngine::s_loadedCarFx{};

void ReplayEngine::Init(const std::wstring& replaysDir) {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    s_replaysDir = replaysDir;
    CreateDirectoryW(replaysDir.c_str(), nullptr);
    Logger::Info("ReplayEngine initialized. Directory: " + Logger::ToNarrow(replaysDir));
}

ReplayFrame ReplayEngine::BuildFrameFromTelemetry(const CarTelemetry& tele, float relTime) {
    ReplayFrame frame;
    frame.time = relTime;
    for (int i = 0; i < 16; ++i) {
        frame.transformMatrix[i] = tele.transformMatrix[i];
    }
    frame.speedKmh = tele.speedKmh;
    frame.rpm = tele.rpm;
    frame.gear = tele.gear;
    frame.steerAngle = tele.steerAngle;

    for (int w = 0; w < 4; ++w) {
        for (int i = 0; i < 16; ++i) {
            frame.wheelMatrices[w][i] = tele.wheels[w].matrix[i];
            frame.caliperMatrices[w][i] = tele.wheels[w].caliperMatrix[i];
        }
        frame.wheelSmoke[w] = tele.wheels[w].smokeIntensity;
    }

    if (tele.brake > 0.1f) frame.flags |= (1 << 0);
    if (tele.handbrake > 0.1f) frame.flags |= (1 << 1);
    if (tele.accel > 0.1f) frame.flags |= (1 << 2);
    return frame;
}

bool ReplayEngine::StartRecording(const std::string& trackName, const std::string& carName) {
    RecordTarget target;
    target.carPtr = NativeCarX::GetPlayerCarPtr();
    target.carName = carName;
    target.isLocalPlayer = true;
    return StartRecordingMulti(trackName, { target });
}

bool ReplayEngine::StartRecordingMulti(const std::string& trackName, const std::vector<RecordTarget>& targets) {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    if (s_isRecording) return false;

    s_recordedTracks.clear();
    if (targets.empty()) {
        RecordTarget fallback;
        fallback.carPtr = NativeCarX::GetPlayerCarPtr();
        fallback.carName = "Player Car";
        fallback.isLocalPlayer = true;
        s_recordedTracks.push_back(ReplayTrack{ fallback.carName, true, fallback.carPtr, {} });
    } else {
        for (const auto& t : targets) {
            ReplayTrack rt;
            rt.carName = t.carName.empty() ? "Car" : t.carName;
            rt.isLocalPlayer = t.isLocalPlayer;
            rt.sourceCarPtr = t.carPtr;
            s_recordedTracks.push_back(std::move(rt));
        }
    }

    s_currentHeader = ReplayHeader();
    s_currentHeader.version = 4;
    strncpy_s(s_currentHeader.trackName, trackName.c_str(), sizeof(s_currentHeader.trackName) - 1);

    std::string headerCarSummary = s_recordedTracks[0].carName;
    if (s_recordedTracks.size() > 1) {
        headerCarSummary += " +" + std::to_string(s_recordedTracks.size() - 1);
    }
    strncpy_s(s_currentHeader.carName, headerCarSummary.c_str(), sizeof(s_currentHeader.carName) - 1);
    s_currentHeader.recordedTimestamp = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    s_currentCarFx = CarFX::CaptureActiveConfig(CarFX::kTargetBody);
    s_currentCarFxTrackIdx = 0;

    s_recordStartTime = -1.0f;
    s_isRecording = true;
    Logger::Info("Replay recording started (" + std::to_string(s_recordedTracks.size()) + " cars): " + headerCarSummary + " on " + trackName +
                 (s_currentCarFx.enabled ? (" [CarFX: " + s_currentCarFx.shaderPath + "]") : ""));
    return true;
}

void ReplayEngine::RecordFrame(const CarTelemetry& tele, float currentTime) {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    if (!s_isRecording || s_recordedTracks.empty()) return;

    if (s_recordStartTime < 0.0f) {
        s_recordStartTime = currentTime;
    }

    s_recordedTracks[0].frames.push_back(BuildFrameFromTelemetry(tele, currentTime - s_recordStartTime));
}

void ReplayEngine::RecordMultiFrame(const std::vector<CarTelemetry>& allCars, float currentTime) {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    if (!s_isRecording || s_recordedTracks.empty() || allCars.empty()) return;

    if (s_recordStartTime < 0.0f) {
        s_recordStartTime = currentTime;
    }
    float relTime = currentTime - s_recordStartTime;

    for (auto& track : s_recordedTracks) {
        const CarTelemetry* match = nullptr;
        if (track.sourceCarPtr) {
            for (const auto& car : allCars) {
                if (car.carPtr == track.sourceCarPtr) {
                    match = &car;
                    break;
                }
            }
        }
        if (!match && track.isLocalPlayer) {
            for (const auto& car : allCars) {
                if (car.isLocalPlayer) {
                    match = &car;
                    track.sourceCarPtr = car.carPtr;
                    break;
                }
            }
        }
        if (match) {
            track.frames.push_back(BuildFrameFromTelemetry(*match, relTime));
        }
    }
}

bool ReplayEngine::StopRecording(const std::string& customFilename) {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    if (!s_isRecording) return false;

    s_isRecording = false;

    // Drop empty tracks (e.g., car never appeared in telemetry)
    s_recordedTracks.erase(
        std::remove_if(s_recordedTracks.begin(), s_recordedTracks.end(),
            [](const ReplayTrack& t) { return t.frames.empty(); }),
        s_recordedTracks.end()
    );

    if (s_recordedTracks.empty()) {
        Logger::Warn("Replay recording finished but no frames recorded.");
        return false;
    }

    uint32_t maxFrames = 0;
    float maxDuration = 0.0f;
    for (const auto& t : s_recordedTracks) {
        maxFrames = (std::max)(maxFrames, static_cast<uint32_t>(t.frames.size()));
        if (!t.frames.empty()) {
            maxDuration = (std::max)(maxDuration, t.frames.back().time);
        }
    }

    s_currentHeader.version = 4;
    s_currentHeader.frameCount = maxFrames;
    s_currentHeader.duration = maxDuration;

    std::string headerCarSummary = s_recordedTracks[0].carName;
    if (s_recordedTracks.size() > 1) {
        headerCarSummary += " +" + std::to_string(s_recordedTracks.size() - 1);
    }
    memset(s_currentHeader.carName, 0, sizeof(s_currentHeader.carName));
    strncpy_s(s_currentHeader.carName, headerCarSummary.c_str(), sizeof(s_currentHeader.carName) - 1);

    std::string filename = customFilename;
    if (filename.empty()) {
        auto now = std::chrono::system_clock::now();
        auto in_time_t = std::chrono::system_clock::to_time_t(now);
        std::tm t;
        localtime_s(&t, &in_time_t);
        std::ostringstream ss;
        ss << "replay_" << std::put_time(&t, "%Y%m%d_%H%M%S") << ".cxr2";
        filename = ss.str();
    } else if (filename.find(".cxr2") == std::string::npos) {
        filename += ".cxr2";
    }

    std::wstring fullPath = s_replaysDir + L"\\" + Logger::ToWide(filename);
    std::ofstream out(fullPath, std::ios::binary);
    if (!out.is_open()) {
        Logger::Error("Failed to save replay to: " + Logger::ToNarrow(fullPath));
        return false;
    }

    out.write(reinterpret_cast<const char*>(&s_currentHeader), sizeof(ReplayHeader));
    uint32_t trackCount = static_cast<uint32_t>(s_recordedTracks.size());
    out.write(reinterpret_cast<const char*>(&trackCount), sizeof(trackCount));

    int localTrackIdx = 0;
    for (size_t ti = 0; ti < s_recordedTracks.size(); ++ti) {
        const auto& track = s_recordedTracks[ti];
        if (track.isLocalPlayer) localTrackIdx = static_cast<int>(ti);
        ReplayTrackHeader th;
        strncpy_s(th.carName, track.carName.c_str(), sizeof(th.carName) - 1);
        th.isLocalPlayer = track.isLocalPlayer ? 1 : 0;
        th.frameCount = static_cast<uint32_t>(track.frames.size());
        out.write(reinterpret_cast<const char*>(&th), sizeof(ReplayTrackHeader));
        out.write(reinterpret_cast<const char*>(track.frames.data()), track.frames.size() * sizeof(ReplayFrame));
    }

    ReplayCarFxFooter cfx{};
    cfx.enabled = s_currentCarFx.enabled ? 1 : 0;
    cfx.overrideMode = static_cast<uint8_t>(s_currentCarFx.overrideMode);
    cfx.overrideStealth = static_cast<uint8_t>(s_currentCarFx.overrideStealth);
    cfx.trackIndex = localTrackIdx;
    if (s_currentCarFx.enabled && !s_currentCarFx.shaderPath.empty()) {
        strncpy_s(cfx.shaderPath, s_currentCarFx.shaderPath.c_str(), sizeof(cfx.shaderPath) - 1);
        memcpy(cfx.params, s_currentCarFx.params, sizeof(cfx.params));
        for (int i = 0; i < 4; ++i) {
            if (!s_currentCarFx.texPaths[i].empty()) {
                strncpy_s(cfx.texPaths[i], s_currentCarFx.texPaths[i].c_str(), sizeof(cfx.texPaths[i]) - 1);
            }
        }
    }
    out.write(reinterpret_cast<const char*>(&cfx), sizeof(ReplayCarFxFooter));
    out.close();

    Logger::Info("Replay saved (" + std::to_string(trackCount) + " tracks, " + std::to_string(maxFrames) +
        " frames, " + std::to_string(maxDuration) + "s) to " + filename);
    return true;
}

bool ReplayEngine::IsRecording() {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    return s_isRecording;
}

float ReplayEngine::GetCurrentRecordDuration() {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    if (!s_isRecording || s_recordedTracks.empty()) return 0.0f;
    float maxDur = 0.0f;
    for (const auto& t : s_recordedTracks) {
        if (!t.frames.empty()) {
            maxDur = (std::max)(maxDur, t.frames.back().time);
        }
    }
    return maxDur;
}

uint32_t ReplayEngine::GetCurrentRecordFrameCount() {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    uint32_t maxFrames = 0;
    for (const auto& t : s_recordedTracks) {
        maxFrames = (std::max)(maxFrames, static_cast<uint32_t>(t.frames.size()));
    }
    return maxFrames;
}

uint32_t ReplayEngine::GetCurrentRecordTrackCount() {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    return static_cast<uint32_t>(s_recordedTracks.size());
}

bool ReplayEngine::LoadReplay(const std::string& filename) {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);

    std::wstring fullPath = s_replaysDir + L"\\" + Logger::ToWide(filename);
    std::ifstream in(fullPath, std::ios::binary);
    if (!in.is_open()) {
        Logger::Error("Failed to open replay file: " + filename);
        return false;
    }

    ReplayHeader header;
    in.read(reinterpret_cast<char*>(&header), sizeof(ReplayHeader));
    if (header.magic[0] != 'C' || header.magic[1] != 'X' || header.magic[2] != 'R' || header.magic[3] != '2') {
        Logger::Error("Invalid replay file header magic!");
        return false;
    }

    s_loadedTracks.clear();

    if (header.version == 1) {
#pragma pack(push, 1)
        struct ReplayFrameV1 {
            float time;
            float transformMatrix[16];
            float speedKmh;
            float rpm;
            int32_t gear;
            float steerAngle;
            float wheelMatrices[4][16];
            float wheelSmoke[4];
            uint32_t flags;
        };
#pragma pack(pop)
        ReplayTrack track;
        track.carName = header.carName;
        track.isLocalPlayer = true;
        track.frames.resize(header.frameCount);

        std::vector<ReplayFrameV1> v1Frames(header.frameCount);
        in.read(reinterpret_cast<char*>(v1Frames.data()), header.frameCount * sizeof(ReplayFrameV1));
        for (uint32_t i = 0; i < header.frameCount; ++i) {
            ReplayFrame& dst = track.frames[i];
            const ReplayFrameV1& src = v1Frames[i];
            dst.time = src.time;
            std::copy(src.transformMatrix, src.transformMatrix + 16, dst.transformMatrix);
            dst.speedKmh = src.speedKmh;
            dst.rpm = src.rpm;
            dst.gear = src.gear;
            dst.steerAngle = src.steerAngle;
            std::copy(&src.wheelMatrices[0][0], &src.wheelMatrices[0][0] + 64, &dst.wheelMatrices[0][0]);
            std::copy(src.wheelSmoke, src.wheelSmoke + 4, dst.wheelSmoke);
            dst.flags = src.flags;
            std::fill(&dst.caliperMatrices[0][0], &dst.caliperMatrices[0][0] + 64, 0.0f);
        }
        s_loadedTracks.push_back(std::move(track));
    } else if (header.version == 2) {
        ReplayTrack track;
        track.carName = header.carName;
        track.isLocalPlayer = true;
        track.frames.resize(header.frameCount);
        in.read(reinterpret_cast<char*>(track.frames.data()), header.frameCount * sizeof(ReplayFrame));
        s_loadedTracks.push_back(std::move(track));
    } else {
        uint32_t trackCount = 0;
        in.read(reinterpret_cast<char*>(&trackCount), sizeof(trackCount));
        if (trackCount > 64) trackCount = 64;
        for (uint32_t t = 0; t < trackCount; ++t) {
            ReplayTrackHeader th;
            in.read(reinterpret_cast<char*>(&th), sizeof(ReplayTrackHeader));
            if (!in.good()) break;

            ReplayTrack track;
            track.carName = th.carName;
            track.isLocalPlayer = (th.isLocalPlayer != 0);
            if (th.frameCount > 0 && th.frameCount < 10000000) {
                track.frames.resize(th.frameCount);
                in.read(reinterpret_cast<char*>(track.frames.data()), th.frameCount * sizeof(ReplayFrame));
            }
            if (!track.frames.empty()) {
                s_loadedTracks.push_back(std::move(track));
            }
        }
    }
    s_loadedCarFx = CarFX::ReplayShaderConfig{};
    s_loadedCarFxTrackIdx = 0;

    if (header.version >= 4) {
        ReplayCarFxFooter cfx{};
        in.read(reinterpret_cast<char*>(&cfx), sizeof(ReplayCarFxFooter));
        if (in.good() && cfx.footerMagic[0] == 'C' && cfx.footerMagic[1] == 'F' &&
            cfx.footerMagic[2] == 'X' && cfx.footerMagic[3] == '1') {
            if (cfx.enabled != 0 && cfx.shaderPath[0] != '\0') {
                s_loadedCarFx.enabled = true;
                s_loadedCarFx.overrideMode = cfx.overrideMode;
                s_loadedCarFx.overrideStealth = cfx.overrideStealth;
                cfx.shaderPath[sizeof(cfx.shaderPath) - 1] = '\0';
                s_loadedCarFx.shaderPath = cfx.shaderPath;
                memcpy(s_loadedCarFx.params, cfx.params, sizeof(s_loadedCarFx.params));
                for (int i = 0; i < 4; ++i) {
                    cfx.texPaths[i][sizeof(cfx.texPaths[i]) - 1] = '\0';
                    if (cfx.texPaths[i][0] != '\0') s_loadedCarFx.texPaths[i] = cfx.texPaths[i];
                }
                s_loadedCarFxTrackIdx = std::clamp(cfx.trackIndex, 0,
                    static_cast<int>(s_loadedTracks.empty() ? 0 : s_loadedTracks.size() - 1));
            }
        }
    }
    in.close();

    if (s_loadedTracks.empty()) {
        s_isLoaded = false;
        s_loadedCarFx = CarFX::ReplayShaderConfig{};
        Logger::Error("Loaded replay contained 0 valid tracks: " + filename);
        return false;
    }

    s_loadedHeader = header;
    s_isLoaded = true;

    Logger::Info("Loaded replay: " + filename + " (" + std::to_string(s_loadedTracks.size()) +
        " tracks, " + std::to_string(header.frameCount) + " frames, " + std::to_string(header.duration) + "s)" +
        (s_loadedCarFx.enabled ? (" [CarFX: " + s_loadedCarFx.shaderPath + "]") : ""));
    return true;
}

void ReplayEngine::UnloadReplay() {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    s_isLoaded = false;
    s_loadedTracks.clear();
    s_loadedCarFx = CarFX::ReplayShaderConfig{};
    s_loadedCarFxTrackIdx = 0;
}

bool ReplayEngine::GetLoadedCarFx(CarFX::ReplayShaderConfig& outConfig, int* outTrackIdx) {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    if (!s_isLoaded || !s_loadedCarFx.enabled || s_loadedCarFx.shaderPath.empty()) {
        outConfig = CarFX::ReplayShaderConfig{};
        if (outTrackIdx) *outTrackIdx = 0;
        return false;
    }
    outConfig = s_loadedCarFx;
    if (outTrackIdx) *outTrackIdx = s_loadedCarFxTrackIdx;
    return true;
}

std::string ReplayEngine::GetLoadedCarFxShaderName() {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    if (!s_isLoaded || !s_loadedCarFx.enabled) return "";
    return s_loadedCarFx.shaderPath;
}

bool ReplayEngine::IsReplayLoaded() {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    return s_isLoaded && !s_loadedTracks.empty();
}

float ReplayEngine::GetReplayDuration() {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    return s_isLoaded ? s_loadedHeader.duration : 0.0f;
}

uint32_t ReplayEngine::GetReplayFrameCount() {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    return s_isLoaded ? s_loadedHeader.frameCount : 0;
}

uint32_t ReplayEngine::GetReplayTrackCount() {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    return s_isLoaded ? static_cast<uint32_t>(s_loadedTracks.size()) : 0;
}

std::string ReplayEngine::GetReplayTrackName(int trackIdx) {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    if (!s_isLoaded || trackIdx < 0 || trackIdx >= static_cast<int>(s_loadedTracks.size())) return "";
    return s_loadedTracks[trackIdx].carName;
}

bool ReplayEngine::IsReplayTrackLocal(int trackIdx) {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    if (!s_isLoaded || trackIdx < 0 || trackIdx >= static_cast<int>(s_loadedTracks.size())) return false;
    return s_loadedTracks[trackIdx].isLocalPlayer;
}

uint32_t ReplayEngine::GetReplayTrackFrameCount(int trackIdx) {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    if (!s_isLoaded || trackIdx < 0 || trackIdx >= static_cast<int>(s_loadedTracks.size())) return 0;
    return static_cast<uint32_t>(s_loadedTracks[trackIdx].frames.size());
}

static void ColMajorToQuat(const float* m, float& qx, float& qy, float& qz, float& qw) {
    float m00 = m[0], m10 = m[1], m20 = m[2];
    float m01 = m[4], m11 = m[5], m21 = m[6];
    float m02 = m[8], m12 = m[9], m22 = m[10];
    float trace = m00 + m11 + m22;
    if (trace > 0.0f) {
        float s = 0.5f / std::sqrt(trace + 1.0f);
        qw = 0.25f / s;
        qx = (m21 - m12) * s;
        qy = (m02 - m20) * s;
        qz = (m10 - m01) * s;
    } else if (m00 > m11 && m00 > m22) {
        float s = 2.0f * std::sqrt(1.0f + m00 - m11 - m22);
        qw = (m21 - m12) / s;
        qx = 0.25f * s;
        qy = (m01 + m10) / s;
        qz = (m02 + m20) / s;
    } else if (m11 > m22) {
        float s = 2.0f * std::sqrt(1.0f + m11 - m00 - m22);
        qw = (m02 - m20) / s;
        qx = (m01 + m10) / s;
        qy = 0.25f * s;
        qz = (m12 + m21) / s;
    } else {
        float s = 2.0f * std::sqrt(1.0f + m22 - m00 - m11);
        qw = (m10 - m01) / s;
        qx = (m02 + m20) / s;
        qy = (m12 + m21) / s;
        qz = 0.25f * s;
    }
    float len = std::sqrt(qx * qx + qy * qy + qz * qz + qw * qw);
    if (len > 1e-6f) {
        qx /= len; qy /= len; qz /= len; qw /= len;
    }
}

void ReplayEngine::InterpolateMatrix(const float* m0, const float* m1, float factor, float* outMatrix) {
    float x0, y0, z0, w0, x1, y1, z1, w1;
    ColMajorToQuat(m0, x0, y0, z0, w0);
    ColMajorToQuat(m1, x1, y1, z1, w1);

    float dot = x0 * x1 + y0 * y1 + z0 * z1 + w0 * w1;
    if (dot < 0.0f) {
        dot = -dot;
        x1 = -x1; y1 = -y1; z1 = -z1; w1 = -w1;
    }

    float qx, qy, qz, qw;
    if (dot > 0.9995f) {
        qx = x0 + (x1 - x0) * factor;
        qy = y0 + (y1 - y0) * factor;
        qz = z0 + (z1 - z0) * factor;
        qw = w0 + (w1 - w0) * factor;
    } else {
        float theta = std::acos(std::clamp(dot, -1.0f, 1.0f));
        float sinTheta = std::sin(theta);
        float s0 = std::sin((1.0f - factor) * theta) / sinTheta;
        float s1 = std::sin(factor * theta) / sinTheta;
        qx = s0 * x0 + s1 * x1;
        qy = s0 * y0 + s1 * y1;
        qz = s0 * z0 + s1 * z1;
        qw = s0 * w0 + s1 * w1;
    }

    float qLen = std::sqrt(qx * qx + qy * qy + qz * qz + qw * qw);
    if (qLen > 1e-6f) {
        qx /= qLen; qy /= qLen; qz /= qLen; qw /= qLen;
    }

    float xx = qx * qx, yy = qy * qy, zz = qz * qz;
    float xy = qx * qy, xz = qx * qz, yz = qy * qz;
    float wx = qw * qx, wy = qw * qy, wz = qw * qz;

    outMatrix[0]  = 1.0f - 2.0f * (yy + zz);
    outMatrix[1]  = 2.0f * (xy + wz);
    outMatrix[2]  = 2.0f * (xz - wy);
    outMatrix[3]  = 0.0f;

    outMatrix[4]  = 2.0f * (xy - wz);
    outMatrix[5]  = 1.0f - 2.0f * (xx + zz);
    outMatrix[6]  = 2.0f * (yz + wx);
    outMatrix[7]  = 0.0f;

    outMatrix[8]  = 2.0f * (xz + wy);
    outMatrix[9]  = 2.0f * (yz - wx);
    outMatrix[10] = 1.0f - 2.0f * (xx + yy);
    outMatrix[11] = 0.0f;

    outMatrix[12] = m0[12] + (m1[12] - m0[12]) * factor;
    outMatrix[13] = m0[13] + (m1[13] - m0[13]) * factor;
    outMatrix[14] = m0[14] + (m1[14] - m0[14]) * factor;
    outMatrix[15] = 1.0f;
}

void ReplayEngine::InterpolateFrames(const ReplayFrame& f0, const ReplayFrame& f1, float factor, float targetTime, ReplayFrame& outFrame) {
    outFrame.time = targetTime;
    InterpolateMatrix(f0.transformMatrix, f1.transformMatrix, factor, outFrame.transformMatrix);

    outFrame.speedKmh = f0.speedKmh + (f1.speedKmh - f0.speedKmh) * factor;
    outFrame.rpm = f0.rpm + (f1.rpm - f0.rpm) * factor;
    outFrame.steerAngle = f0.steerAngle + (f1.steerAngle - f0.steerAngle) * factor;
    outFrame.gear = (factor < 0.5f) ? f0.gear : f1.gear;
    outFrame.flags = (factor < 0.5f) ? f0.flags : f1.flags;

    for (int w = 0; w < 4; ++w) {
        InterpolateMatrix(f0.wheelMatrices[w], f1.wheelMatrices[w], factor, outFrame.wheelMatrices[w]);
        float cMag0 = std::fabs(f0.caliperMatrices[w][0]) + std::fabs(f0.caliperMatrices[w][5]) + std::fabs(f0.caliperMatrices[w][10]);
        float cMag1 = std::fabs(f1.caliperMatrices[w][0]) + std::fabs(f1.caliperMatrices[w][5]) + std::fabs(f1.caliperMatrices[w][10]);
        if (cMag0 > 0.01f && cMag1 > 0.01f) {
            InterpolateMatrix(f0.caliperMatrices[w], f1.caliperMatrices[w], factor, outFrame.caliperMatrices[w]);
        } else {
            std::fill(outFrame.caliperMatrices[w], outFrame.caliperMatrices[w] + 16, 0.0f);
        }
        outFrame.wheelSmoke[w] = f0.wheelSmoke[w] + (f1.wheelSmoke[w] - f0.wheelSmoke[w]) * factor;
    }
}

bool ReplayEngine::GetInterpolatedFrame(float targetTime, ReplayFrame& outFrame) {
    return GetInterpolatedTrackFrame(0, targetTime, outFrame);
}

bool ReplayEngine::GetInterpolatedTrackFrame(int trackIdx, float targetTime, ReplayFrame& outFrame) {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    if (!s_isLoaded || trackIdx < 0 || trackIdx >= static_cast<int>(s_loadedTracks.size())) return false;

    const auto& frames = s_loadedTracks[trackIdx].frames;
    if (frames.empty()) return false;

    if (targetTime <= frames.front().time) {
        outFrame = frames.front();
        return true;
    }
    if (targetTime >= frames.back().time) {
        outFrame = frames.back();
        return true;
    }

    auto it = std::lower_bound(frames.begin(), frames.end(), targetTime,
        [](const ReplayFrame& f, float t) { return f.time < t; });

    if (it == frames.begin()) {
        outFrame = *it;
        return true;
    }

    const ReplayFrame& f1 = *it;
    const ReplayFrame& f0 = *(it - 1);
    float delta = f1.time - f0.time;
    float factor = (delta > 0.00001f) ? ((targetTime - f0.time) / delta) : 0.0f;
    factor = std::clamp(factor, 0.0f, 1.0f);

    InterpolateFrames(f0, f1, factor, targetTime, outFrame);
    return true;
}

std::vector<ReplayInfo> ReplayEngine::ListSavedReplays() {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    std::vector<ReplayInfo> results;

    std::wstring searchPattern = s_replaysDir + L"\\*.cxr2";
    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(searchPattern.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return results;

    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            std::wstring fullPath = s_replaysDir + L"\\" + fd.cFileName;
            std::ifstream in(fullPath, std::ios::binary);
            if (in.is_open()) {
                ReplayHeader h;
                in.read(reinterpret_cast<char*>(&h), sizeof(ReplayHeader));
                if (h.magic[0] == 'C' && h.magic[1] == 'X' && h.magic[2] == 'R' && h.magic[3] == '2') {
                    ReplayInfo info;
                    info.filename = Logger::ToNarrow(fd.cFileName);
                    info.trackName = h.trackName;
                    info.carName = h.carName;
                    info.duration = h.duration;
                    info.frameCount = h.frameCount;
                    info.trackCount = 1;
                    if (h.version >= 3) {
                        uint32_t tc = 1;
                        in.read(reinterpret_cast<char*>(&tc), sizeof(tc));
                        if (in.good() && tc > 0 && tc <= 64) {
                            info.trackCount = tc;
                        }
                    }
                    if (h.version >= 4) {
                        in.clear();
                        in.seekg(0, std::ios::end);
                        std::streamoff fileSz = in.tellg();
                        if (fileSz >= static_cast<std::streamoff>(sizeof(ReplayHeader) + sizeof(ReplayCarFxFooter))) {
                            in.seekg(-static_cast<std::streamoff>(sizeof(ReplayCarFxFooter)), std::ios::end);
                            ReplayCarFxFooter cfx{};
                            in.read(reinterpret_cast<char*>(&cfx), sizeof(ReplayCarFxFooter));
                            if (in.good() && cfx.footerMagic[0] == 'C' && cfx.footerMagic[1] == 'F' &&
                                cfx.footerMagic[2] == 'X' && cfx.footerMagic[3] == '1' && cfx.enabled != 0) {
                                cfx.shaderPath[sizeof(cfx.shaderPath) - 1] = '\0';
                                if (cfx.shaderPath[0] != '\0') {
                                    info.hasCarFx = true;
                                    info.carFxShader = cfx.shaderPath;
                                }
                            }
                        }
                    }

                    std::time_t t = static_cast<std::time_t>(h.recordedTimestamp);
                    std::tm tmInfo;
                    localtime_s(&tmInfo, &t);
                    char dateBuf[64];
                    std::strftime(dateBuf, sizeof(dateBuf), "%Y-%m-%d %H:%M", &tmInfo);
                    info.recordedDate = dateBuf;

                    results.push_back(info);
                }
                in.close();
            }
        }
    } while (FindNextFileW(hFind, &fd));

    FindClose(hFind);
    return results;
}

std::wstring ReplayEngine::GetReplaysDirectory() {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    return s_replaysDir;
}
