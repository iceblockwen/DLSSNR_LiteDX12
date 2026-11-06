#pragma once
#include <windows.h>
#include <string>

namespace litedx12
{
enum class State : unsigned
{
    Waiting,
    Ready,
    Disabled,
    Failed
};
// One switch for every log line in the plugin.
// Off: no log file is written and no message is formatted.
// Basic: startup, identity, state changes and failures.
// Verbose: additional diagnostics.
enum class Detail : unsigned
{
    Off = 0,
    Basic = 1,
    Verbose = 2
};
void Initialize(HMODULE plugin);
void Fail(DWORD error);
// Folder of the plugin DLL, resolved once at Initialize. Files beside the plugin
// (the model, the log) are found through it.
const std::wstring& PluginFolder();
// Lock-free hot-path queries.
bool Enabled();
bool Usable();
bool DirectResolve();
// Diagnostics.ForceTiming: run the GPU timing path with the panel closed. Panel checkbox, off by
// default. Costs four timestamps and one query resolve per frame while enabled; the timestamp
// queries also force the pipeline drain that a DirectResolve UAV write needs to become visible.
bool ForceTiming();
void SetForceTiming(bool value);
// Diagnostics.RequireComputeSignature: refuse recordings without a captured compute root.
bool RequireComputeSignature();
// Diagnostics.Level: 0 off, 1 basic, 2 verbose. Lock-free.
Detail DetailLevel() noexcept;
// Formats only when the current level is at least `level`, so Off builds no string.
void LogFormat(Detail level, const wchar_t* format, ...) noexcept;
// hooks.cpp: the startup detours (factory exports, loader, GetProcAddress).
void InstallCapture();
}
