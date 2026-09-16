#include "stdafx.h"
#include "MenuTools.h"

#include "MenuCommon/TrayIcon.h"

#include <commctrl.h>
#include <windowsx.h>

#pragma comment(lib, "comctl32.lib")

// Window information
LONG wndOldWidth = -1;
LONG wndOldHeight = -1;

namespace
{
	constexpr UINT_PTR kHideTopSubclassId = 1;

	DWORD GetScalerRegDword(LPCWSTR name, DWORD defaultVal)
	{
		HKEY hKey;
		DWORD val = defaultVal;
		if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\MenuTools", 0, KEY_QUERY_VALUE, &hKey) == ERROR_SUCCESS)
		{
			DWORD size = sizeof(val);
			RegQueryValueExW(hKey, name, NULL, NULL, (LPBYTE)&val, &size);
			RegCloseKey(hKey);
		}
		return val;
	}

	void SetScalerRegDword(LPCWSTR name, DWORD val)
	{
		HKEY hKey;
		if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\MenuTools", 0, NULL, 0, KEY_SET_VALUE, NULL, &hKey, NULL) == ERROR_SUCCESS)
		{
			RegSetValueExW(hKey, name, 0, REG_DWORD, (const BYTE*)&val, sizeof(val));
			RegCloseKey(hKey);
		}
	}

#ifndef SM_CXPADDEDBORDER
#define SM_CXPADDEDBORDER 92
#endif

	int GetMetric(HWND hWnd, int nIndex)
	{
		typedef UINT (WINAPI *GetDpiForWindow_t)(HWND);
		typedef int (WINAPI *GetSystemMetricsForDpi_t)(int, UINT);

		static auto pfnGetDpiForWindow = (GetDpiForWindow_t)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
		static auto pfnGetSystemMetricsForDpi = (GetSystemMetricsForDpi_t)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetSystemMetricsForDpi");

		if (pfnGetDpiForWindow && pfnGetSystemMetricsForDpi)
		{
			UINT dpi = pfnGetDpiForWindow(hWnd);
			if (dpi != 0)
			{
				return pfnGetSystemMetricsForDpi(nIndex, dpi);
			}
		}
		return GetSystemMetrics(nIndex);
	}

	LRESULT CALLBACK HideTopSubclassProc(
		HWND hWnd,
		UINT uMsg,
		WPARAM wParam,
		LPARAM lParam,
		UINT_PTR uIdSubclass,
		DWORD_PTR dwRefData)
	{
		UNREFERENCED_PARAMETER(dwRefData);

		switch (uMsg)
		{
		case WM_NCCALCSIZE:
		{
			if (wParam == TRUE && lParam != 0)
			{
				auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam);

				// Before default processing this is the proposed WINDOW rect.
				const LONG windowTop = params->rgrc[0].top;

				// Let the target application + Windows calculate its normal
				// client rect first.
				const LRESULT result = DefSubclassProc(hWnd, uMsg, wParam, lParam);

				// Extend only the client area's TOP to the window edge.
				// Left/right/bottom calculations remain untouched.
				params->rgrc[0].top = windowTop;

				return result;
			}
			break;
		}

		case WM_NCHITTEST:
		{
			const LRESULT hit = DefSubclassProc(hWnd, uMsg, wParam, lParam);

			// Don't override application-specific hit testing.
			if (hit != HTCLIENT)
			{
				return hit;
			}

			// Only emulate top resizing for genuinely resizable windows.
			SetLastError(0);
			const LONG_PTR style = GetWindowLongPtr(hWnd, GWL_STYLE);

			if ((style == 0 && GetLastError() != 0) ||
				!(style & WS_THICKFRAME) ||
				IsZoomed(hWnd))
			{
				return hit;
			}

			RECT rc;
			if (!GetWindowRect(hWnd, &rc))
			{
				return hit;
			}

			const POINT pt = {
				GET_X_LPARAM(lParam),
				GET_Y_LPARAM(lParam)
			};

			const int frameX =
				GetMetric(hWnd, SM_CXSIZEFRAME) +
				GetMetric(hWnd, SM_CXPADDEDBORDER);

			const int frameY =
				GetMetric(hWnd, SM_CYSIZEFRAME) +
				GetMetric(hWnd, SM_CXPADDEDBORDER);

			if (pt.y >= rc.top && pt.y < rc.top + frameY)
			{
				if (pt.x < rc.left + frameX)
					return HTTOPLEFT;

				if (pt.x >= rc.right - frameX)
					return HTTOPRIGHT;

				return HTTOP;
			}

			return hit;
		}

		case WM_NCDESTROY:
		{
			RemoveProp(hWnd, MT_PROP_ORIG_STYLE);
			RemoveWindowSubclass(hWnd, HideTopSubclassProc, uIdSubclass);
			return DefSubclassProc(hWnd, uMsg, wParam, lParam);
		}
		}

		return DefSubclassProc(hWnd, uMsg, wParam, lParam);
	}

	BOOL EnableHideTop(HWND hWnd)
	{
		SetLastError(0);
		const LONG_PTR curStyle = GetWindowLongPtr(hWnd, GWL_STYLE);
		if (curStyle == 0 && GetLastError() != 0)
		{
			return FALSE;
		}

		if (!(curStyle & WS_CAPTION))
		{
			return FALSE;
		}

		if (!SetProp(hWnd, MT_PROP_ORIG_STYLE, (HANDLE)curStyle))
		{
			return FALSE;
		}

		if (!SetWindowSubclass(hWnd, HideTopSubclassProc, kHideTopSubclassId, 0))
		{
			RemoveProp(hWnd, MT_PROP_ORIG_STYLE);
			return FALSE;
		}

		SetLastError(0);
		const LONG_PTR oldStyle = SetWindowLongPtr(hWnd, GWL_STYLE, curStyle & ~WS_CAPTION);
		if (oldStyle == 0 && GetLastError() != 0)
		{
			RemoveWindowSubclass(hWnd, HideTopSubclassProc, kHideTopSubclassId);
			RemoveProp(hWnd, MT_PROP_ORIG_STYLE);
			return FALSE;
		}

		if (!SetWindowPos(hWnd, NULL, 0, 0, 0, 0,
				SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED))
		{
			// Rollback on SetWindowPos failure
			SetLastError(0);
			SetWindowLongPtr(hWnd, GWL_STYLE, curStyle);
			RemoveWindowSubclass(hWnd, HideTopSubclassProc, kHideTopSubclassId);
			RemoveProp(hWnd, MT_PROP_ORIG_STYLE);
			return FALSE;
		}

		return TRUE;
	}

	BOOL DisableHideTop(HWND hWnd)
	{
		HANDLE hOldStyle = GetProp(hWnd, MT_PROP_ORIG_STYLE);
		if (!hOldStyle)
		{
			return FALSE;
		}

		LONG_PTR origStyle = (LONG_PTR)hOldStyle;

		SetLastError(0);
		LONG_PTR curStyle = GetWindowLongPtr(hWnd, GWL_STYLE);
		if (curStyle == 0 && GetLastError() != 0)
		{
			return FALSE;
		}

		if (!RemoveWindowSubclass(hWnd, HideTopSubclassProc, kHideTopSubclassId))
		{
			return FALSE;
		}

		SetLastError(0);
		LONG_PTR result = SetWindowLongPtr(hWnd, GWL_STYLE, curStyle | (origStyle & WS_CAPTION));
		if (result == 0 && GetLastError() != 0)
		{
			// Re-attach subclass to avoid inconsistent state
			SetWindowSubclass(hWnd, HideTopSubclassProc, kHideTopSubclassId, 0);
			return FALSE;
		}

		RemoveProp(hWnd, MT_PROP_ORIG_STYLE);
		SetWindowPos(hWnd, NULL, 0, 0, 0, 0,
			SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);

		return TRUE;
	}

	HWND FindScalerController()
	{
		HWND hMT = FindWindowW(L"MENUTOOLS64", NULL);
		if (!hMT)
		{
			hMT = FindWindowW(L"MENUTOOLS", NULL);
		}
		return hMT;
	}
}

BOOL MenuTools::Install(HWND hWnd)
{
	// Visible window
	if (!IsWindow(hWnd) || !IsWindowVisible(hWnd))
	{
		return FALSE;
	}

	HMENU hMenuSystem = GetSystemMenu(hWnd, FALSE);
	if (!hMenuSystem)
	{
		return FALSE;
	}

	if (!IsMenuItem(hMenuSystem, MT_MENU_ALWAYS_ON_TOP))
	{
		InsertMenu(hMenuSystem, SC_CLOSE, MF_BYCOMMAND | MF_STRING, MT_MENU_ALWAYS_ON_TOP, _T("&Always on Top"));
	}

	if (!IsMenuItem(hMenuSystem, MT_MENU_HIDE_TOP))
	{
		InsertMenu(hMenuSystem, SC_CLOSE, MF_BYCOMMAND | MF_STRING, MT_MENU_HIDE_TOP, _T("&Hide top"));
	}

	if (!IsMenuItem(hMenuSystem, MT_MENU_MINIMIZE_TO_TRAY))
	{
		InsertMenu(hMenuSystem, SC_CLOSE, MF_BYCOMMAND | MF_STRING, MT_MENU_MINIMIZE_TO_TRAY, _T("Minimi&ze to Tray"));

	}

	// Fullscreen Submenu
	bool hasFullscreen = false;
	int count = GetMenuItemCount(hMenuSystem);
	for (int i = 0; i < count; ++i)
	{
		HMENU hSub = GetSubMenu(hMenuSystem, i);
		if (hSub && (IsMenuItem(hSub, MT_MENU_FULLSCREEN_NEAREST) || IsMenuItem(hSub, MT_MENU_FULLSCREEN_BICUBIC)))
		{
			hasFullscreen = true;
			break;
		}
	}

	if (!hasFullscreen)
	{
		HMENU hSubMenu = CreatePopupMenu();
		AppendMenu(hSubMenu, MF_STRING, MT_MENU_FULLSCREEN_NEAREST, _T("Nearest Neighbor"));
		AppendMenu(hSubMenu, MF_STRING, MT_MENU_FULLSCREEN_BICUBIC, _T("Bicubic"));
		AppendMenu(hSubMenu, MF_STRING, MT_MENU_FULLSCREEN_LANCZOS, _T("Lanczos"));
		AppendMenu(hSubMenu, MF_STRING, MT_MENU_FULLSCREEN_FSR, _T("FSR"));
		AppendMenu(hSubMenu, MF_STRING, MT_MENU_FULLSCREEN_ANIME4K_3D, _T("Anime4K 3D"));
		AppendMenu(hSubMenu, MF_STRING, MT_MENU_FULLSCREEN_ANIME4K_3D_AA, _T("Anime4K 3D AA"));
		AppendMenu(hSubMenu, MF_SEPARATOR, 0, NULL);
		AppendMenu(hSubMenu, MF_STRING, MT_MENU_FULLSCREEN_ASPECT_RATIO, _T("Preserve aspect ratio"));
		AppendMenu(hSubMenu, MF_STRING, MT_MENU_FULLSCREEN_EXIT, _T("Exit Fullscreen"));

		InsertMenu(hMenuSystem, SC_CLOSE, MF_BYCOMMAND | MF_POPUP, (UINT_PTR)hSubMenu, _T("&Fullscreen"));
	}

	if (!IsMenuItem(hMenuSystem, MT_MENU_SEPARATOR))
	{
		InsertMenu(hMenuSystem, SC_CLOSE, MF_BYCOMMAND | MF_SEPARATOR, MT_MENU_SEPARATOR, NULL);
	}

	return TRUE;
}

BOOL MenuTools::Uninstall(HWND hWnd)
{
	if (!IsWindow(hWnd))
	{
		return FALSE;
	}

	BOOL bSuccess = TRUE;

	// Restore window caption and remove subclass if top was hidden
	DisableHideTop(hWnd);

	HMENU hMenuSystem = GetSystemMenu(hWnd, FALSE);
	if (!hMenuSystem)
	{
		return bSuccess;
	}

	// Delete Menu Tools
	if (!DeleteMenu(hMenuSystem, MT_MENU_ALWAYS_ON_TOP, MF_BYCOMMAND))
	{
		bSuccess = FALSE;
	}
	if (!DeleteMenu(hMenuSystem, MT_MENU_HIDE_TOP, MF_BYCOMMAND))
	{
		bSuccess = FALSE;
	}
	if (!DeleteMenu(hMenuSystem, MT_MENU_MINIMIZE_TO_TRAY, MF_BYCOMMAND))
	{
		bSuccess = FALSE;
	}

	// Delete Fullscreen Submenu
	int count = GetMenuItemCount(hMenuSystem);
	for (int i = 0; i < count; ++i)
	{
		HMENU hSub = GetSubMenu(hMenuSystem, i);
		if (hSub && (IsMenuItem(hSub, MT_MENU_FULLSCREEN_NEAREST) || IsMenuItem(hSub, MT_MENU_FULLSCREEN_BICUBIC)))
		{
			RemoveMenu(hMenuSystem, i, MF_BYPOSITION);
			DestroyMenu(hSub);
			break;
		}
	}

	if (!DeleteMenu(hMenuSystem, MT_MENU_SEPARATOR, MF_BYCOMMAND))
	{
		bSuccess = FALSE;
	}

	return bSuccess;
}

VOID MenuTools::Status(HWND hWnd)
{
	if (!IsWindow(hWnd))
	{
		return;
	}

	HMENU hMenuSystem = GetSystemMenu(hWnd, FALSE);
	if (!hMenuSystem)
	{
		return;
	}

	// Always on Top
	if (GetWindowLongPtr(hWnd, GWL_EXSTYLE) & WS_EX_TOPMOST)
	{
		CheckMenuItem(hMenuSystem, MT_MENU_ALWAYS_ON_TOP, MF_BYCOMMAND | MF_CHECKED);
	}
	else
	{
		CheckMenuItem(hMenuSystem, MT_MENU_ALWAYS_ON_TOP, MF_BYCOMMAND | MF_UNCHECKED);
	}

	// Hide top
	if (GetProp(hWnd, MT_PROP_ORIG_STYLE) != NULL)
	{
		CheckMenuItem(hMenuSystem, MT_MENU_HIDE_TOP, MF_BYCOMMAND | MF_CHECKED);
	}
	else
	{
		CheckMenuItem(hMenuSystem, MT_MENU_HIDE_TOP, MF_BYCOMMAND | MF_UNCHECKED);
	}

	// Minimize to Tray
	if (mTrays.count(hWnd))
	{
		CheckMenuItem(hMenuSystem, MT_MENU_MINIMIZE_TO_TRAY, MF_BYCOMMAND | MF_CHECKED);
	}
	else
	{
		CheckMenuItem(hMenuSystem, MT_MENU_MINIMIZE_TO_TRAY, MF_BYCOMMAND | MF_UNCHECKED);
	}

	// Fullscreen Submenu Status
	int count = GetMenuItemCount(hMenuSystem);
	for (int i = 0; i < count; ++i)
	{
		HMENU hSub = GetSubMenu(hMenuSystem, i);
		if (hSub && (IsMenuItem(hSub, MT_MENU_FULLSCREEN_NEAREST) || IsMenuItem(hSub, MT_MENU_FULLSCREEN_BICUBIC)))
		{
			DWORD filterId = GetScalerRegDword(L"ScalerFilter", MT_SCALER_FILTER_BICUBIC);
			DWORD preserveAspect = GetScalerRegDword(L"PreserveAspect", 1);

			if (filterId > MT_SCALER_FILTER_NEAREST)
			{
				filterId = MT_SCALER_FILTER_BICUBIC;
			}

			UINT radioPos = 1;
			switch (filterId)
			{
			case MT_SCALER_FILTER_NEAREST: radioPos = 0; break;
			case MT_SCALER_FILTER_BICUBIC: radioPos = 1; break;
			case MT_SCALER_FILTER_LANCZOS: radioPos = 2; break;
			case MT_SCALER_FILTER_FSR: radioPos = 3; break;
			case MT_SCALER_FILTER_ANIME4K_3D: radioPos = 4; break;
			case MT_SCALER_FILTER_ANIME4K_3D_AA: radioPos = 5; break;
			default: radioPos = 1; break;
			}

			CheckMenuRadioItem(hSub, 0, 5, radioPos, MF_BYPOSITION);

			CheckMenuItem(hSub, MT_MENU_FULLSCREEN_ASPECT_RATIO,
				MF_BYCOMMAND | (preserveAspect ? MF_CHECKED : MF_UNCHECKED));

			BOOL isScaled = (GetProp(hWnd, MT_PROP_SCALED) != NULL);
			if (isScaled)
			{
				HANDLE hMutex = OpenMutexW(SYNCHRONIZE, FALSE, MT_SCALER_MUTEX_NAME);
				if (!hMutex)
				{
					RemovePropW(hWnd, MT_PROP_SCALED);
					isScaled = FALSE;
				}
				else
				{
					CloseHandle(hMutex);
				}
			}
			EnableMenuItem(hSub, MT_MENU_FULLSCREEN_EXIT,
				MF_BYCOMMAND | (isScaled ? MF_ENABLED : MF_GRAYED));

			break;
		}
	}

	return;
}

BOOL MenuTools::TrayProc(HWND hWnd, WPARAM wParam, LPARAM lParam)
{
	UNREFERENCED_PARAMETER(wParam);

	// Tray Icon
	switch (lParam)
	{
		// Restore
	case WM_LBUTTONDBLCLK:
	{
		// Hide tray icon
		if (mTrays.count(hWnd))
		{
			mTrays.erase(hWnd);
		}

		ShowWindow(hWnd, SW_SHOW);
		SetForegroundWindow(hWnd);

		return TRUE;
	}
	}

	return FALSE;
}

BOOL MenuTools::WndProc(HWND hWnd, WPARAM wParam, LPARAM lParam)
{
	UNREFERENCED_PARAMETER(lParam);

	int wmId = (int)(wParam & 0xFFFF);

	// Handle menu messages
	switch (wmId)
	{
		// Always On Top
	case MT_MENU_ALWAYS_ON_TOP:
	{
		if (!(GetWindowLongPtr(hWnd, GWL_EXSTYLE) & WS_EX_TOPMOST))
		{
			// Set
			SetWindowPos(hWnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
		}
		else
		{
			// Remove
			SetWindowPos(hWnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
		}
		return TRUE;
	}
		// Hide top
	case MT_MENU_HIDE_TOP:
	{
		if (GetProp(hWnd, MT_PROP_ORIG_STYLE))
		{
			DisableHideTop(hWnd);
		}
		else
		{
			EnableHideTop(hWnd);
		}
		return TRUE;
	}
		// Minimize to Tray
	case MT_MENU_MINIMIZE_TO_TRAY:
	{
		// Check if tray is already add
		if (mTrays.count(hWnd))
		{
			// Destroy tray icon
			mTrays.erase(hWnd);

			// Restore window
			ShowWindow(hWnd, SW_SHOW);
			SetForegroundWindow(hWnd);
		}
		else
		{
			// Insert
			mTrays.insert(Tray_Pair(hWnd, TrayIcon(hWnd)));
			// Show tray icon
			if (mTrays.at(hWnd).Show())
			{
				// Hide window
				ShowWindow(hWnd, SW_HIDE);
			}
			else
			{
				// Destroy tray icon
				mTrays.erase(hWnd);
			}
		}

		return TRUE;
	}
	case MT_MENU_FULLSCREEN_NEAREST:
	case MT_MENU_FULLSCREEN_BICUBIC:
	case MT_MENU_FULLSCREEN_LANCZOS:
	case MT_MENU_FULLSCREEN_FSR:
	case MT_MENU_FULLSCREEN_ANIME4K_3D:
	case MT_MENU_FULLSCREEN_ANIME4K_3D_AA:
	{
		DWORD filter = MT_SCALER_FILTER_BICUBIC;
		switch (wmId)
		{
		case MT_MENU_FULLSCREEN_NEAREST:
			filter = MT_SCALER_FILTER_NEAREST;
			break;
		case MT_MENU_FULLSCREEN_BICUBIC:
			filter = MT_SCALER_FILTER_BICUBIC;
			break;
		case MT_MENU_FULLSCREEN_LANCZOS:
			filter = MT_SCALER_FILTER_LANCZOS;
			break;
		case MT_MENU_FULLSCREEN_FSR:
			filter = MT_SCALER_FILTER_FSR;
			break;
		case MT_MENU_FULLSCREEN_ANIME4K_3D:
			filter = MT_SCALER_FILTER_ANIME4K_3D;
			break;
		case MT_MENU_FULLSCREEN_ANIME4K_3D_AA:
			filter = MT_SCALER_FILTER_ANIME4K_3D_AA;
			break;
		}
		SetScalerRegDword(L"ScalerFilter", filter);

		HWND hMT = FindScalerController();
		if (hMT)
		{
			UINT msg = RegisterWindowMessageW(MT_MSG_SCALER_NAME);
			PostMessageW(hMT, msg, MT_SCALER_CMD_START, (LPARAM)hWnd);
		}
		return TRUE;
	}
	case MT_MENU_FULLSCREEN_ASPECT_RATIO:
	{
		DWORD preserveAspect = GetScalerRegDword(L"PreserveAspect", 1);
		preserveAspect = preserveAspect ? 0 : 1;
		SetScalerRegDword(L"PreserveAspect", preserveAspect);

		HWND hMT = FindScalerController();
		if (hMT)
		{
			UINT msg = RegisterWindowMessageW(MT_MSG_SCALER_NAME);
			PostMessageW(hMT, msg, MT_SCALER_CMD_TOGGLE_ASPECT, (LPARAM)hWnd);
		}
		return TRUE;
	}
	case MT_MENU_FULLSCREEN_EXIT:
	{
		HWND hMT = FindScalerController();
		if (hMT)
		{
			UINT msg = RegisterWindowMessageW(MT_MSG_SCALER_NAME);
			PostMessageW(hMT, msg, MT_SCALER_CMD_STOP, (LPARAM)hWnd);
		}
		return TRUE;
	}
	}

	return FALSE;
}

// Helpers
BOOL IsMenuItem(HMENU hMenu, UINT item)
{
	MENUITEMINFO mmi;
	ZeroMemory(&mmi, sizeof(MENUITEMINFO));
	mmi.cbSize = sizeof(MENUITEMINFO);
	mmi.fMask = MIIM_ID;

	return GetMenuItemInfo(hMenu, item, FALSE, &mmi);
}