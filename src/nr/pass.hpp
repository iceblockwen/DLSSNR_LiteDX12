#pragma once
#include "white_point.hpp"
#include "ngx_parameters.hpp"
#include <string>
namespace litedx12::nr
{
// The one runtime that owns NGX capabilities and device validation. Forward
// declared here so the pass does not depend on the model headers.
class ModelRuntime;
struct PassSettings
{
    WhiteSettings white;
    bool gpuTiming{};       // runtime panel demand only; never persisted
    bool panelStatistics{}; // runtime demand only; never persisted
    float differenceGain = 1;
    bool composed = true;

    unsigned preset{}, style{}, autoMask = 1, uiCorrection{};
    float intensity = 1, structure = 1, tone = 1, skin = -1;
};
struct PassStatus
{
    HRESULT error = S_OK;
    bool frameSkipped{}; // input/recording rejection; does not latch a model failure
    unsigned ngxResult{};
    bool ready{}, edited{};
    unsigned readyPasses{}, actualPasses{}, failedPass{};
    bool timingValid{};
    float gpuMilliseconds{}, modelMilliseconds{};
    WhiteStatus white;
    bool hasInput{}, sceneLinear{}, gameExposureTexture{};
    bool preExposurePresent{}, exposureScalePresent{};
    float preExposure = 1, exposureScale = 1;
    HRESULT whiteMeasurementError = S_OK;
    bool gpuExposureValid{}, gpuDivisorValid{};
    float gpuExposure{}, gpuDivisor = 1;
    UINT64 inputTime{};
    bool inputConverted[3]{};
    float motionScaleX = 1, motionScaleY = 1;
    const char* reason = "Waiting for NR input";
};
// Inputs and scratch result rest in NON_PIXEL_SHADER_RESOURCE; an optional native
// resolveTarget arrives and leaves in UNORDERED_ACCESS. The recording owner must
// submit/discard frame.ticket and retain keepAlive until its fence completes.
// Feature initialization uses the caller's list, as in Lite's default route.
// Steady-state evaluations may follow already submitted work: the actual Execute
// owner orders ticket dependencies before submitting the recorded list. Descriptor
// reuse and readback still require completion.
// Not thread-safe: every call is made under the frame ledger's lock.
class PassExecutor12
{
  public:
    PassExecutor12(ID3D12Device*, ModelRuntime&);
    ~PassExecutor12();
    HRESULT Record(ID3D12GraphicsCommandList*, const PassFrame&, const Exposure&, const PassSettings&,
                   bool reset, std::shared_ptr<void>& keepAlive, ID3D12Resource*& result);
    PassStatus Snapshot() const;
    void InvalidateHistory();
    void RetireCompletedInputs(); // drop game texture references after the real fence
  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
bool LinearColorFormat(DXGI_FORMAT);
bool HasParkedModelFeatures();
void RetireModelFeatures();
}
