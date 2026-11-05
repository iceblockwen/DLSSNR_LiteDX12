#include "panel_impl.hpp"
#include "../graphics_registry.hpp"
#include "ui_widgets.hpp"
#include "../input/input.hpp"
#include "../input/input_keyboard.hpp"
#include "../nr/capture12.hpp"
#include "../nr/status.hpp"
#include "../nr/model_settings.hpp"

#include "../nr/model_runtime.hpp"
#include "../config.hpp"
#include "window_subclass.hpp"
#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx12.h>
#include <array>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <vector>
#include <stdexcept>
#include <cstdio>
#include <cstring>
#include <string>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
using Microsoft::WRL::ComPtr;
using namespace litedx12::panel_ui;
// The shell: construction, window subclass, teardown, and the public Panel methods.
// The Direct3D 12 renderer is in panel_gpu.cpp, the ImGui window in panel_ui.cpp.
namespace litedx12
{
using panel_detail::Check;
using panel_detail::RtvFormat;

namespace
{
bool ForegroundMatches(HWND window)
{
    const auto foreground = GetForegroundWindow();
    return window && foreground && GetAncestor(window, GA_ROOT) == GetAncestor(foreground, GA_ROOT);
}
bool WindowFocused(HWND window)
{
    // The render HWND may be a child of the actual input/foreground window.
    // Opt also treats the shared input root as focused.
    return ForegroundMatches(window);
}
}

void Panel::Impl::MenuKey(bool down)
{
    const bool released = menuHotkey.Observe(down);
    if (!released || ignoreMenuRelease)
    {
        return;
    }
    if (context)
    {
        ImGui::SetCurrentContext(context);
    }
    status.visible = !status.visible;
    inputFocused = WindowFocused(hwnd);
    SyncPreviewVisibility();
    CapturePanelInput(hwnd, status.visible && inputFocused && ForegroundMatches(hwnd));
}
void Panel::Impl::PollMenuKey()
{
    const auto route = Windows().Find(hwnd);
    if (!route || route->Active() != this || status.panel == State::Failed)
    {
        return;
    }
    // Opt's fallback, restricted to the single reserved key. Bypass the
    // game's blocked GetAsyncKeyState hook; no timer or extra thread.
    if (!ForegroundMatches(hwnd))
    {
        menuHotkey.down = false;
        ignoreMenuRelease = true;
        return;
    }
    PanelInputScope internalInput;
    const bool down = (GetAsyncKeyState(VK_INSERT) & 0x8000) != 0;
    if (ignoreMenuRelease)
    {
        menuHotkey.down = down;
        if (!down)
        {
            ignoreMenuRelease = false;
        }
        return;
    }
    MenuKey(down);
}

bool Panel::Impl::CommitTuning(nr::ModelSettings* target)
{
    if (std::none_of(tuningPending.begin(), tuningPending.end(), [](bool p)
                     {
                         return p;
                     }))
    {
        return false;
    }
    auto settings = target ? *target : nr::GetModelSettings();
    float* values[]{&settings.pass.intensity, &settings.pass.structure, &settings.pass.tone,
                    &settings.pass.skin};
    for (unsigned n = 0; n < 4; ++n)
    {
        if (tuningPending[n])
        {
            *values[n] = tuningValues[n];
        }
    }
    tuningPending.fill(false);
    if (target)
    {
        *target = settings;
    }
    else
    {
        nr::SetModelSettings(settings);
    }
    return true;
}

void Panel::Impl::SyncPreviewVisibility()
{
    const bool visible = status.visible && hwnd && status.panel != State::Failed;
    if (previewVisibility == visible)
    {
        return;
    }
    nr::SetPanelVisible(this, visible);
    previewVisibility = visible;
    if (!visible)
    {
        CommitTuning();
        if (scaleEdited)
        {
            nr::SetDisplayScale(nr::DisplayScale(), true);
            scaleEdited = false;
        }
        FlushConfig();
    }
}

Panel::Impl::~Impl()
{
    Unsubclass();
    try
    {
        Drain();
    }
    catch (...)
    {
    }
    DestroyRenderer();
    if (device12)
    {
        nr::ReleaseDevice12(device12.Get());
    }
    nr::RetireCompleted();
    if (context)
    {
        ImGui::SetCurrentContext(context);
        if (win32Ready)
        {
            ImGui_ImplWin32_Shutdown();
        }
        ImGui::DestroyContext(context);
    }
    if (event)
    {
        CloseHandle(event);
    }
}

WindowSubclassRegistry<Panel::Impl>& Panel::Impl::Windows()
{
    static auto* windows = new WindowSubclassRegistry<Impl>;
    return *windows;
}

bool Panel::Impl::RouteKeyboard(MSG& message, bool removed)
{
    // Mouse, activation and engine messages need no queue interception. Do not
    // make them wait for rendering/model initialization before DispatchMessage.
    const UINT kind = message.message;
    if (kind != WM_KEYDOWN && kind != WM_KEYUP && kind != WM_SYSKEYDOWN &&
        kind != WM_SYSKEYUP && kind != WM_CHAR && kind != WM_SYSCHAR &&
        kind != WM_DEADCHAR && kind != WM_SYSDEADCHAR && kind != WM_UNICHAR)
    {
        return false;
    }
    std::lock_guard lock(Graphics().mutex);
    const auto route = Windows().Find(message.hwnd);
    if (!route || !route->Active() ||
        !PanelKeyboardQueue(message.hwnd, message.message, message.wParam, message.lParam))
    {
        return false;
    }
    if (removed)
    {
        // Translate before replacing the engine's copy, otherwise WM_CHAR is
        // lost when a game examines its queue before calling DispatchMessage.
        if (message.wParam != VK_INSERT)
        {
            TranslateMessage(&message);
        }
        WindowProc(message.hwnd, message.message, message.wParam, message.lParam);
    }
    message.message = WM_NULL;
    message.wParam = 0;
    message.lParam = 0;
    return true;
}

LRESULT CALLBACK Panel::Impl::WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (PanelCursorMessage(message))
    {
        return 0;
    }
    std::unique_lock lock(Graphics().mutex);
    auto* route = Windows().Find(window);
    if (!route)
    {
        lock.unlock();
        return DefWindowProcW(window, message, wparam, lparam);
    }
    auto old = route->original;
    auto* self = route->Active();
    if (!self)
    {
        lock.unlock();
        const auto result = CallWindowProcW(old, window, message, wparam, lparam);
        if (message == WM_NCDESTROY)
        {
            lock.lock();
            Windows().Erase(window);
        }
        return result;
    }
    if (self->context)
    {
        ImGui::SetCurrentContext(self->context);
    }
    if (message == WM_INPUT)
    {
        UINT hotkey{};
        if (PanelRawHotkey(window, reinterpret_cast<HRAWINPUT>(lparam), hotkey))
        {
            WindowProc(window, hotkey, VK_INSERT, 0);
            return DefWindowProcW(window, message, wparam, lparam);
        }
    }
    if (message == WM_KILLFOCUS || (message == WM_ACTIVATEAPP && !wparam))
    {
        self->inputFocused = false;
        self->ignoreMenuRelease = true;
        self->menuHotkey.down = false;
        CapturePanelInput(window, false);
        PanelKeyboardFocusLost();
    }
    if (message == WM_SETFOCUS || (message == WM_ACTIVATEAPP && wparam))
    {
        self->inputFocused = message == WM_SETFOCUS || WindowFocused(window);
        if (self->status.visible)
        {
            CapturePanelInput(window, self->inputFocused && ForegroundMatches(window));
        }
    }
    if ((message == WM_KEYDOWN || message == WM_KEYUP || message == WM_SYSKEYDOWN ||
         message == WM_SYSKEYUP) &&
        wparam == VK_INSERT)
    {
        PanelKeyboardMessage(window, message, wparam, lparam);
        if (message == WM_KEYDOWN || message == WM_SYSKEYDOWN)
        {
            self->ignoreMenuRelease = false;
            self->MenuKey(true);
        }
        if (message == WM_KEYUP || message == WM_SYSKEYUP)
        {
            self->MenuKey(false);
            self->ignoreMenuRelease = false;
        }
        return 0;
    }
    const bool keyboardBlocked = PanelKeyboardMessage(window, message, wparam, lparam);
    if (self->context && self->status.visible && PanelInputCaptured(window))
    {
        ImGui::SetCurrentContext(self->context);
        if (message == WM_INPUT && PanelInputCaptured(window))
        {
            if (PanelRawInput(window, reinterpret_cast<HRAWINPUT>(lparam)))
            {
                return DefWindowProcW(window, message, wparam, lparam);
            }
            // A keyboard release owed to the game must reach its original
            // input handler even if the panel opened after the press.
        }
        PanelMouseMessage(window, message, lparam);
        LRESULT handled{};
        {
            PanelInputScope internalInput;
            handled = ImGui_ImplWin32_WndProcHandler(window, message, wparam, lparam);
        }
        if (message == WM_SETCURSOR && PanelInputCaptured(window) && handled)
        {
            return handled;
        }
        if (PanelInputCaptured(window) && message >= WM_MOUSEFIRST && message <= WM_MOUSELAST)
        {
            return 0;
        }
    }
    if (keyboardBlocked)
    {
        return 0;
    }
    if (message == WM_NCDESTROY)
    {
        try
        {
            self->Drain();
            self->DestroyRenderer();
        }
        catch (...)
        {
            self->Failure(E_FAIL);
        }
        while (auto* remaining = Windows().Find(window))
        {
            auto* owner = remaining->Active();
            if (!owner)
            {
                break;
            }
            owner->Unsubclass();
            owner->hwnd = nullptr;
            owner->status.visible = false;
        }
    }
    // The game may synchronously wait for its rendering thread while handling
    // activation, capture or a click. That thread also needs Graphics().mutex.
    // Keep only the native callback value; do not use self/route after unlock.
    lock.unlock();
    const auto result = CallWindowProcW(old, window, message, wparam, lparam);
    if (message == WM_NCDESTROY)
    {
        lock.lock();
        Windows().Erase(window);
    }
    return result;
}

void Panel::Impl::Unsubclass()
{
    status.visible = false;
    SyncPreviewVisibility();
    if (context)
    {
        ImGui::SetCurrentContext(context);
    }
    if (!hwnd || !windowAttached)
    {
        return;
    }
    const auto route = Windows().Find(hwnd);
    if (route && route->Active() == this)
    {
        CapturePanelInput(hwnd, false);
    }
    Windows().Detach(hwnd, this, WindowProc);
    windowAttached = false;
}

void Panel::Impl::Failure(HRESULT hr)
{
    CapturePanelInput(hwnd, false);
    status.error = hr;
    status.panel = State::Failed;
    status.visible = false;
    SyncPreviewVisibility();
    wcscpy_s(status.reason, L"Overlay failed; original DXGI presentation continues");
}

Panel::Panel(IDXGISwapChain* swapchain, ID3D12CommandQueue* capturedQueue)
    : impl(std::make_unique<Impl>())
{
    auto& p = *impl;
    p.swap = swapchain;
    DXGI_SWAP_CHAIN_DESC desc{};
    Check(swapchain->GetDesc(&desc));
    p.hwnd = desc.OutputWindow;
    GUITHREADINFO info{sizeof(info)};
    p.inputFocused = GetGUIThreadInfo(GetWindowThreadProcessId(p.hwnd, nullptr), &info) &&
                     info.hwndFocus == p.hwnd;
    p.status.width = desc.BufferDesc.Width;
    p.status.height = desc.BufferDesc.Height;
    {
        Check(swapchain->GetDevice(IID_PPV_ARGS(&p.device12)));
        if (!capturedQueue || capturedQueue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT)
        {
            throw E_NOINTERFACE;
        }
        ComPtr<ID3D12Device> queueDevice;
        Check(capturedQueue->GetDevice(IID_PPV_ARGS(&queueDevice)));
        if (queueDevice.Get() != p.device12.Get())
        {
            throw E_INVALIDARG;
        }
        // NR submission tracking is independent of renderer/Resources demand.
        // Otherwise a real submitted creation is marked discarded on list Reset
        // and the model is repeatedly recreated with unretirable handles.
        InstallFrameQueueCapture(capturedQueue);
        p.queue = capturedQueue;
        Check(p.device12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&p.fence)));
        p.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!p.event)
        {
            throw HRESULT_FROM_WIN32(GetLastError());
        }
    }
    p.status.graphics = State::Ready;
    if (!p.hwnd)
    {
        throw E_NOTIMPL;
    }
    p.context = ImGui::CreateContext();
    ImGui::SetCurrentContext(p.context);
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    auto& style = ImGui::GetStyle();
    style.WindowRounding = 7;
    style.FrameRounding = 4;
    style.WindowPadding = ImVec2(18, 14);
    style.ItemSpacing = ImVec2(10, 8);
    p.baseStyle = style;
    const float scale = ImGui_ImplWin32_GetDpiScaleForHwnd(p.hwnd);
    p.dpiScale = p.fontDpiScale = scale;
    style.ScaleAllSizes(scale);
    style.MouseCursorScale = 1.f;
    wchar_t windows[MAX_PATH]{};
    GetWindowsDirectoryW(windows, MAX_PATH);
    const std::wstring fontPath = std::wstring(windows) + L"\\Fonts\\segoeui.ttf";
    char utf8[MAX_PATH * 3]{};
    WideCharToMultiByte(CP_UTF8, 0, fontPath.c_str(), -1, utf8, sizeof(utf8), nullptr, nullptr);
    if (!io.Fonts->AddFontFromFileTTF(utf8, 14 * scale))
    {
        io.Fonts->AddFontDefault();
        io.FontGlobalScale = scale;
        p.fontDpiScale = 1;
    }
    if (!ImGui_ImplWin32_Init(p.hwnd))
    {
        throw E_FAIL;
    }
    p.win32Ready = true;
    Check(Impl::Windows().Attach(p.hwnd, &p, Impl::WindowProc));
    p.windowAttached = true;
    InstallPanelInput(Impl::RouteKeyboard);
    p.inputFocused = WindowFocused(p.hwnd);
}
Panel::~Panel()
{
    Close();
}
void Panel::Close()
{
    if (!impl)
    {
        return;
    }
    impl->Unsubclass();
    try
    {
        impl->Drain();
    }
    catch (...)
    {
        // Lite also retains resources when its retire wait fails. Do not free
        // allocator/font/upload/backbuffer memory still in use by a live GPU.
        if (impl->device12 && SUCCEEDED(impl->device12->GetDeviceRemovedReason()))
        {
            // Parking the Impl keeps its frame buffers referenced; no second list.
            static auto* pending = new std::vector<std::unique_ptr<Impl>>;
            pending->push_back(std::move(impl));
            return;
        }
    }
    impl.reset();
}
void Panel::Draw(UINT flags) noexcept
{
    auto& p = *impl;
    if (!p.hwnd || (flags & DXGI_PRESENT_TEST))
    {
        return;
    }
    try
    {
        p.PollMenuKey();
        nr::RetireCompleted();
        if (p.device12)
        {
            nr::PrepareModel12(p.device12.Get());
        }
    }
    catch (...)
    {
    }
    if (!p.status.visible || p.status.panel == State::Failed)
    {
        return;
    }
    try
    {
        PanelInputScope internalInput;
        ImGui::SetCurrentContext(p.context);
        p.inputFocused = WindowFocused(p.hwnd);
        CapturePanelInput(p.hwnd, p.inputFocused && ForegroundMatches(p.hwnd));
        const float scale = ImGui_ImplWin32_GetDpiScaleForHwnd(p.hwnd);
        if (scale != p.dpiScale)
        {
            ImGui::GetStyle() = p.baseStyle;
            ImGui::GetStyle().ScaleAllSizes(scale);
            ImGui::GetStyle().MouseCursorScale = 1.f;
            p.dpiScale = scale;
            ImGui::GetIO().FontGlobalScale = scale / p.fontDpiScale;
        }
        p.renderStage = L"renderer initialization";
        if (!p.rendererReady)
        {
            p.InitRenderer();
        }
        p.renderStage = L"DX12 panel draw";
        p.Draw12();
    }
    catch (HRESULT hr)
    {
        p.Failure(hr);
    }
    catch (...)
    {
        p.Failure(E_FAIL);
    }
}
void Panel::BeforeResize()
{
    auto& p = *impl;
    p.renderStage = L"swapchain resize preparation";
    p.Drain();
    p.DestroyRenderer();
    nr::RetireCompleted();
}
void Panel::AfterResize(HRESULT hr, ID3D12CommandQueue* replacement)
{
    auto& p = *impl;
    p.renderStage = L"swapchain resize completion";
    if (replacement)
    {
        ComPtr<ID3D12Device> device;
        Check(replacement->GetDevice(IID_PPV_ARGS(&device)));
        if (device.Get() != p.device12.Get() ||
            replacement->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT)
        {
            throw E_INVALIDARG;
        }
        InstallFrameQueueCapture(replacement);
        p.queue = replacement;
    }
    DXGI_SWAP_CHAIN_DESC desc{};
    if (SUCCEEDED(p.swap->GetDesc(&desc)))
    {
        p.status.width = desc.BufferDesc.Width;
        p.status.height = desc.BufferDesc.Height;
    }
    // Failed ResizeBuffers still leaves the old buffers usable; rebuild on the next visible frame.
    p.status.panel = State::Waiting;
    p.status.error = hr;
}
void Panel::Fail(HRESULT error) noexcept
{
    impl->Failure(error);
}
}
