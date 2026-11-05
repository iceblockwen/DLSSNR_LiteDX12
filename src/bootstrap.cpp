// SPDX-License-Identifier: GPL-3.0-or-later
// Lite loader.cpp/hooks.cpp startup, checked against Opt's dllmain.cpp and
// LibraryLoad_Hooks.cpp. All initialization runs after DLL_PROCESS_ATTACH.
#include "bootstrap.hpp"
#include "graphics.hpp"
#include "config_writer.hpp"
#include "nr/model_runtime.hpp"
#include <atomic>
namespace {
HMODULE own{};
std::once_flag once;
std::atomic<DWORD> startupError{};
}
namespace litedx12 {
DWORD StartupError() { return startupError.load(); }
}
extern "C" __declspec(dllexport) DWORD WINAPI Start(void*) {
    std::call_once(once, [] {
        try {
            wchar_t path[MAX_PATH]{};
            if (!GetSystemDirectoryW(path, MAX_PATH) || wcscat_s(path, L"\\dxgi.dll")) throw DWORD(ERROR_BAD_PATHNAME);
            const auto system = LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (!system) throw GetLastError();
            litedx12::Initialize(own, system);
            const bool enabled = litedx12::Enabled();
            // Install capture before any future heavyweight model preload.
            if (enabled) litedx12::InstallCapture();
            litedx12::StartConfigWriter();
            if (enabled) litedx12::nr::DiscoverModelRuntime();
        } catch (DWORD error) {
            litedx12::Fail(error); startupError = error;
        } catch (HRESULT error) {
            litedx12::Fail(DWORD(error)); startupError = DWORD(error);
        } catch (...) {
            litedx12::Fail(ERROR_DLL_INIT_FAILED); startupError = ERROR_DLL_INIT_FAILED;
        }
    });
    return litedx12::StartupError();
}
extern "C" __declspec(dllexport) BOOL WINAPI LiteAttachFactory(REFIID iid, void** output) {
    if (!litedx12::Usable()) return FALSE;
    litedx12::AttachFactory(iid, output); return TRUE;
}
BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) { if (reason == DLL_PROCESS_ATTACH) own = module; return TRUE; }
