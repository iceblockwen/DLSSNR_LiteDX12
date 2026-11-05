// The one contract between the observation layer and the pipeline: what the game
// handed its own upscaler for this frame, plus the working set the pass records.
#pragma once
#include "gpu_util.hpp"
#include "gpu_ticket.hpp"
#include <array>
#include <memory>
namespace litedx12::nr
{
struct Exposure
{
    bool sceneLinear{}, prePresent{}, exposurePresent{};
    float pre = 1, scale = 1;
};
struct Guides
{
    float motionScaleX = 1, motionScaleY = 1;
    float displayScale = 1;
    bool depthInverted{}, flipDepth{}, depthContrast{}; // depthContrast affects preview only
    bool motionResolutionKnown = true;                  // explicit inputs know their encoding; NGX needs create flags
    unsigned originX[3]{}, originY[3]{}, width[3]{}, height[3]{};
};
struct ResourceMetadata
{
    D3D12_RESOURCE_DESC description{};
    bool valid{};
};
// Metadata belongs to the resource at the same index and is valid only for this frame.
// Capture validates device ownership before passing it to the NR executor.
// What the observation layer hands the pipeline. Resources are borrowed for this call.
struct FrameInput
{
    ID3D12Resource* resources[3]{};
    std::array<ResourceMetadata, 3> resourceMetadata{};
    D3D12_RESOURCE_STATES states[3]{D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
    Guides guides;
    Exposure exposure;
    ID3D12Resource* exposureResource{};
    ResourceMetadata exposureMetadata{};
    D3D12_RESOURCE_STATES exposureState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    bool reset{};
};
// The pass's working set: owned references, the ticket proving submission, and the
// state the recording left the resources in.
struct PassFrame
{
    std::array<ComPtr<ID3D12Resource>, 3> resources;
    std::array<ResourceMetadata, 3> resourceMetadata{};
    // Optional native output, distinct from the immutable input; arrives/leaves UAV.
    // Owned by this recording until its submission ticket completes.
    ComPtr<ID3D12Resource> resolveTarget;
    ResourceMetadata resolveTargetMetadata{};
    Guides guides;
    // The Execute owner runs ticket.PrepareSubmission on the actual queue before Execute.
    std::shared_ptr<GpuTicket> ticket;
    ComPtr<ID3D12Resource> exposureTexture;
    ResourceMetadata exposureMetadata{};
};
}
