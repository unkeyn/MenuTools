#pragma once

#include <stdint.h>

// Debug
#define MT_DEBUG_ONLY_X86					FALSE
#define MT_DEBUG_ONLY_X64					FALSE

// 64 bits
#ifdef _WIN64
#define BUILD(x)							x ## 64

// 32 bits
#elif _WIN32
#define BUILD(x)							x
#endif

// Strings
#define MT_DLL_NAME							_T("MenuToolsHook.dll")
#define MT_DLL_NAME64						_T("MenuToolsHook64.dll")
#define MT_EXE_NAME							_T("MenuTools.exe")
#define MT_EXE_NAME64						_T("MenuTools64.exe")
#define MT_JOB_NAME							_T("MenuToolsJob")

// Hook
#define MT_HOOK_PROC_CWP					"CallWndProc"
#define MT_HOOK_PROC_GMP					"GetMsgProc"

// Hook -> Messages
#define MT_HOOK_MSG_QUIT					RegisterWindowMessage(_T("MenuToolsQuit"))
#define MT_HOOK_MSG_TRAY					(WM_USER + 0x210)

// Menu
#define MT_MENU_ALWAYS_ON_TOP				(WM_USER + 0x2010)
#define MT_MENU_HIDE_TOP					(WM_USER + 0x2040)
#define MT_MENU_MINIMIZE_TO_TRAY			(WM_USER + 0x2020)
#define MT_MENU_SEPARATOR					(WM_USER + 0x2030)

#define MT_MENU_FULLSCREEN					(WM_USER + 0x2050)
#define MT_MENU_FULLSCREEN_BICUBIC			(WM_USER + 0x2051)
#define MT_MENU_FULLSCREEN_LANCZOS			(WM_USER + 0x2052)
#define MT_MENU_FULLSCREEN_FSR				(WM_USER + 0x2053)
#define MT_MENU_FULLSCREEN_ANIME4K_3D		(WM_USER + 0x2054)
#define MT_MENU_FULLSCREEN_ANIME4K_3D_AA	(WM_USER + 0x2055)
#define MT_MENU_FULLSCREEN_ASPECT_RATIO		(WM_USER + 0x2056)
#define MT_MENU_FULLSCREEN_EXIT				(WM_USER + 0x2057)
#define MT_MENU_FULLSCREEN_NEAREST			(WM_USER + 0x2058)

// Scaler Filter Enum & IDs
enum ScalerFilter : uint32_t
{
	SCALER_FILTER_BICUBIC		= 0,
	SCALER_FILTER_LANCZOS		= 1,
	SCALER_FILTER_FSR			= 2,
	SCALER_FILTER_ANIME4K_3D	= 3,
	SCALER_FILTER_ANIME4K_3D_AA	= 4,
	SCALER_FILTER_NEAREST		= 5
};

#define MT_SCALER_FILTER_BICUBIC			0
#define MT_SCALER_FILTER_LANCZOS			1
#define MT_SCALER_FILTER_FSR				2
#define MT_SCALER_FILTER_ANIME4K_3D			3
#define MT_SCALER_FILTER_ANIME4K_3D_AA		4
#define MT_SCALER_FILTER_NEAREST			5

// Hotkey ID
#define MT_HOTKEY_SCALER_ID					0x5343 // "SC"

// Scaler Executable
#define MT_SCALER_EXE_NAME					_T("MenuToolsScaler.exe")

// Properties
#define MT_PROP_ORIG_STYLE					_T("MenuTools_HideTop_OrigStyle")
#define MT_PROP_SCALED						_T("MenuTools_Scaled")

// Scaler IPC Messages
#define MT_MSG_SCALER_NAME					_T("MenuTools_Scaler_Msg")

// Scaler IPC Sync Objects
#define MT_SCALER_MUTEX_NAME				_T("Local\\MenuToolsScaler.Active")
#define MT_SCALER_EXIT_EVENT_NAME			_T("Local\\MenuToolsScaler.Exit")

// Scaler IPC commands (passed in wParam)
#define MT_SCALER_CMD_START					1
#define MT_SCALER_CMD_STOP					2
#define MT_SCALER_CMD_TOGGLE_ASPECT			3

// Tray
#define MT_TRAY_MESSAGE						(WM_USER + 0x200)