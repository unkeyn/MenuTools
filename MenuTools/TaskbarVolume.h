#pragma once

#include <windows.h>

constexpr UINT WM_TASKBAR_VOLUME_WHEEL = WM_APP + 10;
constexpr UINT WM_TASKBAR_VOLUME_MUTE  = WM_APP + 11;

namespace TaskbarVolume
{
	BOOL Install(HWND hWndMain);
	VOID Uninstall();
	VOID OnVolumeMessage(UINT uMsg, WPARAM wParam, LPARAM lParam);
}
