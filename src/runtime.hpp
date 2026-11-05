#pragma once
#include <windows.h>

namespace litedx12 {
enum class State : unsigned { Waiting, Ready, Disabled, Failed };
// One switch for every log line in the plugin.
// Off: no log file is written and no message is formatted.
// Basic: startup, identity, state changes and failures.
// Verbose: additional diagnostics.
enum class Detail : unsigned { Off = 0, Basic = 1, Verbose = 2 };
// Fixed-size, versioned snapshot. No pointers into runtime-owned memory.
struct Status {
    bool enabled = true;
    wchar_t pluginPath[1024]{};
    wchar_t systemPath[1024]{};
};
void Initialize(HMODULE plugin, HMODULE system);
void Fail(DWORD error);
Status Snapshot();
// Lock-free hot-path queries; Snapshot copies the full status block.
bool Enabled();
bool Usable();
bool SkipSmallSwapchains();
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
}
