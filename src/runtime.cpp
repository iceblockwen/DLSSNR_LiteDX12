#include "runtime.hpp"
#include "graphics.hpp"
#include "nr/status.hpp"
#include "nr/model_settings.hpp"
#include "config_storage.hpp"
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdarg>
#include <cwchar>
#include <deque>
#include <filesystem>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <share.h>

#define LITE_WIDEN2(text) L##text
#define LITE_WIDEN(text) LITE_WIDEN2(text)

namespace {
std::mutex statusMutex;
litedx12::Status status;
std::atomic<bool> enabled{true}, usable{true}, logging{true};
std::atomic<unsigned> detail{1};
std::atomic<bool> skipSmallSwapchains{};
std::atomic<bool> directResolve{};
// Diagnostics.ForceTiming: run the GPU timing path even with the panel closed. The panel's
// checkbox writes this; it defaults to off.
std::atomic<bool> forceTiming{};
std::atomic<bool> requireComputeSignature{true};
// Present, window messages and capture paths only enqueue. One detached worker
// owns the file; the pinned module keeps this state for the process lifetime.
struct LogQueue {
    std::mutex mutex;
    std::condition_variable ready;
    std::deque<std::wstring> lines;
    std::wstring path;
    size_t dropped{};
    bool started{};
    void Run() {
        FILE* file{};
        std::unique_lock lock(mutex);
        for (;;) {
            ready.wait(lock, [&] { return !lines.empty(); });
            auto batch = std::move(lines);
            lines.clear();
            if (dropped) {
                wchar_t note[128]{};
                swprintf_s(note, L"Log queue saturated; %llu earlier line(s) dropped",
                           static_cast<unsigned long long>(dropped));
                batch.emplace_front(note);
                dropped = 0;
            }
            // One file per process run: the previous session's events never mix in.
            if (!file) file = _wfsopen(path.c_str(), L"w, ccs=UTF-8", _SH_DENYNO);
            lock.unlock();
            if (file) {
                for (const auto& line : batch) fwprintf(file, L"%ls\n", line.c_str());
                fflush(file);
            }
            lock.lock();
        }
    }
};
LogQueue& Logs() { static auto* queue = new LogQueue; return *queue; }
}
namespace litedx12 {
void Initialize(HMODULE plugin, HMODULE system) {
    std::wstring ini, logPath, configPath;
    {
        std::lock_guard lock(statusMutex);
        GetModuleFileNameW(plugin, status.pluginPath, 1024);
        GetModuleFileNameW(system, status.systemPath, 1024);
        const auto folder = std::filesystem::path(status.pluginPath).parent_path();
        ini = InitializeConfigStorage((folder / L"DLSSNR_LiteDX12.ini").wstring());
        // The log follows the active configuration directory so a read-only game
        // folder still leaves a usable log when the INI was redirected.
        const auto config = ConfigStorageSnapshot();
        configPath = config.saveFile;
        const auto logFolder =
            config.saveFile.empty() ? folder : std::filesystem::path(config.saveFile).parent_path();
        logPath = (logFolder / L"DLSSNR_LiteDX12.log").wstring();
        // A read-only game folder cannot receive the log. Fall back to the
        // per-game profile folder the configuration writer already uses. Off
        // must not create a file, so the probe only runs when logging is on.
        if (logging) {
            FILE* probe{};
            if (_wfopen_s(&probe, logPath.c_str(), L"a") || !probe) {
                if (!config.fallbackFile.empty()) {
                    const auto fallback =
                        std::filesystem::path(config.fallbackFile).parent_path();
                    std::error_code error;
                    std::filesystem::create_directories(fallback, error);
                    logPath = (fallback / L"DLSSNR_LiteDX12.log").wstring();
                }
            } else
                fclose(probe);
        }
        { std::lock_guard logLock(Logs().mutex); Logs().path = logPath; }
        status.enabled = GetPrivateProfileIntW(L"General", L"Enabled", 1, ini.c_str()) != 0;
        // Level is authoritative; Log=0/1 keeps existing configurations working.
        const int configured = GetPrivateProfileIntW(L"Diagnostics", L"Level", -1, ini.c_str());
        unsigned level = configured >= 0
                             ? unsigned(configured)
                             : GetPrivateProfileIntW(L"General", L"Log", 1, ini.c_str()) ? 1u : 0u;
        level = level > 2 ? 2u : level;
        enabled = status.enabled; usable = status.enabled;
        logging = level >= 1;
        detail.store(level, std::memory_order_relaxed);
    }
    // Compatibility policies are INI-driven; no executable-name checks.
    directResolve = GetPrivateProfileIntW(L"Diagnostics", L"DirectResolve", 0, ini.c_str()) != 0;
    forceTiming = GetPrivateProfileIntW(L"Diagnostics", L"ForceTiming", 0, ini.c_str()) != 0;
    skipSmallSwapchains = GetPrivateProfileIntW(L"Diagnostics", L"SkipSmallSwapchains", 0, ini.c_str()) != 0;
    requireComputeSignature=GetPrivateProfileIntW(L"Diagnostics",L"RequireComputeSignature",1,ini.c_str())!=0;
    nr::ConfigureModel(ini.c_str());
    nr::SetDepthContrast(GetPrivateProfileIntW(L"Diagnostics", L"DepthContrast", 0, ini.c_str()) != 0, false);
    nr::SetDepthFlip(GetPrivateProfileIntW(L"Resources", L"DepthUpsideDown", 0, ini.c_str()) != 0, false);
    wchar_t previewScale[32]{};
    GetPrivateProfileStringW(L"Diagnostics", L"ResourceScale", L"1", previewScale, 32, ini.c_str());
    nr::SetDisplayScale(static_cast<float>(_wtof(previewScale)), false);
    const auto current = Snapshot();
    const auto level = unsigned(DetailLevel());
    if (level) {
        SYSTEMTIME now{};
        GetLocalTime(&now);
        wchar_t header[2600]{};
        swprintf_s(header, L"[%04u-%02u-%02u %02u:%02u:%02u] DLSSNR_LiteDX12 " LITE_WIDEN(LITE_VERSION) L" PID=%lu enabled=%u level=%u\nRuntime: %ls\nSystem DXGI: %ls\nStartup: configuring graphics capture; NR model not initialized.",
            now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond,
            GetCurrentProcessId(), current.enabled, level, current.pluginPath, current.systemPath);
        Log(header);
        LogFormat(Detail::Basic, L"Log file: %ls", logPath.c_str());
        if (!configPath.empty())
            LogFormat(Detail::Basic, L"Configuration: %ls", configPath.c_str());
    }
    if (SkipSmallSwapchains())
        Log(L"Compatibility: SkipSmallSwapchains=1; swapchains with actual width or height below 100 returned unchanged");
    if (DirectResolve())
        Log(L"Compatibility: DirectResolve=1; NR copies inputs and resolves into the captured color UAV");
    if (ForceTiming())
        Log(L"Performance: ForceTiming=1; GPU timing queries run with the panel closed (needed when NR resolves into the game's own colour)");
    if (!RequireComputeSignature())
        Log(L"Compatibility: RequireComputeSignature=0; NR runs without a captured compute root signature");
}
void Fail(DWORD error) {
    usable = false;
    LogFormat(Detail::Basic, L"System DXGI forwarding failed; plugin unavailable (error %lu)",
              static_cast<unsigned long>(error));
}
Status Snapshot() { std::lock_guard lock(statusMutex); return status; }
bool Enabled() { return enabled.load(std::memory_order_relaxed); }
bool Usable() { return usable.load(std::memory_order_relaxed); }
bool SkipSmallSwapchains() { return skipSmallSwapchains.load(std::memory_order_relaxed); }
bool DirectResolve() { return directResolve.load(std::memory_order_relaxed); }
bool ForceTiming() { return forceTiming.load(std::memory_order_relaxed); }
void SetForceTiming(bool value) { forceTiming.store(value, std::memory_order_relaxed); }
bool RequireComputeSignature() { return requireComputeSignature.load(std::memory_order_relaxed); }
Detail DetailLevel() noexcept {
    return static_cast<Detail>(detail.load(std::memory_order_relaxed));
}
void LogFormat(Detail level, const wchar_t* format, ...) noexcept {
    if (!format || DetailLevel() < level) return;
    wchar_t text[2600]{};
    va_list args;
    va_start(args, format);
    _vsnwprintf_s(text, std::size(text), _TRUNCATE, format, args);
    va_end(args);
    Log(text);
}
void Log(const wchar_t* message) noexcept {
    try {
        if (!message || !*message) return;
        if (!logging) return;
        auto& queue = Logs();
        {
            std::lock_guard lock(queue.mutex);
            if (queue.path.empty()) return;
            if (queue.lines.size() >= 4096) { ++queue.dropped; return; }
            queue.lines.emplace_back(message);
            if (!queue.started) { std::thread([&queue] { queue.Run(); }).detach(); queue.started = true; }
        }
        queue.ready.notify_one();
    } catch (...) {}
}
}
