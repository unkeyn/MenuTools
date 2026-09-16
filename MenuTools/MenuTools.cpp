// MenuTools.cpp : Defines the entry point for the application.
//

#include "stdafx.h"
#include "MenuTools.h"
#include "Hooks.h"
#include "Startup.h"
#include "TaskbarVolume.h"

#include "MenuCommon/TrayIcon.h"
#include <shlwapi.h>
#include <stdio.h>

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "advapi32.lib")

#define MAX_LOADSTRING 100

#ifndef MOD_NOREPEAT
#define MOD_NOREPEAT 0x4000
#endif

#ifndef MSGFLT_ALLOW
#define MSGFLT_ALLOW 1
#endif

// Global Variables:
HINSTANCE hInst = NULL;							// current instance
HWND hWnd = NULL;								// current window handle

// Scaler Globals
static HANDLE g_hScalerProcess = NULL;
static HWND g_ScaledHWnd = NULL;
static UINT g_uMsgScaler = 0;

static bool IsScalerManager()
{
#ifdef _WIN64
	return true;
#else
	BOOL bIsWOW64 = FALSE;
	if (IsWow64Process(GetCurrentProcess(), &bIsWOW64) && bIsWOW64)
	{
		return false; // 32-bit on 64-bit Windows: delegate to MenuTools64.exe
	}
	return true; // pure 32-bit Windows: MenuTools.exe is the manager
#endif
}

static DWORD GetScalerRegDword(LPCWSTR name, DWORD defaultVal)
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

static bool IsScalerActive()
{
	HANDLE h = OpenMutexW(SYNCHRONIZE, FALSE, MT_SCALER_MUTEX_NAME);
	if (!h)
		return false;
	CloseHandle(h);
	return true;
}

static void StopScaler()
{
	// Signal exit event (created by scaler)
	HANDLE hExit = OpenEventW(EVENT_MODIFY_STATE, FALSE, MT_SCALER_EXIT_EVENT_NAME);
	if (hExit)
	{
		SetEvent(hExit);
		CloseHandle(hExit);
	}

	// Wait on the owned scaler process handle
	if (g_hScalerProcess)
	{
		if (WaitForSingleObject(g_hScalerProcess, 1500) == WAIT_TIMEOUT)
		{
			TerminateProcess(g_hScalerProcess, 0);
		}
		CloseHandle(g_hScalerProcess);
		g_hScalerProcess = NULL;
	}
	else if (IsScalerActive())
	{
		DWORD start = GetTickCount();
		while (IsScalerActive() && (GetTickCount() - start < 1500))
		{
			Sleep(50);
		}
	}

	if (g_ScaledHWnd && IsWindow(g_ScaledHWnd))
	{
		RemovePropW(g_ScaledHWnd, MT_PROP_SCALED);
		g_ScaledHWnd = NULL;
	}
}

static void StartScaler(HWND targetHWnd)
{
	if (!IsScalerManager()) return;
	if (!IsWindow(targetHWnd)) return;

	StopScaler();

	int retries = 20;
	while (IsScalerActive() && --retries > 0)
	{
		Sleep(50);
	}

	DWORD targetPid = 0;
	GetWindowThreadProcessId(targetHWnd, &targetPid);
	if (!targetPid) return;

	DWORD filterId = GetScalerRegDword(L"ScalerFilter", MT_SCALER_FILTER_BICUBIC);
	DWORD preserveAspect = GetScalerRegDword(L"PreserveAspect", 1);

	wchar_t szExeDir[MAX_PATH];
	GetModuleFileNameW(NULL, szExeDir, MAX_PATH);
	PathRemoveFileSpecW(szExeDir);

	wchar_t szScalerPath[MAX_PATH];
	PathCombineW(szScalerPath, szExeDir, MT_SCALER_EXE_NAME);

	if (GetFileAttributesW(szScalerPath) == INVALID_FILE_ATTRIBUTES)
	{
		PathCombineW(szScalerPath, szExeDir, L"Scaler\\MenuToolsScaler.exe");
	}

	if (GetFileAttributesW(szScalerPath) == INVALID_FILE_ATTRIBUTES)
	{
		return;
	}

	wchar_t szCmd[MAX_PATH * 2];
	swprintf_s(szCmd, MAX_PATH * 2, L"\"%ls\" %p %lu %lu %lu", szScalerPath, targetHWnd, targetPid, filterId, preserveAspect);

	STARTUPINFOW si = { sizeof(si) };
	PROCESS_INFORMATION pi = { 0 };
	if (CreateProcessW(szScalerPath, szCmd, NULL, NULL, FALSE, 0, NULL, szExeDir, &si, &pi))
	{
		CloseHandle(pi.hThread);
		g_hScalerProcess = pi.hProcess;
		g_ScaledHWnd = targetHWnd;
	}
}

static void OnScalerHotkey()
{
	if (!IsScalerManager()) return;

	if (IsScalerActive())
	{
		StopScaler();
		return;
	}

	HWND fg = GetForegroundWindow();
	if (!fg || fg == hWnd || fg == GetDesktopWindow()) return;

	HWND hTray = FindWindowW(L"Shell_TrayWnd", NULL);
	if (fg == hTray) return;

	HWND hProgman = FindWindowW(L"Progman", NULL);
	if (fg == hProgman) return;

	HWND hWorker = FindWindowW(L"WorkerW", NULL);
	if (fg == hWorker) return;

	DWORD fgPid = 0;
	GetWindowThreadProcessId(fg, &fgPid);
	if (fgPid == GetCurrentProcessId()) return;

	StartScaler(fg);
}

// Global Variables (Title/Class):
TCHAR szTitle[MAX_LOADSTRING];					// The title bar text
TCHAR szWindowClass[MAX_LOADSTRING];			// the main window class name
UINT uTrayId;

// Forward declarations of functions included in this code module:
ATOM				MyRegisterClass(HINSTANCE hInstance);
BOOL				InitInstance(HINSTANCE, int);
LRESULT CALLBACK	WndProc(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK	About(HWND, UINT, WPARAM, LPARAM);

int APIENTRY _tWinMain(_In_ HINSTANCE hInstance,
	_In_opt_ HINSTANCE hPrevInstance,
	_In_ LPTSTR    lpCmdLine,
	_In_ int       nCmdShow)
{
	UNREFERENCED_PARAMETER(hPrevInstance);
	UNREFERENCED_PARAMETER(lpCmdLine);
	UNREFERENCED_PARAMETER(nCmdShow);

	Startup startup;
	// Command line arguments
	if (!startup.ParseFlags(GetCommandLineW()))
	{
		return FALSE;
	}

	// Single instance
	if (!startup.CreateJob())
	{
		return FALSE;
	}

	// Initialize global strings
#ifdef _WIN64
	LoadString(hInstance, IDS_APP_TITLE64, szTitle, MAX_LOADSTRING);
	lstrcpyW(szWindowClass, L"MENUTOOLS64");
#else
	LoadString(hInstance, IDS_APP_TITLE, szTitle, MAX_LOADSTRING);
	LoadString(hInstance, IDC_MENUTOOLS, szWindowClass, MAX_LOADSTRING);
#endif
	MyRegisterClass(hInstance);

	// Perform application initialization:
	if (!InitInstance(hInstance, SW_HIDE))
	{
		return FALSE;
	}

	// Register Scaler IPC message in all instances
	g_uMsgScaler = RegisterWindowMessageW(MT_MSG_SCALER_NAME);

	// Register Scaler IPC and Hotkey (only in the designated scaler manager process)
	if (IsScalerManager())
	{
		typedef BOOL (WINAPI *pfnChangeWindowMessageFilterEx)(HWND, UINT, DWORD, PVOID);
		pfnChangeWindowMessageFilterEx pfnFilter = (pfnChangeWindowMessageFilterEx)GetProcAddress(GetModuleHandleW(L"user32.dll"), "ChangeWindowMessageFilterEx");
		if (pfnFilter)
		{
			pfnFilter(hWnd, g_uMsgScaler, MSGFLT_ALLOW, NULL);
		}
		if (!RegisterHotKey(hWnd, MT_HOTKEY_SCALER_ID, MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, 'A'))
		{
			RegisterHotKey(hWnd, MT_HOTKEY_SCALER_ID, MOD_ALT | MOD_SHIFT, 'A');
		}
	}

#ifndef _WIN64
	// Create tray icon
	TrayIcon tray(hWnd);
	tray.SetCallBackMessage(MT_TRAY_MESSAGE);
	uTrayId = tray.Show();

	// Hide it...
	if (startup.flags & Startup::HIDE_TRAY)
	{
		tray.Hide();
	}
#endif

	// Install wide hooks
	Hooks hooks;
	if (!hooks.Install())
	{
		return FALSE;
	}

	// Install taskbar volume hook
#ifdef _WIN64
	TaskbarVolume::Install(hWnd);
#else
	BOOL bIsWOW64Taskbar = FALSE;
	if (IsWow64Process(GetCurrentProcess(), &bIsWOW64Taskbar) && !bIsWOW64Taskbar)
	{
		TaskbarVolume::Install(hWnd);
	}
#endif

	// Main message loop:
	MSG msg;
	while (GetMessage(&msg, NULL, 0, 0))
	{
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}

	TaskbarVolume::Uninstall();
	if (IsScalerManager())
	{
		UnregisterHotKey(hWnd, MT_HOTKEY_SCALER_ID);
		StopScaler();
	}

	return (int)msg.wParam;
}



//
//  FUNCTION: MyRegisterClass()
//
//  PURPOSE: Registers the window class.
//
ATOM MyRegisterClass(HINSTANCE hInstance)
{
	WNDCLASSEX wcex;

	wcex.cbSize = sizeof(WNDCLASSEX);

	wcex.style = CS_HREDRAW | CS_VREDRAW;
	wcex.lpfnWndProc = WndProc;
	wcex.cbClsExtra = 0;
	wcex.cbWndExtra = 0;
	wcex.hInstance = hInstance;
	wcex.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_MENUTOOLS));
	wcex.hCursor = LoadCursor(NULL, IDC_ARROW);
	wcex.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
	wcex.lpszMenuName = MAKEINTRESOURCE(IDC_MENUTOOLS);
	wcex.lpszClassName = szWindowClass;
	wcex.hIconSm = LoadIcon(wcex.hInstance, MAKEINTRESOURCE(IDI_SMALL));

	return RegisterClassEx(&wcex);
}

//
//   FUNCTION: InitInstance(HINSTANCE, int)
//
//   PURPOSE: Saves instance handle and creates main window
//
//   COMMENTS:
//
//        In this function, we save the instance handle in a global variable and
//        create and display the main program window.
//
BOOL InitInstance(HINSTANCE hInstance, int nCmdShow)
{
	hInst = hInstance; // Store instance handle in our global variable

	hWnd = CreateWindow(szWindowClass, szTitle, WS_OVERLAPPEDWINDOW,
		CW_USEDEFAULT, 0, 0, 0, NULL, NULL, hInstance, NULL);

	if (!hWnd)
	{
		return FALSE;
	}

	FILE* f = _wfopen(L"C:\\Program Files\\MenuTools\\hwnd.txt", L"a");
	if (f) {
		fwprintf(f, L"PID=%lu, x%d, hWnd=%p, Class='%ls', Title='%ls'\n",
			GetCurrentProcessId(), (int)(sizeof(void*) * 8), hWnd, szWindowClass, szTitle);
		fclose(f);
	}

	ShowWindow(hWnd, nCmdShow);

	return TRUE;
}

//
//  FUNCTION: WndProc(HWND, UINT, WPARAM, LPARAM)
//
//  PURPOSE:  Processes messages for the main window.
//
//  WM_COMMAND	- process the application menu
//  WM_DESTROY	- post a quit message and return
//
//
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	int wmId, wmEvent;

	switch (message)
	{
#ifndef _WIN64
		// Notify icon message
	case MT_TRAY_MESSAGE:
	{
		// Taskbar icon id
		if (wParam == uTrayId)
		{
			// Message
			switch (lParam)
			{
			case WM_RBUTTONDOWN:
			{
				POINT pt;
				GetCursorPos(&pt);

				SetForegroundWindow(hWnd);

				HMENU hMenu = GetSubMenu(GetMenu(hWnd), 0);
				TrackPopupMenuEx(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, hWnd, NULL);
				PostMessage(hWnd, WM_NULL, 0, 0);
			}
			}
		}

		return DefWindowProc(hWnd, message, wParam, lParam);
	}
#endif
	case WM_COMMAND:
		wmId = LOWORD(wParam);
		wmEvent = HIWORD(wParam);
		// Parse the menu selections:
		switch (wmId)
		{
		case IDM_ABOUT:
			DialogBox(hInst, MAKEINTRESOURCE(IDD_ABOUTBOX), hWnd, About);
			break;
		case IDM_EXIT:
			DestroyWindow(hWnd);
			break;
		default:
			return DefWindowProc(hWnd, message, wParam, lParam);
		}
		break;
	case WM_TASKBAR_VOLUME_WHEEL:
	case WM_TASKBAR_VOLUME_MUTE:
		TaskbarVolume::OnVolumeMessage(message, wParam, lParam);
		break;
	case WM_HOTKEY:
		if (wParam == MT_HOTKEY_SCALER_ID && IsScalerManager())
		{
			OnScalerHotkey();
		}
		break;
	case WM_DESTROY:
		TaskbarVolume::Uninstall();
		if (IsScalerManager())
		{
			UnregisterHotKey(hWnd, MT_HOTKEY_SCALER_ID);
			StopScaler();
		}
		PostQuitMessage(0);
		break;
	default:
		if (message == g_uMsgScaler && g_uMsgScaler != 0)
		{
			if (IsScalerManager())
			{
				HWND targetHWnd = (HWND)lParam;
				if (wParam == MT_SCALER_CMD_START)
				{
					StartScaler(targetHWnd);
				}
				else if (wParam == MT_SCALER_CMD_STOP)
				{
					StopScaler();
				}
				else if (wParam == MT_SCALER_CMD_TOGGLE_ASPECT)
				{
					if (IsScalerActive() && g_ScaledHWnd == targetHWnd)
					{
						StartScaler(targetHWnd);
					}
				}
			}
			else
			{
				HWND hMT64 = FindWindowW(L"MENUTOOLS64", NULL);
				if (hMT64)
				{
					PostMessageW(hMT64, message, wParam, lParam);
				}
			}
			return 0;
		}
		return DefWindowProc(hWnd, message, wParam, lParam);
	}
	return 0;
}

// Message handler for about box.
INT_PTR CALLBACK About(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
	UNREFERENCED_PARAMETER(lParam);
	switch (message)
	{
	case WM_INITDIALOG:
		return (INT_PTR)TRUE;

	case WM_COMMAND:
		if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL)
		{
			EndDialog(hDlg, LOWORD(wParam));
			return (INT_PTR)TRUE;
		}
		break;
	}
	return (INT_PTR)FALSE;
}
