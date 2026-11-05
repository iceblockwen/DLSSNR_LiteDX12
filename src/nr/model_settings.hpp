#pragma once
#include "pass.hpp"
namespace litedx12::nr
{
struct ModelSettings
{
    bool enabled{};
    PassSettings pass;
};
void ConfigureModel(const wchar_t* ini);
ModelSettings GetModelSettings();
void SetModelSettings(const ModelSettings&, bool persist = true);
bool ModelRequested();
bool ModelActive();
bool ProcessingActive();
void PublishPass(const PassStatus&);
// Verbose-only (Diagnostics.Level=2): exact NR skip accounting. No work at Level 0/1.
PassStatus LastPass();
UINT64 ModelRevision();
// Panel checkbox: run the timing path with the panel closed, persisted to [Diagnostics] ForceTiming.
void SetForceTimingPersisted(bool value);
}
