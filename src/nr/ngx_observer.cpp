// Observes the game's own NGX Direct3D 12 SR create/evaluate/release through the
// loader GetProcAddress hook and hands the evaluate's resources to the NR pass.
// Opt NR EvaluateAfterUpscale parameter handoff checked; this is resource capture only.
#include "capture12.hpp"
#include "ngx_parameters.hpp"
#include "model_runtime.hpp"
#include "model_settings.hpp"
#include "pass.hpp"
#include "guide_contract.hpp"
#include "command_guard.hpp"

#include "../graphics.hpp"
#include <array>
#include <map>
#include <mutex>
#include <cstring>
#include <type_traits>
namespace litedx12::nr
{
namespace
{
std::recursive_mutex mutex;
struct Metadata : GuideMetadata
{
    bool rayReconstruction{};
};
struct Entry
{
    HMODULE module{};
    // The game's original NGX functions: create, evaluate, release, evaluate_C. Written
    // once under mutex by ResolveNgx; the stubs read them lock-free on every call.
    std::atomic<FARPROC> hot[4]{};
    // One key per feature handle: every entry here is a D3D12 feature.
    std::map<void*, Metadata> features;
};
std::array<Entry, 8> entries;
GameDlssStatus gameDlss;
size_t lastEntry{};
void* lastFeature{};
thread_local bool evaluating{};
Metadata Describe(const Parameters* p, int feature)
{
    Metadata m;
    m.rayReconstruction = feature == 13;
    int flags{};
    m.flagsKnown = p->Get("DLSS.Feature.Create.Flags", &flags) == 1;
    m.flags = unsigned(flags);
    m.renderW = Read<unsigned>(p, "Width");
    m.renderH = Read<unsigned>(p, "Height");
    m.outW = Read<unsigned>(p, "OutWidth");
    m.outH = Read<unsigned>(p, "OutHeight");
    return m;
}
ID3D12Resource* GameResource(const Parameters* p, const Metadata& m,
                             const char* key, const char* rrKey)
{
    auto* resource = Read<ID3D12Resource*>(p, key);
    // Opt's RR path accepts the DLSSD aliases when the common key is absent/null.
    return !resource && m.rayReconstruction ? Read<ID3D12Resource*>(p, rrKey) : resource;
}
Guides Describe(const Parameters* p, const Metadata& m, ID3D12Resource* const* resources,
                std::array<ResourceMetadata, 3>& resourceMetadata)
{
    Guides g;
    g.motionScaleX = Read<float>(p, "MV.Scale.X", 1);
    g.motionScaleY = Read<float>(p, "MV.Scale.Y", 1);
    g.depthInverted = m.flagsKnown && (m.flags & 8) != 0;
    g.originX[1] = Read<unsigned>(p, "DLSS.Input.Depth.Subrect.Base.X");
    g.originY[1] = Read<unsigned>(p, "DLSS.Input.Depth.Subrect.Base.Y");
    g.originX[2] = Read<unsigned>(p, "DLSS.Input.MV.Subrect.Base.X");
    g.originY[2] = Read<unsigned>(p, "DLSS.Input.MV.Subrect.Base.Y");
    const auto w = Read<unsigned>(p, "DLSS.Render.Subrect.Dimensions.Width", m.renderW);
    const auto h = Read<unsigned>(p, "DLSS.Render.Subrect.Dimensions.Height", m.renderH);
    std::array<TextureExtent, 3> sizes{};
    for (unsigned n = 0; n < 3; ++n)
    {
        if (resources[n])
        {
            auto& metadata = resourceMetadata[n];
            metadata.description = resources[n]->GetDesc();
            metadata.valid = true;
            const auto& d = metadata.description;
            sizes[n] = {unsigned(d.Width), d.Height};
        }
    }
    if (FAILED(ResolveGuideExtents(g, m, sizes, w, h)))
    {
        throw E_INVALIDARG;
    }
    return g;
}
Exposure DescribeExposure(const Parameters* p, const Metadata& m, DXGI_FORMAT format)
{
    Exposure e;
    e.sceneLinear = m.flagsKnown && (m.flags & 1) && LinearColorFormat(format);
    e.prePresent = p->Get("DLSS.Pre.Exposure", &e.pre) == 1;
    e.exposurePresent = p->Get("DLSS.Exposure.Scale", &e.scale) == 1;
    return e;
}
template <size_t N>
unsigned __cdecl Create(
    ID3D12GraphicsCommandList* commands, int feature, Parameters* p, void** out)
{
    using Fn = unsigned(__cdecl*)(ID3D12GraphicsCommandList*, int, Parameters*, void**);
    const auto address = entries[N].hot[0].load(std::memory_order_acquire);
    if (!address)
    {
        return 0xBAD0000Bu;
    }
    try
    {
        InstallCommandState(commands);
    }
    catch (...)
    {
    }
    Metadata metadata;
    bool described = false;
    if ((feature == 1 || feature == 13) && p)
    {
        try
        {
            // The original create may reuse these keys for optimal-settings queries.
            // Preserve the caller's creation contract before entering that call.
            metadata = Describe(p, feature);
            described = true;
        }
        catch (...)
        {
        }
    }
    auto result = reinterpret_cast<Fn>(address)(commands, feature, p, out);
    if (result == 1 && described && out && *out)
    {
        try
        {
            std::lock_guard lock(mutex);
            entries[N].features[*out] = metadata;
        }
        catch (...)
        {
        }
    }
    return result;
}
template <size_t N, bool C = false>
unsigned __cdecl Evaluate(
    ID3D12GraphicsCommandList* commands, void* handle,
    std::conditional_t<C, const Parameters*, Parameters*> p, void* callback)
{
    using ParameterPointer = std::conditional_t<C, const Parameters*, Parameters*>;
    using Fn = unsigned(__cdecl*)(ID3D12GraphicsCommandList*, void*, ParameterPointer, void*);
    const auto address = entries[N].hot[C ? 3 : 1].load(std::memory_order_acquire);
    if (!address)
    {
        return 0xBAD0000Bu;
    }
    if (evaluating)
    {
        return reinterpret_cast<Fn>(address)(commands, handle, p, callback);
    }
    struct Guard
    {
        Guard()
        {
            evaluating = true;
        }
        ~Guard()
        {
            evaluating = false;
        }
    } guard;
    auto result = reinterpret_cast<Fn>(address)(commands, handle, p, callback);
    if (p && commands)
    {
        Metadata m;
        bool found = false;
        {
            std::lock_guard lock(mutex);
            const auto it = entries[N].features.find(handle);
            if (it != entries[N].features.end())
            {
                m = it->second;
                found = true;
                gameDlss.observed = true;
                gameDlss.released = false;
                gameDlss.result = result;
                gameDlss.lastEvaluation = GetTickCount64();
                lastEntry = N;
                lastFeature = handle;
            }
        }
        if (found && result == 1 && ProcessingActive())
        {
            try
            {
                FrameInput input;
                input.resources[0] = GameResource(p, m, "Output", "DLSSD.Output");
                input.resources[1] = GameResource(p, m, "Depth", "DLSSD.Depth");
                input.resources[2] = GameResource(p, m, "MotionVectors", "DLSSD.MotionVectors");
                input.guides = Describe(p, m, input.resources, input.resourceMetadata);
                if (input.resources[0])
                {
                    input.exposure =
                        DescribeExposure(p, m, input.resourceMetadata[0].description.Format);
                }
                input.exposureResource = Read<ID3D12Resource*>(p, "ExposureTexture");
                input.reset = Read<unsigned>(p, "Reset") != 0;
                // Lite's post-SR state contract; direct reads/copies restore originals.
                Capture12(commands, input, handle);
            }
            catch (...)
            {
            }
        }
    }
    return result;
}
template <size_t N>
unsigned __cdecl Release(void* handle)
{
    using Fn = unsigned(__cdecl*)(void*);
    const auto address = entries[N].hot[2].load(std::memory_order_acquire);
    {
        std::lock_guard lock(mutex);
        entries[N].features.erase(handle);
        if (gameDlss.observed && lastEntry == N && lastFeature == handle && !gameDlss.released)
        {
            gameDlss.released = true;
        }
    }
    ReleaseModelStream(handle);
    return address ? reinterpret_cast<Fn>(address)(handle) : 1;
}
template <size_t... N>
auto Build(std::index_sequence<N...>)
{
    return std::array<std::array<FARPROC, 4>, sizeof...(N)>{
        std::array<FARPROC, 4>{reinterpret_cast<FARPROC>(&Create<N>), reinterpret_cast<FARPROC>(&Evaluate<N>),
                               reinterpret_cast<FARPROC>(&Release<N>), reinterpret_cast<FARPROC>(&Evaluate<N, true>)}...};
}
}
GameDlssStatus GameDlssSnapshot()
{
    std::lock_guard lock(mutex);
    return gameDlss;
}
FARPROC ResolveNgx(HMODULE module, LPCSTR name, FARPROC original)
{
    if (!original || ngxLookupBypass || reinterpret_cast<ULONG_PTR>(name) <= 0xffff)
    {
        return original;
    }
    // Every intercepted export shares this prefix, so one comparison rules out the
    // unrelated lookups that share the hook.
    static constexpr char prefix[]{"NVSDK_NGX_D3D12_"};
    if (strncmp(name, prefix, sizeof(prefix) - 1) != 0)
    {
        return original;
    }
    static const char* names[]{"NVSDK_NGX_D3D12_CreateFeature", "NVSDK_NGX_D3D12_EvaluateFeature",
                               "NVSDK_NGX_D3D12_ReleaseFeature", "NVSDK_NGX_D3D12_EvaluateFeature_C"};
    unsigned field = 4;
    for (unsigned n = 0; n < 4; ++n)
    {
        if (strcmp(name, names[n]) == 0)
        {
            field = n;
            break;
        }
    }
    if (field == 4)
    {
        return original;
    }
    static const auto thunks = Build(std::make_index_sequence<8>{});
    std::lock_guard lock(mutex);
    for (size_t n = 0; n < entries.size(); ++n)
    {
        auto& e = entries[n];
        if (e.module && e.module != module)
        {
            continue;
        }
        if (!e.module)
        {
            HMODULE pinned{};
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                                    reinterpret_cast<LPCWSTR>(original), &pinned))
            {
                return original;
            }
            e.module = module;
        }
        e.hot[field].store(original, std::memory_order_release);
        return thunks[n][field];
    }
    return original;
}
}
