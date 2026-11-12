#include "safe_mode.h"
#include "settings.h"
#include "crash_handler.h"
#include "logger.h"
#include "urushi_ui.h"

#include <shellapi.h>
#include <atomic>
#include <imgui.h>

namespace SafeMode {

namespace {

std::atomic<Mode> g_mode{ Mode::Booting };
std::atomic<bool> g_featuresStarted{ false };
void (*g_startFn)() = nullptr;
bool g_rememberChoice = false;   // dialog checkbox (render thread only)

const char* ModeName(Mode m) {
    switch (m) {
    case Mode::Booting: return "booting";
    case Mode::AwaitingChoice: return "awaiting-choice";
    case Mode::Safe: return "SAFE";
    case Mode::Starting: return "starting";
    case Mode::Full: return "full";
    }
    return "unknown";
}

void SetMode(Mode m) {
    g_mode.store(m, std::memory_order_release);
    CrashHandler::SetModeName(ModeName(m));
}

DWORD WINAPI StartThread(LPVOID) {
    if (g_startFn) g_startFn();
    g_featuresStarted.store(true, std::memory_order_release);
    SetMode(Mode::Full);
    Logger::Info("Safe mode left: features started.");
    return 0;
}

void BeginStart() {
    Mode expected = g_mode.load();
    if (expected == Mode::Starting || expected == Mode::Full) return;
    SetMode(Mode::Starting);
    HANDLE h = CreateThread(nullptr, 0, StartThread, nullptr, 0, nullptr);
    if (h) CloseHandle(h);
}

bool Ru() { return !UrushiUI::IsEnglish(); }
const char* Tr(const char* ru, const char* en) { return Ru() ? ru : en; }

void OpenCrashDir() {
    const std::wstring dir = CrashHandler::GetCrashDir();
    ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void DrawCrashInfo() {
    const auto& pc = CrashHandler::GetPreviousCrash();
    if (!pc.crashed) return;
    UrushiUI::Text(Tr("Предыдущая сессия завершилась аварийно.", "The previous session crashed."), UrushiUI::Tone::Yellow);
    if (!pc.summary.empty()) UrushiUI::TextWrapped(pc.summary, UrushiUI::Tone::Muted);
    UrushiUI::Text(pc.inUrushi ? Tr("Падение произошло в коде URUSHI.", "The crash happened inside URUSHI code.")
                               : Tr("Падение произошло вне кода URUSHI (игра/драйвер).", "The crash happened outside URUSHI (game/driver)."),
                   pc.inUrushi ? UrushiUI::Tone::Red : UrushiUI::Tone::Muted);
    UrushiUI::Spacing(4);
}

void DrawPrompt() {
    ImGuiIO& io = ImGui::GetIO();
    io.MouseDrawCursor = true; // the game may hide the OS cursor on the loading screen

    const float S = UrushiUI::GetUiScale();
    const ImVec2 disp = io.DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(disp.x * 0.5f, disp.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(500.0f * S, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowFocus();
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoScrollbar;
    if (ImGui::Begin("URUSHI##safe_prompt", nullptr, flags)) {
        UrushiUI::Text(Tr("Запустить с Safe mode?", "Start in Safe mode?"), UrushiUI::Tone::Accent);
        UrushiUI::Spacing(4);
        DrawCrashInfo();
        UrushiUI::TextWrapped(
            Tr("Safe mode отключает все функции мода (скрипты, хуки игры, CarFX, оптимизатор, реплеи). "
               "Сам мод остаётся загруженным, функции можно включить позже из этого же окна.",
               "Safe mode turns off every feature (scripts, game hooks, CarFX, optimizer, replays). "
               "The mod itself stays loaded and you can enable features later from this same window."),
            UrushiUI::Tone::Muted);
        UrushiUI::Spacing(6);

        UrushiUI::Toggle(Tr("Запомнить выбор", "Remember my choice"), &g_rememberChoice);
        UrushiUI::Text(Tr("Можно изменить позже: Система → Стабильность", "Changeable later: System → Stability"), UrushiUI::Tone::Muted);
        UrushiUI::Spacing(8);

        const float bw = (ImGui::GetContentRegionAvail().x - 8.0f * S) * 0.5f;
        bool pickSafe = UrushiUI::Button(Tr("Safe mode  (Y)", "Safe mode  (Y)"), bw, 34.0f, UrushiUI::ButtonStyle::Primary);
        ImGui::SameLine(0.0f, 8.0f * S);
        bool pickNormal = UrushiUI::Button(Tr("Обычный запуск  (N)", "Normal start  (N)"), bw, 34.0f);

        if (ImGui::IsKeyPressed(ImGuiKey_Y, false)) pickSafe = true;
        if (ImGui::IsKeyPressed(ImGuiKey_N, false)) pickNormal = true;

        if (pickSafe) ChooseSafe(g_rememberChoice);
        else if (pickNormal) RequestFull(g_rememberChoice);
    }
    ImGui::End();
}

void DrawSafeWindow() {
    const float S = UrushiUI::GetUiScale();
    ImGui::SetNextWindowPos(ImVec2(40.0f * S, 40.0f * S), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(440.0f * S, 0.0f), ImGuiCond_Always);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize;
    if (ImGui::Begin("URUSHI - Safe mode##safe_window", nullptr, flags)) {
        const Mode m = g_mode.load();
        UrushiUI::Badge("SAFE MODE", UrushiUI::Tone::Yellow);
        UrushiUI::Spacing(4);
        DrawCrashInfo();
        UrushiUI::TextWrapped(
            Tr("Все функции мода отключены. Игра работает без вмешательства URUSHI.",
               "All mod features are disabled. The game runs without URUSHI interfering."),
            UrushiUI::Tone::Muted);
        UrushiUI::Spacing(6);

        // crash policy
        UrushiUI::Text(Tr("Что делать после краша", "After a crash"), UrushiUI::Tone::Muted);
        int pol = static_cast<int>(Settings::GetCrashPolicy());
        if (UrushiUI::Segmented("safe_policy", &pol,
                                { Tr("Спрашивать", "Ask"), "Safe mode", Tr("Обычный запуск", "Normal") })) {
            Settings::SetCrashPolicy(static_cast<Settings::CrashPolicy>(pol));
        }
        UrushiUI::Spacing(6);

        const float bw = ImGui::GetContentRegionAvail().x;
        if (m == Mode::Starting) {
            UrushiUI::Text(Tr("Запуск функций...", "Starting features..."), UrushiUI::Tone::Accent);
        } else if (UrushiUI::Button(Tr("Выйти из Safe mode (включить функции)", "Leave Safe mode (enable features)"),
                                    bw, 34.0f, UrushiUI::ButtonStyle::Primary)) {
            RequestFull(false);
        }
        if (UrushiUI::Button(Tr("Открыть папку отчётов о крашах", "Open crash reports folder"), bw, 28.0f, UrushiUI::ButtonStyle::Ghost)) {
            OpenCrashDir();
        }
    }
    ImGui::End();
}

} // namespace

bool Decide() {
    CrashHandler::PreviousCrash pc = CrashHandler::ConsumePreviousCrash();

    if (Settings::GetBool("stability", "force_safe_next", false)) {
        Settings::SetBool("stability", "force_safe_next", false);
        Logger::Warn("Safe mode requested by the user for this launch.");
        SetMode(Mode::Safe);
        return false;
    }

    if (!pc.crashed) {
        return true; // caller starts the features and then calls MarkFull()
    }

    Logger::Warn("Previous session crashed: " + pc.summary + (pc.inUrushi ? " [in URUSHI]" : " [outside URUSHI]"));
    switch (Settings::GetCrashPolicy()) {
    case Settings::CrashPolicy::Safe:
        Logger::Warn("Crash policy = safe: starting in Safe mode.");
        SetMode(Mode::Safe);
        return false;
    case Settings::CrashPolicy::Normal:
        Logger::Info("Crash policy = normal: starting normally.");
        return true;
    default:
        Logger::Info("Crash policy = ask: waiting for the user's choice.");
        SetMode(Mode::AwaitingChoice);
        return false;
    }
}

void MarkFull() {
    g_featuresStarted.store(true, std::memory_order_release);
    SetMode(Mode::Full);
}

Mode GetMode() { return g_mode.load(std::memory_order_acquire); }
bool IsFull() { return g_mode.load(std::memory_order_acquire) == Mode::Full; }
bool FeaturesStarted() { return g_featuresStarted.load(std::memory_order_acquire); }

void SetStartFeaturesCallback(void (*fn)()) { g_startFn = fn; }

void RequestFull(bool remember) {
    if (remember) Settings::SetCrashPolicy(Settings::CrashPolicy::Normal);
    ImGui::GetIO().MouseDrawCursor = false;
    BeginStart();
}

void ChooseSafe(bool remember) {
    if (remember) Settings::SetCrashPolicy(Settings::CrashPolicy::Safe);
    ImGui::GetIO().MouseDrawCursor = false;
    SetMode(Mode::Safe);
    Logger::Info("User chose Safe mode.");
}

void RenderUI(bool menuOpen) {
    switch (g_mode.load(std::memory_order_acquire)) {
    case Mode::AwaitingChoice:
        DrawPrompt();
        break;
    case Mode::Safe:
    case Mode::Starting:
        if (menuOpen) DrawSafeWindow();
        break;
    default:
        break;
    }
}

} // namespace SafeMode
