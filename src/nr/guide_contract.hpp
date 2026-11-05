// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "frame_input.hpp"
#include <algorithm>
namespace litedx12::nr {
struct GuideMetadata {
    unsigned flags{},renderW{},renderH{},outW{},outH{};
    bool flagsKnown{};
};
struct TextureExtent { unsigned width{},height{}; };
// Lite pass.cpp RunPass: allocations are bounds, not the active image.
inline HRESULT ResolveGuideExtents(Guides& g,const GuideMetadata& m,
    const std::array<TextureExtent,3>& allocations,unsigned renderW,unsigned renderH) {
    g.motionResolutionKnown=m.flagsKnown;
    const auto bounded=[](unsigned size,unsigned base,unsigned wanted,unsigned& result) {
        if(base>=size||!wanted)return false;
        result=std::min(size-base,wanted);return true;
    };
    // A successful NGX Get can still return zero: Opt treats that as unspecified.
    // Prefer the creation size, then the available depth allocation.
    if(!renderW)renderW=m.renderW?m.renderW:
        allocations[1].width-std::min(allocations[1].width,g.originX[1]);
    if(!renderH)renderH=m.renderH?m.renderH:
        allocations[1].height-std::min(allocations[1].height,g.originY[1]);
    // Carry the declared output size through to PassExecutor's partial-output guard.
    // Do not clamp it: an oversized declaration must fail validation, not look full-sized.
    g.width[0]=m.outW;
    g.height[0]=m.outH;
    if(allocations[1].width && (!bounded(allocations[1].width,g.originX[1],renderW,g.width[1]) ||
        !bounded(allocations[1].height,g.originY[1],renderH,g.height[1])))return E_INVALIDARG;
    if(allocations[2].width && m.flagsKnown) {
        const bool low=(m.flags&2)!=0;
        const unsigned w=low?renderW:(m.outW?m.outW:allocations[0].width);
        const unsigned h=low?renderH:(m.outH?m.outH:allocations[0].height);
        if(!bounded(allocations[2].width,g.originX[2],w,g.width[2]) ||
           !bounded(allocations[2].height,g.originY[2],h,g.height[2]))return E_INVALIDARG;
    }
    return S_OK;
}
}
