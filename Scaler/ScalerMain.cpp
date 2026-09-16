#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <tchar.h>
#include <dwmapi.h>
#define boolean bool
#include <initguid.h>
#include <winstring.h>
#include <roapi.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <windows.foundation.h>
#include <windows.graphics.capture.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <shlwapi.h>
#include <stdio.h>
#include <cmath>
#include <vector>
#include <algorithm>
#include <atomic>

#include "MenuCommon/Defines.h"

// Precompiled Shaders
#include "Shaders/Nearest_CS.h"
#include "Shaders/Bicubic_CS.h"
#include "Shaders/Lanczos_CS.h"
#include "Shaders/FSR_EASU_CS.h"
#include "Shaders/FSR_RCAS_CS.h"
#include "Shaders/Anime4K_3D_Pass1_CS.h"
#include "Shaders/Anime4K_3D_Pass2_CS.h"
#include "Shaders/Anime4K_3D_Pass3_CS.h"
#include "Shaders/Anime4K_3D_AA_Pass1_CS.h"
#include "Shaders/Anime4K_3D_AA_Pass2_CS.h"
#include "Shaders/Anime4K_3D_AA_Pass3_CS.h"
#include "Shaders/Anime4K_Final_CS.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "runtimeobject.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shlwapi.lib")

// DWM API Constant Guards
#ifndef DWMWA_EXTENDED_FRAME_BOUNDS
#define DWMWA_EXTENDED_FRAME_BOUNDS 9
#endif
#ifndef DWMWA_NCRENDERING_ENABLED
#define DWMWA_NCRENDERING_ENABLED 1
#endif
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWA_BORDER_COLOR
#define DWMWA_BORDER_COLOR 34
#endif
#ifndef DWMWA_COLOR_NONE
#define DWMWA_COLOR_NONE 0xFFFFFFFE
#endif
#ifndef DWMWA_VISIBLE_FRAME_BORDER_THICKNESS
#define DWMWA_VISIBLE_FRAME_BORDER_THICKNESS 37
#endif
#ifndef DWMWCP_DEFAULT
#define DWMWCP_DEFAULT 0
#define DWMWCP_DONOTROUND 1
#define DWMWCP_ROUND 2
#define DWMWCP_ROUNDSMALL 3
#endif

// Scaler Constant Buffer Struct (matches HLSL ScalerCB)
struct ScalerCBData
{
    float SourceSize[2];
    float SourceOffset[2];
    float TargetSize[2];
    float TargetOffset[2];
    float FullTargetSize[2];
    float SourceTexSize[2];
};

// Logging Utility
static void ScalerLog(const char* fmt, ...)
{
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    OutputDebugStringA(buf);

    FILE* f = fopen("C:\\Program Files\\MenuTools\\scaler_debug.log", "a");
    if (!f)
    {
        wchar_t tempPath[MAX_PATH];
        if (GetTempPathW(MAX_PATH, tempPath))
        {
            wchar_t logPath[MAX_PATH];
            PathCombineW(logPath, tempPath, L"menutools_scaler.log");
            f = _wfopen(logPath, L"a");
        }
    }
    if (f)
    {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(f, "[%02d:%02d:%02d.%03d] %s", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, buf);
        fclose(f);
    }
}

// Global Lifecycle & State
static std::atomic<bool> g_ShuttingDown{ false };
static bool g_FrameArrivedSubscribed = false;
static EventRegistrationToken g_FrameArrivedToken = { 0 };
static bool g_RoInitialized = false;

static HANDLE g_hActiveMutex = NULL;
static HANDLE g_hExitEvent = NULL;
static HANDLE g_hFrameEvent = NULL;
static HANDLE g_hTargetProcess = NULL;
static HWINEVENTHOOK g_hForegroundHook = NULL;
static HWINEVENTHOOK g_hMinimizeHook = NULL;
static HWINEVENTHOOK g_hLocationHook = NULL;
static std::atomic_bool g_ExitRequested{ false };
static std::atomic_bool g_GeometryDirty{ true };

static DWORD g_PrevTargetCornerPref = DWMWCP_DEFAULT;
static bool g_HasTargetCornerPref = false;

static HWND g_ScalerHWnd = NULL;
static HWND g_TargetHWnd = NULL;
static DWORD g_TargetPid = 0;
static int g_FilterId = MT_SCALER_FILTER_BICUBIC;
static bool g_PreserveAspect = true;
static bool g_Running = true;

static int g_VpX = 0;
static int g_VpY = 0;
static int g_VpW = 0;
static int g_VpH = 0;

// Resolved WGC texture-space crop & source rect
static RECT g_CaptureBounds = { 0 };
static RECT g_DesiredSourceRect = { 0 };
static int g_CropOffsetX = 0;
static int g_CropOffsetY = 0;
static int g_CropSizeW = 0;
static int g_CropSizeH = 0;
static int g_ClientW = 0;
static int g_ClientH = 0;

// Visible region inside target client area
static int g_SourceViewX = 0;
static int g_SourceViewY = 0;
static int g_SourceViewW = 0;
static int g_SourceViewH = 0;

static ABI::Windows::Graphics::SizeInt32 g_CachedContentSize{ 0, 0 };
static ABI::Windows::Graphics::SizeInt32 g_CaptureItemSize{ 0, 0 };
static ABI::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice* g_pDirect3DDevice = nullptr;

static ID3D11Device* g_pDevice = nullptr;
static ID3D11DeviceContext* g_pContext = nullptr;
static IDXGISwapChain1* g_pSwapChain = nullptr;
static ID3D11UnorderedAccessView* g_pBackBufferUAV = nullptr;
static ID3D11Buffer* g_pConstantBuffer = nullptr;
static ID3D11SamplerState* g_pPointSampler = nullptr;
static ID3D11SamplerState* g_pLinearSampler = nullptr;

// Compute Shaders
static ID3D11ComputeShader* g_pNearestCS = nullptr;
static ID3D11ComputeShader* g_pBicubicCS = nullptr;
static ID3D11ComputeShader* g_pLanczosCS = nullptr;
static ID3D11ComputeShader* g_pFsrEasuCS = nullptr;
static ID3D11ComputeShader* g_pFsrRcasCS = nullptr;
static ID3D11ComputeShader* g_pAnime4K_3D_P1 = nullptr;
static ID3D11ComputeShader* g_pAnime4K_3D_P2 = nullptr;
static ID3D11ComputeShader* g_pAnime4K_3D_P3 = nullptr;
static ID3D11ComputeShader* g_pAnime4K_3D_AA_P1 = nullptr;
static ID3D11ComputeShader* g_pAnime4K_3D_AA_P2 = nullptr;
static ID3D11ComputeShader* g_pAnime4K_3D_AA_P3 = nullptr;
static ID3D11ComputeShader* g_pAnime4K_Final = nullptr;

// Intermediate Textures
static ID3D11Texture2D* g_pFsrIntermediateTex = nullptr;
static ID3D11ShaderResourceView* g_pFsrIntermediateSRV = nullptr;
static ID3D11UnorderedAccessView* g_pFsrIntermediateUAV = nullptr;
static UINT g_FsrIntermediateW = 0;
static UINT g_FsrIntermediateH = 0;

static ID3D11Texture2D* g_pA4KTex1 = nullptr;
static ID3D11ShaderResourceView* g_pA4KSRV1 = nullptr;
static ID3D11UnorderedAccessView* g_pA4KUAV1 = nullptr;

static ID3D11Texture2D* g_pA4KTex2 = nullptr;
static ID3D11ShaderResourceView* g_pA4KSRV2 = nullptr;
static ID3D11UnorderedAccessView* g_pA4KUAV2 = nullptr;

static ID3D11Texture2D* g_pA4KTex2x = nullptr;
static ID3D11ShaderResourceView* g_pA4KSRV2x = nullptr;
static ID3D11UnorderedAccessView* g_pA4KUAV2x = nullptr;
static UINT g_A4KAllocW = 0;
static UINT g_A4KAllocH = 0;
static DXGI_FORMAT g_A4KFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
static DWORD g_A4KPassMode = 0;

// Windows Graphics Capture
static ABI::Windows::Graphics::Capture::IGraphicsCaptureItem* g_pCaptureItem = nullptr;
static EventRegistrationToken g_CaptureItemClosedToken = { 0 };
static bool g_CaptureItemClosedSubscribed = false;
static ABI::Windows::Graphics::Capture::IDirect3D11CaptureFramePool* g_pFramePool = nullptr;
static ABI::Windows::Graphics::Capture::IGraphicsCaptureSession* g_pSession = nullptr;

// Custom Scaler Thread Messages
#define WM_APP_SCALER_FOCUS_LOST     (WM_APP + 101)
#define WM_APP_SCALER_TARGET_CLOSED  (WM_APP + 102)

// Forward Declarations
void ShutdownScaler();
void RenderFrame();
void UpdateGeometry(int screenW, int screenH, int captureW, int captureH);
bool ResolveWgcCaptureBounds(int targetW, int targetH, RECT& outCaptureBounds);
void UpdateScalerCB(
    float sourceW, float sourceH,
    float sourceOffsetX, float sourceOffsetY,
    float sourceTexW, float sourceTexH,
    float targetW, float targetH,
    float targetX, float targetY);

// Check integrity level: medium cannot interact with high IL
bool CheckIntegrityLevel(DWORD targetPid)
{
    HANDLE hTarget = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, targetPid);
    if (!hTarget) return true; // Could not query, proceed

    DWORD targetIL = 0;
    HANDLE hTargetToken = NULL;
    if (OpenProcessToken(hTarget, TOKEN_QUERY, &hTargetToken))
    {
        DWORD len = 0;
        GetTokenInformation(hTargetToken, TokenIntegrityLevel, NULL, 0, &len);
        if (len > 0)
        {
            std::vector<BYTE> buf(len);
            if (GetTokenInformation(hTargetToken, TokenIntegrityLevel, buf.data(), len, &len))
            {
                auto pTIL = (TOKEN_MANDATORY_LABEL*)buf.data();
                DWORD subAuthCount = (DWORD)(UCHAR)(*GetSidSubAuthorityCount(pTIL->Label.Sid));
                if (subAuthCount > 0)
                {
                    targetIL = *GetSidSubAuthority(pTIL->Label.Sid, subAuthCount - 1);
                }
            }
        }
        CloseHandle(hTargetToken);
    }
    CloseHandle(hTarget);

    DWORD currentIL = 0;
    HANDLE hCurToken = NULL;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hCurToken))
    {
        DWORD len = 0;
        GetTokenInformation(hCurToken, TokenIntegrityLevel, NULL, 0, &len);
        if (len > 0)
        {
            std::vector<BYTE> buf(len);
            if (GetTokenInformation(hCurToken, TokenIntegrityLevel, buf.data(), len, &len))
            {
                auto pTIL = (TOKEN_MANDATORY_LABEL*)buf.data();
                DWORD subAuthCount = (DWORD)(UCHAR)(*GetSidSubAuthorityCount(pTIL->Label.Sid));
                if (subAuthCount > 0)
                {
                    currentIL = *GetSidSubAuthority(pTIL->Label.Sid, subAuthCount - 1);
                }
            }
        }
        CloseHandle(hCurToken);
    }

    // SECURITY_MANDATORY_HIGH_RID = 0x3000, SECURITY_MANDATORY_MEDIUM_RID = 0x2000
    if (targetIL > currentIL && currentIL != 0)
    {
        return false;
    }
    return true;
}

// Check if foreground window is our scaler, the target window, or an owned dialog of the target
static bool IsForegroundOurAppOrTarget(HWND hWnd)
{
    if (!hWnd) return false;
    if (hWnd == g_TargetHWnd || hWnd == g_ScalerHWnd) return true;

    // Traverse the owner chain (up to 16 levels) to support nested dialogs: Target -> Dialog A -> Dialog B
    HWND hCur = hWnd;
    for (int depth = 0; depth < 16; ++depth)
    {
        HWND hOwner = GetWindow(hCur, GW_OWNER);
        if (!hOwner) break;
        if (hOwner == g_TargetHWnd) return true;
        hCur = hOwner;
    }

    return false;
}

// WinEventProc for Focus and Minimize changes
void CALLBACK FocusWinEventProc(
    HWINEVENTHOOK hWinEventHook,
    DWORD event,
    HWND hWnd,
    LONG idObject,
    LONG idChild,
    DWORD idEventThread,
    DWORD dwmsEventTime)
{
    UNREFERENCED_PARAMETER(hWinEventHook);
    UNREFERENCED_PARAMETER(idObject);
    UNREFERENCED_PARAMETER(idChild);
    UNREFERENCED_PARAMETER(idEventThread);
    UNREFERENCED_PARAMETER(dwmsEventTime);

    if (g_ShuttingDown.load(std::memory_order_acquire))
    {
        return;
    }

    if (event == EVENT_SYSTEM_FOREGROUND)
    {
        if (!IsForegroundOurAppOrTarget(hWnd))
        {
            if (g_ScalerHWnd && !g_ExitRequested.exchange(true))
            {
                ScalerLog("Focus lost: foreground transitioned to %p. Requesting exit.\n", hWnd);
                PostMessageW(g_ScalerHWnd, WM_APP_SCALER_FOCUS_LOST, (WPARAM)hWnd, 0);
            }
        }
    }
    else if (event == EVENT_SYSTEM_MINIMIZESTART)
    {
        if (hWnd == g_TargetHWnd)
        {
            if (g_ScalerHWnd && !g_ExitRequested.exchange(true))
            {
                ScalerLog("Target window minimized. Requesting exit.\n");
                PostMessageW(g_ScalerHWnd, WM_APP_SCALER_FOCUS_LOST, (WPARAM)hWnd, 0);
            }
        }
    }
}

// WinEventProc for Target Window Location Changes (moves, resizes)
void CALLBACK LocationWinEventProc(
    HWINEVENTHOOK hWinEventHook,
    DWORD event,
    HWND hWnd,
    LONG idObject,
    LONG idChild,
    DWORD idEventThread,
    DWORD dwmsEventTime)
{
    UNREFERENCED_PARAMETER(hWinEventHook);
    UNREFERENCED_PARAMETER(event);
    UNREFERENCED_PARAMETER(idChild);
    UNREFERENCED_PARAMETER(idEventThread);
    UNREFERENCED_PARAMETER(dwmsEventTime);

    if (g_ShuttingDown.load(std::memory_order_acquire))
    {
        return;
    }

    if (hWnd == g_TargetHWnd && idObject == OBJID_WINDOW)
    {
        g_GeometryDirty.store(true, std::memory_order_release);
        if (g_hFrameEvent)
        {
            SetEvent(g_hFrameEvent);
        }
    }
}

// Handler for GraphicsCaptureItem::Closed
class CaptureItemClosedHandler : public ABI::Windows::Foundation::ITypedEventHandler<
    ABI::Windows::Graphics::Capture::GraphicsCaptureItem*,
    IInspectable*>
{
    LONG m_refCount = 1;
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        *ppv = static_cast<IUnknown*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_refCount); }
    ULONG STDMETHODCALLTYPE Release() override
    {
        LONG val = InterlockedDecrement(&m_refCount);
        if (val == 0) delete this;
        return val;
    }
    HRESULT STDMETHODCALLTYPE Invoke(
        ABI::Windows::Graphics::Capture::IGraphicsCaptureItem* sender,
        IInspectable* args) override
    {
        UNREFERENCED_PARAMETER(sender);
        UNREFERENCED_PARAMETER(args);

        if (g_ShuttingDown.load(std::memory_order_acquire))
        {
            return S_OK;
        }

        if (g_ScalerHWnd && !g_ExitRequested.exchange(true))
        {
            ScalerLog("IGraphicsCaptureItem Closed event received. Requesting exit.\n");
            PostMessageW(g_ScalerHWnd, WM_APP_SCALER_TARGET_CLOSED, 0, 0);
        }
        return S_OK;
    }
};

// FrameArrived handler for WGC
class FrameArrivedHandler : public ABI::Windows::Foundation::ITypedEventHandler<
    ABI::Windows::Graphics::Capture::Direct3D11CaptureFramePool*,
    IInspectable*>
{
    LONG m_refCount = 1;
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        *ppv = static_cast<IUnknown*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_refCount); }
    ULONG STDMETHODCALLTYPE Release() override
    {
        LONG val = InterlockedDecrement(&m_refCount);
        if (val == 0) delete this;
        return val;
    }
    HRESULT STDMETHODCALLTYPE Invoke(
        ABI::Windows::Graphics::Capture::IDirect3D11CaptureFramePool* sender,
        IInspectable* args) override
    {
        UNREFERENCED_PARAMETER(sender);
        UNREFERENCED_PARAMETER(args);

        // Atomic shutdown gate: never signal if shutting down
        if (g_ShuttingDown.load(std::memory_order_acquire))
        {
            return S_OK;
        }

        if (g_hFrameEvent)
        {
            SetEvent(g_hFrameEvent);
        }
        return S_OK;
    }
};

// Note on Input Routing Architecture:
// Magpie virtualizes cursor position and allows native input routing through a
// mouse-transparent scaling window; MenuTools intentionally uses a smaller
// direct-forwarding implementation with endpoint-accurate coordinate mapping and
// GUI focus/child window resolution.

// Forward mouse wheel messages to target window with multi-monitor coordinate translation
void ForwardWheelEvent(UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (!IsWindow(g_TargetHWnd)) return;

    // 1. Virtual screen coordinates -> scaler overlay client coordinates
    // Crucial for multi-monitor setups with non-zero or negative origins (e.g. secondary monitor at (-1920, 0)).
    POINT ptOverlay = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
    ScreenToClient(g_ScalerHWnd, &ptOverlay);

    // 2. Check if cursor is within active viewport
    if (ptOverlay.x < g_VpX || ptOverlay.x >= g_VpX + g_VpW ||
        ptOverlay.y < g_VpY || ptOverlay.y >= g_VpY + g_VpH)
    {
        return;
    }

    int destX = ptOverlay.x - g_VpX;
    int destY = ptOverlay.y - g_VpY;

    // 3. Endpoint-preserving mapping to source view dimensions
    float mappedX = (g_VpW > 1) ? (float)round((double)destX * (g_SourceViewW - 1) / (g_VpW - 1)) : 0.0f;
    float mappedY = (g_VpH > 1) ? (float)round((double)destY * (g_SourceViewH - 1) / (g_VpH - 1)) : 0.0f;
    mappedX = (std::max)(0.0f, (std::min)(mappedX, (float)(g_SourceViewW - 1)));
    mappedY = (std::max)(0.0f, (std::min)(mappedY, (float)(g_SourceViewH - 1)));

    // 4. Target screen coordinates within desiredSourceRect
    POINT ptTargetScreen = {
        g_DesiredSourceRect.left + g_SourceViewX + (int)mappedX,
        g_DesiredSourceRect.top  + g_SourceViewY + (int)mappedY
    };
    LPARAM targetLParam = MAKELPARAM((SHORT)ptTargetScreen.x, (SHORT)ptTargetScreen.y);

    // 5. Convert to target client coordinates for recipient resolution
    POINT ptTargetClient = ptTargetScreen;
    ScreenToClient(g_TargetHWnd, &ptTargetClient);

    // 6. Recipient resolution
    DWORD dwTargetThread = GetWindowThreadProcessId(g_TargetHWnd, NULL);
    GUITHREADINFO gti = { sizeof(gti) };
    HWND hRecipient = NULL;
    if (dwTargetThread && GetGUIThreadInfo(dwTargetThread, &gti) && gti.hwndFocus)
    {
        if (gti.hwndFocus == g_TargetHWnd || IsChild(g_TargetHWnd, gti.hwndFocus))
        {
            hRecipient = gti.hwndFocus;
        }
    }
    if (!hRecipient)
    {
        POINT ptInCur = ptTargetClient;
        HWND hCur = g_TargetHWnd;
        while (true)
        {
            POINT ptChild = ptInCur;
            MapWindowPoints(g_TargetHWnd, hCur, &ptChild, 1);
            HWND hChild = RealChildWindowFromPoint(hCur, ptChild);
            if (!hChild || hChild == hCur) break;
            if (!(GetWindowLongPtrW(hChild, GWL_STYLE) & WS_VISIBLE)) break;
            hCur = hChild;
        }
        hRecipient = hCur;
    }
    if (!hRecipient) hRecipient = g_TargetHWnd;

    // 7. Post exact unquantized wParam with translated screen coordinates
    PostMessageW(hRecipient, msg, wParam, targetLParam);
}

// Forward mouse clicks, movements, double clicks, and extra buttons to target window
void ForwardMouseEvent(UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (!IsWindow(g_TargetHWnd)) return;

    if (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN || msg == WM_XBUTTONDOWN)
    {
        if (GetForegroundWindow() != g_TargetHWnd)
        {
            SetForegroundWindow(g_TargetHWnd);
        }
    }

    int x = GET_X_LPARAM(lParam);
    int y = GET_Y_LPARAM(lParam);

    if (x >= g_VpX && x < g_VpX + g_VpW && y >= g_VpY && y < g_VpY + g_VpH)
    {
        int destX = x - g_VpX;
        int destY = y - g_VpY;

        float mappedX = (g_VpW > 1) ? (float)round((double)destX * (g_SourceViewW - 1) / (g_VpW - 1)) : 0.0f;
        float mappedY = (g_VpH > 1) ? (float)round((double)destY * (g_SourceViewH - 1) / (g_VpH - 1)) : 0.0f;
        mappedX = (std::max)(0.0f, (std::min)(mappedX, (float)(g_SourceViewW - 1)));
        mappedY = (std::max)(0.0f, (std::min)(mappedY, (float)(g_SourceViewH - 1)));

        POINT ptTargetScreen = {
            g_DesiredSourceRect.left + g_SourceViewX + (int)mappedX,
            g_DesiredSourceRect.top  + g_SourceViewY + (int)mappedY
        };

        POINT ptClient = ptTargetScreen;
        ScreenToClient(g_TargetHWnd, &ptClient);
        HWND hCur = g_TargetHWnd;
        while (true)
        {
            POINT ptInCur = ptClient;
            MapWindowPoints(g_TargetHWnd, hCur, &ptInCur, 1);
            HWND hChild = RealChildWindowFromPoint(hCur, ptInCur);
            if (!hChild || hChild == hCur) break;
            if (!(GetWindowLongPtrW(hChild, GWL_STYLE) & WS_VISIBLE)) break;
            hCur = hChild;
        }
        HWND hRecipient = hCur ? hCur : g_TargetHWnd;

        POINT ptInRecipient = ptClient;
        if (hRecipient != g_TargetHWnd)
        {
            MapWindowPoints(g_TargetHWnd, hRecipient, &ptInRecipient, 1);
        }

        bool isDblClk = (msg == WM_LBUTTONDBLCLK || msg == WM_RBUTTONDBLCLK ||
                         msg == WM_MBUTTONDBLCLK || msg == WM_XBUTTONDBLCLK);
        if (isDblClk)
        {
            ULONG_PTR classStyle = GetClassLongPtrW(hRecipient, GCL_STYLE);
            if (!(classStyle & CS_DBLCLKS))
            {
                // Emulate as button down if recipient window class lacks CS_DBLCLKS
                UINT downMsg = (msg == WM_LBUTTONDBLCLK) ? WM_LBUTTONDOWN :
                               (msg == WM_RBUTTONDBLCLK) ? WM_RBUTTONDOWN :
                               (msg == WM_MBUTTONDBLCLK) ? WM_MBUTTONDOWN : WM_XBUTTONDOWN;
                PostMessageW(hRecipient, downMsg, wParam, MAKELPARAM(ptInRecipient.x, ptInRecipient.y));
                return;
            }
        }

        PostMessageW(hRecipient, msg, wParam, MAKELPARAM(ptInRecipient.x, ptInRecipient.y));
    }
}

// Overlay Window Proc
LRESULT CALLBACK ScalerWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_MOUSEACTIVATE:
        // Critical: never activate overlay window; target application remains foreground!
        return MA_NOACTIVATE;

    case WM_SETCURSOR:
        SetCursor(LoadCursor(NULL, IDC_ARROW));
        return TRUE;

    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDBLCLK:
        ForwardMouseEvent(msg, wParam, lParam);
        return 0;

    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
    case WM_XBUTTONDBLCLK:
        ForwardMouseEvent(msg, wParam, lParam);
        return TRUE;

    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
        ForwardWheelEvent(msg, wParam, lParam);
        return 0;

    case WM_APP_SCALER_FOCUS_LOST:
    {
        HWND newFg = (HWND)wParam;
        ScalerLog("Focus lost: foreground transitioned to %p. Requesting exit.\n", newFg);
        g_Running = false;
        if (g_hExitEvent)
        {
            SetEvent(g_hExitEvent);
        }
        return 0;
    }

    case WM_APP_SCALER_TARGET_CLOSED:
    {
        ScalerLog("Target window closed (WGC item closed). Requesting exit.\n");
        g_Running = false;
        if (g_hExitEvent)
        {
            SetEvent(g_hExitEvent);
        }
        return 0;
    }

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// Update Constant Buffer helper for pipeline transitions
void UpdateScalerCB(
    float sourceW, float sourceH,
    float sourceOffsetX, float sourceOffsetY,
    float sourceTexW, float sourceTexH,
    float targetW, float targetH,
    float targetX, float targetY)
{
    if (!g_pContext || !g_pConstantBuffer) return;

    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr = g_pContext->Map(g_pConstantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (SUCCEEDED(hr))
    {
        ScalerCBData* cb = (ScalerCBData*)mapped.pData;
        cb->SourceSize[0] = sourceW;
        cb->SourceSize[1] = sourceH;
        cb->SourceOffset[0] = sourceOffsetX;
        cb->SourceOffset[1] = sourceOffsetY;
        cb->TargetSize[0] = targetW;
        cb->TargetSize[1] = targetH;
        cb->TargetOffset[0] = targetX;
        cb->TargetOffset[1] = targetY;
        cb->FullTargetSize[0] = (float)g_VpW;
        cb->FullTargetSize[1] = (float)g_VpH;
        cb->SourceTexSize[0] = sourceTexW;
        cb->SourceTexSize[1] = sourceTexH;
        g_pContext->Unmap(g_pConstantBuffer, 0);
    }
}

// Resolve the real WGC capture bounds from screen-space candidates
bool ResolveWgcCaptureBounds(int targetW, int targetH, RECT& outCaptureBounds)
{
    if (!IsWindow(g_TargetHWnd)) return false;

    HMONITOR hMon = MonitorFromWindow(g_TargetHWnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(hMon, &mi);

    // 1. DWMWA_EXTENDED_FRAME_BOUNDS
    RECT rcExtended = { 0 };
    HRESULT hrExt = DwmGetWindowAttribute(g_TargetHWnd, DWMWA_EXTENDED_FRAME_BOUNDS, &rcExtended, sizeof(rcExtended));
    if (FAILED(hrExt))
    {
        GetWindowRect(g_TargetHWnd, &rcExtended);
    }

    // 2. EXTENDED_FRAME_BOUNDS intersected with monitor.rcMonitor
    RECT rcMonIntersect = { 0 };
    bool hasMonIntersect = (IntersectRect(&rcMonIntersect, &rcExtended, &mi.rcMonitor) != 0);

    // 3. Target client screen rect
    RECT rcClient = { 0 };
    GetClientRect(g_TargetHWnd, &rcClient);
    RECT rcClientScreen = rcClient;
    MapWindowPoints(g_TargetHWnd, NULL, (LPPOINT)&rcClientScreen, 2);

    // 4. EXTENDED_FRAME_BOUNDS intersected with monitor.rcWork
    RECT rcWorkIntersect = { 0 };
    bool hasWorkIntersect = (IntersectRect(&rcWorkIntersect, &rcExtended, &mi.rcWork) != 0);

    // 5. GetWindowRect (legacy fallback)
    RECT rcWindow = { 0 };
    GetWindowRect(g_TargetHWnd, &rcWindow);

    struct Candidate
    {
        RECT rect;
        const char* name;
        int width;
        int height;
        bool valid;
    };

    Candidate candidates[5] = {
        { rcExtended, "EXTENDED_FRAME_BOUNDS", rcExtended.right - rcExtended.left, rcExtended.bottom - rcExtended.top, true },
        { rcMonIntersect, "EXTENDED_FRAME_BOUNDS_MONITOR_CLIP", rcMonIntersect.right - rcMonIntersect.left, rcMonIntersect.bottom - rcMonIntersect.top, hasMonIntersect },
        { rcClientScreen, "CLIENT_SCREEN_RECT", rcClientScreen.right - rcClientScreen.left, rcClientScreen.bottom - rcClientScreen.top, true },
        { rcWorkIntersect, "EXTENDED_FRAME_BOUNDS_WORK_CLIP", rcWorkIntersect.right - rcWorkIntersect.left, rcWorkIntersect.bottom - rcWorkIntersect.top, hasWorkIntersect },
        { rcWindow, "GET_WINDOW_RECT", rcWindow.right - rcWindow.left, rcWindow.bottom - rcWindow.top, true }
    };

    int bestIndex = -1;
    int bestScore = 999999;

    for (int i = 0; i < 5; i++)
    {
        if (!candidates[i].valid || candidates[i].width <= 0 || candidates[i].height <= 0)
        {
            continue;
        }

        int diffW = abs(candidates[i].width - targetW);
        int diffH = abs(candidates[i].height - targetH);

        // Per-axis tolerance: abs(candidateW - targetW) <= 1 && abs(candidateH - targetH) <= 1
        if (diffW <= 1 && diffH <= 1)
        {
            int score = (diffW + diffH) * 10 + i;
            if (score < bestScore)
            {
                bestScore = score;
                bestIndex = i;
            }
        }
    }

    if (bestIndex >= 0)
    {
        outCaptureBounds = candidates[bestIndex].rect;
        return true;
    }

    ScalerLog("Warning: No WGC capture candidate matched target size (%d,%d) within 1px tolerance!\n", targetW, targetH);
    for (int i = 0; i < 5; i++)
    {
        ScalerLog("  Candidate [%d] %s: [%ld,%ld,%ld,%ld] (w=%d, h=%d)\n",
            i, candidates[i].name,
            candidates[i].rect.left, candidates[i].rect.top, candidates[i].rect.right, candidates[i].rect.bottom,
            candidates[i].width, candidates[i].height);
    }

    outCaptureBounds = candidates[0].rect;
    return false;
}

// Geometry & Crop calculations
void UpdateGeometry(int screenW, int screenH, int captureW, int captureH)
{
    if (!IsWindow(g_TargetHWnd)) return;

    // 1. Resolve WGC Capture Bounds
    ResolveWgcCaptureBounds(captureW, captureH, g_CaptureBounds);

    // 2. Query Extended Frame Bounds
    RECT rcExtended = { 0 };
    HRESULT hrExt = DwmGetWindowAttribute(g_TargetHWnd, DWMWA_EXTENDED_FRAME_BOUNDS, &rcExtended, sizeof(rcExtended));
    if (FAILED(hrExt))
    {
        rcExtended = g_CaptureBounds;
    }

    // 3. Query Optional Visible Frame Border Thickness (default 0 on failure)
    UINT borderThickness = 0;
    HRESULT hrThick = DwmGetWindowAttribute(g_TargetHWnd, DWMWA_VISIBLE_FRAME_BORDER_THICKNESS, &borderThickness, sizeof(borderThickness));
    if (FAILED(hrThick))
    {
        borderThickness = 0;
    }

    // 4. Query DWM NC Rendering Enabled for diagnostic logging
    BOOL ncRenderingEnabled = FALSE;
    DwmGetWindowAttribute(g_TargetHWnd, DWMWA_NCRENDERING_ENABLED, &ncRenderingEnabled, sizeof(ncRenderingEnabled));

    // 5. Inset extended frame bounds by visible frame border thickness
    RECT innerFrame = rcExtended;
    innerFrame.left += borderThickness;
    innerFrame.top += borderThickness;
    innerFrame.right -= borderThickness;
    innerFrame.bottom -= borderThickness;

    // 6. Target Client Rect in Screen Coordinates
    RECT rcClient = { 0 };
    GetClientRect(g_TargetHWnd, &rcClient);
    RECT rcClientScreen = rcClient;
    MapWindowPoints(g_TargetHWnd, NULL, (LPPOINT)&rcClientScreen, 2);

    // 7. desiredSourceRect = intersection(clientScreenRect, innerFrame)
    if (!IntersectRect(&g_DesiredSourceRect, &rcClientScreen, &innerFrame))
    {
        g_DesiredSourceRect = rcClientScreen;
    }

    if (g_DesiredSourceRect.right <= g_DesiredSourceRect.left ||
        g_DesiredSourceRect.bottom <= g_DesiredSourceRect.top)
    {
        g_DesiredSourceRect = rcClientScreen;
    }

    // 8. Final GPU Crop parameters (in WGC texture space)
    g_CropOffsetX = g_DesiredSourceRect.left - g_CaptureBounds.left;
    g_CropOffsetY = g_DesiredSourceRect.top  - g_CaptureBounds.top;
    g_CropSizeW   = g_DesiredSourceRect.right - g_DesiredSourceRect.left;
    g_CropSizeH   = g_DesiredSourceRect.bottom - g_DesiredSourceRect.top;

    g_ClientW = (std::max)(1, g_CropSizeW);
    g_ClientH = (std::max)(1, g_CropSizeH);

    // 9. Viewport and Source View calculation
    if (g_FilterId == MT_SCALER_FILTER_PIXEL_PERFECT)
    {
        if (g_ClientW > screenW || g_ClientH > screenH)
        {
            // Oversized source: Pure 1:1 scale with center crop
            g_VpW = (std::min)(g_ClientW, screenW);
            g_VpH = (std::min)(g_ClientH, screenH);
            g_VpX = (screenW - g_VpW) / 2;
            g_VpY = (screenH - g_VpH) / 2;

            // Center crop within client area
            g_SourceViewW = g_VpW;
            g_SourceViewH = g_VpH;
            g_SourceViewX = (g_ClientW - g_SourceViewW) / 2;
            g_SourceViewY = (g_ClientH - g_SourceViewH) / 2;
        }
        else
        {
            // Normal integer scale >= 1
            int scaleX = screenW / g_ClientW;
            int scaleY = screenH / g_ClientH;
            int scale = (std::min)(scaleX, scaleY);
            if (scale < 1) scale = 1;

            g_VpW = g_ClientW * scale;
            g_VpH = g_ClientH * scale;
            g_VpX = (screenW - g_VpW) / 2;
            g_VpY = (screenH - g_VpH) / 2;

            g_SourceViewX = 0;
            g_SourceViewY = 0;
            g_SourceViewW = g_ClientW;
            g_SourceViewH = g_ClientH;
        }
    }
    else if (g_PreserveAspect && g_ClientW > 0 && g_ClientH > 0)
    {
        float targetAR = (float)g_ClientW / (float)g_ClientH;
        float screenAR = (float)screenW / (float)screenH;

        if (targetAR < screenAR)
        {
            // Pillarbox
            g_VpH = screenH;
            g_VpW = (int)(screenH * targetAR + 0.5f);
            g_VpX = (screenW - g_VpW) / 2;
            g_VpY = 0;
        }
        else
        {
            // Letterbox
            g_VpW = screenW;
            g_VpH = (int)(screenW / targetAR + 0.5f);
            g_VpX = 0;
            g_VpY = (screenH - g_VpH) / 2;
        }

        g_SourceViewX = 0;
        g_SourceViewY = 0;
        g_SourceViewW = g_ClientW;
        g_SourceViewH = g_ClientH;
    }
    else
    {
        // Stretch to fill fullscreen
        g_VpX = 0;
        g_VpY = 0;
        g_VpW = screenW;
        g_VpH = screenH;

        g_SourceViewX = 0;
        g_SourceViewY = 0;
        g_SourceViewW = g_ClientW;
        g_SourceViewH = g_ClientH;
    }

    // 10. One-Time Diagnostic Logging
    RECT rcWindow = { 0 };
    GetWindowRect(g_TargetHWnd, &rcWindow);

    ScalerLog("Geometry Updated:\n"
        "  GetWindowRect:               [%ld, %ld, %ld, %ld] (w=%ld, h=%ld)\n"
        "  ExtendedFrameBounds:         [%ld, %ld, %ld, %ld] (w=%ld, h=%ld)\n"
        "  ClientScreenRect:            [%ld, %ld, %ld, %ld] (w=%ld, h=%ld)\n"
        "  VisibleFrameBorderThickness: %u\n"
        "  NCRenderingEnabled:          %d\n"
        "  GraphicsCaptureItem.Size:    [%d, %d]\n"
        "  CaptureTargetSize:           [%d, %d]\n"
        "  ContentSize:                 [%d, %d]\n"
        "  ResolvedCaptureBounds:       [%ld, %ld, %ld, %ld] (w=%ld, h=%ld)\n"
        "  DesiredSourceRect:           [%ld, %ld, %ld, %ld] (w=%ld, h=%ld)\n"
        "  Final SourceOffset:          [%d, %d]\n"
        "  Final SourceSize:            [%d, %d]\n"
        "  Viewport:                    [%d, %d, %d, %d], SourceView: [%d, %d, %d, %d]\n",
        rcWindow.left, rcWindow.top, rcWindow.right, rcWindow.bottom, rcWindow.right - rcWindow.left, rcWindow.bottom - rcWindow.top,
        rcExtended.left, rcExtended.top, rcExtended.right, rcExtended.bottom, rcExtended.right - rcExtended.left, rcExtended.bottom - rcExtended.top,
        rcClientScreen.left, rcClientScreen.top, rcClientScreen.right, rcClientScreen.bottom, rcClientScreen.right - rcClientScreen.left, rcClientScreen.bottom - rcClientScreen.top,
        borderThickness,
        (int)ncRenderingEnabled,
        g_CaptureItemSize.Width, g_CaptureItemSize.Height,
        captureW, captureH,
        g_CachedContentSize.Width, g_CachedContentSize.Height,
        g_CaptureBounds.left, g_CaptureBounds.top, g_CaptureBounds.right, g_CaptureBounds.bottom, g_CaptureBounds.right - g_CaptureBounds.left, g_CaptureBounds.bottom - g_CaptureBounds.top,
        g_DesiredSourceRect.left, g_DesiredSourceRect.top, g_DesiredSourceRect.right, g_DesiredSourceRect.bottom, g_DesiredSourceRect.right - g_DesiredSourceRect.left, g_DesiredSourceRect.bottom - g_DesiredSourceRect.top,
        g_CropOffsetX, g_CropOffsetY,
        g_CropSizeW, g_CropSizeH,
        g_VpX, g_VpY, g_VpW, g_VpH, g_SourceViewX, g_SourceViewY, g_SourceViewW, g_SourceViewH);
}

// Preallocate FSR intermediate texture
bool EnsureFSRIntermediate(UINT width, UINT height)
{
    if (g_pFsrIntermediateTex && g_pFsrIntermediateSRV && g_pFsrIntermediateUAV &&
        g_FsrIntermediateW == width && g_FsrIntermediateH == height)
    {
        return true;
    }

    if (g_pFsrIntermediateSRV) { g_pFsrIntermediateSRV->Release(); g_pFsrIntermediateSRV = nullptr; }
    if (g_pFsrIntermediateUAV) { g_pFsrIntermediateUAV->Release(); g_pFsrIntermediateUAV = nullptr; }
    if (g_pFsrIntermediateTex) { g_pFsrIntermediateTex->Release(); g_pFsrIntermediateTex = nullptr; }

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = width;
    td.Height = height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

    HRESULT hr = g_pDevice->CreateTexture2D(&td, nullptr, &g_pFsrIntermediateTex);
    if (FAILED(hr) || !g_pFsrIntermediateTex)
    {
        ScalerLog("FSR CreateTexture2D failed: 0x%08X\n", hr);
        return false;
    }
    hr = g_pDevice->CreateShaderResourceView(g_pFsrIntermediateTex, nullptr, &g_pFsrIntermediateSRV);
    if (FAILED(hr) || !g_pFsrIntermediateSRV)
    {
        ScalerLog("FSR CreateShaderResourceView failed: 0x%08X\n", hr);
        return false;
    }
    hr = g_pDevice->CreateUnorderedAccessView(g_pFsrIntermediateTex, nullptr, &g_pFsrIntermediateUAV);
    if (FAILED(hr) || !g_pFsrIntermediateUAV)
    {
        ScalerLog("FSR CreateUnorderedAccessView failed: 0x%08X\n", hr);
        return false;
    }

    g_FsrIntermediateW = width;
    g_FsrIntermediateH = height;
    return true;
}

static bool CreateA4KTexture(UINT width, UINT height, DXGI_FORMAT fmt, ID3D11Texture2D** ppTex, ID3D11ShaderResourceView** ppSRV, ID3D11UnorderedAccessView** ppUAV)
{
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = width;
    td.Height = height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

    HRESULT hr = g_pDevice->CreateTexture2D(&td, nullptr, ppTex);
    if (FAILED(hr) || !*ppTex)
    {
        ScalerLog("A4K CreateTexture2D (%ux%u, fmt %d) failed: 0x%08X\n", width, height, (int)fmt, hr);
        return false;
    }
    hr = g_pDevice->CreateShaderResourceView(*ppTex, nullptr, ppSRV);
    if (FAILED(hr) || !*ppSRV)
    {
        ScalerLog("A4K CreateShaderResourceView failed: 0x%08X\n", hr);
        return false;
    }
    hr = g_pDevice->CreateUnorderedAccessView(*ppTex, nullptr, ppUAV);
    if (FAILED(hr) || !*ppUAV)
    {
        ScalerLog("A4K CreateUnorderedAccessView failed: 0x%08X\n", hr);
        return false;
    }
    return true;
}

// Preallocate Anime4K intermediate textures with FP16/FP32 fallback support
bool EnsureAnime4KIntermediates(UINT width, UINT height)
{
    if (g_pA4KTex1 && g_pA4KTex2 && g_pA4KTex2x &&
        g_pA4KSRV1 && g_pA4KUAV1 && g_pA4KSRV2 && g_pA4KUAV2 && g_pA4KSRV2x && g_pA4KUAV2x &&
        g_A4KAllocW == width && g_A4KAllocH == height)
    {
        return true;
    }

    if (g_pA4KSRV1) { g_pA4KSRV1->Release(); g_pA4KSRV1 = nullptr; }
    if (g_pA4KUAV1) { g_pA4KUAV1->Release(); g_pA4KUAV1 = nullptr; }
    if (g_pA4KTex1) { g_pA4KTex1->Release(); g_pA4KTex1 = nullptr; }

    if (g_pA4KSRV2) { g_pA4KSRV2->Release(); g_pA4KSRV2 = nullptr; }
    if (g_pA4KUAV2) { g_pA4KUAV2->Release(); g_pA4KUAV2 = nullptr; }
    if (g_pA4KTex2) { g_pA4KTex2->Release(); g_pA4KTex2 = nullptr; }

    if (g_pA4KSRV2x) { g_pA4KSRV2x->Release(); g_pA4KSRV2x = nullptr; }
    if (g_pA4KUAV2x) { g_pA4KUAV2x->Release(); g_pA4KUAV2x = nullptr; }
    if (g_pA4KTex2x) { g_pA4KTex2x->Release(); g_pA4KTex2x = nullptr; }

    DWORD regFormat = 0;
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\MenuTools", 0, KEY_QUERY_VALUE, &hKey) == ERROR_SUCCESS)
    {
        DWORD size = sizeof(regFormat);
        RegQueryValueExW(hKey, L"ScalerA4KFormat", NULL, NULL, (LPBYTE)&regFormat, &size);
        DWORD sizeMode = sizeof(g_A4KPassMode);
        RegQueryValueExW(hKey, L"ScalerA4KPassMode", NULL, NULL, (LPBYTE)&g_A4KPassMode, &sizeMode);
        RegCloseKey(hKey);
    }
    g_A4KFormat = (regFormat == 1) ? DXGI_FORMAT_R32G32B32A32_FLOAT : DXGI_FORMAT_R16G16B16A16_FLOAT;

    bool ok = CreateA4KTexture(width, height, g_A4KFormat, &g_pA4KTex1, &g_pA4KSRV1, &g_pA4KUAV1) &&
              CreateA4KTexture(width, height, g_A4KFormat, &g_pA4KTex2, &g_pA4KSRV2, &g_pA4KUAV2) &&
              CreateA4KTexture(width * 2, height * 2, g_A4KFormat, &g_pA4KTex2x, &g_pA4KSRV2x, &g_pA4KUAV2x);

    if (!ok && g_A4KFormat == DXGI_FORMAT_R16G16B16A16_FLOAT)
    {
        ScalerLog("A4K: FP16 failed, falling back to FP32 (R32G32B32A32_FLOAT)...\n");
        g_A4KFormat = DXGI_FORMAT_R32G32B32A32_FLOAT;
        ok = CreateA4KTexture(width, height, g_A4KFormat, &g_pA4KTex1, &g_pA4KSRV1, &g_pA4KUAV1) &&
             CreateA4KTexture(width, height, g_A4KFormat, &g_pA4KTex2, &g_pA4KSRV2, &g_pA4KUAV2) &&
             CreateA4KTexture(width * 2, height * 2, g_A4KFormat, &g_pA4KTex2x, &g_pA4KSRV2x, &g_pA4KUAV2x);
    }

    if (ok)
    {
        g_A4KAllocW = width;
        g_A4KAllocH = height;
        ScalerLog("A4K intermediates allocated: %ux%u, format=%d, passMode=%lu\n", width, height, (int)g_A4KFormat, g_A4KPassMode);
        return true;
    }
    else
    {
        ScalerLog("A4K intermediates allocation failed!\n");
        return false;
    }
}

// Main Render Loop for FrameArrived
void RenderFrame()
{
    if (g_ShuttingDown.load(std::memory_order_acquire))
        return;

    if (!g_pFramePool || !g_pDevice || !g_pContext || !g_pSwapChain || !g_pBackBufferUAV)
        return;

    ABI::Windows::Graphics::Capture::IDirect3D11CaptureFrame* frame = nullptr;
    HRESULT hr = g_pFramePool->TryGetNextFrame(&frame);
    if (FAILED(hr) || !frame) return;

    ABI::Windows::Graphics::DirectX::Direct3D11::IDirect3DSurface* surface = nullptr;
    hr = frame->get_Surface(&surface);
    if (FAILED(hr) || !surface)
    {
        frame->Release();
        return;
    }

    // Check for ContentSize changes
    ABI::Windows::Graphics::SizeInt32 contentSize = { 0, 0 };
    if (SUCCEEDED(frame->get_ContentSize(&contentSize)))
    {
        if (contentSize.Width > 0 && contentSize.Height > 0 &&
            (contentSize.Width != g_CachedContentSize.Width || contentSize.Height != g_CachedContentSize.Height))
        {
            ScalerLog("ContentSize changed: [%d, %d] -> [%d, %d]. Recreating frame pool.\n",
                g_CachedContentSize.Width, g_CachedContentSize.Height,
                contentSize.Width, contentSize.Height);

            g_CachedContentSize = contentSize;
            if (g_pFramePool && g_pDirect3DDevice)
            {
                g_pFramePool->Recreate(
                    g_pDirect3DDevice,
                    ABI::Windows::Graphics::DirectX::DirectXPixelFormat_B8G8R8A8UIntNormalized,
                    2,
                    contentSize);
            }
            g_GeometryDirty.store(true, std::memory_order_release);
        }
    }

    Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess* dxgiAccess = nullptr;
    hr = surface->QueryInterface(IID_PPV_ARGS(&dxgiAccess));
    if (FAILED(hr) || !dxgiAccess)
    {
        surface->Release();
        frame->Release();
        return;
    }

    ID3D11Texture2D* capturedTexture = nullptr;
    hr = dxgiAccess->GetInterface(IID_PPV_ARGS(&capturedTexture));
    dxgiAccess->Release();
    surface->Release();

    if (FAILED(hr) || !capturedTexture)
    {
        frame->Release();
        return;
    }

    D3D11_TEXTURE2D_DESC texDesc;
    capturedTexture->GetDesc(&texDesc);

    // Get screen dimensions from swapchain
    DXGI_SWAP_CHAIN_DESC1 scDesc;
    g_pSwapChain->GetDesc1(&scDesc);
    int screenW = (int)scDesc.Width;
    int screenH = (int)scDesc.Height;

    if (g_GeometryDirty.exchange(false, std::memory_order_acq_rel))
    {
        int targetW = (g_CachedContentSize.Width > 0) ? g_CachedContentSize.Width : (int)texDesc.Width;
        int targetH = (g_CachedContentSize.Height > 0) ? g_CachedContentSize.Height : (int)texDesc.Height;
        UpdateGeometry(screenW, screenH, targetW, targetH);
    }

    // Invariant Check
    if (g_CropOffsetX < 0 || g_CropOffsetY < 0 ||
        g_CropOffsetX + g_CropSizeW > (int)texDesc.Width ||
        g_CropOffsetY + g_CropSizeH > (int)texDesc.Height ||
        g_CropSizeW <= 0 || g_CropSizeH <= 0)
    {
        ScalerLog("Crop invariant violation! Offset=[%d, %d], Size=[%d, %d], Tex=[%u, %u]\n",
            g_CropOffsetX, g_CropOffsetY, g_CropSizeW, g_CropSizeH, texDesc.Width, texDesc.Height);
        capturedTexture->Release();
        frame->Release();
        return;
    }

    // Create SRV for captured texture
    ID3D11ShaderResourceView* pCapturedSRV = nullptr;
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = texDesc.Format;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    hr = g_pDevice->CreateShaderResourceView(capturedTexture, &srvDesc, &pCapturedSRV);

    if (SUCCEEDED(hr) && pCapturedSRV)
    {
        // Update Constant Buffer
        UpdateScalerCB(
            (float)g_SourceViewW, (float)g_SourceViewH,
            (float)(g_CropOffsetX + g_SourceViewX), (float)(g_CropOffsetY + g_SourceViewY),
            (float)texDesc.Width, (float)texDesc.Height,
            (float)g_VpW, (float)g_VpH,
            (float)g_VpX, (float)g_VpY);

        // Clear backbuffer UAV to black if letterbox or pillarbox is active
        const bool hasBars = (g_VpX != 0 || g_VpY != 0 || g_VpW != screenW || g_VpH != screenH);
        if (hasBars)
        {
            float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
            g_pContext->ClearUnorderedAccessViewFloat(g_pBackBufferUAV, black);
        }

        // Bind Samplers & Constant Buffer
        ID3D11SamplerState* samplers[] = { g_pPointSampler, g_pLinearSampler };
        g_pContext->CSSetSamplers(0, 2, samplers);
        g_pContext->CSSetConstantBuffers(0, 1, &g_pConstantBuffer);

        ID3D11ShaderResourceView* nullSRV[] = { nullptr, nullptr };
        ID3D11UnorderedAccessView* nullUAV[] = { nullptr };

        // Execute selected Filter Pipeline
        switch (g_FilterId)
        {
        case MT_SCALER_FILTER_NEAREST:
        case MT_SCALER_FILTER_PIXEL_PERFECT:
        {
            if (g_pNearestCS)
            {
                g_pContext->CSSetShader(g_pNearestCS, nullptr, 0);
                g_pContext->CSSetShaderResources(0, 1, &pCapturedSRV);
                g_pContext->CSSetUnorderedAccessViews(0, 1, &g_pBackBufferUAV, nullptr);
                g_pContext->Dispatch((g_VpW + 15) / 16, (g_VpH + 15) / 16, 1);
                g_pContext->CSSetShaderResources(0, 1, nullSRV);
                g_pContext->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);
            }
            break;
        }
        case MT_SCALER_FILTER_BICUBIC:
        {
            if (g_pBicubicCS)
            {
                g_pContext->CSSetShader(g_pBicubicCS, nullptr, 0);
                g_pContext->CSSetShaderResources(0, 1, &pCapturedSRV);
                g_pContext->CSSetUnorderedAccessViews(0, 1, &g_pBackBufferUAV, nullptr);
                g_pContext->Dispatch((g_VpW + 15) / 16, (g_VpH + 15) / 16, 1);
                g_pContext->CSSetShaderResources(0, 1, nullSRV);
                g_pContext->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);
            }
            break;
        }
        case MT_SCALER_FILTER_LANCZOS:
        {
            if (g_pLanczosCS)
            {
                g_pContext->CSSetShader(g_pLanczosCS, nullptr, 0);
                g_pContext->CSSetShaderResources(0, 1, &pCapturedSRV);
                g_pContext->CSSetUnorderedAccessViews(0, 1, &g_pBackBufferUAV, nullptr);
                g_pContext->Dispatch((g_VpW + 15) / 16, (g_VpH + 15) / 16, 1);
                g_pContext->CSSetShaderResources(0, 1, nullSRV);
                g_pContext->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);
            }
            break;
        }
        case MT_SCALER_FILTER_FSR:
        {
            if (EnsureFSRIntermediate((UINT)g_VpW, (UINT)g_VpH) && g_pFsrEasuCS && g_pFsrRcasCS)
            {
                // Pass 1: EASU
                UpdateScalerCB(
                    (float)g_SourceViewW, (float)g_SourceViewH,
                    (float)(g_CropOffsetX + g_SourceViewX), (float)(g_CropOffsetY + g_SourceViewY),
                    (float)texDesc.Width, (float)texDesc.Height,
                    (float)g_VpW, (float)g_VpH,
                    0.0f, 0.0f);

                g_pContext->CSSetShader(g_pFsrEasuCS, nullptr, 0);
                g_pContext->CSSetShaderResources(0, 1, &pCapturedSRV);
                g_pContext->CSSetUnorderedAccessViews(0, 1, &g_pFsrIntermediateUAV, nullptr);
                g_pContext->Dispatch((g_VpW + 15) / 16, (g_VpH + 15) / 16, 1);
                g_pContext->CSSetShaderResources(0, 1, nullSRV);
                g_pContext->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);

                // Pass 2: RCAS
                UpdateScalerCB(
                    (float)g_VpW, (float)g_VpH,
                    0.0f, 0.0f,
                    (float)g_VpW, (float)g_VpH,
                    (float)g_VpW, (float)g_VpH,
                    (float)g_VpX, (float)g_VpY);

                g_pContext->CSSetShader(g_pFsrRcasCS, nullptr, 0);
                g_pContext->CSSetShaderResources(0, 1, &g_pFsrIntermediateSRV);
                g_pContext->CSSetUnorderedAccessViews(0, 1, &g_pBackBufferUAV, nullptr);
                g_pContext->Dispatch((g_VpW + 15) / 16, (g_VpH + 15) / 16, 1);
                g_pContext->CSSetShaderResources(0, 1, nullSRV);
                g_pContext->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);
            }
            break;
        }
        case MT_SCALER_FILTER_ANIME4K_3D:
        case MT_SCALER_FILTER_ANIME4K_3D_AA:
        {
            if (EnsureAnime4KIntermediates((UINT)g_SourceViewW, (UINT)g_SourceViewH))
            {
                ID3D11ComputeShader* p1 = (g_FilterId == MT_SCALER_FILTER_ANIME4K_3D) ? g_pAnime4K_3D_P1 : g_pAnime4K_3D_AA_P1;
                ID3D11ComputeShader* p2 = (g_FilterId == MT_SCALER_FILTER_ANIME4K_3D) ? g_pAnime4K_3D_P2 : g_pAnime4K_3D_AA_P2;
                ID3D11ComputeShader* p3 = (g_FilterId == MT_SCALER_FILTER_ANIME4K_3D) ? g_pAnime4K_3D_P3 : g_pAnime4K_3D_AA_P3;

                if (g_A4KPassMode == 1) // Diagnostic Pass Isolation: Capture -> Final (Bypass)
                {
                    UpdateScalerCB(
                        (float)g_SourceViewW, (float)g_SourceViewH,
                        (float)(g_CropOffsetX + g_SourceViewX), (float)(g_CropOffsetY + g_SourceViewY),
                        (float)texDesc.Width, (float)texDesc.Height,
                        (float)g_VpW, (float)g_VpH,
                        (float)g_VpX, (float)g_VpY);

                    g_pContext->CSSetShader(g_pAnime4K_Final, nullptr, 0);
                    g_pContext->CSSetShaderResources(0, 1, &pCapturedSRV);
                    g_pContext->CSSetUnorderedAccessViews(0, 1, &g_pBackBufferUAV, nullptr);
                    g_pContext->Dispatch((g_VpW + 15) / 16, (g_VpH + 15) / 16, 1);
                    g_pContext->CSSetShaderResources(0, 1, nullSRV);
                    g_pContext->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);
                }
                else if (g_A4KPassMode == 2) // Diagnostic Pass Isolation: Pass 1 -> Final
                {
                    UpdateScalerCB(
                        (float)g_SourceViewW, (float)g_SourceViewH,
                        (float)(g_CropOffsetX + g_SourceViewX), (float)(g_CropOffsetY + g_SourceViewY),
                        (float)texDesc.Width, (float)texDesc.Height,
                        (float)g_SourceViewW, (float)g_SourceViewH,
                        0.0f, 0.0f);

                    g_pContext->CSSetShader(p1, nullptr, 0);
                    g_pContext->CSSetShaderResources(0, 1, &pCapturedSRV);
                    g_pContext->CSSetUnorderedAccessViews(0, 1, &g_pA4KUAV1, nullptr);
                    g_pContext->Dispatch((g_SourceViewW + 15) / 16, (g_SourceViewH + 15) / 16, 1);
                    g_pContext->CSSetShaderResources(0, 1, nullSRV);
                    g_pContext->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);

                    UpdateScalerCB(
                        (float)g_SourceViewW, (float)g_SourceViewH,
                        0.0f, 0.0f,
                        (float)g_A4KAllocW, (float)g_A4KAllocH,
                        (float)g_VpW, (float)g_VpH,
                        (float)g_VpX, (float)g_VpY);

                    g_pContext->CSSetShader(g_pAnime4K_Final, nullptr, 0);
                    g_pContext->CSSetShaderResources(0, 1, &g_pA4KSRV1);
                    g_pContext->CSSetUnorderedAccessViews(0, 1, &g_pBackBufferUAV, nullptr);
                    g_pContext->Dispatch((g_VpW + 15) / 16, (g_VpH + 15) / 16, 1);
                    g_pContext->CSSetShaderResources(0, 1, nullSRV);
                    g_pContext->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);
                }
                else if (g_A4KPassMode == 3) // Diagnostic Pass Isolation: Pass 1 -> Pass 2 -> Final
                {
                    UpdateScalerCB(
                        (float)g_SourceViewW, (float)g_SourceViewH,
                        (float)(g_CropOffsetX + g_SourceViewX), (float)(g_CropOffsetY + g_SourceViewY),
                        (float)texDesc.Width, (float)texDesc.Height,
                        (float)g_SourceViewW, (float)g_SourceViewH,
                        0.0f, 0.0f);

                    g_pContext->CSSetShader(p1, nullptr, 0);
                    g_pContext->CSSetShaderResources(0, 1, &pCapturedSRV);
                    g_pContext->CSSetUnorderedAccessViews(0, 1, &g_pA4KUAV1, nullptr);
                    g_pContext->Dispatch((g_SourceViewW + 15) / 16, (g_SourceViewH + 15) / 16, 1);
                    g_pContext->CSSetShaderResources(0, 1, nullSRV);
                    g_pContext->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);

                    UpdateScalerCB(
                        (float)g_SourceViewW, (float)g_SourceViewH,
                        0.0f, 0.0f,
                        (float)g_A4KAllocW, (float)g_A4KAllocH,
                        (float)g_SourceViewW, (float)g_SourceViewH,
                        0.0f, 0.0f);

                    g_pContext->CSSetShader(p2, nullptr, 0);
                    g_pContext->CSSetShaderResources(0, 1, &g_pA4KSRV1);
                    g_pContext->CSSetUnorderedAccessViews(0, 1, &g_pA4KUAV2, nullptr);
                    g_pContext->Dispatch((g_SourceViewW + 15) / 16, (g_SourceViewH + 15) / 16, 1);
                    g_pContext->CSSetShaderResources(0, 1, nullSRV);
                    g_pContext->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);

                    UpdateScalerCB(
                        (float)g_SourceViewW, (float)g_SourceViewH,
                        0.0f, 0.0f,
                        (float)g_A4KAllocW, (float)g_A4KAllocH,
                        (float)g_VpW, (float)g_VpH,
                        (float)g_VpX, (float)g_VpY);

                    g_pContext->CSSetShader(g_pAnime4K_Final, nullptr, 0);
                    g_pContext->CSSetShaderResources(0, 1, &g_pA4KSRV2);
                    g_pContext->CSSetUnorderedAccessViews(0, 1, &g_pBackBufferUAV, nullptr);
                    g_pContext->Dispatch((g_VpW + 15) / 16, (g_VpH + 15) / 16, 1);
                    g_pContext->CSSetShaderResources(0, 1, nullSRV);
                    g_pContext->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);
                }
                else // Standard mode 0: Full Pass 1 -> Pass 2 -> Pass 3 (2x) -> Final (Catmull-Rom to Viewport)
                {
                    // Pass 1
                    UpdateScalerCB(
                        (float)g_SourceViewW, (float)g_SourceViewH,
                        (float)(g_CropOffsetX + g_SourceViewX), (float)(g_CropOffsetY + g_SourceViewY),
                        (float)texDesc.Width, (float)texDesc.Height,
                        (float)g_SourceViewW, (float)g_SourceViewH,
                        0.0f, 0.0f);

                    g_pContext->CSSetShader(p1, nullptr, 0);
                    g_pContext->CSSetShaderResources(0, 1, &pCapturedSRV);
                    g_pContext->CSSetUnorderedAccessViews(0, 1, &g_pA4KUAV1, nullptr);
                    g_pContext->Dispatch((g_SourceViewW + 15) / 16, (g_SourceViewH + 15) / 16, 1);
                    g_pContext->CSSetShaderResources(0, 1, nullSRV);
                    g_pContext->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);

                    // Pass 2
                    UpdateScalerCB(
                        (float)g_SourceViewW, (float)g_SourceViewH,
                        0.0f, 0.0f,
                        (float)g_A4KAllocW, (float)g_A4KAllocH,
                        (float)g_SourceViewW, (float)g_SourceViewH,
                        0.0f, 0.0f);

                    g_pContext->CSSetShader(p2, nullptr, 0);
                    g_pContext->CSSetShaderResources(0, 1, &g_pA4KSRV1);
                    g_pContext->CSSetUnorderedAccessViews(0, 1, &g_pA4KUAV2, nullptr);
                    g_pContext->Dispatch((g_SourceViewW + 15) / 16, (g_SourceViewH + 15) / 16, 1);
                    g_pContext->CSSetShaderResources(0, 1, nullSRV);
                    g_pContext->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);

                    // Pass 3 (2x Output)
                    UpdateScalerCB(
                        (float)g_SourceViewW, (float)g_SourceViewH,
                        (float)(g_CropOffsetX + g_SourceViewX), (float)(g_CropOffsetY + g_SourceViewY),
                        (float)texDesc.Width, (float)texDesc.Height,
                        (float)(g_SourceViewW * 2), (float)(g_SourceViewH * 2),
                        0.0f, 0.0f);

                    ID3D11ShaderResourceView* p3SRVs[] = { pCapturedSRV, g_pA4KSRV2 };
                    g_pContext->CSSetShader(p3, nullptr, 0);
                    g_pContext->CSSetShaderResources(0, 2, p3SRVs);
                    g_pContext->CSSetUnorderedAccessViews(0, 1, &g_pA4KUAV2x, nullptr);
                    g_pContext->Dispatch((g_SourceViewW + 15) / 16, (g_SourceViewH + 15) / 16, 1);
                    g_pContext->CSSetShaderResources(0, 2, nullSRV);
                    g_pContext->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);

                    // Pass 4: Final Catmull-Rom Resize to Viewport
                    UpdateScalerCB(
                        (float)(g_SourceViewW * 2), (float)(g_SourceViewH * 2),
                        0.0f, 0.0f,
                        (float)(g_A4KAllocW * 2), (float)(g_A4KAllocH * 2),
                        (float)g_VpW, (float)g_VpH,
                        (float)g_VpX, (float)g_VpY);

                    g_pContext->CSSetShader(g_pAnime4K_Final, nullptr, 0);
                    g_pContext->CSSetShaderResources(0, 1, &g_pA4KSRV2x);
                    g_pContext->CSSetUnorderedAccessViews(0, 1, &g_pBackBufferUAV, nullptr);
                    g_pContext->Dispatch((g_VpW + 15) / 16, (g_VpH + 15) / 16, 1);
                    g_pContext->CSSetShaderResources(0, 1, nullSRV);
                    g_pContext->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);
                }
            }
            break;
        }
        }

        g_pContext->CSSetShader(nullptr, nullptr, 0);

        // Present with 1 VSync interval
        hr = g_pSwapChain->Present(1, 0);
        if (FAILED(hr))
        {
            ScalerLog("Present failed: 0x%08X\n", hr);
        }
        else
        {
            static bool s_FirstFrameLogged = false;
            if (!s_FirstFrameLogged)
            {
                ScalerLog("First frame presented successfully! Filter=%d (Format=%d, Viewport=%dx%d)\n",
                    g_FilterId, (int)g_A4KFormat, g_VpW, g_VpH);
                s_FirstFrameLogged = true;
            }
        }

        pCapturedSRV->Release();
    }

    capturedTexture->Release();
    frame->Release();
}

// Unified Shutdown function for all exit/error paths
void ShutdownScaler()
{
    g_ShuttingDown.store(true, std::memory_order_release);

    ScalerLog("ShutdownScaler starting...\n");

    // 1. Unhook focus, minimize, and location hooks
    if (g_hForegroundHook)
    {
        UnhookWinEvent(g_hForegroundHook);
        g_hForegroundHook = NULL;
    }
    if (g_hMinimizeHook)
    {
        UnhookWinEvent(g_hMinimizeHook);
        g_hMinimizeHook = NULL;
    }
    if (g_hLocationHook)
    {
        UnhookWinEvent(g_hLocationHook);
        g_hLocationHook = NULL;
    }

    // 2. Unsubscribe FrameArrived
    if (g_pFramePool && g_FrameArrivedSubscribed)
    {
        g_pFramePool->remove_FrameArrived(g_FrameArrivedToken);
        g_FrameArrivedSubscribed = false;
        g_FrameArrivedToken.value = 0;
    }

    // 3. Close GraphicsCaptureSession
    if (g_pCaptureItem)
    {
        if (g_CaptureItemClosedSubscribed)
        {
            g_pCaptureItem->remove_Closed(g_CaptureItemClosedToken);
            g_CaptureItemClosedSubscribed = false;
            g_CaptureItemClosedToken.value = 0;
        }
        g_pCaptureItem->Release();
        g_pCaptureItem = nullptr;
    }

    if (g_pSession)
    {
        ABI::Windows::Foundation::IClosable* pClosable = nullptr;
        if (SUCCEEDED(g_pSession->QueryInterface(IID_PPV_ARGS(&pClosable))))
        {
            pClosable->Close();
            pClosable->Release();
        }
        g_pSession->Release();
        g_pSession = nullptr;
    }

    // 4. Close Direct3D11CaptureFramePool
    if (g_pFramePool)
    {
        ABI::Windows::Foundation::IClosable* pClosable = nullptr;
        if (SUCCEEDED(g_pFramePool->QueryInterface(IID_PPV_ARGS(&pClosable))))
        {
            pClosable->Close();
            pClosable->Release();
        }
        g_pFramePool->Release();
        g_pFramePool = nullptr;
    }

    // 5. Remove UI property from target window and restore corner preference
    if (g_TargetHWnd && IsWindow(g_TargetHWnd))
    {
        RemovePropW(g_TargetHWnd, MT_PROP_SCALED);

        if (g_HasTargetCornerPref)
        {
            DWORD actualPid = 0;
            if (GetWindowThreadProcessId(g_TargetHWnd, &actualPid) && actualPid == g_TargetPid)
            {
                DwmSetWindowAttribute(g_TargetHWnd, DWMWA_WINDOW_CORNER_PREFERENCE, &g_PrevTargetCornerPref, sizeof(g_PrevTargetCornerPref));
            }
            g_HasTargetCornerPref = false;
        }
    }

    // 6. Release intermediate resources
    if (g_pFsrIntermediateSRV) { g_pFsrIntermediateSRV->Release(); g_pFsrIntermediateSRV = nullptr; }
    if (g_pFsrIntermediateUAV) { g_pFsrIntermediateUAV->Release(); g_pFsrIntermediateUAV = nullptr; }
    if (g_pFsrIntermediateTex) { g_pFsrIntermediateTex->Release(); g_pFsrIntermediateTex = nullptr; }

    if (g_pA4KSRV1) { g_pA4KSRV1->Release(); g_pA4KSRV1 = nullptr; }
    if (g_pA4KUAV1) { g_pA4KUAV1->Release(); g_pA4KUAV1 = nullptr; }
    if (g_pA4KTex1) { g_pA4KTex1->Release(); g_pA4KTex1 = nullptr; }

    if (g_pA4KSRV2) { g_pA4KSRV2->Release(); g_pA4KSRV2 = nullptr; }
    if (g_pA4KUAV2) { g_pA4KUAV2->Release(); g_pA4KUAV2 = nullptr; }
    if (g_pA4KTex2) { g_pA4KTex2->Release(); g_pA4KTex2 = nullptr; }

    if (g_pA4KSRV2x) { g_pA4KSRV2x->Release(); g_pA4KSRV2x = nullptr; }
    if (g_pA4KUAV2x) { g_pA4KUAV2x->Release(); g_pA4KUAV2x = nullptr; }
    if (g_pA4KTex2x) { g_pA4KTex2x->Release(); g_pA4KTex2x = nullptr; }

    // 7. Release Compute Shaders
    if (g_pNearestCS) { g_pNearestCS->Release(); g_pNearestCS = nullptr; }
    if (g_pBicubicCS) { g_pBicubicCS->Release(); g_pBicubicCS = nullptr; }
    if (g_pLanczosCS) { g_pLanczosCS->Release(); g_pLanczosCS = nullptr; }
    if (g_pFsrEasuCS) { g_pFsrEasuCS->Release(); g_pFsrEasuCS = nullptr; }
    if (g_pFsrRcasCS) { g_pFsrRcasCS->Release(); g_pFsrRcasCS = nullptr; }
    if (g_pAnime4K_3D_P1) { g_pAnime4K_3D_P1->Release(); g_pAnime4K_3D_P1 = nullptr; }
    if (g_pAnime4K_3D_P2) { g_pAnime4K_3D_P2->Release(); g_pAnime4K_3D_P2 = nullptr; }
    if (g_pAnime4K_3D_P3) { g_pAnime4K_3D_P3->Release(); g_pAnime4K_3D_P3 = nullptr; }
    if (g_pAnime4K_3D_AA_P1) { g_pAnime4K_3D_AA_P1->Release(); g_pAnime4K_3D_AA_P1 = nullptr; }
    if (g_pAnime4K_3D_AA_P2) { g_pAnime4K_3D_AA_P2->Release(); g_pAnime4K_3D_AA_P2 = nullptr; }
    if (g_pAnime4K_3D_AA_P3) { g_pAnime4K_3D_AA_P3->Release(); g_pAnime4K_3D_AA_P3 = nullptr; }
    if (g_pAnime4K_Final) { g_pAnime4K_Final->Release(); g_pAnime4K_Final = nullptr; }

    // 8. Release WinRT device & D3D11 core resources
    if (g_pDirect3DDevice)
    {
        g_pDirect3DDevice->Release();
        g_pDirect3DDevice = nullptr;
    }
    if (g_pPointSampler) { g_pPointSampler->Release(); g_pPointSampler = nullptr; }
    if (g_pLinearSampler) { g_pLinearSampler->Release(); g_pLinearSampler = nullptr; }
    if (g_pConstantBuffer) { g_pConstantBuffer->Release(); g_pConstantBuffer = nullptr; }
    if (g_pBackBufferUAV) { g_pBackBufferUAV->Release(); g_pBackBufferUAV = nullptr; }
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pContext)
    {
        g_pContext->ClearState();
        g_pContext->Release();
        g_pContext = nullptr;
    }
    if (g_pDevice) { g_pDevice->Release(); g_pDevice = nullptr; }

    // 9. Destroy Overlay Window
    if (g_ScalerHWnd)
    {
        DestroyWindow(g_ScalerHWnd);
        g_ScalerHWnd = NULL;
    }

    // 10. Close Sync Objects
    if (g_hFrameEvent) { CloseHandle(g_hFrameEvent); g_hFrameEvent = NULL; }
    if (g_hExitEvent) { CloseHandle(g_hExitEvent); g_hExitEvent = NULL; }
    if (g_hTargetProcess) { CloseHandle(g_hTargetProcess); g_hTargetProcess = NULL; }
    if (g_hActiveMutex)
    {
        CloseHandle(g_hActiveMutex);
        g_hActiveMutex = NULL;
    }

    if (g_RoInitialized)
    {
        RoUninitialize();
        g_RoInitialized = false;
    }

    ScalerLog("ShutdownScaler complete.\n");
}

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow)
{
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(nCmdShow);

    // Enable Per-Monitor V2 DPI awareness if supported
    typedef BOOL (WINAPI *pfn_SetProcessDpiAwarenessContext)(HANDLE);
    auto pfnSetDpiAwareness = (pfn_SetProcessDpiAwarenessContext)GetProcAddress(
        GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext");
    if (pfnSetDpiAwareness)
    {
        // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 = ((HANDLE)-4)
        pfnSetDpiAwareness((HANDLE)-4);
    }

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(lpCmdLine, &argc);
    if (!argv || argc < 4)
    {
        if (argv) LocalFree(argv);
        return 1;
    }

    g_TargetHWnd = (HWND)(uintptr_t)wcstoull(argv[0], nullptr, 16);
    g_TargetPid = (DWORD)wcstoul(argv[1], nullptr, 10);
    g_FilterId = _wtoi(argv[2]);
    g_PreserveAspect = (_wtoi(argv[3]) != 0);
    LocalFree(argv);

    ScalerLog("Starting Scaler: HWND=%p, PID=%lu, Filter=%d, PreserveAspect=%d\n",
        g_TargetHWnd, g_TargetPid, g_FilterId, (int)g_PreserveAspect);

    if (!IsWindow(g_TargetHWnd))
    {
        ScalerLog("Invalid target HWND\n");
        return 1;
    }

    // Verify PID to prevent HWND reuse race condition
    DWORD actualPid = 0;
    GetWindowThreadProcessId(g_TargetHWnd, &actualPid);
    if (actualPid != g_TargetPid)
    {
        ScalerLog("PID mismatch: expected %lu, got %lu\n", g_TargetPid, actualPid);
        return 1;
    }

    // Open target process handle for exit wait & synchronization
    g_hTargetProcess = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, g_TargetPid);
    if (!g_hTargetProcess)
    {
        ScalerLog("OpenProcess for target PID %lu failed: %lu\n", g_TargetPid, GetLastError());
    }

    // Integrity Level Check (UIPI)
    if (!CheckIntegrityLevel(g_TargetPid))
    {
        MessageBoxW(NULL, L"Cannot scale a target window with a higher Integrity Level (Administrator).", L"MenuTools Scaler", MB_ICONWARNING | MB_OK);
        return 1;
    }

    // Save and configure target window corner preference BEFORE querying geometry / capture item size
    DWORD prevCorner = DWMWCP_DEFAULT;
    HRESULT hrCorner = DwmGetWindowAttribute(g_TargetHWnd, DWMWA_WINDOW_CORNER_PREFERENCE, &prevCorner, sizeof(prevCorner));
    if (SUCCEEDED(hrCorner))
    {
        g_PrevTargetCornerPref = prevCorner;
    }
    else
    {
        g_PrevTargetCornerPref = DWMWCP_DEFAULT;
    }
    g_HasTargetCornerPref = true;

    DWORD noRound = DWMWCP_DONOTROUND;
    DwmSetWindowAttribute(g_TargetHWnd, DWMWA_WINDOW_CORNER_PREFERENCE, &noRound, sizeof(noRound));

    // Mutex as existence-marker: Scaler owns the mutex throughout execution
    g_hActiveMutex = CreateMutexW(nullptr, FALSE, MT_SCALER_MUTEX_NAME);
    if (!g_hActiveMutex)
    {
        ScalerLog("Failed to create active mutex: %lu\n", GetLastError());
        return ERROR_CREATE_FAILED;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        ScalerLog("Scaler already active (ERROR_ALREADY_EXISTS)\n");
        CloseHandle(g_hActiveMutex);
        g_hActiveMutex = NULL;
        return ERROR_ALREADY_EXISTS;
    }

    // Exit signal event (created by scaler, controller only opens and signals)
    g_hExitEvent = CreateEventW(nullptr, TRUE, FALSE, MT_SCALER_EXIT_EVENT_NAME);
    if (!g_hExitEvent)
    {
        ScalerLog("Failed to create exit event: %lu\n", GetLastError());
        ShutdownScaler();
        return 1;
    }
    ResetEvent(g_hExitEvent);

    // Frame synchronization event
    g_hFrameEvent = CreateEventW(nullptr, FALSE, FALSE, NULL);
    if (!g_hFrameEvent)
    {
        ScalerLog("Failed to create frame event: %lu\n", GetLastError());
        ShutdownScaler();
        return 1;
    }

    // Initialize COM / WinRT
    HRESULT hr = RoInitialize(RO_INIT_MULTITHREADED);
    if (FAILED(hr))
    {
        ScalerLog("RoInitialize failed: 0x%08X\n", hr);
        ShutdownScaler();
        return 1;
    }
    g_RoInitialized = true;

    // Get monitor of target window
    HMONITOR hMon = MonitorFromWindow(g_TargetHWnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(hMon, &mi);
    int screenW = mi.rcMonitor.right - mi.rcMonitor.left;
    int screenH = mi.rcMonitor.bottom - mi.rcMonitor.top;

    // Register Scaler Window Class
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = ScalerWndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = L"MenuToolsScalerWindowClass";
    RegisterClassExW(&wc);

    // Create Borderless Fullscreen Overlay Window
    g_ScalerHWnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        wc.lpszClassName,
        L"MenuTools Scaler",
        WS_POPUP | WS_VISIBLE,
        mi.rcMonitor.left, mi.rcMonitor.top,
        screenW, screenH,
        NULL, NULL, hInstance, NULL);

    if (!g_ScalerHWnd)
    {
        ScalerLog("Failed to create scaler window\n");
        ShutdownScaler();
        return 1;
    }

    // Configure overlay window: do not round corners and remove any DWM border
    DWORD overlayNoRound = DWMWCP_DONOTROUND;
    DwmSetWindowAttribute(g_ScalerHWnd, DWMWA_WINDOW_CORNER_PREFERENCE, &overlayNoRound, sizeof(overlayNoRound));
    COLORREF noBorderColor = DWMWA_COLOR_NONE;
    DwmSetWindowAttribute(g_ScalerHWnd, DWMWA_BORDER_COLOR, &noBorderColor, sizeof(noBorderColor));

    // Initialize Direct3D 11 (try debug layer first, fallback to standard)
    D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL fl;
    UINT devFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        devFlags | D3D11_CREATE_DEVICE_DEBUG,
        featureLevels, 1,
        D3D11_SDK_VERSION,
        &g_pDevice,
        &fl,
        &g_pContext);

    if (FAILED(hr))
    {
        hr = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            devFlags,
            featureLevels, 1,
            D3D11_SDK_VERSION,
            &g_pDevice,
            &fl,
            &g_pContext);
    }

    if (FAILED(hr) || !g_pDevice)
    {
        ScalerLog("D3D11CreateDevice failed: 0x%08X\n", hr);
        ShutdownScaler();
        return 1;
    }

    // Query DXGI Factory to create Flip-Discard SwapChain
    IDXGIDevice2* dxgiDevice = nullptr;
    g_pDevice->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
    IDXGIAdapter* adapter = nullptr;
    dxgiDevice->GetAdapter(&adapter);
    IDXGIFactory2* factory = nullptr;
    adapter->GetParent(IID_PPV_ARGS(&factory));

    DXGI_SWAP_CHAIN_DESC1 scDesc = {};
    scDesc.Width = screenW;
    scDesc.Height = screenH;
    scDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scDesc.SampleDesc.Count = 1;
    scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT | DXGI_USAGE_UNORDERED_ACCESS;
    scDesc.BufferCount = 2;
    scDesc.Scaling = DXGI_SCALING_NONE;
    scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    hr = factory->CreateSwapChainForHwnd(g_pDevice, g_ScalerHWnd, &scDesc, nullptr, nullptr, &g_pSwapChain);
    factory->Release();
    adapter->Release();

    if (FAILED(hr) || !g_pSwapChain)
    {
        ScalerLog("CreateSwapChainForHwnd failed: 0x%08X\n", hr);
        dxgiDevice->Release();
        ShutdownScaler();
        return 1;
    }

    // Create UAV for swapchain backbuffer
    ID3D11Texture2D* pBackBuffer = nullptr;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    hr = g_pDevice->CreateUnorderedAccessView(pBackBuffer, nullptr, &g_pBackBufferUAV);
    pBackBuffer->Release();

    if (FAILED(hr) || !g_pBackBufferUAV)
    {
        ScalerLog("CreateUnorderedAccessView on BackBuffer failed: 0x%08X\n", hr);
        dxgiDevice->Release();
        ShutdownScaler();
        return 1;
    }

    // Create Constant Buffer (48 bytes, multiple of 16)
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = sizeof(ScalerCBData);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = g_pDevice->CreateBuffer(&bd, nullptr, &g_pConstantBuffer);
    if (FAILED(hr) || !g_pConstantBuffer)
    {
        ScalerLog("CreateBuffer for ConstantBuffer failed: 0x%08X\n", hr);
        dxgiDevice->Release();
        ShutdownScaler();
        return 1;
    }

    // Create Samplers
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    g_pDevice->CreateSamplerState(&sd, &g_pPointSampler);

    sd.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    g_pDevice->CreateSamplerState(&sd, &g_pLinearSampler);

    // Create Compute Shaders from precompiled headers
    g_pDevice->CreateComputeShader(g_Nearest_CS, g_Nearest_CS_size, nullptr, &g_pNearestCS);
    g_pDevice->CreateComputeShader(g_Bicubic_CS, g_Bicubic_CS_size, nullptr, &g_pBicubicCS);
    g_pDevice->CreateComputeShader(g_Lanczos_CS, g_Lanczos_CS_size, nullptr, &g_pLanczosCS);
    g_pDevice->CreateComputeShader(g_FsrEasu_CS, g_FsrEasu_CS_size, nullptr, &g_pFsrEasuCS);
    g_pDevice->CreateComputeShader(g_FsrRcas_CS, g_FsrRcas_CS_size, nullptr, &g_pFsrRcasCS);
    g_pDevice->CreateComputeShader(g_Anime4K_3D_Pass1_CS, g_Anime4K_3D_Pass1_CS_size, nullptr, &g_pAnime4K_3D_P1);
    g_pDevice->CreateComputeShader(g_Anime4K_3D_Pass2_CS, g_Anime4K_3D_Pass2_CS_size, nullptr, &g_pAnime4K_3D_P2);
    g_pDevice->CreateComputeShader(g_Anime4K_3D_Pass3_CS, g_Anime4K_3D_Pass3_CS_size, nullptr, &g_pAnime4K_3D_P3);
    g_pDevice->CreateComputeShader(g_Anime4K_3D_AA_Pass1_CS, g_Anime4K_3D_AA_Pass1_CS_size, nullptr, &g_pAnime4K_3D_AA_P1);
    g_pDevice->CreateComputeShader(g_Anime4K_3D_AA_Pass2_CS, g_Anime4K_3D_AA_Pass2_CS_size, nullptr, &g_pAnime4K_3D_AA_P2);
    g_pDevice->CreateComputeShader(g_Anime4K_3D_AA_Pass3_CS, g_Anime4K_3D_AA_Pass3_CS_size, nullptr, &g_pAnime4K_3D_AA_P3);
    g_pDevice->CreateComputeShader(g_Anime4K_Final_CS, g_Anime4K_Final_CS_size, nullptr, &g_pAnime4K_Final);

    // Validate shaders for the chosen filter
    bool shadersValid = false;
    switch (g_FilterId)
    {
    case MT_SCALER_FILTER_NEAREST:
    case MT_SCALER_FILTER_PIXEL_PERFECT:
        shadersValid = (g_pNearestCS != nullptr);
        break;
    case MT_SCALER_FILTER_BICUBIC:
        shadersValid = (g_pBicubicCS != nullptr);
        break;
    case MT_SCALER_FILTER_LANCZOS:
        shadersValid = (g_pLanczosCS != nullptr);
        break;
    case MT_SCALER_FILTER_FSR:
        shadersValid = (g_pFsrEasuCS != nullptr && g_pFsrRcasCS != nullptr);
        break;
    case MT_SCALER_FILTER_ANIME4K_3D:
        shadersValid = (g_pAnime4K_3D_P1 && g_pAnime4K_3D_P2 && g_pAnime4K_3D_P3 && g_pAnime4K_Final);
        break;
    case MT_SCALER_FILTER_ANIME4K_3D_AA:
        shadersValid = (g_pAnime4K_3D_AA_P1 && g_pAnime4K_3D_AA_P2 && g_pAnime4K_3D_AA_P3 && g_pAnime4K_Final);
        break;
    }

    if (!shadersValid)
    {
        ScalerLog("Compute shaders for filter %d failed to compile/create!\n", g_FilterId);
        dxgiDevice->Release();
        ShutdownScaler();
        return 1;
    }

    // Wrap Direct3D 11 device into WinRT IDirect3DDevice
    IInspectable* inspectableDevice = nullptr;
    HMODULE hD3D11 = GetModuleHandleW(L"d3d11.dll");
    typedef HRESULT (WINAPI *pfn_CreateDirect3D11DeviceFromDXGIDevice)(IDXGIDevice*, IInspectable**);
    auto pfnCreateD3DDevice = (pfn_CreateDirect3D11DeviceFromDXGIDevice)GetProcAddress(hD3D11, "CreateDirect3D11DeviceFromDXGIDevice");
    pfnCreateD3DDevice(dxgiDevice, &inspectableDevice);
    dxgiDevice->Release();

    g_pDirect3DDevice = (ABI::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice*)inspectableDevice;
    if (!g_pDirect3DDevice)
    {
        ScalerLog("CreateDirect3D11DeviceFromDXGIDevice failed\n");
        ShutdownScaler();
        return 1;
    }

    // Create GraphicsCaptureItem for target window
    HSTRING hstrItem;
    WindowsCreateString(L"Windows.Graphics.Capture.GraphicsCaptureItem", 44, &hstrItem);
    IGraphicsCaptureItemInterop* itemInterop = nullptr;
    RoGetActivationFactory(hstrItem, IID_PPV_ARGS(&itemInterop));
    WindowsDeleteString(hstrItem);

    itemInterop->CreateForWindow(g_TargetHWnd, IID_PPV_ARGS(&g_pCaptureItem));
    itemInterop->Release();

    if (!g_pCaptureItem)
    {
        ScalerLog("CreateForWindow failed\n");
        ShutdownScaler();
        return 1;
    }

    // Subscribe to CaptureItem Closed event
    auto closedHandler = new CaptureItemClosedHandler();
    hr = g_pCaptureItem->add_Closed(closedHandler, &g_CaptureItemClosedToken);
    closedHandler->Release();
    if (SUCCEEDED(hr))
    {
        g_CaptureItemClosedSubscribed = true;
    }

    g_pCaptureItem->get_Size(&g_CaptureItemSize);
    g_CachedContentSize = g_CaptureItemSize;

    // Compute geometry and preallocate intermediate textures BEFORE WGC session
    UpdateGeometry(screenW, screenH, g_CaptureItemSize.Width, g_CaptureItemSize.Height);

    if (g_FilterId == MT_SCALER_FILTER_FSR)
    {
        if (!EnsureFSRIntermediate((UINT)g_VpW, (UINT)g_VpH))
        {
            ScalerLog("Preallocation of FSR intermediate textures failed!\n");
            ShutdownScaler();
            return 1;
        }
    }
    else if (g_FilterId == MT_SCALER_FILTER_ANIME4K_3D || g_FilterId == MT_SCALER_FILTER_ANIME4K_3D_AA)
    {
        if (!EnsureAnime4KIntermediates((UINT)g_SourceViewW, (UINT)g_SourceViewH))
        {
            ScalerLog("Preallocation of Anime4K intermediate textures failed!\n");
            ShutdownScaler();
            return 1;
        }
    }

    // Create Direct3D11CaptureFramePool
    HSTRING hstrPool;
    WindowsCreateString(L"Windows.Graphics.Capture.Direct3D11CaptureFramePool", 51, &hstrPool);
    ABI::Windows::Graphics::Capture::IDirect3D11CaptureFramePoolStatics2* poolStatics2 = nullptr;
    RoGetActivationFactory(hstrPool, IID_PPV_ARGS(&poolStatics2));
    WindowsDeleteString(hstrPool);

    if (poolStatics2)
    {
        poolStatics2->CreateFreeThreaded(
            g_pDirect3DDevice,
            ABI::Windows::Graphics::DirectX::DirectXPixelFormat_B8G8R8A8UIntNormalized,
            2,
            g_CaptureItemSize,
            &g_pFramePool);
        poolStatics2->Release();
    }

    if (!g_pFramePool)
    {
        ScalerLog("CreateFreeThreaded failed\n");
        ShutdownScaler();
        return 1;
    }

    // Register FrameArrived Callback
    auto handler = new FrameArrivedHandler();
    hr = g_pFramePool->add_FrameArrived(handler, &g_FrameArrivedToken);
    handler->Release();
    if (FAILED(hr))
    {
        ScalerLog("add_FrameArrived failed: 0x%08X\n", hr);
        ShutdownScaler();
        return 1;
    }
    g_FrameArrivedSubscribed = true;

    // Create Capture Session
    hr = g_pFramePool->CreateCaptureSession(g_pCaptureItem, &g_pSession);

    if (FAILED(hr) || !g_pSession)
    {
        ScalerLog("CreateCaptureSession failed: 0x%08X\n", hr);
        ShutdownScaler();
        return 1;
    }

    // Disable cursor in capture stream if supported (Windows 10 2004+)
    ABI::Windows::Graphics::Capture::IGraphicsCaptureSession2* session2 = nullptr;
    if (SUCCEEDED(g_pSession->QueryInterface(IID_PPV_ARGS(&session2))))
    {
        session2->put_IsCursorCaptureEnabled(false);
        session2->Release();
    }

    // Hook focus loss / Alt+Tab and target window minimization via targeted point hooks
    g_hForegroundHook = SetWinEventHook(
        EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
        NULL, FocusWinEventProc,
        0, 0, WINEVENT_OUTOFCONTEXT);

    g_hMinimizeHook = SetWinEventHook(
        EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZESTART,
        NULL, FocusWinEventProc,
        0, 0, WINEVENT_OUTOFCONTEXT);

    // Hook target window location changes (moves, resizes) specifically for g_TargetPid
    g_hLocationHook = SetWinEventHook(
        EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE,
        NULL, LocationWinEventProc,
        g_TargetPid, 0, WINEVENT_OUTOFCONTEXT);

    // Mark target as scaled (UI marker only)
    SetPropW(g_TargetHWnd, MT_PROP_SCALED, (HANDLE)1);

    // ALL D3D RESOURCES & SHADERS VERIFIED: NOW SAFELY START CAPTURE
    hr = g_pSession->StartCapture();
    if (FAILED(hr))
    {
        ScalerLog("StartCapture failed: 0x%08X\n", hr);
        ShutdownScaler();
        return 1;
    }

    ScalerLog("Scaler initialized successfully and capture active.\n");

    // Main Message & Render Loop
    HANDLE waitHandles[3] = { g_hFrameEvent, g_hExitEvent, NULL };
    DWORD handleCount = 2;
    if (g_hTargetProcess)
    {
        waitHandles[2] = g_hTargetProcess;
        handleCount = 3;
    }

    while (g_Running && !g_ShuttingDown.load(std::memory_order_acquire))
    {
        DWORD waitRes = MsgWaitForMultipleObjectsEx(handleCount, waitHandles, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);

        if (waitRes == WAIT_OBJECT_0)
        {
            // Frame arrived (or location change triggered)
            RenderFrame();
        }
        else if (waitRes == WAIT_OBJECT_0 + 1)
        {
            ScalerLog("Exit event signaled, terminating loop.\n");
            break;
        }
        else if (handleCount == 3 && waitRes == WAIT_OBJECT_0 + 2)
        {
            ScalerLog("Target process terminated, terminating loop.\n");
            break;
        }

        // Target alive & state checks
        if (!IsWindow(g_TargetHWnd))
        {
            ScalerLog("Target HWND no longer exists, terminating loop.\n");
            break;
        }

        DWORD currentPid = 0;
        GetWindowThreadProcessId(g_TargetHWnd, &currentPid);
        if (currentPid != g_TargetPid)
        {
            ScalerLog("Target PID changed (HWND reuse detected), terminating loop.\n");
            break;
        }

        if (IsIconic(g_TargetHWnd))
        {
            ScalerLog("Target window minimized (IsIconic), terminating loop.\n");
            break;
        }

        // Process Window Messages (mouse forward, quit, etc.)
        MSG msg;
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
            {
                g_Running = false;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    ShutdownScaler();
    return 0;
}
