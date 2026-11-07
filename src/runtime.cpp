#include "runtime.hpp"
#include "graphics.hpp"
#include "config.hpp"
#include "nr/status.hpp"
#include "nr/model_settings.hpp"
#include "nr/model_runtime.hpp"
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

namespace
{
std::wstring pluginFolder;
std::atomic<bool> enabled{true}, usable{true}, logging{true};
std::atomic<unsigned> detail{1};
std::atomic<bool> directResolve{};
// Diagnostics.ForceTiming: run the GPU timing path even with the panel closed. The panel's
// checkbox writes this; it defaults to off.
std::atomic<bool> forceTiming{};
std::atomic<bool> requireComputeSignature{true};
// Present, window messages and capture paths only enqueue. One detached worker
// owns the file; the pinned module keeps this state for the process lifetime.
struct LogQueue
{
    std::mutex mutex;
    std::condition_variable ready;
    std::deque<std::wstring> lines;
    std::wstring path;
    size_t dropped{};
    bool started{};
    void Run()
    {
        FILE* file{};
        std::unique_lock lock(mutex);
        for (;;)
        {
            ready.wait(lock, [&]
                       {
                           return !lines.empty();
                       });
            auto batch = std::move(lines);
            lines.clear();
            if (dropped)
            {
                wchar_t note[128]{};
                swprintf_s(note, L"Log queue saturated; %llu earlier line(s) dropped",
                           static_cast<unsigned long long>(dropped));
                batch.emplace_front(note);
                dropped = 0;
            }
            // One file per process run: the previous session's events never mix in.
            if (!file)
            {
                file = _wfsopen(path.c_str(), L"w, ccs=UTF-8", _SH_DENYNO);
            }
            lock.unlock();
            if (file)
            {
                for (const auto& line : batch)
                {
                    fwprintf(file, L"%ls\n", line.c_str());
                }
                fflush(file);
            }
            lock.lock();
        }
    }
};
LogQueue& Logs()
{
    static auto* queue = new LogQueue;
    return *queue;
}
}
namespace litedx12
{
void Initialize(HMODULE plugin)
{
    wchar_t path[1024]{};
    GetModuleFileNameW(plugin, path, 1024);
    const std::filesystem::path folder = std::filesystem::path(path).parent_path();
    pluginFolder = folder.wstring();
    const auto ini = InitializeConfigStorage((folder / L"DLSSNR_LiteDX12.ini").wstring());
    // Path only: the queue opens the file on its first line, so a run that
    // never logs leaves nothing behind.
    {
        std::lock_guard logLock(Logs().mutex);
        Logs().path = (folder / L"DLSSNR_LiteDX12.log").wstring();
    }
    const bool requested = GetPrivateProfileIntW(L"General", L"Enabled", 1, ini.c_str()) != 0;
    // Level is authoritative; Log=0/1 keeps existing configurations working.
    const int configured = GetPrivateProfileIntW(L"Diagnostics", L"Level", -1, ini.c_str());
    unsigned level = configured >= 0
                         ? unsigned(configured)
                     : GetPrivateProfileIntW(L"General", L"Log", 1, ini.c_str()) ? 1u
                                                                                 : 0u;
    level = level > 2 ? 2u : level;
    enabled = requested;
    usable = requested;
    logging = level >= 1;
    detail.store(level, std::memory_order_relaxed);
    // Compatibility policies are INI-driven; no executable-name checks.
    directResolve = GetPrivateProfileIntW(L"Diagnostics", L"DirectResolve", 0, ini.c_str()) != 0;
    forceTiming = GetPrivateProfileIntW(L"Diagnostics", L"ForceTiming", 0, ini.c_str()) != 0;
    requireComputeSignature = GetPrivateProfileIntW(L"Diagnostics", L"RequireComputeSignature", 1, ini.c_str()) != 0;
    nr::ConfigureModel(ini.c_str());
    nr::SetDepthContrast(GetPrivateProfileIntW(L"Diagnostics", L"DepthContrast", 0, ini.c_str()) != 0, false);
    wchar_t previewScale[32]{};
    GetPrivateProfileStringW(L"Diagnostics", L"ResourceScale", L"1", previewScale, 32, ini.c_str());
    nr::SetDisplayScale(static_cast<float>(_wtof(previewScale)), false);
}
const std::wstring& PluginFolder()
{
    return pluginFolder;
}
void Fail(DWORD)
{
    usable = false;
}
bool Enabled()
{
    return enabled.load(std::memory_order_relaxed);
}
bool Usable()
{
    return usable.load(std::memory_order_relaxed);
}
bool DirectResolve()
{
    return directResolve.load(std::memory_order_relaxed);
}
bool ForceTiming()
{
    return forceTiming.load(std::memory_order_relaxed);
}
void SetForceTiming(bool value)
{
    forceTiming.store(value, std::memory_order_relaxed);
}
bool RequireComputeSignature()
{
    return requireComputeSignature.load(std::memory_order_relaxed);
}
Detail DetailLevel() noexcept
{
    return static_cast<Detail>(detail.load(std::memory_order_relaxed));
}
void LogFormat(Detail level, const wchar_t* format, ...) noexcept
{
    if (!format || DetailLevel() < level)
    {
        return;
    }
    wchar_t text[2600]{};
    va_list args;
    va_start(args, format);
    _vsnwprintf_s(text, std::size(text), _TRUNCATE, format, args);
    va_end(args);
    Log(text);
}
void Log(const wchar_t* message) noexcept
{
    try
    {
        if (!message || !*message)
        {
            return;
        }
        if (!logging)
        {
            return;
        }
        auto& queue = Logs();
        {
            std::lock_guard lock(queue.mutex);
            if (queue.path.empty())
            {
                return;
            }
            if (queue.lines.size() >= 4096)
            {
                ++queue.dropped;
                return;
            }
            queue.lines.emplace_back(message);
            if (!queue.started)
            {
                std::thread([&queue]
                            {
                                queue.Run();
                            })
                    .detach();
                queue.started = true;
            }
        }
        queue.ready.notify_one();
    }
    catch (...)
    {
    }
}
}

// ---- Start: Lite loader.cpp/hooks.cpp startup, checked against Opt's dllmain.cpp and
// LibraryLoad_Hooks.cpp. All initialization runs after DLL_PROCESS_ATTACH. ----
namespace
{
HMODULE own{};
std::once_flag once;
std::atomic<DWORD> startupError{};
}
extern "C" __declspec(dllexport) DWORD WINAPI Start(void*)
{
    std::call_once(once, []
                   {
                       try
                       {
                           litedx12::Initialize(own);
                           const bool active = litedx12::Enabled();
                           // Install capture before any future heavyweight model preload.
                           if (active)
                           {
                               litedx12::InstallCapture();
                           }
                           litedx12::StartConfigWriter();
                           if (active)
                           {
                               litedx12::nr::DiscoverModelRuntime();
                           }
                       }
                       catch (DWORD error)
                       {
                           litedx12::Fail(error);
                           startupError = error;
                       }
                       catch (HRESULT error)
                       {
                           litedx12::Fail(DWORD(error));
                           startupError = DWORD(error);
                       }
                       catch (...)
                       {
                           litedx12::Fail(ERROR_DLL_INIT_FAILED);
                           startupError = ERROR_DLL_INIT_FAILED;
                       }
                   });
    return startupError.load();
}
BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        own = module;
    }
    return TRUE;
}
