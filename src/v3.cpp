#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <d3dkmthk.h>
#include <wrl/client.h>
#include <cstdio>
#include <iostream>
#include <cstdint>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <iomanip>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

using Microsoft::WRL::ComPtr;

static std::wofstream gLog;

static std::wstring ExeDir() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring p(path);
    auto pos = p.find_last_of(L"\\/");
    return pos == std::wstring::npos ? L"" : p.substr(0, pos + 1);
}

static void Log(const std::wstring& s) {
    if (!gLog.is_open()) gLog.open(ExeDir() + L"clone_overlay_v3.log", std::ios::out | std::ios::trunc);
    SYSTEMTIME st{};
    GetLocalTime(&st);
    std::wstringstream line;
    line << L"[" << std::setfill(L'0') << std::setw(2) << st.wHour << L":"
         << std::setw(2) << st.wMinute << L":" << std::setw(2) << st.wSecond << L"] " << s;
    std::wcout << line.str() << std::endl;
    if (gLog.is_open()) gLog << line.str() << std::endl;
}

static std::wstring NtHex(NTSTATUS s) {
    std::wstringstream ss;
    ss << L"0x" << std::hex << std::uppercase << static_cast<uint32_t>(s);
    return ss.str();
}

static std::wstring HrHex(HRESULT hr) {
    std::wstringstream ss;
    ss << L"0x" << std::hex << std::uppercase << static_cast<uint32_t>(hr);
    return ss.str();
}

static const wchar_t* TechName(DISPLAYCONFIG_VIDEO_OUTPUT_TECHNOLOGY t) {
    switch (t) {
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INTERNAL: return L"Internal";
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI: return L"HDMI";
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EXTERNAL: return L"DisplayPort external";
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EMBEDDED: return L"DisplayPort embedded";
        default: return L"Other";
    }
}

struct CloneInfo {
    bool clone = false;
    UINT sourceId = 0;
    std::wstring gdiName = L"\\\\.\\DISPLAY1";
};

static CloneInfo QueryCloneInfo() {
    CloneInfo ci;
    UINT32 pathCount = 0, modeCount = 0;
    LONG rc = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount);
    if (rc != ERROR_SUCCESS) {
        Log(L"GetDisplayConfigBufferSizes failed: " + std::to_wstring(rc));
        return ci;
    }
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    rc = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr);
    if (rc != ERROR_SUCCESS) {
        Log(L"QueryDisplayConfig failed: " + std::to_wstring(rc));
        return ci;
    }
    Log(L"Active display paths=" + std::to_wstring(pathCount));
    for (UINT32 i = 0; i < pathCount; ++i) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME sn{};
        sn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        sn.header.size = sizeof(sn);
        sn.header.adapterId = paths[i].sourceInfo.adapterId;
        sn.header.id = paths[i].sourceInfo.id;
        DisplayConfigGetDeviceInfo(&sn.header);
        DISPLAYCONFIG_TARGET_DEVICE_NAME tn{};
        tn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        tn.header.size = sizeof(tn);
        tn.header.adapterId = paths[i].targetInfo.adapterId;
        tn.header.id = paths[i].targetInfo.id;
        DisplayConfigGetDeviceInfo(&tn.header);
        std::wstringstream ss;
        ss << L"Path " << i
           << L": source=" << paths[i].sourceInfo.id
           << L" target=" << paths[i].targetInfo.id
           << L" tech=" << static_cast<int>(paths[i].targetInfo.outputTechnology)
           << L" (" << TechName(paths[i].targetInfo.outputTechnology) << L")"
           << L" GDI=" << sn.viewGdiDeviceName
           << L" monitor=\"" << tn.monitorFriendlyDeviceName << L"\"";
        Log(ss.str());
        if (i == 0) {
            ci.sourceId = paths[i].sourceInfo.id;
            if (sn.viewGdiDeviceName[0]) ci.gdiName = sn.viewGdiDeviceName;
        }
    }
    for (UINT32 i = 0; i < pathCount; ++i) {
        for (UINT32 j = i + 1; j < pathCount; ++j) {
            const auto& a = paths[i].sourceInfo;
            const auto& b = paths[j].sourceInfo;
            if (a.id == b.id && a.adapterId.HighPart == b.adapterId.HighPart && a.adapterId.LowPart == b.adapterId.LowPart) ci.clone = true;
        }
    }
    Log(std::wstring(L"Topology: ") + (ci.clone ? L"CLONE DETECTED" : L"clone not detected"));
    return ci;
}

struct KmtSession {
    D3DKMT_HANDLE adapter = 0;
    D3DKMT_HANDLE device = 0;
    D3DKMT_HANDLE context = 0;
    UINT sourceId = 0;
    std::wstring gdiName;
    void Close() {
        if (context) {
            D3DKMT_DESTROYCONTEXT d{};
            d.hContext = context;
            NTSTATUS s = D3DKMTDestroyContext(&d);
            Log(L"D3DKMTDestroyContext: " + NtHex(s));
            context = 0;
        }
        if (device) {
            D3DKMT_DESTROYDEVICE d{};
            d.hDevice = device;
            NTSTATUS s = D3DKMTDestroyDevice(&d);
            Log(L"D3DKMTDestroyDevice: " + NtHex(s));
            device = 0;
        }
        if (adapter) {
            D3DKMT_CLOSEADAPTER c{};
            c.hAdapter = adapter;
            NTSTATUS s = D3DKMTCloseAdapter(&c);
            Log(L"D3DKMTCloseAdapter: " + NtHex(s));
            adapter = 0;
        }
    }
    ~KmtSession() { Close(); }
};

static bool OpenKmt(const CloneInfo& ci, KmtSession& k) {
    k.sourceId = ci.sourceId;
    k.gdiName = ci.gdiName;
    D3DKMT_OPENADAPTERFROMGDIDISPLAYNAME oa{};
    wcsncpy_s(oa.DeviceName, ci.gdiName.c_str(), _TRUNCATE);
    NTSTATUS s = D3DKMTOpenAdapterFromGdiDisplayName(&oa);
    Log(L"D3DKMTOpenAdapterFromGdiDisplayName(" + ci.gdiName + L"): " + NtHex(s));
    if (s < 0) return false;
    k.adapter = oa.hAdapter;
    k.sourceId = oa.VidPnSourceId;

    D3DKMT_CREATEDEVICE cd{};
    cd.hAdapter = k.adapter;
    s = D3DKMTCreateDevice(&cd);
    Log(L"D3DKMTCreateDevice: " + NtHex(s) + L" hDevice=" + std::to_wstring(cd.hDevice));
    if (s < 0) return false;
    k.device = cd.hDevice;

    D3DKMT_CREATECONTEXTVIRTUAL cc{};
    cc.hDevice = k.device;
    cc.NodeOrdinal = 0;
    cc.EngineAffinity = 0;
    cc.ClientHint = D3DKMT_CLIENTHINT_DX11;
    s = D3DKMTCreateContextVirtual(&cc);
    Log(L"D3DKMTCreateContextVirtual: " + NtHex(s) + L" hContext=" + std::to_wstring(cc.hContext));
    if (s >= 0) k.context = cc.hContext;
    return true;
}

struct Graphics {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
};

static bool CreateGraphics(Graphics& g) {
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL got{};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                   levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                                   &g.device, &got, &g.context);
    if (hr == E_INVALIDARG) {
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                               &levels[1], 1, D3D11_SDK_VERSION,
                               &g.device, &got, &g.context);
    }
    Log(L"D3D11CreateDevice: " + HrHex(hr) + L" feature=" + std::to_wstring(static_cast<int>(got)));
    return SUCCEEDED(hr);
}

struct SharedTextureKmt {
    std::wstring label;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    UINT width = 0, height = 0;
    ComPtr<ID3D11Texture2D> texture;
    HANDLE sharedHandle = nullptr;
    D3DKMT_HANDLE resource = 0;
    std::vector<D3DDDI_OPENALLOCATIONINFO2> allocations;
    std::vector<unsigned char> privateRuntime;
    std::vector<unsigned char> resourcePrivate;
    std::vector<unsigned char> totalPrivate;
    void Close(KmtSession& k) {
        if (resource && k.device) {
            D3DKMT_DESTROYALLOCATION d{};
            d.hDevice = k.device;
            d.hResource = resource;
            NTSTATUS s = D3DKMTDestroyAllocation(&d);
            Log(label + L" D3DKMTDestroyAllocation: " + NtHex(s));
            resource = 0;
        }
        if (sharedHandle) {
            CloseHandle(sharedHandle);
            sharedHandle = nullptr;
        }
        texture.Reset();
    }
};

static bool CreateSharedTexture(Graphics& g, KmtSession& k, const wchar_t* label,
                                DXGI_FORMAT fmt, UINT width, UINT height, SharedTextureKmt& out) {
    out.label = label;
    out.format = fmt;
    out.width = width;
    out.height = height;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = width;
    td.Height = height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = (fmt == DXGI_FORMAT_NV12 || fmt == DXGI_FORMAT_P010)
        ? D3D11_BIND_SHADER_RESOURCE
        : (D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
    HRESULT hr = g.device->CreateTexture2D(&td, nullptr, &out.texture);
    Log(out.label + L" CreateTexture2D(" + std::to_wstring(static_cast<int>(fmt)) + L"): " + HrHex(hr));
    if (FAILED(hr)) return false;
    if (td.BindFlags & D3D11_BIND_RENDER_TARGET) {
        ComPtr<ID3D11RenderTargetView> rtv;
        if (SUCCEEDED(g.device->CreateRenderTargetView(out.texture.Get(), nullptr, &rtv))) {
            const float color[4] = {0.95f, 0.08f, 0.70f, 1.0f};
            g.context->ClearRenderTargetView(rtv.Get(), color);
            g.context->Flush();
        }
    }
    ComPtr<IDXGIResource1> res1;
    hr = out.texture.As(&res1);
    if (FAILED(hr)) {
        Log(out.label + L" QI IDXGIResource1 failed: " + HrHex(hr));
        return false;
    }
    hr = res1->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &out.sharedHandle);
    Log(out.label + L" CreateSharedHandle: " + HrHex(hr));
    if (FAILED(hr)) return false;

    D3DKMT_QUERYRESOURCEINFOFROMNTHANDLE qi{};
    qi.hDevice = k.device;
    qi.hNtHandle = out.sharedHandle;
    NTSTATUS s = D3DKMTQueryResourceInfoFromNtHandle(&qi);
    {
        std::wstringstream ss;
        ss << out.label << L" QueryResourceInfoFromNtHandle: " << NtHex(s)
           << L" allocations=" << qi.NumAllocations
           << L" runtime=" << qi.PrivateRuntimeDataSize
           << L" resourcePriv=" << qi.ResourcePrivateDriverDataSize
           << L" totalPriv=" << qi.TotalPrivateDriverDataSize;
        Log(ss.str());
    }
    if (s < 0 || qi.NumAllocations == 0) return false;
    out.allocations.resize(qi.NumAllocations);
    out.privateRuntime.resize(qi.PrivateRuntimeDataSize ? qi.PrivateRuntimeDataSize : 1);
    out.resourcePrivate.resize(qi.ResourcePrivateDriverDataSize ? qi.ResourcePrivateDriverDataSize : 1);
    out.totalPrivate.resize(qi.TotalPrivateDriverDataSize ? qi.TotalPrivateDriverDataSize : 1);

    D3DKMT_OPENRESOURCEFROMNTHANDLE op{};
    op.hDevice = k.device;
    op.hNtHandle = out.sharedHandle;
    op.NumAllocations = qi.NumAllocations;
    op.pOpenAllocationInfo2 = out.allocations.data();
    op.PrivateRuntimeDataSize = qi.PrivateRuntimeDataSize;
    op.pPrivateRuntimeData = qi.PrivateRuntimeDataSize ? out.privateRuntime.data() : nullptr;
    op.ResourcePrivateDriverDataSize = qi.ResourcePrivateDriverDataSize;
    op.pResourcePrivateDriverData = qi.ResourcePrivateDriverDataSize ? out.resourcePrivate.data() : nullptr;
    op.TotalPrivateDriverDataBufferSize = qi.TotalPrivateDriverDataSize;
    op.pTotalPrivateDriverDataBuffer = qi.TotalPrivateDriverDataSize ? out.totalPrivate.data() : nullptr;
    s = D3DKMTOpenResourceFromNtHandle(&op);
    Log(out.label + L" D3DKMTOpenResourceFromNtHandle: " + NtHex(s) + L" hResource=" + std::to_wstring(op.hResource));
    if (s < 0) return false;
    out.resource = op.hResource;
    for (size_t i = 0; i < out.allocations.size(); ++i) {
        std::wstringstream ss;
        ss << out.label << L" allocation[" << i << L"] hAllocation=" << out.allocations[i].hAllocation
           << L" gpuVA=0x" << std::hex << out.allocations[i].GpuVirtualAddress;
        Log(ss.str());
    }
    return true;
}

static D3DKMT_MULTIPLANE_OVERLAY_ATTRIBUTES3 MakeAttrs(UINT w, UINT h, LONG x, LONG y,
                                                       D3DKMT_MULTIPLANE_OVERLAY_BLEND blend) {
    D3DKMT_MULTIPLANE_OVERLAY_ATTRIBUTES3 a{};
    a.Flags = 0;
    a.SrcRect = {0, 0, static_cast<LONG>(w), static_cast<LONG>(h)};
    a.DstRect = {x, y, x + static_cast<LONG>(w), y + static_cast<LONG>(h)};
    a.ClipRect = a.DstRect;
    a.Rotation = D3DDDI_ROTATION_IDENTITY;
    a.Blend = blend;
    a.DirtyRectCount = 0;
    a.pDirtyRects = nullptr;
    a.ColorSpace = D3DDDI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    a.StretchQuality = DXGKMT_MULTIPLANE_OVERLAY_STRETCH_QUALITY_BILINEAR;
    a.SDRWhiteLevel = 0;
    return a;
}

struct SupportResult {
    NTSTATUS status = static_cast<NTSTATUS>(0xC0000001L);
    bool supported = false;
    UINT returnValue = 0;
};

static SupportResult CheckSupport3(KmtSession& k, SharedTextureKmt& tex, UINT layer,
                                   LONG x, LONG y, D3DKMT_MULTIPLANE_OVERLAY_BLEND blend) {
    SupportResult r{};
    auto attrs = MakeAttrs(tex.width, tex.height, x, y, blend);
    D3DKMT_CHECK_MULTIPLANE_OVERLAY_PLANE3 plane{};
    plane.LayerIndex = layer;
    plane.hResource = tex.resource;
    plane.CompSurfaceLuid = LUID{};
    plane.VidPnSourceId = k.sourceId;
    plane.pPlaneAttributes = &attrs;
    D3DKMT_CHECK_MULTIPLANE_OVERLAY_PLANE3* p = &plane;
    D3DKMT_CHECKMULTIPLANEOVERLAYSUPPORT3 chk{};
    chk.hAdapter = k.adapter;
    chk.hDevice = k.device;
    chk.PlaneCount = 1;
    chk.ppOverlayPlanes = &p;
    r.status = D3DKMTCheckMultiPlaneOverlaySupport3(&chk);
    r.supported = chk.Supported != FALSE;
    r.returnValue = chk.ReturnInfo.Value;
    std::wstringstream ss;
    ss << tex.label << L" CheckMPO3 layer=" << layer
       << L" status=" << NtHex(r.status)
       << L" supported=" << (r.supported ? L"TRUE" : L"FALSE")
       << L" return=0x" << std::hex << chk.ReturnInfo.Value
       << L" failingPlane=" << std::dec << chk.ReturnInfo.FailingPlane
       << L" tryAgain=" << chk.ReturnInfo.TryAgain;
    Log(ss.str());
    return r;
}

static SupportResult CheckSupport3Pair(KmtSession& k, SharedTextureKmt& a, SharedTextureKmt& b) {
    SupportResult r{};
    auto aa = MakeAttrs(a.width, a.height, 80, 80, D3DKMT_MULTIPLANE_OVERLAY_BLEND_OPAQUE);
    auto ab = MakeAttrs(b.width, b.height, 180, 180, D3DKMT_MULTIPLANE_OVERLAY_BLEND_OPAQUE);
    D3DKMT_CHECK_MULTIPLANE_OVERLAY_PLANE3 pa{};
    pa.LayerIndex = 0; pa.hResource = a.resource; pa.VidPnSourceId = k.sourceId; pa.pPlaneAttributes = &aa;
    D3DKMT_CHECK_MULTIPLANE_OVERLAY_PLANE3 pb{};
    pb.LayerIndex = 1; pb.hResource = b.resource; pb.VidPnSourceId = k.sourceId; pb.pPlaneAttributes = &ab;
    D3DKMT_CHECK_MULTIPLANE_OVERLAY_PLANE3* planes[2] = {&pa, &pb};
    D3DKMT_CHECKMULTIPLANEOVERLAYSUPPORT3 chk{};
    chk.hAdapter = k.adapter;
    chk.hDevice = k.device;
    chk.PlaneCount = 2;
    chk.ppOverlayPlanes = planes;
    r.status = D3DKMTCheckMultiPlaneOverlaySupport3(&chk);
    r.supported = chk.Supported != FALSE;
    r.returnValue = chk.ReturnInfo.Value;
    std::wstringstream ss;
    ss << L"PAIR CheckMPO3 status=" << NtHex(r.status)
       << L" supported=" << (r.supported ? L"TRUE" : L"FALSE")
       << L" return=0x" << std::hex << chk.ReturnInfo.Value
       << L" failingPlane=" << std::dec << chk.ReturnInfo.FailingPlane
       << L" tryAgain=" << chk.ReturnInfo.TryAgain;
    Log(ss.str());
    return r;
}

static NTSTATUS TryPresent3(KmtSession& k, SharedTextureKmt& tex) {
    if (!k.context || tex.allocations.empty()) {
        Log(L"Present3 skipped: missing KMT context or allocation");
        return static_cast<NTSTATUS>(0xC000000DL);
    }
    auto attrs = MakeAttrs(tex.width, tex.height, 120, 120, D3DKMT_MULTIPLANE_OVERLAY_BLEND_OPAQUE);
    std::vector<D3DKMT_HANDLE> allocs;
    for (auto& a : tex.allocations) allocs.push_back(a.hAllocation);
    D3DKMT_MULTIPLANE_OVERLAY3 plane{};
    plane.LayerIndex = 1;
    plane.InputFlags.Enabled = 1;
    plane.FlipInterval = D3DDDI_FLIPINTERVAL_ONE;
    plane.AllocationCount = static_cast<UINT>(allocs.size());
    plane.pAllocationList = allocs.data();
    plane.pPlaneAttributes = &attrs;
    D3DKMT_MULTIPLANE_OVERLAY3* pp = &plane;
    D3DKMT_HANDLE ctx = k.context;
    D3DKMT_PRESENT_MULTIPLANE_OVERLAY3 present{};
    present.hAdapter = k.adapter;
    present.ContextCount = 1;
    present.pContextList = &ctx;
    present.VidPnSourceId = k.sourceId;
    present.PresentCount = 1;
    present.PresentPlaneCount = 1;
    present.ppPresentPlanes = &pp;
    NTSTATUS s = D3DKMTPresentMultiPlaneOverlay3(&present);
    Log(L"D3DKMTPresentMultiPlaneOverlay3: " + NtHex(s));
    return s;
}

int wmain() {
    SetConsoleOutputCP(CP_UTF8);
    Log(L"========== CloneOverlayProbe V3 START ==========");
    Log(L"V3: real shared D3D11 resources -> KMT resource/allocation -> CheckMultiPlaneOverlaySupport3 -> optional PresentMultiPlaneOverlay3.");
    CloneInfo ci = QueryCloneInfo();
    if (!ci.clone) Log(L"WARNING: not in Win+P Duplicate/Clone mode.");
    Graphics gfx;
    if (!CreateGraphics(gfx)) {
        Log(L"FATAL: D3D11 device creation failed");
        return 2;
    }
    KmtSession k;
    if (!OpenKmt(ci, k)) {
        Log(L"FATAL: KMT session open failed");
        return 3;
    }
    Log(L"KMT sourceId=" + std::to_wstring(k.sourceId));
    SharedTextureKmt bgra, rgb10, nv12;
    const bool okBGRA = CreateSharedTexture(gfx, k, L"BGRA8", DXGI_FORMAT_B8G8R8A8_UNORM, 640, 180, bgra);
    const bool okRGB10 = CreateSharedTexture(gfx, k, L"RGB10A2", DXGI_FORMAT_R10G10B10A2_UNORM, 640, 180, rgb10);
    const bool okNV12 = CreateSharedTexture(gfx, k, L"NV12", DXGI_FORMAT_NV12, 640, 180, nv12);
    SupportResult sBGRA{}, sRGB10{}, sNV12{}, sPair{};
    if (okBGRA) sBGRA = CheckSupport3(k, bgra, 1, 80, 80, D3DKMT_MULTIPLANE_OVERLAY_BLEND_OPAQUE);
    if (okRGB10) sRGB10 = CheckSupport3(k, rgb10, 1, 80, 280, D3DKMT_MULTIPLANE_OVERLAY_BLEND_OPAQUE);
    if (okNV12) sNV12 = CheckSupport3(k, nv12, 1, 80, 480, D3DKMT_MULTIPLANE_OVERLAY_BLEND_OPAQUE);
    if (okRGB10 && okNV12) sPair = CheckSupport3Pair(k, rgb10, nv12);
    Log(L"========== V3 SUMMARY ==========");
    Log(std::wstring(L"BGRA8 resource=") + (okBGRA ? L"OK" : L"FAIL") + L" support3=" + (sBGRA.supported ? L"TRUE" : L"FALSE"));
    Log(std::wstring(L"RGB10 resource=") + (okRGB10 ? L"OK" : L"FAIL") + L" support3=" + (sRGB10.supported ? L"TRUE" : L"FALSE"));
    Log(std::wstring(L"NV12 resource=") + (okNV12 ? L"OK" : L"FAIL") + L" support3=" + (sNV12.supported ? L"TRUE" : L"FALSE"));
    Log(std::wstring(L"RGB10+NV12 pair support3=") + (sPair.supported ? L"TRUE" : L"FALSE"));
    std::wcout << L"\nPress P to TRY direct KMT PresentMultiPlaneOverlay3 with RGB10. Press Enter to skip.\n> ";
    std::wstring input;
    std::getline(std::wcin, input);
    if (!input.empty() && (input[0] == L'P' || input[0] == L'p')) {
        if (okRGB10 && sRGB10.supported) {
            NTSTATUS ps = TryPresent3(k, rgb10);
            Log(L"Present3 attempted, status=" + NtHex(ps) + L". Observe laptop and HDMI for any difference.");
            std::wcout << L"Press Enter to finish after observing both screens..." << std::endl;
            std::getline(std::wcin, input);
        } else {
            Log(L"Present3 not attempted because RGB10 CheckMPO3 did not report supported.");
        }
    } else {
        Log(L"Present3 skipped by user.");
    }
    nv12.Close(k);
    rgb10.Close(k);
    bgra.Close(k);
    Log(L"========== CloneOverlayProbe V3 END ==========");
    return 0;
}
