namespace MenuTools
{
	// Functions
	BOOL Install(HWND hWnd);
	BOOL Uninstall(HWND hWnd);
	VOID Status(HWND hWnd);

	// Callback
	BOOL TrayProc(HWND hWnd, WPARAM wParam, LPARAM lParam);
	BOOL WndProc(HWND hWnd, WPARAM wParam, LPARAM lParam);
};

// TODO: Create a class to hold this information
// Window information
extern LONG wndOldWidth;
extern LONG wndOldHeight;

// Helpers
BOOL IsMenuItem(HMENU hMenu, UINT item);