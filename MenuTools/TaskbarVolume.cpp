#include "stdafx.h"
#include "TaskbarVolume.h"

#include <uiautomation.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <tlhelp32.h>
#include <appmodel.h>
#include <windowsx.h>

#include <string>
#include <vector>
#include <algorithm>
#include <atomic>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

namespace
{
#ifdef _DEBUG
	void DebugLog(const WCHAR* fmt, ...)
	{
		WCHAR buf[512];
		va_list args;
		va_start(args, fmt);
		_vsnwprintf_s(buf, 512, _TRUNCATE, fmt, args);
		va_end(args);

		OutputDebugStringW(buf);
	}
#else
	inline void DebugLog(const WCHAR*, ...) {}
#endif
	// GUIDs for Core Audio
	const GUID XIID_IMMDeviceEnumerator = {
		0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
	const GUID XIID_MMDeviceEnumerator = {
		0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
	const GUID XIID_IAudioSessionManager2 = {
		0x77AA99A0, 0x1BD6, 0x484F, {0x8B, 0xC7, 0x2C, 0x65, 0x4C, 0x9A, 0x9B, 0x6F}};

	// Ring buffer for mouse events
	struct VolumeEvent
	{
		POINT pt{ 0, 0 };
		short delta{ 0 };
		BYTE flags{ 0 }; // 0x01 = isShift
	};

	constexpr UINT kEventRingBufferSize = 8;
	VolumeEvent g_eventRing[kEventRingBufferSize];
	std::atomic<UINT> g_eventWriteIndex{ 0 };

	// Hooks and handles
	HHOOK g_hMouseHook = NULL;
	HWND g_hMainWnd = NULL;
	HWND g_hOsdWnd = NULL;
	HFONT g_hOsdFont = NULL;
	WCHAR g_szOsdText[32] = { 0 };

	// Taskbar hit-test cache
	RECT g_rcTaskbar = { 0 };
	ULONGLONG g_lastTaskbarRectCheck = 0;

	// Taskbar button cache (TTL: 2500 ms)
	struct CachedTaskbarItem
	{
		RECT rect{ 0, 0, 0, 0 };
		std::wstring autoId;
		std::wstring name;
		ULONGLONG timestamp{ 0 };
	};
	CachedTaskbarItem g_cachedItem;

	// Lazy COM interfaces
	IUIAutomation* g_pAutomation = NULL;
	IMMDeviceEnumerator* g_pDeviceEnumerator = NULL;

	ULONGLONG MyGetTickCount64()
	{
		typedef ULONGLONG (WINAPI *GetTickCount64_t)(VOID);
		static auto pfn = (GetTickCount64_t)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetTickCount64");
		if (pfn) return pfn();
		return GetTickCount();
	}

	BOOL MyQueryFullProcessImageNameW(HANDLE hProcess, DWORD dwFlags, LPWSTR lpExeName, PDWORD lpdwSize)
	{
		typedef BOOL (WINAPI *QueryFullProcessImageNameW_t)(HANDLE, DWORD, LPWSTR, PDWORD);
		static auto pfn = (QueryFullProcessImageNameW_t)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "QueryFullProcessImageNameW");
		if (pfn) return pfn(hProcess, dwFlags, lpExeName, lpdwSize);
		return FALSE;
	}

	// Case-insensitive string search helper
	bool ContainsIgnoreCase(const std::wstring& str, const std::wstring& substr)
	{
		if (substr.empty() || str.empty())
		{
			return false;
		}

		auto it = std::search(
			str.begin(), str.end(),
			substr.begin(), substr.end(),
			[](wchar_t ch1, wchar_t ch2) {
				return towlower(ch1) == towlower(ch2);
			}
		);
		return it != str.end();
	}

	// Helper to extract base name from full executable path without extension
	std::wstring GetExeBaseName(const std::wstring& fullPath)
	{
		size_t slash = fullPath.find_last_of(L"\\/");
		std::wstring filename = (slash != std::wstring::npos) ? fullPath.substr(slash + 1) : fullPath;
		size_t dot = filename.find_last_of(L'.');
		if (dot != std::wstring::npos)
		{
			filename = filename.substr(0, dot);
		}
		return filename;
	}

	// Helper to find parent PID using Toolhelp32
	DWORD GetParentProcessId(DWORD pid)
	{
		DWORD ppid = 0;
		HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
		if (hSnap != INVALID_HANDLE_VALUE)
		{
			PROCESSENTRY32W pe;
			pe.dwSize = sizeof(pe);
			if (Process32FirstW(hSnap, &pe))
			{
				do
				{
					if (pe.th32ProcessID == pid)
					{
						ppid = pe.th32ParentProcessID;
						break;
					}
				} while (Process32NextW(hSnap, &pe));
			}
			CloseHandle(hSnap);
		}
		return ppid;
	}

	// OSD Window Procedure
	constexpr UINT_PTR TIMER_OSD_HIDE = 1;

	LRESULT CALLBACK OsdWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
	{
		switch (uMsg)
		{
		case WM_PAINT:
		{
			PAINTSTRUCT ps;
			HDC hdc = BeginPaint(hWnd, &ps);

			RECT rc;
			GetClientRect(hWnd, &rc);

			// Background: Dark Gray (RGB 30, 30, 30)
			HBRUSH hBgBrush = CreateSolidBrush(RGB(30, 30, 30));
			FillRect(hdc, &rc, hBgBrush);
			DeleteObject(hBgBrush);

			// Border: Subtle 1px (RGB 65, 65, 65)
			HPEN hPen = CreatePen(PS_SOLID, 1, RGB(65, 65, 65));
			HPEN hOldPen = (HPEN)SelectObject(hdc, hPen);
			HBRUSH hOldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));
			Rectangle(hdc, rc.left, rc.top, rc.right, rc.bottom);
			SelectObject(hdc, hOldBrush);
			SelectObject(hdc, hOldPen);
			DeleteObject(hPen);

			// Text
			SetBkMode(hdc, TRANSPARENT);
			SetTextColor(hdc, RGB(245, 245, 245));
			HFONT hOldFont = (HFONT)SelectObject(hdc, g_hOsdFont);
			DrawTextW(hdc, g_szOsdText, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
			SelectObject(hdc, hOldFont);

			EndPaint(hWnd, &ps);
			return 0;
		}

		case WM_TIMER:
		{
			if (wParam == TIMER_OSD_HIDE)
			{
				KillTimer(hWnd, TIMER_OSD_HIDE);
				ShowWindow(hWnd, SW_HIDE);
				return 0;
			}
			break;
		}

		case WM_ERASEBKGND:
			return 1;
		}

		return DefWindowProcW(hWnd, uMsg, wParam, lParam);
	}

	VOID ShowOsd(const WCHAR* text, const RECT& buttonRect)
	{
		if (!g_hOsdWnd)
		{
			return;
		}

		wcsncpy_s(g_szOsdText, text, _TRUNCATE);

		const int osdWidth = 56;
		const int osdHeight = 26;

		int x = (buttonRect.left + buttonRect.right - osdWidth) / 2;
		int y = buttonRect.top - osdHeight - 6;

		// If taskbar is on top of screen, place OSD below it
		if (buttonRect.top < 50)
		{
			y = buttonRect.bottom + 6;
		}

		SetWindowPos(g_hOsdWnd, HWND_TOPMOST, x, y, osdWidth, osdHeight, SWP_NOACTIVATE | SWP_SHOWWINDOW);
		InvalidateRect(g_hOsdWnd, NULL, TRUE);

		// Reset hide timer to 800 ms
		SetTimer(g_hOsdWnd, TIMER_OSD_HIDE, 800, NULL);
	}

	// Taskbar hit-test check
	bool IsPointOverTaskbar(POINT pt)
	{
		ULONGLONG now = MyGetTickCount64();
		if (now - g_lastTaskbarRectCheck > 3000 || IsRectEmpty(&g_rcTaskbar))
		{
			HWND hTray = FindWindowW(L"Shell_TrayWnd", NULL);
			if (hTray)
			{
				GetWindowRect(hTray, &g_rcTaskbar);
				g_lastTaskbarRectCheck = now;
			}
		}

		if (PtInRect(&g_rcTaskbar, pt))
		{
			return true;
		}

		// Fallback for secondary taskbar on multi-monitor
		HWND hWindow = WindowFromPoint(pt);
		if (hWindow)
		{
			HWND hRoot = GetAncestor(hWindow, GA_ROOT);
			if (hRoot)
			{
				WCHAR szClass[64] = { 0 };
				if (GetClassNameW(hRoot, szClass, 64) > 0)
				{
					if (wcscmp(szClass, L"Shell_TrayWnd") == 0 || wcscmp(szClass, L"Shell_SecondaryTrayWnd") == 0)
					{
						return true;
					}
				}
			}
		}

		return false;
	}

	// Low-level mouse hook procedure
	LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam)
	{
		if (nCode != HC_ACTION)
		{
			return CallNextHookEx(g_hMouseHook, nCode, wParam, lParam);
		}

		if (wParam == WM_MOUSEWHEEL || wParam == WM_MBUTTONUP)
		{
			MSLLHOOKSTRUCT* pMouse = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
			POINT pt = pMouse->pt;

			if (IsPointOverTaskbar(pt))
			{
				DebugLog(L"[Hook] Over taskbar pt=(%ld,%ld) msg=0x%04X", pt.x, pt.y, (UINT)wParam);

				UINT idx = g_eventWriteIndex.fetch_add(1, std::memory_order_relaxed) % kEventRingBufferSize;
				g_eventRing[idx].pt = pt;

				if (wParam == WM_MOUSEWHEEL)
				{
					short delta = GET_WHEEL_DELTA_WPARAM(pMouse->mouseData);
					bool isShift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
					g_eventRing[idx].delta = delta;
					g_eventRing[idx].flags = isShift ? 1 : 0;
					PostMessageW(g_hMainWnd, WM_TASKBAR_VOLUME_WHEEL, static_cast<WPARAM>(idx), 0);
				}
				else if (wParam == WM_MBUTTONUP)
				{
					g_eventRing[idx].delta = 0;
					g_eventRing[idx].flags = 0;
					PostMessageW(g_hMainWnd, WM_TASKBAR_VOLUME_MUTE, static_cast<WPARAM>(idx), 0);
				}
			}
		}

		return CallNextHookEx(g_hMouseHook, nCode, wParam, lParam);
	}

	// Resolve taskbar item under cursor via UIA with short-lived caching
	bool ResolveTaskbarItem(POINT pt, std::wstring& outAutoId, std::wstring& outName, RECT& outRect)
	{
		ULONGLONG now = MyGetTickCount64();

		// Cache check
		if (now - g_cachedItem.timestamp < 2500 && PtInRect(&g_cachedItem.rect, pt))
		{
			if (!g_cachedItem.autoId.empty())
			{
				outAutoId = g_cachedItem.autoId;
				outName = g_cachedItem.name;
				outRect = g_cachedItem.rect;
				return true;
			}
			return false;
		}

		// Lazy initialize UI Automation
		if (!g_pAutomation)
		{
			HRESULT hr = CoCreateInstance(__uuidof(CUIAutomation), NULL, CLSCTX_INPROC_SERVER,
				__uuidof(IUIAutomation), (void**)&g_pAutomation);
			if (FAILED(hr) || !g_pAutomation)
			{
				DebugLog(L"[Resolver] CoCreateInstance CUIAutomation failed: 0x%08lX", hr);
				return false;
			}
			DebugLog(L"[Resolver] CUIAutomation created successfully: %p", g_pAutomation);
		}

		IUIAutomationElement* pElem = NULL;
		HRESULT hr = g_pAutomation->ElementFromPoint(pt, &pElem);
		if (FAILED(hr) || !pElem)
		{
			DebugLog(L"[Resolver] ElementFromPoint failed: hr=0x%08lX, pElem=%p", hr, pElem);
			return false;
		}

		// Check if we hit a taskbar button peer or a child of one
		BSTR bstrClass = NULL;
		pElem->get_CurrentClassName(&bstrClass);

		if (!bstrClass || wcscmp(bstrClass, L"Taskbar.TaskListButtonAutomationPeer") != 0)
		{
			// Walk up tree to find button peer
			IUIAutomationTreeWalker* pWalker = NULL;
			if (SUCCEEDED(g_pAutomation->get_ControlViewWalker(&pWalker)) && pWalker)
			{
				IUIAutomationElement* pCur = pElem;
				pCur->AddRef();

				for (int depth = 0; depth < 4; depth++)
				{
					IUIAutomationElement* pParent = NULL;
					if (FAILED(pWalker->GetParentElement(pCur, &pParent)) || !pParent)
					{
						break;
					}

					pCur->Release();
					pCur = pParent;

					BSTR parentClass = NULL;
					pCur->get_CurrentClassName(&parentClass);
					if (parentClass)
					{
						if (wcscmp(parentClass, L"Taskbar.TaskListButtonAutomationPeer") == 0)
						{
							SysFreeString(parentClass);
							pElem->Release();
							pElem = pCur;
							pCur = NULL;
							break;
						}
						SysFreeString(parentClass);
					}
				}

				if (pCur) pCur->Release();
				pWalker->Release();
			}
		}
		if (bstrClass) SysFreeString(bstrClass);

		BSTR bstrAutoId = NULL;
		BSTR bstrName = NULL;
		RECT bRect = { 0 };

		pElem->get_CurrentAutomationId(&bstrAutoId);
		pElem->get_CurrentName(&bstrName);
		pElem->get_CurrentBoundingRectangle(&bRect);
		pElem->Release();

		g_cachedItem.rect = bRect;
		g_cachedItem.timestamp = now;

		if (bstrAutoId && wcsncmp(bstrAutoId, L"Appid:", 6) == 0)
		{
			g_cachedItem.autoId = bstrAutoId;
			g_cachedItem.name = bstrName ? bstrName : L"";
			outAutoId = g_cachedItem.autoId;
			outName = g_cachedItem.name;
			outRect = bRect;

			if (bstrAutoId) SysFreeString(bstrAutoId);
			if (bstrName) SysFreeString(bstrName);
			return true;
		}

		g_cachedItem.autoId.clear();
		g_cachedItem.name.clear();

		if (bstrAutoId) SysFreeString(bstrAutoId);
		if (bstrName) SysFreeString(bstrName);
		return false;
	}

	// Audio matching hierarchy:
	// 1. AUMID
	// 2. Exact executable path
	// 3. Process ancestry (parent PID tree)
	// 4. Exe basename fallback
	bool DoesSessionMatchApp(DWORD sessionPid, const std::wstring& targetAutoId, const std::wstring& targetName)
	{
		if (sessionPid == 0)
		{
			return false;
		}

		HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, sessionPid);
		if (!hProc)
		{
			return false;
		}

		WCHAR szExePath[MAX_PATH] = { 0 };
		DWORD dwSize = MAX_PATH;
		MyQueryFullProcessImageNameW(hProc, 0, szExePath, &dwSize);

		WCHAR szAumid[256] = { 0 };
		UINT32 aumidLen = 256;
		typedef LONG (WINAPI *GetAppUserModelId_t)(HANDLE, UINT32*, PWSTR);
		static auto pfnGetAumid = (GetAppUserModelId_t)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetApplicationUserModelId");

		if (pfnGetAumid)
		{
			LONG aumidRes = pfnGetAumid(hProc, &aumidLen, szAumid);
			if (aumidRes != ERROR_SUCCESS)
			{
				szAumid[0] = 0;
			}
		}

		CloseHandle(hProc);

		// 1. AUMID match
		if (szAumid[0] != 0 && ContainsIgnoreCase(targetAutoId, szAumid))
		{
			return true;
		}

		// 2. Exact executable path match
		if (szExePath[0] != 0 && ContainsIgnoreCase(targetAutoId, szExePath))
		{
			return true;
		}

		std::wstring baseName = GetExeBaseName(szExePath);

		// 3. Process ancestry check (for utility/renderer audio subprocesses)
		DWORD parentPid = GetParentProcessId(sessionPid);
		if (parentPid != 0 && parentPid != sessionPid)
		{
			HANDLE hParent = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, parentPid);
			if (hParent)
			{
				WCHAR szParentPath[MAX_PATH] = { 0 };
				DWORD dwParentSize = MAX_PATH;
				MyQueryFullProcessImageNameW(hParent, 0, szParentPath, &dwParentSize);

				WCHAR szParentAumid[256] = { 0 };
				UINT32 parentAumidLen = 256;
				if (pfnGetAumid && pfnGetAumid(hParent, &parentAumidLen, szParentAumid) == ERROR_SUCCESS)
				{
					if (ContainsIgnoreCase(targetAutoId, szParentAumid))
					{
						CloseHandle(hParent);
						return true;
					}
				}

				if (szParentPath[0] != 0 && ContainsIgnoreCase(targetAutoId, szParentPath))
				{
					CloseHandle(hParent);
					return true;
				}

				std::wstring parentBase = GetExeBaseName(szParentPath);
				if (!parentBase.empty() && (ContainsIgnoreCase(targetAutoId, parentBase) || ContainsIgnoreCase(targetName, parentBase)))
				{
					CloseHandle(hParent);
					return true;
				}

				CloseHandle(hParent);
			}
		}

		// 4. Basename fallback
		if (!baseName.empty())
		{
			if (ContainsIgnoreCase(targetAutoId, baseName) || ContainsIgnoreCase(targetName, baseName))
			{
				return true;
			}
		}

		return false;
	}

	// Adjust volume or toggle mute for matching sessions on a device
	bool AdjustVolumeOnDevice(IMMDevice* pDevice, const std::wstring& targetAutoId, const std::wstring& targetName,
		bool isMuteToggle, float deltaStep, float& outNewVol, bool& outMuted)
	{
		IAudioSessionManager2* pMgr = NULL;
		HRESULT hr = pDevice->Activate(XIID_IAudioSessionManager2, CLSCTX_ALL, NULL, (void**)&pMgr);
		if (FAILED(hr) || !pMgr)
		{
			return false;
		}

		// Create session enumerator fresh on each check (Microsoft recommendation)
		IAudioSessionEnumerator* pEnum = NULL;
		hr = pMgr->GetSessionEnumerator(&pEnum);
		if (FAILED(hr) || !pEnum)
		{
			pMgr->Release();
			return false;
		}

		int count = 0;
		pEnum->GetCount(&count);
		bool foundMatch = false;

		for (int i = 0; i < count; i++)
		{
			IAudioSessionControl* pCtrl = NULL;
			if (SUCCEEDED(pEnum->GetSession(i, &pCtrl)) && pCtrl)
			{
				IAudioSessionControl2* pCtrl2 = NULL;
				pCtrl->QueryInterface(__uuidof(IAudioSessionControl2), (void**)&pCtrl2);

				if (pCtrl2)
				{
					if (pCtrl2->IsSystemSoundsSession() != S_OK)
					{
						DWORD pid = 0;
						pCtrl2->GetProcessId(&pid);

						if (DoesSessionMatchApp(pid, targetAutoId, targetName))
						{
							ISimpleAudioVolume* pVol = NULL;
							pCtrl->QueryInterface(__uuidof(ISimpleAudioVolume), (void**)&pVol);
							if (pVol)
							{
								if (isMuteToggle)
								{
									BOOL bCurrentMute = FALSE;
									pVol->GetMute(&bCurrentMute);
									BOOL bNewMute = !bCurrentMute;
									pVol->SetMute(bNewMute, NULL);
									pVol->GetMasterVolume(&outNewVol);
									outMuted = (bNewMute == TRUE);
									foundMatch = true;
								}
								else
								{
									float curVol = 0.0f;
									pVol->GetMasterVolume(&curVol);
									float newVol = curVol + deltaStep;
									if (newVol < 0.0f) newVol = 0.0f;
									if (newVol > 1.0f) newVol = 1.0f;

									pVol->SetMasterVolume(newVol, NULL);

									// Auto unmute when raising volume from zero
									BOOL bMuted = FALSE;
									pVol->GetMute(&bMuted);
									if (bMuted && newVol > 0.0f)
									{
										pVol->SetMute(FALSE, NULL);
										bMuted = FALSE;
									}

									outNewVol = newVol;
									outMuted = (bMuted == TRUE);
									foundMatch = true;
								}
								pVol->Release();
							}
						}
					}
					pCtrl2->Release();
				}
				pCtrl->Release();
			}
		}

		pEnum->Release();
		pMgr->Release();
		return foundMatch;
	}

	// Two-stage audio session resolution: default endpoint first, then all active endpoints
	bool AdjustAppVolume(const std::wstring& targetAutoId, const std::wstring& targetName,
		bool isMuteToggle, float deltaStep, float& outNewVol, bool& outMuted)
	{
		if (!g_pDeviceEnumerator)
		{
			HRESULT hr = CoCreateInstance(XIID_MMDeviceEnumerator, NULL, CLSCTX_INPROC_SERVER,
				XIID_IMMDeviceEnumerator, (void**)&g_pDeviceEnumerator);
			if (FAILED(hr) || !g_pDeviceEnumerator)
			{
				return false;
			}
		}

		// Stage 1: Check default render endpoint
		IMMDevice* pDefaultDevice = NULL;
		HRESULT hr = g_pDeviceEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pDefaultDevice);
		if (SUCCEEDED(hr) && pDefaultDevice)
		{
			bool matched = AdjustVolumeOnDevice(pDefaultDevice, targetAutoId, targetName,
				isMuteToggle, deltaStep, outNewVol, outMuted);
			pDefaultDevice->Release();
			if (matched)
			{
				return true;
			}
		}

		// Stage 2: Fallback to all active endpoints (e.g. Discord routed to headset)
		IMMDeviceCollection* pCollection = NULL;
		hr = g_pDeviceEnumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &pCollection);
		if (SUCCEEDED(hr) && pCollection)
		{
			UINT devCount = 0;
			pCollection->GetCount(&devCount);
			bool matched = false;

			for (UINT i = 0; i < devCount; i++)
			{
				IMMDevice* pDev = NULL;
				if (SUCCEEDED(pCollection->Item(i, &pDev)) && pDev)
				{
					if (AdjustVolumeOnDevice(pDev, targetAutoId, targetName, isMuteToggle, deltaStep, outNewVol, outMuted))
					{
						matched = true;
						pDev->Release();
						break;
					}
					pDev->Release();
				}
			}

			pCollection->Release();
			return matched;
		}

		return false;
	}
}

namespace TaskbarVolume
{
	BOOL Install(HWND hWndMain)
	{
		HRESULT hrCo = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
		DebugLog(L"[TaskbarVolume] Install: hWndMain=%p, CoInitializeEx=0x%08lX", hWndMain, hrCo);

		g_hMainWnd = hWndMain;

		// Register OSD window class
		WNDCLASSEXW wc = { 0 };
		wc.cbSize = sizeof(wc);
		wc.lpfnWndProc = OsdWndProc;
		wc.hInstance = GetModuleHandleW(NULL);
		wc.lpszClassName = L"MenuTools_OSD";
		wc.hCursor = LoadCursor(NULL, IDC_ARROW);
		RegisterClassExW(&wc);

		// Create OSD font (Segoe UI 14pt semi-bold)
		g_hOsdFont = CreateFontW(-15, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
			DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
			CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

		// Create hidden OSD window
		g_hOsdWnd = CreateWindowExW(
			WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST,
			L"MenuTools_OSD",
			L"",
			WS_POPUP,
			0, 0, 56, 26,
			NULL, NULL, GetModuleHandleW(NULL), NULL);

		// Install low-level mouse hook
		g_hMouseHook = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc, GetModuleHandleW(NULL), 0);
		DebugLog(L"[TaskbarVolume] SetWindowsHookEx returned: %p, LastError=%lu", g_hMouseHook, GetLastError());
		return (g_hMouseHook != NULL);
	}

	VOID Uninstall()
	{
		DebugLog(L"[TaskbarVolume] Uninstall called");

		if (g_hMouseHook)
		{
			UnhookWindowsHookEx(g_hMouseHook);
			g_hMouseHook = NULL;
		}

		if (g_hOsdWnd)
		{
			KillTimer(g_hOsdWnd, TIMER_OSD_HIDE);
			DestroyWindow(g_hOsdWnd);
			g_hOsdWnd = NULL;
		}

		if (g_hOsdFont)
		{
			DeleteObject(g_hOsdFont);
			g_hOsdFont = NULL;
		}

		if (g_pAutomation)
		{
			g_pAutomation->Release();
			g_pAutomation = NULL;
		}

		if (g_pDeviceEnumerator)
		{
			g_pDeviceEnumerator->Release();
			g_pDeviceEnumerator = NULL;
		}

		CoUninitialize();
	}

	VOID OnVolumeMessage(UINT uMsg, WPARAM wParam, LPARAM lParam)
	{
		UNREFERENCED_PARAMETER(lParam);

		UINT eventIdx = static_cast<UINT>(wParam) % kEventRingBufferSize;
		VolumeEvent ev = g_eventRing[eventIdx];

		DebugLog(L"[Controller] OnVolumeMessage uMsg=0x%04X, pt=(%ld,%ld), delta=%d", uMsg, ev.pt.x, ev.pt.y, ev.delta);

		std::wstring autoId;
		std::wstring name;
		RECT buttonRect = { 0 };

		if (!ResolveTaskbarItem(ev.pt, autoId, name, buttonRect))
		{
			DebugLog(L"[Controller] ResolveTaskbarItem returned false for pt=(%ld,%ld)", ev.pt.x, ev.pt.y);
			return;
		}

		DebugLog(L"[Controller] Resolved item: autoId='%ls', name='%ls', rect=[%ld,%ld,%ld,%ld]",
			autoId.c_str(), name.c_str(), buttonRect.left, buttonRect.top, buttonRect.right, buttonRect.bottom);

		bool isMute = (uMsg == WM_TASKBAR_VOLUME_MUTE);
		bool isShift = ((ev.flags & 1) != 0);

		float step = isShift ? 0.01f : 0.05f;
		if (ev.delta < 0)
		{
			step = -step;
		}

		float newVol = 0.0f;
		bool isMuted = false;

		if (AdjustAppVolume(autoId, name, isMute, step, newVol, isMuted))
		{
			WCHAR szDisplay[16] = { 0 };
			if (isMuted)
			{
				wcscpy_s(szDisplay, L"MUTE");
			}
			else
			{
				swprintf_s(szDisplay, L"%.2f", newVol);
			}

			DebugLog(L"[Controller] Showing OSD: '%ls'", szDisplay);
			ShowOsd(szDisplay, buttonRect);
		}
		else
		{
			DebugLog(L"[Controller] AdjustAppVolume found no matching audio session for '%ls'", autoId.c_str());
		}
	}
}
