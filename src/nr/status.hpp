// SPDX-License-Identifier: GPL-3.0-or-later
// Panel/preview state: whether the resource preview is on, the display scale, the
// depth preview flips, and the status block the panel reads. No frame work here.
#pragma once
#include "frame_input.hpp"
#include "../runtime.hpp"
#include <mutex>
namespace litedx12::nr
{
struct ResourceInfo
{
    unsigned width{}, height{}, format{};
    bool present{};
};
struct PreviewStatus
{
    ResourceInfo resources[3]{};
    float motionScaleX = 1, motionScaleY = 1;
};
// The preview has one mode: all three inputs side by side, so only its demand toggles.
bool PreviewSelected();
// Preview demand only; model resource processing must use its own demand.
bool PreviewActive();
bool PanelVisible();
void SetPanelVisible(const void* owner, bool visible);
void SetPreviewSelected(bool);
float DisplayScale();
void SetDisplayScale(float, bool persist = true);
bool DepthContrast();
void SetDepthContrast(bool, bool persist = true);
bool DepthFlip();
void SetDepthFlip(bool, bool persist = true);
PreviewStatus PreviewSnapshot();
// Shared with the capture driver: one status block, one lock, one panel registry.
namespace detail
{
std::recursive_mutex& StatusMutex();
PreviewStatus& StatusBlock();
}
}
