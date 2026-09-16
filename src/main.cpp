#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <dcomp.h>
#include <d3dkmthk.h>
#include <wrl/client.h>
#include <string>
#include <vector>
#include <sstream>
#include <fstream>
#include <iomanip>
#include <algorithm>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "ole32.lib")

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

using Microsoft::WRL::ComPtr;

static const wchar_t* kControlClass = L"AMDCloneOverlayProbe.Control";
static const wchar_t* kOverlayClass = L"AMDCloneOverlayProbe.Overlay";

enum class Mode {
    Hidden = 0,
    LayeredGDI = 1,
    DComp = 2,
    Restricted = 3,
    OpaqueCandidate = 4
};

struct OutputInfo {
    ComPtr<IDXGIOutput> output;
    DXGI_OUTPUT_DESC desc{};
    UINT index = 0;
};

struct TargetInfo {
    UINT sourceId = 0;
    UINT targetId = 0;
    DISPLAYCONFIG_VIDEO_OUTPUT_TECHNOLOGY technology = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_OTHER;
    std::wstring sourceGdi;
    std::wstring friendlyName;
    std::wstring monitorPath;
};

struct AppState {
    HWND control = nullptr;
    HWND overlay = nullptr;
    Mode mode = Mode::LayeredGDI;
    bool presentRestrict = true;
    bool captureExclude = false;
    size_t selectedOutput = 0;
    std::vector<OutputInfo> outputs;
    std::vector<TargetInfo> targets;
    std::wstring topology = L"not checked";
    std::wstring mpoSummary = L"not probed";
    std::wstring dxgiSummary = L"not probed";
    std::wstring lastError;

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    ComPtr<IDCompositionDevice> dcomp;
    ComPtr<IDCompositionTarget> target;
    ComPtr<IDCompositionVisual> visual;
    ComPtr<IDXGISwapChain1> swap;
};

static AppState g;
static std::wofstream gLog;

static void Log(const std::wstring& text) {
    if (!gLog.is_open()) {
        wchar_t exe[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring path(exe);
        const auto p = path.find_last_of(L"\\/");
        if (p != std::wstring::npos) path.resize(p + 1);
        path += L"clone_overlay_test.log";
        gLog.open(path.c_str(), std::ios::app);
    }
    SYSTEMTIME st{};
    GetLocalTime(&st);
    if (gLog.is_open()) {
        gLog << L"[" << std::setfill(L'0') << std::setw(2) << st.wHour << L":"
             << std::setw(2) << st.wMinute << L":" << std::setw(2) << st.wSecond << L"] "
             << text << std::endl;
    }
}

static std::wstring HrText(HRESULT hr) {
    std::wstringstream ss;
    ss << L"0x" << std::hex << std::uppercase << static_cast<unsigned long>(hr);
    return ss.str();
}

static std::wstring NtText(NTSTATUS status) {
    std::wstringstream ss;
    ss << L"0x" << std::hex << std::uppercase << static_cast<unsigned long>(status);
    return ss.str();
}

static std::wstring TechnologyName(DISPLAYCONFIG_VIDEO_OUTPUT_TECHNOLOGY t) {
    switch (t) {
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HD15: return L"VGA/HD15";
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_SVIDEO: return L"S-Video";
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_COMPOSITE_VIDEO: return L"Composite";
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_COMPONENT_VIDEO: return L"Component";
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DVI: return L"DVI";
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI: return L"HDMI";
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_LVDS: return L"LVDS";
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_D_JPN: return L"D-JPN";
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_SDI: return L"SDI";
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EXTERNAL: return L"DisplayPort external";
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EMBEDDED: return L"DisplayPort embedded";
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_UDI_EXTERNAL: return L"UDI external";
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_UDI_EMBEDDED: return L"UDI embedded";
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_SDTVDONGLE: return L"SDTV dongle";
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_MIRACAST: return L"Miracast";
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INTERNAL: return L"Internal";
        default: return L"Other/unknown";
    }
}

static std::wstring FormatName(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_B8G8R8A8_UNORM: return L"BGRA8";
        case DXGI_FORMAT_R8G8B8A8_UNORM: return L"RGBA8";
        case DXGI_FORMAT_R10G10B10A2_UNORM: return L"RGB10A2";
        case DXGI_FORMAT_NV12: return L"NV12";
        case DXGI_FORMAT_YUY2: return L"YUY2";
        case DXGI_FORMAT_P010: return L"P010";
        default: return L"format#" + std::to_wstring(static_cast<int>(f));
    }
}

static bool EnsureGraphics() {
    if (g.device) return true;

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL requested[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL actual{};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                   requested, ARRAYSIZE(requested), D3D11_SDK_VERSION,
                                   &g.device, &actual, &g.context);
    if (hr == E_INVALIDARG) {
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                               &requested[1], 1, D3D11_SDK_VERSION,
                               &g.device, &actual, &g.context);
    }
    if (FAILED(hr)) {
        g.lastError = L"D3D11CreateDevice failed: " + HrText(hr);
        Log(g.lastError);
        return false;
    }

    ComPtr<IDXGIDevice> dxgiDevice;
    hr = g.device.As(&dxgiDevice);
    if (FAILED(hr)) return false;
    hr = dxgiDevice->GetAdapter(&g.adapter);
    if (FAILED(hr)) return false;

    ComPtr<IDXGIFactory> baseFactory;
    hr = g.adapter->GetParent(IID_PPV_ARGS(&baseFactory));
    if (FAILED(hr)) return false;
    hr = baseFactory.As(&g.factory);
    if (FAILED(hr)) return false;

    DXGI_ADAPTER_DESC desc{};
    g.adapter->GetDesc(&desc);
    Log(L"D3D adapter: " + std::wstring(desc.Description));
    return true;
}

static void QueryTopology() {
    g.targets.clear();
    UINT32 pathCount = 0, modeCount = 0;
    LONG rc = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount);
    if (rc != ERROR_SUCCESS) {
        g.topology = L"QueryDisplayConfig unavailable";
        return;
    }

    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    rc = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr);
    if (rc != ERROR_SUCCESS) {
        g.topology = L"QueryDisplayConfig failed";
        return;
    }

    bool clone = false;
    for (UINT32 i = 0; i < pathCount; ++i) {
        for (UINT32 j = i + 1; j < pathCount; ++j) {
            const auto& a = paths[i].sourceInfo;
            const auto& b = paths[j].sourceInfo;
            if (a.id == b.id && a.adapterId.HighPart == b.adapterId.HighPart && a.adapterId.LowPart == b.adapterId.LowPart)
                clone = true;
        }
    }

    std::wstringstream ss;
    ss << (clone ? L"CLONE DETECTED" : L"clone not detected") << L" | active paths=" << pathCount;
    g.topology = ss.str();
    Log(L"Topology: " + g.topology);

    for (UINT32 i = 0; i < pathCount; ++i) {
        const auto& p = paths[i];
        TargetInfo ti;
        ti.sourceId = p.sourceInfo.id;
        ti.targetId = p.targetInfo.id;
        ti.technology = p.targetInfo.outputTechnology;

        DISPLAYCONFIG_SOURCE_DEVICE_NAME sname{};
        sname.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        sname.header.size = sizeof(sname);
        sname.header.adapterId = p.sourceInfo.adapterId;
        sname.header.id = p.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&sname.header) == ERROR_SUCCESS)
            ti.sourceGdi = sname.viewGdiDeviceName;

        DISPLAYCONFIG_TARGET_DEVICE_NAME tname{};
        tname.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        tname.header.size = sizeof(tname);
        tname.header.adapterId = p.targetInfo.adapterId;
        tname.header.id = p.targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&tname.header) == ERROR_SUCCESS) {
            ti.friendlyName = tname.monitorFriendlyDeviceName;
            ti.monitorPath = tname.monitorDevicePath;
        }
        g.targets.push_back(ti);

        std::wstringstream ps;
        ps << L"Path " << i
           << L": source=" << ti.sourceId
           << L" target=" << ti.targetId
           << L" technology=" << static_cast<int>(ti.technology)
           << L" (" << TechnologyName(ti.technology) << L")"
           << L" GDI=" << (ti.sourceGdi.empty() ? L"?" : ti.sourceGdi)
           << L" monitor=\"" << (ti.friendlyName.empty() ? L"?" : ti.friendlyName) << L"\"";
        Log(ps.str());
    }
}

static void EnumerateOutputs() {
    g.outputs.clear();
    if (!EnsureGraphics()) return;

    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIOutput> output;
        HRESULT hr = g.adapter->EnumOutputs(i, &output);
        if (hr == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(hr)) continue;

        OutputInfo oi;
        oi.output = output;
        oi.index = i;
        output->GetDesc(&oi.desc);
        g.outputs.push_back(oi);

        std::wstringstream ss;
        ss << L"DXGI output " << i << L": " << oi.desc.DeviceName
           << L" attached=" << (oi.desc.AttachedToDesktop ? L"yes" : L"no")
           << L" rect=" << oi.desc.DesktopCoordinates.left << L"," << oi.desc.DesktopCoordinates.top
           << L"-" << oi.desc.DesktopCoordinates.right << L"," << oi.desc.DesktopCoordinates.bottom;
        Log(ss.str());
    }

    if (g.selectedOutput >= g.outputs.size()) g.selectedOutput = 0;
}

static void ProbeDxgiOverlayCaps() {
    if (!EnsureGraphics()) return;
    const DXGI_FORMAT formats[] = {
        DXGI_FORMAT_B8G8R8A8_UNORM,
        DXGI_FORMAT_R10G10B10A2_UNORM,
        DXGI_FORMAT_NV12,
        DXGI_FORMAT_YUY2,
        DXGI_FORMAT_P010
    };

    bool anyOverlay = false;
    for (const auto& oi : g.outputs) {
        std::wstringstream base;
        base << L"DXGI overlay probe output " << oi.index << L" (" << oi.desc.DeviceName << L")";
        Log(base.str());

        ComPtr<IDXGIOutput2> o2;
        if (SUCCEEDED(oi.output.As(&o2))) {
            const BOOL supports = o2->SupportsOverlays();
            anyOverlay = anyOverlay || (supports != FALSE);
            Log(std::wstring(L"  SupportsOverlays=") + (supports ? L"TRUE" : L"FALSE"));
        } else {
            Log(L"  IDXGIOutput2 unavailable");
        }

        ComPtr<IDXGIOutput3> o3;
        if (SUCCEEDED(oi.output.As(&o3))) {
            for (DXGI_FORMAT f : formats) {
                UINT flags = 0;
                HRESULT hr = o3->CheckOverlaySupport(f, g.device.Get(), &flags);
                std::wstringstream fs;
                fs << L"  CheckOverlaySupport " << FormatName(f)
                   << L": hr=" << HrText(hr)
                   << L" flags=0x" << std::hex << std::uppercase << flags;
                Log(fs.str());
            }
        } else {
            Log(L"  IDXGIOutput3 unavailable");
        }

        ComPtr<IDXGIOutput6> o6;
        if (SUCCEEDED(oi.output.As(&o6))) {
            UINT flags = 0;
            HRESULT hr = o6->CheckHardwareCompositionSupport(&flags);
            std::wstringstream hs;
            hs << L"  CheckHardwareCompositionSupport: hr=" << HrText(hr)
               << L" flags=0x" << std::hex << std::uppercase << flags;
            Log(hs.str());
        } else {
            Log(L"  IDXGIOutput6 unavailable");
        }
    }

    g.dxgiSummary = L"DXGI outputs=" + std::to_wstring(g.outputs.size()) +
                    L", SupportsOverlays=" + (anyOverlay ? std::wstring(L"yes") : std::wstring(L"no"));
}

static void ProbeVideoProcessorCaps() {
    if (!EnsureGraphics()) return;

    ComPtr<ID3D11VideoDevice> videoDevice;
    HRESULT hr = g.device.As(&videoDevice);
    if (FAILED(hr)) {
        Log(L"Video processor: ID3D11VideoDevice unavailable: " + HrText(hr));
        return;
    }

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC desc{};
    desc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    desc.InputFrameRate.Numerator = 60;
    desc.InputFrameRate.Denominator = 1;
    desc.InputWidth = 1920;
    desc.InputHeight = 1080;
    desc.OutputFrameRate.Numerator = 60;
    desc.OutputFrameRate.Denominator = 1;
    desc.OutputWidth = 1920;
    desc.OutputHeight = 1080;
    desc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

    ComPtr<ID3D11VideoProcessorEnumerator> enumerator;
    hr = videoDevice->CreateVideoProcessorEnumerator(&desc, &enumerator);
    if (FAILED(hr)) {
        Log(L"Video processor enumerator creation failed: " + HrText(hr));
        return;
    }

    const DXGI_FORMAT formats[] = {
        DXGI_FORMAT_B8G8R8A8_UNORM,
        DXGI_FORMAT_NV12,
        DXGI_FORMAT_YUY2,
        DXGI_FORMAT_P010
    };
    for (DXGI_FORMAT f : formats) {
        UINT flags = 0;
        hr = enumerator->CheckVideoProcessorFormat(f, &flags);
        std::wstringstream fs;
        fs << L"VideoProcessor format " << FormatName(f)
           << L": hr=" << HrText(hr)
           << L" flags=0x" << std::hex << std::uppercase << flags;
        Log(fs.str());
    }
}

static void ProbeD3DFormatSupport() {
    if (!EnsureGraphics()) return;
    const DXGI_FORMAT formats[] = {
        DXGI_FORMAT_B8G8R8A8_UNORM,
        DXGI_FORMAT_R10G10B10A2_UNORM,
        DXGI_FORMAT_NV12,
        DXGI_FORMAT_YUY2,
        DXGI_FORMAT_P010
    };
    for (DXGI_FORMAT f : formats) {
        UINT flags = 0;
        HRESULT hr = g.device->CheckFormatSupport(f, &flags);
        std::wstringstream fs;
        fs << L"D3D11 CheckFormatSupport " << FormatName(f)
           << L": hr=" << HrText(hr)
           << L" flags=0x" << std::hex << std::uppercase << flags;
        Log(fs.str());
    }
}

static void ProbeKmtMpoCaps() {
    std::wstring gdiName;
    for (const auto& t : g.targets) {
        if (!t.sourceGdi.empty()) {
            gdiName = t.sourceGdi;
            break;
        }
    }
    if (gdiName.empty() && !g.outputs.empty()) gdiName = g.outputs[0].desc.DeviceName;
    if (gdiName.empty()) {
        g.mpoSummary = L"KMT: no GDI display name";
        Log(g.mpoSummary);
        return;
    }

    D3DKMT_OPENADAPTERFROMGDIDISPLAYNAME open{};
    wcsncpy_s(open.DeviceName, _countof(open.DeviceName), gdiName.c_str(), _TRUNCATE);
    NTSTATUS st = D3DKMTOpenAdapterFromGdiDisplayName(&open);
    if (st != 0) {
        g.mpoSummary = L"KMT open failed " + NtText(st);
        Log(L"D3DKMTOpenAdapterFromGdiDisplayName(" + gdiName + L") failed: " + NtText(st));
        return;
    }

    {
        std::wstringstream os;
        os << L"KMT adapter opened: GDI=" << gdiName
           << L" VidPnSourceId=" << open.VidPnSourceId
           << L" LUID=" << std::hex << open.AdapterLuid.HighPart << L":" << open.AdapterLuid.LowPart;
        Log(os.str());
    }

    D3DKMT_GET_MULTIPLANE_OVERLAY_CAPS caps{};
    caps.hAdapter = open.hAdapter;
    caps.VidPnSourceId = open.VidPnSourceId;
    st = D3DKMTGetMultiPlaneOverlayCaps(&caps);

    if (st == 0) {
        std::wstringstream cs;
        cs << L"KMT MPO caps: MaxPlanes=" << caps.MaxPlanes
           << L" MaxRGBPlanes=" << caps.MaxRGBPlanes
           << L" MaxYUVPlanes=" << caps.MaxYUVPlanes
           << L" caps=0x" << std::hex << std::uppercase << caps.OverlayCaps.Value << std::dec
           << L" Shared=" << caps.OverlayCaps.Shared
           << L" StretchRGB=" << caps.OverlayCaps.StretchRGB
           << L" StretchYUV=" << caps.OverlayCaps.StretchYUV
           << L" Immediate=" << caps.OverlayCaps.Immediate
           << L" Version3DDI=" << caps.OverlayCaps.Version3DDISupport
           << L" stretch=" << caps.MaxStretchFactor
           << L" shrink=" << caps.MaxShrinkFactor;
        Log(cs.str());

        std::wstringstream sum;
        sum << L"KMT MPO planes=" << caps.MaxPlanes
            << L" (RGB " << caps.MaxRGBPlanes << L", YUV " << caps.MaxYUVPlanes << L")"
            << L", shared=" << (caps.OverlayCaps.Shared ? L"yes" : L"no")
            << L", v3DDI=" << (caps.OverlayCaps.Version3DDISupport ? L"yes" : L"no");
        g.mpoSummary = sum.str();
    } else {
        g.mpoSummary = L"KMT MPO query failed " + NtText(st);
        Log(g.mpoSummary);
    }

    D3DKMT_CLOSEADAPTER closeArgs{};
    closeArgs.hAdapter = open.hAdapter;
    NTSTATUS closeStatus = D3DKMTCloseAdapter(&closeArgs);
    if (closeStatus != 0) Log(L"D3DKMTCloseAdapter failed: " + NtText(closeStatus));
}

static void ProbeAllCapabilities() {
    Log(L"========== V2 CAPABILITY PROBE BEGIN ==========");
    ProbeDxgiOverlayCaps();
    ProbeD3DFormatSupport();
    ProbeVideoProcessorCaps();
    ProbeKmtMpoCaps();
    Log(L"========== V2 CAPABILITY PROBE END ==========");
    if (g.control) InvalidateRect(g.control, nullptr, TRUE);
}

static void DestroyDComp() {
    g.swap.Reset();
    g.visual.Reset();
    g.target.Reset();
    g.dcomp.Reset();
}

static bool CreateDComp(bool restricted, bool opaqueCandidate) {
    DestroyDComp();
    if (!EnsureGraphics()) return false;
    if (restricted && g.outputs.empty()) {
        g.lastError = L"No DXGI output available";
        return false;
    }

    RECT rc{};
    GetClientRect(g.overlay, &rc);
    const UINT width = static_cast<UINT>(rc.right - rc.left);
    const UINT height = static_cast<UINT>(rc.bottom - rc.top);

    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = width;
    sd.Height = height;
    sd.Format = opaqueCandidate ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.Stereo = FALSE;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    sd.AlphaMode = opaqueCandidate ? DXGI_ALPHA_MODE_IGNORE : DXGI_ALPHA_MODE_PREMULTIPLIED;

    IDXGIOutput* restrictOutput = restricted ? g.outputs[g.selectedOutput].output.Get() : nullptr;
    HRESULT hr = g.factory->CreateSwapChainForComposition(g.device.Get(), &sd, restrictOutput, &g.swap);
    if (FAILED(hr)) {
        g.lastError = L"CreateSwapChainForComposition failed: " + HrText(hr);
        Log(g.lastError);
        return false;
    }

    ComPtr<IDXGIDevice> dxgiDevice;
    hr = g.device.As(&dxgiDevice);
    if (FAILED(hr)) return false;
    hr = DCompositionCreateDevice(dxgiDevice.Get(), IID_PPV_ARGS(&g.dcomp));
    if (FAILED(hr)) {
        g.lastError = L"DCompositionCreateDevice failed: " + HrText(hr);
        Log(g.lastError);
        return false;
    }

    hr = g.dcomp->CreateTargetForHwnd(g.overlay, TRUE, &g.target);
    if (FAILED(hr)) return false;
    hr = g.dcomp->CreateVisual(&g.visual);
    if (FAILED(hr)) return false;
    hr = g.visual->SetContent(g.swap.Get());
    if (FAILED(hr)) return false;
    hr = g.target->SetRoot(g.visual.Get());
    if (FAILED(hr)) return false;
    hr = g.dcomp->Commit();
    if (FAILED(hr)) return false;

    ComPtr<ID3D11Texture2D> backBuffer;
    hr = g.swap->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (FAILED(hr)) return false;
    ComPtr<ID3D11RenderTargetView> rtv;
    hr = g.device->CreateRenderTargetView(backBuffer.Get(), nullptr, &rtv);
    if (FAILED(hr)) return false;

    const float cyan[4] = { 0.02f, 0.70f, 0.95f, opaqueCandidate ? 1.0f : 0.88f };
    g.context->ClearRenderTargetView(rtv.Get(), cyan);

    DXGI_PRESENT_PARAMETERS pp{};
    UINT presentFlags = (restricted && g.presentRestrict) ? DXGI_PRESENT_RESTRICT_TO_OUTPUT : 0;
    hr = g.swap->Present1(1, presentFlags, &pp);
    if (FAILED(hr)) {
        g.lastError = L"Present1 failed: " + HrText(hr);
        Log(g.lastError);
        return false;
    }

    std::wstringstream ss;
    ss << L"DComp present: restricted=" << (restricted ? L"yes" : L"no")
       << L" output=" << g.selectedOutput
       << L" flag=" << (g.presentRestrict ? L"ON" : L"OFF")
       << L" candidate=" << (opaqueCandidate ? L"RGB10 opaque" : L"normal BGRA alpha");
    Log(ss.str());
    return true;
}

static void ApplyCaptureAffinity() {
    if (!g.overlay) return;
    SetWindowDisplayAffinity(g.overlay, g.captureExclude ? WDA_EXCLUDEFROMCAPTURE : WDA_NONE);
}

static void ApplyMode(Mode mode) {
    g.mode = mode;
    g.lastError.clear();

    if (mode == Mode::Hidden) {
        DestroyDComp();
        ShowWindow(g.overlay, SW_HIDE);
        InvalidateRect(g.control, nullptr, TRUE);
        return;
    }

    ShowWindow(g.overlay, SW_SHOWNOACTIVATE);
    LONG_PTR ex = GetWindowLongPtrW(g.overlay, GWL_EXSTYLE);
    ex |= WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;

    if (mode == Mode::LayeredGDI) {
        DestroyDComp();
        ex |= WS_EX_LAYERED;
        ex &= ~WS_EX_NOREDIRECTIONBITMAP;
        SetWindowLongPtrW(g.overlay, GWL_EXSTYLE, ex);
        SetLayeredWindowAttributes(g.overlay, 0, 220, LWA_ALPHA);
    } else {
        ex &= ~WS_EX_LAYERED;
        ex |= WS_EX_NOREDIRECTIONBITMAP;
        SetWindowLongPtrW(g.overlay, GWL_EXSTYLE, ex);
    }

    SetWindowPos(g.overlay, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);

    if (mode == Mode::DComp) CreateDComp(false, false);
    if (mode == Mode::Restricted) CreateDComp(true, false);
    if (mode == Mode::OpaqueCandidate) CreateDComp(false, true);

    ApplyCaptureAffinity();
    RedrawWindow(g.overlay, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
    InvalidateRect(g.control, nullptr, TRUE);
}

static std::wstring ModeName() {
    switch (g.mode) {
        case Mode::Hidden: return L"0 Hidden";
        case Mode::LayeredGDI: return L"1 Layered GDI baseline";
        case Mode::DComp: return L"2 DirectComposition unrestricted";
        case Mode::Restricted: return L"3 DirectComposition RESTRICT_TO_OUTPUT";
        case Mode::OpaqueCandidate: return L"4 Opaque RGB10 MPO candidate";
    }
    return L"unknown";
}

static void DrawControl(HWND hwnd, HDC dc) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    FillRect(dc, &rc, reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(25, 25, 25));

    HFONT font = CreateFontW(-18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    HGDIOBJ old = SelectObject(dc, font);

    std::wstringstream ss;
    ss << L"AMD 680M Clone Overlay Probe v2\n\n"
       << L"Topology: " << g.topology << L"\n"
       << L"Physical targets: " << g.targets.size() << L"   DXGI outputs: " << g.outputs.size() << L"\n"
       << L"DXGI: " << g.dxgiSummary << L"\n"
       << L"MPO:  " << g.mpoSummary << L"\n\n"
       << L"Mode: " << ModeName() << L"\n"
       << L"Selected DXGI output: " << g.selectedOutput;
    if (g.selectedOutput < g.outputs.size()) ss << L" (" << g.outputs[g.selectedOutput].desc.DeviceName << L")";
    ss << L"\nPresent RESTRICT flag: " << (g.presentRestrict ? L"ON" : L"OFF")
       << L"   Capture exclude: " << (g.captureExclude ? L"ON" : L"OFF") << L"\n\n"
       << L"Keys:\n"
       << L"1  normal layered cyan bar (baseline)\n"
       << L"2  DirectComposition BGRA alpha\n"
       << L"3  DirectComposition + RESTRICT_TO_OUTPUT\n"
       << L"4  opaque RGB10 flip-model candidate (more MPO-friendly)\n"
       << L"[ / ]  previous / next DXGI output\n"
       << L"R  toggle DXGI_PRESENT_RESTRICT_TO_OUTPUT\n"
       << L"C  toggle WDA_EXCLUDEFROMCAPTURE\n"
       << L"M  rerun MPO / overlay / video-format capability probe\n"
       << L"F5 refresh topology + outputs + capabilities\n"
       << L"0 hide overlay   Esc exit\n\n"
       << L"Important: full V2 details are written to clone_overlay_test.log.";
    if (!g.lastError.empty()) ss << L"\n\nERROR: " << g.lastError;

    RECT textRect = rc;
    InflateRect(&textRect, -20, -20);
    DrawTextW(dc, ss.str().c_str(), -1, &textRect, DT_LEFT | DT_TOP | DT_WORDBREAK);
    SelectObject(dc, old);
    DeleteObject(font);
}

static void DrawOverlay(HWND hwnd, HDC dc) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    HBRUSH brush = CreateSolidBrush(RGB(8, 185, 242));
    FillRect(dc, &rc, brush);
    DeleteObject(brush);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(0, 0, 0));
    HFONT font = CreateFontW(-32, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    HGDIOBJ old = SelectObject(dc, font);
    DrawTextW(dc, L"V2 OVERLAY PROBE", -1, &rc,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, old);
    DeleteObject(font);
}

static void Refresh(bool probeCaps) {
    QueryTopology();
    EnumerateOutputs();
    if (probeCaps) ProbeAllCapabilities();
    InvalidateRect(g.control, nullptr, TRUE);
}

static void CycleOutput(int delta) {
    if (g.outputs.empty()) return;
    const int n = static_cast<int>(g.outputs.size());
    int v = static_cast<int>(g.selectedOutput);
    v = (v + delta + n) % n;
    g.selectedOutput = static_cast<size_t>(v);
    Log(L"Selected DXGI output: " + std::to_wstring(g.selectedOutput));
    if (g.mode == Mode::Restricted) ApplyMode(Mode::Restricted);
    else InvalidateRect(g.control, nullptr, TRUE);
}

static LRESULT CALLBACK OverlayProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_NCHITTEST: return HTTRANSPARENT;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd, &ps);
            if (g.mode == Mode::LayeredGDI) DrawOverlay(hwnd, dc);
            EndPaint(hwnd, &ps);
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static LRESULT CALLBACK ControlProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_KEYDOWN:
            switch (wp) {
                case '0': ApplyMode(Mode::Hidden); return 0;
                case '1': ApplyMode(Mode::LayeredGDI); return 0;
                case '2': ApplyMode(Mode::DComp); return 0;
                case '3': ApplyMode(Mode::Restricted); return 0;
                case '4': ApplyMode(Mode::OpaqueCandidate); return 0;
                case VK_OEM_4: CycleOutput(-1); return 0;
                case VK_OEM_6: CycleOutput(+1); return 0;
                case 'R':
                    g.presentRestrict = !g.presentRestrict;
                    if (g.mode == Mode::Restricted) ApplyMode(Mode::Restricted);
                    else InvalidateRect(hwnd, nullptr, TRUE);
                    return 0;
                case 'C':
                    g.captureExclude = !g.captureExclude;
                    ApplyCaptureAffinity();
                    InvalidateRect(hwnd, nullptr, TRUE);
                    return 0;
                case 'M':
                    ProbeAllCapabilities();
                    return 0;
                case VK_F5:
                    Refresh(true);
                    if (g.mode == Mode::Restricted) ApplyMode(Mode::Restricted);
                    else if (g.mode == Mode::OpaqueCandidate) ApplyMode(Mode::OpaqueCandidate);
                    return 0;
                case VK_ESCAPE:
                    DestroyWindow(hwnd);
                    return 0;
            }
            break;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd, &ps);
            DrawControl(hwnd, dc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_DESTROY:
            DestroyDComp();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    SetProcessDPIAware();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpfnWndProc = ControlProc;
    wc.lpszClassName = kControlClass;
    RegisterClassExW(&wc);

    WNDCLASSEXW owc{ sizeof(owc) };
    owc.hInstance = instance;
    owc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    owc.lpfnWndProc = OverlayProc;
    owc.lpszClassName = kOverlayClass;
    RegisterClassExW(&owc);

    g.control = CreateWindowExW(WS_EX_APPWINDOW, kControlClass, L"AMD 680M Clone Overlay Probe v2",
                                WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                CW_USEDEFAULT, CW_USEDEFAULT, 940, 760,
                                nullptr, nullptr, instance, nullptr);
    if (!g.control) return 1;

    const int sw = GetSystemMetrics(SM_CXSCREEN);
    int width = sw - 120;
    if (width < 520) width = 520;
    if (width > 1100) width = 1100;
    const int x = (sw - width) / 2;

    g.overlay = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                kOverlayClass, L"Clone Overlay Test Slab", WS_POPUP,
                                x, 40, width, 140, nullptr, nullptr, instance, nullptr);
    if (!g.overlay) return 2;

    Refresh(true);
    ApplyMode(Mode::LayeredGDI);
    SetForegroundWindow(g.control);
    Log(L"Probe v2 started");

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    Log(L"Probe v2 exited");
    if (gLog.is_open()) gLog.close();
    CoUninitialize();
    return 0;
}
