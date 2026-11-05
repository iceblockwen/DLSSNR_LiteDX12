// SPDX-License-Identifier: GPL-3.0-or-later
// The process-lifetime registry the graphics layer owns: the panel bound to each
// swapchain and the lock that guards it. panel.cpp includes this to reach the same
// lock, nothing else does.
#pragma once
#include "graphics.hpp"
#include "panel.hpp"
#include <map>
#include <memory>
#include <mutex>
namespace litedx12 {
struct GraphicsRegistry {
    std::recursive_mutex mutex;
    std::map<IDXGISwapChain*, std::shared_ptr<Panel>> panels;
};
GraphicsRegistry& Graphics();
}
