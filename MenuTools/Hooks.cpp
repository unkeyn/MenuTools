#include "stdafx.h"
#include "Hooks.h"
#include <shlwapi.h>

#pragma comment(lib, "shlwapi.lib")

Hooks::Hooks()
{
	//
}

Hooks::~Hooks()
{
	Uninstall();
}

BOOL Hooks::Install()
{
	TCHAR szDllPath[MAX_PATH];
	GetModuleFileName(NULL, szDllPath, MAX_PATH);
	PathRemoveFileSpec(szDllPath);
	PathCombine(szDllPath, szDllPath, BUILD(MT_DLL_NAME));

	// Load hook DLL
	HMODULE hModDLL = LoadLibrary(szDllPath);
	if (!hModDLL)
	{
		return FALSE;
	}

	// CallWndProc function
	HOOKPROC hkCallWndProc = (HOOKPROC)GetProcAddress(hModDLL, MT_HOOK_PROC_CWP);
	if (!hkCallWndProc)
	{
		return FALSE;
	}

	// GetMsgProc function
	HOOKPROC hkGetMsgProc = (HOOKPROC)GetProcAddress(hModDLL, MT_HOOK_PROC_GMP);
	if (!hkGetMsgProc)
	{
		return FALSE;
	}

	// Set hook on CallWndProc
	hhkCallWndProc = SetWindowsHookEx(WH_CALLWNDPROC, hkCallWndProc, hModDLL, NULL);
	if (!hhkCallWndProc)
	{
		return FALSE;
	}

	// Set hook on GetMessage
	hhkGetMessage = SetWindowsHookEx(WH_GETMESSAGE, hkGetMsgProc, hModDLL, NULL);
	if (!hhkGetMessage)
	{
		return FALSE;
	}

	return TRUE;
}

BOOL Hooks::Uninstall()
{
	// Send a especial message to remove menus
	SendMessage(HWND_BROADCAST, MT_HOOK_MSG_QUIT, NULL, NULL);

	BOOL bRetn = TRUE;

	// Unset the CallWndProc hook
	if (!UnhookWindowsHookEx(hhkCallWndProc))
	{
		bRetn = FALSE;
	}

	// Unset the GetMessage
	if (!UnhookWindowsHookEx(hhkGetMessage))
	{
		bRetn = FALSE;
	}

	return bRetn;
}