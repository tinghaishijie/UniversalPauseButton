// Main.c
// UniversalPauseButton
// Joseph Ryan Ries, 2015-2023
// ryanries09@gmail.com
// https://github.com/ryanries/UniversalPauseButton/

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#define WM_TRAYICON (WM_USER + 1)
#define IDT_CURSOR_IDLE_TIMER 2001

#define OEMRESOURCE   // Required for the OCR_* system cursor id constants.

#include <Windows.h>
#include <stdio.h>
#include <TlHelp32.h>
#include <sddl.h>
#include <shlobj.h>
#include "Main.h"
#include "resource.h"

#ifndef OCR_HELP
#define OCR_HELP 32651
#endif

CONFIG gConfig;
HANDLE gDbgConsole = INVALID_HANDLE_VALUE;
BOOL gIsRunning = TRUE;
HANDLE gMutex;
NOTIFYICONDATA gTrayNotifyIconData;
BOOL gIsPaused;
u32 gPreviouslyPausedProcessId;
u32 gLastForegroundProcessId;
BOOL gPausedBySleep;
HANDLE gPauseSignalEvent = NULL;
HWINEVENTHOOK gForegroundHook = NULL;
wchar_t gPausedProcessName[128];

// Idle cursor-hiding state.
HHOOK gMouseHook = NULL;
HCURSOR gBlankCursor = NULL;
BOOL gIsCursorHidden = FALSE;
u32 gLastRealMoveTick = 0;
POINT gLastMousePos = { 0 };
BOOL gHaveLastMousePos = FALSE;

// The complete set of system cursors we override when hiding, so the pointer stays
// invisible over any control. SetSystemCursor(SPI_SETCURSORS) restores them.
static const DWORD gOcrIds[] = {
	OCR_NORMAL, OCR_IBEAM, OCR_WAIT, OCR_CROSS, OCR_UP,
	OCR_SIZENWSE, OCR_SIZENESW, OCR_SIZEWE, OCR_SIZENS, OCR_SIZEALL,
	OCR_NO, OCR_HAND, OCR_APPSTARTING, OCR_HELP
};


int WINAPI wWinMain(_In_ HINSTANCE Instance, _In_opt_ HINSTANCE PrevInstance, _In_ PWSTR CmdLine, _In_ int CmdShow)
{	
	UNREFERENCED_PARAMETER(PrevInstance);
	UNREFERENCED_PARAMETER(CmdLine);
	UNREFERENCED_PARAMETER(CmdShow);

	HMODULE NtDll = NULL;
	MSG WndMsg = { 0 };
	//HHOOK KeyboardHook = NULL;

	if (LoadRegistrySettings() != ERROR_SUCCESS)
	{
		goto Exit;
	}

	gMutex = CreateMutexW(NULL, FALSE, APPNAME);

	if (GetLastError() == ERROR_ALREADY_EXISTS)
	{
		MsgBox(L"An instance of the program is already running.", APPNAME L" Error", MB_OK | MB_ICONERROR);
		goto Exit;
	}
	if ((NtDll = GetModuleHandleW(L"ntdll.dll")) == NULL)
	{
		MsgBox(L"Unable to locate ntdll.dll!\nError: 0x%08lx", APPNAME L" Error", MB_OK | MB_ICONERROR, GetLastError());
		goto Exit;
	}
	if ((NtSuspendProcess = (_NtSuspendProcess)((void*)GetProcAddress(NtDll, "NtSuspendProcess"))) == NULL)
	{
		MsgBox(L"Unable to locate the NtSuspendProcess procedure in the ntdll.dll module!", APPNAME L" Error", MB_OK | MB_ICONERROR);
		goto Exit;
	}
	if ((NtResumeProcess = (_NtResumeProcess)((void*)GetProcAddress(NtDll, "NtResumeProcess"))) == NULL)
	{
		MsgBox(L"Unable to locate the NtResumeProcess procedure in the ntdll.dll module!", APPNAME L" Error", MB_OK | MB_ICONERROR);
		goto Exit;
	}

	// There will be no visible window either way, but in one case, there will be a system tray icon,
	// and in the other case there will be no icon. This is because someone requested that I make this
	// app work even when the user has no shell (explorer.exe) or has replaced their shell with an alternative shell.
	// The Windows system tray API obviously won't work if there is no Windows system tray. I don't know if the user's
	// shell even has a taskbar so I'm skipping that too.

	// A hidden top-level window is required to receive WM_POWERBROADCAST (sleep/wake)
	// notifications and the cursor idle-check timer. So we create the window if the
	// tray icon, the PauseOnSleep feature, or the idle-cursor feature is enabled. The
	// system tray icon itself is only added when TrayIcon is enabled.
	HWND HWnd = NULL;
	if (gConfig.TrayIcon || gConfig.PauseOnSleep || gConfig.HideIdleCursor)
	{
		WNDCLASSW WndClass = { 0 };

		WndClass.hInstance = Instance;
		WndClass.lpszClassName = APPNAME L"_WndClass";
		WndClass.lpfnWndProc = SysTrayCallback;

		if (RegisterClassW(&WndClass) == 0)
		{
			MsgBox(L"Failed to register WindowClass! Error 0x%08lx", APPNAME L" Error", MB_OK | MB_ICONERROR, GetLastError());
			goto Exit;
		}
	
		HWnd = CreateWindowExW(		
			WS_EX_TOOLWINDOW,		
			WndClass.lpszClassName,
			APPNAME L"_Systray_Window",
			WS_ICONIC,
			CW_USEDEFAULT,
			CW_USEDEFAULT,
			CW_USEDEFAULT,
			CW_USEDEFAULT,
			0,
			0,
			Instance,
			0);
	
		if (HWnd == NULL)
		{
			MsgBox(L"Failed to create window! Error 0x%08lx", APPNAME L" Error", MB_OK | MB_ICONERROR, GetLastError());
			goto Exit;
		}

		if (gConfig.TrayIcon)
		{
			gTrayNotifyIconData.cbSize = sizeof(NOTIFYICONDATA);
			gTrayNotifyIconData.hWnd = HWnd;	
			gTrayNotifyIconData.uID = 1982;
			gTrayNotifyIconData.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
			gTrayNotifyIconData.uCallbackMessage = WM_TRAYICON;
			wcscpy_s(gTrayNotifyIconData.szTip, _countof(gTrayNotifyIconData.szTip), APPNAME L" v" VERSION);
			gTrayNotifyIconData.hIcon = (HICON)LoadImageW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDI_ICON1), IMAGE_ICON, 0, 0, 0);

			if (gTrayNotifyIconData.hIcon == NULL)
			{
				MsgBox(L"Failed to load systray icon resource!", APPNAME L" Error", MB_OK | MB_ICONERROR);
				goto Exit;
			}

			if (Shell_NotifyIconW(NIM_ADD, &gTrayNotifyIconData) == FALSE)
			{
				MsgBox(L"Failed to register systray icon!", APPNAME L" Error", MB_OK | MB_ICONERROR);
				goto Exit;
			}
		}
	}

	if (RegisterHotKey(NULL, 1, MOD_NOREPEAT | gConfig.PauseKeyModifiers, gConfig.PauseKey) == 0)
	{
		MsgBox(L"Failed to register hotkey! Error 0x%08lx", APPNAME L" Error", MB_OK | MB_ICONERROR, GetLastError());
		goto Exit;
	}

	DbgPrint(L"Registered hotkey 0x%x with modifiers 0x%x.", gConfig.PauseKey, gConfig.PauseKeyModifiers);

	// The Game Bar widget can toggle pausing by signaling a shared named event. This
	// is useful in the Xbox full-screen experience / Game Bar, where you can focus the
	// widget and press A to pause from the Game Bar UI. Create that event so the widget
	// can toggle pausing.
	if (gConfig.WidgetPause)
	{
		gPauseSignalEvent = CreatePauseSignalEvent();
		RegisterWidgetLaunchProtocol();
		UpdateWidgetState();
	}

	// Add or remove this app from the per-user startup (HKCU Run) key so it launches
	// automatically when the user signs in, according to the Autostart registry setting.
	UpdateAutostartRegistration();

	u32 LastWidgetCheckTick = 0;

	// Track foreground changes via an event hook instead of polling GetForegroundWindow()
	// every tick, so the main loop can block instead of busy-spinning. The callback runs
	// out-of-context on this thread while it pumps messages (no injected DLL / extra thread).
	// It is also used by the idle-cursor feature to re-hide the pointer on app switches.
	if (gConfig.PauseOnSleep || (gConfig.HideIdleCursor && gConfig.HideCursorOnAppSwitch))
	{
		gForegroundHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
			NULL, ForegroundChangeProc, 0, 0,
			WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

		if (gForegroundHook == NULL)
		{
			// Graceful degradation: without the hook we won't track foreground changes,
			// so on sleep we fall back to whatever GetForegroundWindow() returns then.
			DbgPrint(L"SetWinEventHook(EVENT_SYSTEM_FOREGROUND) failed; foreground tracking disabled.");
		}

		// The hook only fires on *changes*, so seed the current foreground process now.
		// Otherwise, if a game is already in the foreground when we start and the user
		// sleeps without ever switching windows, gLastForegroundProcessId would stay 0
		// and nothing would be auto-paused on sleep.
		HWND InitialForeground = GetForegroundWindow();
		if (InitialForeground != NULL)
		{
			u32 InitialProcessId = 0;
			GetWindowThreadProcessId(InitialForeground, &InitialProcessId);
			if (InitialProcessId != 0 &&
				InitialProcessId != GetCurrentProcessId() &&
				!IsGameBarProcessId(InitialProcessId))
			{
				gLastForegroundProcessId = InitialProcessId;
			}
		}
	}

	// Idle-cursor feature: install a global low-level mouse hook and start the idle
	// timer. The hook shows the pointer only on genuine movement (physical or
	// gamepad-driven); the timer re-hides it after CursorIdleTimeoutMs of stillness.
	if (gConfig.HideIdleCursor)
	{
		gMouseHook = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc, GetModuleHandleW(NULL), 0);
		if (gMouseHook == NULL)
		{
				DbgPrint(L"Failed to install low-level mouse hook! Error 0x%08lx; idle-cursor disabled.", GetLastError());
		}
		else
		{
				DbgPrint(L"Idle-cursor enabled. TimeoutMs=%d MoveThreshold=%d OnAppSwitch=%d.",
					gConfig.CursorIdleTimeoutMs, gConfig.CursorMoveThreshold, gConfig.HideCursorOnAppSwitch);

				gLastRealMoveTick = GetTickCount();
				SetTimer(HWnd, IDT_CURSOR_IDLE_TIMER, 250, NULL);
				HideCursorNow();
		}
	}

	while (gIsRunning)
	{
		// Block until something we care about happens: a window/thread message
		// (WM_HOTKEY, WM_POWERBROADCAST, WM_TRAYICON, or a WinEvent callback), the
		// Game Bar widget's pause signal, or the widget-state recheck timeout. This
		// replaces the old Sleep(5) busy-poll so an idle app parks the CPU.
		DWORD HandleCount = 0;
		HANDLE WaitHandles[1];
		if (gPauseSignalEvent != NULL)
		{
			WaitHandles[HandleCount++] = gPauseSignalEvent;
		}

		DWORD Timeout = INFINITE;
		if (gConfig.WidgetPause)
		{
			u32 Elapsed = GetTickCount() - LastWidgetCheckTick;
			Timeout = (Elapsed >= 5000) ? 0 : (5000 - Elapsed);
		}

		DWORD WaitResult = MsgWaitForMultipleObjectsEx(HandleCount, WaitHandles, Timeout,
			QS_ALLINPUT, MWMO_INPUTAVAILABLE);

		if (WaitResult == WAIT_OBJECT_0 + HandleCount)
		{
			// Window/thread messages are available. Draining them dispatches
			// WM_POWERBROADCAST (SysTrayCallback) and the WinEvent foreground callback.
			while (PeekMessageW(&WndMsg, NULL, 0, 0, PM_REMOVE))
			{
				if (WndMsg.message == WM_HOTKEY)
				{
					HandlePauseKeyPress();
				}

				DispatchMessageW(&WndMsg);
			}
		}
		else if (HandleCount == 1 && WaitResult == WAIT_OBJECT_0)
		{
			// The Game Bar widget signaled its pause event, so pause/un-pause works
			// from the Game Bar UI even in the Xbox full-screen experience.
			DbgPrint(L"Pause signal received from Game Bar widget.");
			HandlePauseKeyPress();
		}
		else if (WaitResult == WAIT_FAILED)
		{
			// Avoid a hot error-loop if the wait keeps failing.
			DbgPrint(L"MsgWaitForMultipleObjectsEx failed! Error 0x%08lx", GetLastError());
			Sleep(50);
		}
		// WAIT_TIMEOUT falls through to the widget-state check below.

		// The widget state file lives in the widget package's LocalState folder, which
		// only exists once the widget is installed. If the widget is installed (or
		// reinstalled) after we start, republish the file so the widget shows we're
		// running instead of "Start" — but only when it's actually missing, so we don't
		// write to disk on every recheck while idle.
		if (gConfig.WidgetPause)
		{
			u32 NowTick = GetTickCount();
			if (NowTick - LastWidgetCheckTick >= 5000)
			{
				LastWidgetCheckTick = NowTick;
				RepublishWidgetStateIfMissing();
			}
		}
	}

Exit:
	if (gMouseHook != NULL)
	{
		UnhookWindowsHookEx(gMouseHook);
		gMouseHook = NULL;
	}

	// Never leave the system with an invisible pointer.
	ShowCursorNow();

	if (gBlankCursor != NULL)
	{
		DestroyCursor(gBlankCursor);
		gBlankCursor = NULL;
	}

	if (gForegroundHook != NULL)
	{
		UnhookWinEvent(gForegroundHook);
		gForegroundHook = NULL;
	}

	if (gConfig.WidgetPause)
	{
		// Remove the widget state file so the Game Bar widget correctly shows the
		// "not running / Start" state once we've exited.
		DeleteWidgetStateFile();
	}

	if (gPauseSignalEvent != NULL)
	{
		CloseHandle(gPauseSignalEvent);
		gPauseSignalEvent = NULL;
	}

	return(0);
}

// Out-of-context WinEvent callback that runs on our own thread while it pumps
// messages, so it needs no injected DLL or extra thread. It records the last
// "real" foreground process (excluding our own and the Xbox Game Bar overlay)
// so that if the system sleeps we know which game process to auto-pause even
// after the foreground window has changed to the lock screen / LogonUI. This
// replaces the old per-tick GetForegroundWindow() poll in the main loop.
void CALLBACK ForegroundChangeProc(HWINEVENTHOOK Hook, DWORD Event, HWND Window, LONG IdObject, LONG IdChild, DWORD IdEventThread, DWORD EventTime)
{
	UNREFERENCED_PARAMETER(Hook);
	UNREFERENCED_PARAMETER(IdObject);
	UNREFERENCED_PARAMETER(IdChild);
	UNREFERENCED_PARAMETER(IdEventThread);
	UNREFERENCED_PARAMETER(EventTime);

	if (Event != EVENT_SYSTEM_FOREGROUND || Window == NULL)
	{
		return;
	}

	// Switching apps makes Windows reveal the pointer over the newly active window.
	// Re-hide it at once so it doesn't linger until the idle timeout elapses.
	if (gConfig.HideIdleCursor && gConfig.HideCursorOnAppSwitch)
	{
		// Backdate the idle clock so the pointer stays hidden until real movement.
		gLastRealMoveTick = GetTickCount() - gConfig.CursorIdleTimeoutMs;
		HideCursorNow();
	}

	if (gIsPaused)
	{
		return;
	}

	u32 ForegroundProcessId = 0;
	GetWindowThreadProcessId(Window, &ForegroundProcessId);
	if (ForegroundProcessId != 0 &&
		ForegroundProcessId != GetCurrentProcessId() &&
		ForegroundProcessId != gLastForegroundProcessId &&
		!IsGameBarProcessId(ForegroundProcessId))
	{
		gLastForegroundProcessId = ForegroundProcessId;
	}
}

// The heart of the idle-cursor feature: show the pointer only when it is really
// moved, whether by a physical mouse or a gamepad-driven mapping. Movement is judged
// by actual position change so the zero-delta synthetic WM_MOUSEMOVE that Windows
// emits when you switch foreground apps can't summon the pointer.
LRESULT CALLBACK LowLevelMouseProc(_In_ int Code, _In_ WPARAM WParam, _In_ LPARAM LParam)
{
	if (Code == HC_ACTION)
	{
		const MSLLHOOKSTRUCT* Info = (const MSLLHOOKSTRUCT*)LParam;
		BOOL Activity = FALSE;

		if (WParam == WM_MOUSEMOVE)
		{
			// Only count moves that change the pointer position by more than the
			// threshold. This filters the app-switch synthetic move (zero delta) and
			// tiny jitter, while genuine mouse/gamepad motion passes through.
			if (gHaveLastMousePos)
			{
				LONG dx = Info->pt.x - gLastMousePos.x;
				LONG dy = Info->pt.y - gLastMousePos.y;
				if (dx < 0) dx = -dx;
				if (dy < 0) dy = -dy;
				if ((u32)dx > gConfig.CursorMoveThreshold || (u32)dy > gConfig.CursorMoveThreshold)
				{
					Activity = TRUE;
				}
			}
			gLastMousePos = Info->pt;
			gHaveLastMousePos = TRUE;
		}
		else if (WParam == WM_LBUTTONDOWN || WParam == WM_RBUTTONDOWN ||
				 WParam == WM_MBUTTONDOWN || WParam == WM_XBUTTONDOWN ||
				 WParam == WM_MOUSEWHEEL || WParam == WM_MOUSEHWHEEL)
		{
			// A click or scroll is always intentional activity.
			Activity = TRUE;
		}

		if (Activity)
		{
			gLastRealMoveTick = GetTickCount();
			if (gIsCursorHidden)
			{
				ShowCursorNow();
			}
		}
	}

	return(CallNextHookEx(NULL, Code, WParam, LParam));
}

// Lazily builds a single fully-transparent cursor sized to the system cursor
// dimensions. Returns TRUE if gBlankCursor is available.
BOOL EnsureBlankCursor(void)
{
	if (gBlankCursor != NULL)
	{
		return(TRUE);
	}

	int Width = GetSystemMetrics(SM_CXCURSOR);
	int Height = GetSystemMetrics(SM_CYCURSOR);
	if (Width <= 0 || Height <= 0)
	{
		Width = 32;
		Height = 32;
	}

	size_t MaskBytes = ((size_t)Width * (size_t)Height) / 8;
	BYTE* AndMask = (BYTE*)malloc(MaskBytes);
	BYTE* XorMask = (BYTE*)malloc(MaskBytes);
	if (AndMask == NULL || XorMask == NULL)
	{
		free(AndMask);
		free(XorMask);
		DbgPrint(L"Failed to allocate cursor masks.");
		return(FALSE);
	}

	// AND = 0xFF, XOR = 0x00 -> the screen shows through everywhere (invisible cursor).
	memset(AndMask, 0xFF, MaskBytes);
	memset(XorMask, 0x00, MaskBytes);

	gBlankCursor = CreateCursor(GetModuleHandleW(NULL), 0, 0, Width, Height, AndMask, XorMask);

	free(AndMask);
	free(XorMask);

	if (gBlankCursor == NULL)
	{
		DbgPrint(L"CreateCursor failed! Error 0x%08lx", GetLastError());
		return(FALSE);
	}

	return(TRUE);
}

// Replace every system cursor with the transparent one. SetSystemCursor takes
// ownership of (and destroys) the handle it is given, so each call gets its own copy.
void HideCursorNow(void)
{
	if (gIsCursorHidden || !gConfig.HideIdleCursor)
	{
		return;
	}
	if (!EnsureBlankCursor())
	{
		return;
	}

	for (u32 i = 0; i < _countof(gOcrIds); i++)
	{
		HCURSOR Copy = CopyCursor(gBlankCursor);
		if (Copy != NULL)
		{
			SetSystemCursor(Copy, gOcrIds[i]);
		}
	}

	gIsCursorHidden = TRUE;
	DbgPrint(L"Cursor hidden.");
}

// Restore the user's normal system cursors by reloading them from the registry.
void ShowCursorNow(void)
{
	if (!gIsCursorHidden)
	{
		return;
	}

	SystemParametersInfoW(SPI_SETCURSORS, 0, NULL, SPIF_SENDCHANGE);
	gIsCursorHidden = FALSE;
	DbgPrint(L"Cursor shown (real movement).");
}


void HandlePauseKeyPress(void)
{
	u32 ProcessId = 0;

	// Toggle: if something is currently paused, un-pause it.
	if (gIsPaused)
	{
		UnpausePreviouslyPausedProcess();
		return;
	}

	// Either we configured it to pause a specified process, or we will pause
	// the currently in-focus foreground window.
	if (wcslen(gConfig.ProcessNameToPause) > 0)
	{
		DbgPrint(L"Pause key pressed. Attempting to pause named process %s...", gConfig.ProcessNameToPause);
		ProcessId = FindProcessIdByName(gConfig.ProcessNameToPause);
		if (ProcessId == 0)
		{
			DbgPrint(L"Unable to locate any process with the name %s!", gConfig.ProcessNameToPause);
			return;
		}
	}
	else
	{
		DbgPrint(L"Pause key pressed. Attempting to pause current foreground window...");
		HWND ForegroundWindow = GetForegroundWindow();
		if (ForegroundWindow == NULL)
		{
			MsgBox(L"Failed to detect foreground window!", APPNAME L" Error", MB_OK | MB_ICONERROR);
			return;
		}
		GetWindowThreadProcessId(ForegroundWindow, &ProcessId);
		if (ProcessId == 0)
		{
			MsgBox(L"Failed to get PID from foreground window! Error code 0x%08lx", APPNAME L" Error", MB_OK | MB_ICONERROR, GetLastError());
			return;
		}
	}

	PauseProcessById(ProcessId);
}

// Searches the running processes for one matching the given executable name.
// Returns the PID of the first match, or 0 if not found.
u32 FindProcessIdByName(const wchar_t* ProcessName)
{
	HANDLE ProcessSnapshot = NULL;
	PROCESSENTRY32W ProcessEntry = { sizeof(PROCESSENTRY32W) };
	u32 ProcessId = 0;

	ProcessSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (ProcessSnapshot == INVALID_HANDLE_VALUE)
	{
		MsgBox(L"Failed to create snapshot of running processes! Error 0x%08lx", APPNAME L" Error", MB_OK | MB_ICONERROR, GetLastError());
		return(0);
	}

	if (Process32FirstW(ProcessSnapshot, &ProcessEntry) == FALSE)
	{
		MsgBox(L"Failed to retrieve list of running processes! Error 0x%08lx", APPNAME L" Error", MB_OK | MB_ICONERROR, GetLastError());
		CloseHandle(ProcessSnapshot);
		return(0);
	}

	do
	{
		if (_wcsicmp(ProcessEntry.szExeFile, ProcessName) == 0)
		{
			ProcessId = ProcessEntry.th32ProcessID;
			DbgPrint(L"Found process %s with PID %d.", ProcessEntry.szExeFile, ProcessId);
			break;
		}
	} while (Process32NextW(ProcessSnapshot, &ProcessEntry));

	CloseHandle(ProcessSnapshot);
	return(ProcessId);
}

// Returns TRUE if the given PID belongs to a known Xbox Game Bar / overlay process.
// These processes can momentarily become the foreground window (e.g. when the user
// opens the Game Bar overlay with Win+G), and we don't want to accidentally record
// them as the "game" to auto-pause on sleep, nor suspend Game Bar itself.
BOOL IsGameBarProcessId(u32 ProcessId)
{
	// Known Xbox Game Bar related executable names.
	static const wchar_t* GameBarProcessNames[] =
	{
		L"GameBar.exe",
		L"GameBarFTServer.exe",
		L"GameBarElevatedFT_Alias.exe",
		L"GameBarPresenceWriter.exe",
		L"XboxGameBarWidgets.exe"
	};

	HANDLE ProcessSnapshot = NULL;
	PROCESSENTRY32W ProcessEntry = { sizeof(PROCESSENTRY32W) };
	BOOL IsGameBar = FALSE;

	if (ProcessId == 0)
	{
		return(FALSE);
	}

	ProcessSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (ProcessSnapshot == INVALID_HANDLE_VALUE)
	{
		return(FALSE);
	}

	if (Process32FirstW(ProcessSnapshot, &ProcessEntry))
	{
		do
		{
			if (ProcessEntry.th32ProcessID == ProcessId)
			{
				for (size_t i = 0; i < _countof(GameBarProcessNames); i++)
				{
					if (_wcsicmp(ProcessEntry.szExeFile, GameBarProcessNames[i]) == 0)
					{
						IsGameBar = TRUE;
						break;
					}
				}
				break;
			}
		} while (Process32NextW(ProcessSnapshot, &ProcessEntry));
	}

	CloseHandle(ProcessSnapshot);
	return(IsGameBar);
}

// Suspends all threads of the given process and records it as the currently paused process.
void PauseProcessById(u32 ProcessId)
{
	// NtSuspendProcess only needs PROCESS_SUSPEND_RESUME. Requesting the minimum
	// rights (instead of PROCESS_ALL_ACCESS) lets us pause processes we could not
	// otherwise fully open, and follows least-privilege.
	HANDLE ProcessHandle = OpenProcess(PROCESS_SUSPEND_RESUME, FALSE, ProcessId);
	if (ProcessHandle == NULL)
	{
		MsgBox(L"Failed to open process %d! Error 0x%08lx", APPNAME L" Error", MB_OK | MB_ICONERROR, ProcessId, GetLastError());
		return;
	}
	NtSuspendProcess(ProcessHandle);
	gIsPaused = TRUE;
	gPreviouslyPausedProcessId = ProcessId;
	GetProcessNameById(ProcessId, gPausedProcessName, _countof(gPausedProcessName));
	UpdateWidgetState();
	CloseHandle(ProcessHandle);
	DbgPrint(L"Process %d paused!", ProcessId);
}

// Called when the system is about to enter sleep/suspend. Automatically pauses the
// game process so that it is frozen while the machine sleeps and can be resumed on wake.
void HandleSystemSuspend(void)
{
	u32 ProcessId = 0;

	if (gConfig.PauseOnSleep == FALSE)
	{
		return;
	}

	// If something is already paused (e.g. the user paused manually), leave it as-is
	// and do not take ownership of the resume, so we don't accidentally un-pause on wake.
	if (gIsPaused)
	{
		DbgPrint(L"System suspending, but a process is already paused. Leaving it untouched.");
		return;
	}

	if (wcslen(gConfig.ProcessNameToPause) > 0)
	{
		DbgPrint(L"System suspending. Attempting to auto-pause named process %s...", gConfig.ProcessNameToPause);
		ProcessId = FindProcessIdByName(gConfig.ProcessNameToPause);
	}
	else
	{
		DbgPrint(L"System suspending. Attempting to auto-pause last foreground process %d...", gLastForegroundProcessId);
		ProcessId = gLastForegroundProcessId;
	}

	if (ProcessId == 0)
	{
		DbgPrint(L"No suitable process found to auto-pause on sleep.");
		return;
	}

	PauseProcessById(ProcessId);
	if (gIsPaused)
	{
		gPausedBySleep = TRUE;
		DbgPrint(L"Auto-paused process %d due to system sleep.", ProcessId);
	}
}

// Called when the system resumes from sleep. Automatically un-pauses the process that
// we paused on sleep (but not one the user paused manually).
void HandleSystemResume(void)
{
	if (gConfig.PauseOnSleep == FALSE)
	{
		return;
	}

	if (gIsPaused && gPausedBySleep)
	{
		DbgPrint(L"System resumed. Auto-un-pausing process %d.", gPreviouslyPausedProcessId);
		UnpausePreviouslyPausedProcess();
		gPausedBySleep = FALSE;
	}
}

// Creates the named event that lets the Xbox Game Bar widget toggle the pause state.
// The widget runs inside a UWP AppContainer, so the event's security descriptor grants
// EVENT_MODIFY_STATE | SYNCHRONIZE (0x100002) to ALL APPLICATION PACKAGES (AC) so the
// sandboxed widget can open and set it; the interactive user (WD) gets the same rights.
// A low mandatory-integrity label (S:(ML;;NW;;;LW)) is also applied so the low-integrity
// AppContainer can obtain write (EVENT_MODIFY_STATE) access; the AC DACL ACE alone is not
// sufficient under Mandatory Integrity Control. Returns the event handle, or NULL on failure.
HANDLE CreatePauseSignalEvent(void)
{
	SECURITY_ATTRIBUTES SecurityAttributes = { 0 };
	SecurityAttributes.nLength = sizeof(SecurityAttributes);
	SecurityAttributes.bInheritHandle = FALSE;

	if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
			L"D:(A;;0x100002;;;WD)(A;;0x100002;;;AC)S:(ML;;NW;;;LW)",
			SDDL_REVISION_1,
			&SecurityAttributes.lpSecurityDescriptor,
			NULL) == FALSE)
	{
		DbgPrint(L"Failed to build security descriptor for pause signal event! Error 0x%08lx", GetLastError());
		return(NULL);
	}

	// Auto-reset, initially non-signaled: reading it with WaitForSingleObject(..., 0)
	// consumes a single signal so each widget button press toggles pausing exactly once.
	HANDLE Event = CreateEventW(&SecurityAttributes, FALSE, FALSE, PAUSE_SIGNAL_EVENT_NAME);
	if (Event == NULL)
	{
		DbgPrint(L"Failed to create pause signal event! Error 0x%08lx", GetLastError());
	}
	else
	{
		DbgPrint(L"Created pause signal event '%s' for the Game Bar widget.", PAUSE_SIGNAL_EVENT_NAME);
	}

	LocalFree(SecurityAttributes.lpSecurityDescriptor);
	return(Event);
}

// Registers a private URI scheme (universalpausebutton:) under HKCU\Software\Classes that
// points at this executable, so the Game Bar widget can relaunch the app via
// Launcher.LaunchUriAsync when it isn't running. Registering under HKCU needs no admin.
void RegisterWidgetLaunchProtocol(void)
{
	wchar_t ExePath[MAX_PATH];
	if (GetModuleFileNameW(NULL, ExePath, _countof(ExePath)) == 0)
	{
		DbgPrint(L"Failed to get module file name for protocol registration! Error 0x%08lx", GetLastError());
		return;
	}

	wchar_t Command[MAX_PATH + 16];
	if (swprintf_s(Command, _countof(Command), L"\"%s\" \"%%1\"", ExePath) < 0)
	{
		return;
	}

	HKEY Key = NULL;
	// HKCU\Software\Classes\universalpausebutton
	if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Classes\\" WIDGET_LAUNCH_PROTOCOL,
			0, NULL, 0, KEY_WRITE, NULL, &Key, NULL) == ERROR_SUCCESS)
	{
		const wchar_t* Description = L"URL:" APPNAME L" Protocol";
		RegSetValueExW(Key, NULL, 0, REG_SZ, (const BYTE*)Description,
			(DWORD)((wcslen(Description) + 1) * sizeof(wchar_t)));
		// The presence of "URL Protocol" marks this key as a URI scheme handler.
		RegSetValueExW(Key, L"URL Protocol", 0, REG_SZ, (const BYTE*)L"", sizeof(wchar_t));
		RegCloseKey(Key);
	}

	// HKCU\Software\Classes\universalpausebutton\shell\open\command
	if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Classes\\" WIDGET_LAUNCH_PROTOCOL L"\\shell\\open\\command",
			0, NULL, 0, KEY_WRITE, NULL, &Key, NULL) == ERROR_SUCCESS)
	{
		RegSetValueExW(Key, NULL, 0, REG_SZ, (const BYTE*)Command,
			(DWORD)((wcslen(Command) + 1) * sizeof(wchar_t)));
		RegCloseKey(Key);
		DbgPrint(L"Registered '%s:' launch protocol -> %s", WIDGET_LAUNCH_PROTOCOL, ExePath);
	}
	else
	{
		DbgPrint(L"Failed to register launch protocol command key! Error 0x%08lx", GetLastError());
	}
}

// Registers or unregisters the app in the per-user "Run" key
// (HKCU\Software\Microsoft\Windows\CurrentVersion\Run) so Windows starts it
// automatically at sign-in. Controlled by the Autostart registry setting.
// NOTE: On the Xbox full-screen experience (FSE) handheld shell, classic logon
// autostart (Run keys, Startup folder, logon tasks) is deferred until you exit to
// the desktop; only the FSE "launch at sign in" list starts apps at FSE login.
void UpdateAutostartRegistration(void)
{
	HKEY Key = NULL;
	if (RegOpenKeyExW(HKEY_CURRENT_USER,
			L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
			0, KEY_WRITE, &Key) != ERROR_SUCCESS)
	{
		DbgPrint(L"Failed to open Run key for autostart! Error 0x%08lx", GetLastError());
		return;
	}

	if (gConfig.Autostart)
	{
		wchar_t ExePath[MAX_PATH];
		if (GetModuleFileNameW(NULL, ExePath, _countof(ExePath)) == 0)
		{
			DbgPrint(L"Failed to get module file name for autostart! Error 0x%08lx", GetLastError());
			RegCloseKey(Key);
			return;
		}

		// Quote the path in case it contains spaces.
		wchar_t Command[MAX_PATH + 2];
		if (swprintf_s(Command, _countof(Command), L"\"%s\"", ExePath) < 0)
		{
			RegCloseKey(Key);
			return;
		}

		if (RegSetValueExW(Key, APPNAME, 0, REG_SZ, (const BYTE*)Command,
				(DWORD)((wcslen(Command) + 1) * sizeof(wchar_t))) == ERROR_SUCCESS)
		{
			DbgPrint(L"Autostart enabled -> %s", Command);
		}
		else
		{
			DbgPrint(L"Failed to write autostart Run value! Error 0x%08lx", GetLastError());
		}
	}
	else
	{
		LONG DeleteResult = RegDeleteValueW(Key, APPNAME);
		if (DeleteResult == ERROR_SUCCESS)
		{
			DbgPrint(L"Autostart disabled; removed Run value.");
		}
		else if (DeleteResult != ERROR_FILE_NOT_FOUND)
		{
			DbgPrint(L"Failed to remove autostart Run value! Error 0x%08lx", DeleteResult);
		}
	}

	RegCloseKey(Key);
}

// Resolves the Game Bar widget package's LocalState folder (which has a publisher-hash
// suffix on the package name) and writes the full path of the state file into PathOut.
// Returns TRUE if the folder was found. The folder exists only once the widget package
// has been installed for the current user.
static BOOL GetWidgetStateFilePath(wchar_t* PathOut, size_t PathOutCount)
{
	// The widget package's LocalState folder name carries a stable publisher-hash suffix,
	// so once we resolve the full path we cache it and skip re-enumerating the Packages
	// directory on every subsequent call.
	static wchar_t CachedPath[MAX_PATH] = { 0 };
	if (CachedPath[0] != L'\0')
	{
		wcsncpy_s(PathOut, PathOutCount, CachedPath, _TRUNCATE);
		return(TRUE);
	}

	wchar_t LocalAppData[MAX_PATH];
	if (FAILED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, LocalAppData)))
	{
		return(FALSE);
	}

	wchar_t SearchPattern[MAX_PATH];
	if (swprintf_s(SearchPattern, _countof(SearchPattern),
			L"%s\\Packages\\%s*", LocalAppData, WIDGET_PACKAGE_NAME_PREFIX) < 0)
	{
		return(FALSE);
	}

	WIN32_FIND_DATAW FindData = { 0 };
	HANDLE FindHandle = FindFirstFileW(SearchPattern, &FindData);
	if (FindHandle == INVALID_HANDLE_VALUE)
	{
		return(FALSE);
	}

	BOOL Found = FALSE;
	do
	{
		if (FindData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
		{
			if (swprintf_s(PathOut, PathOutCount, L"%s\\Packages\\%s\\LocalState\\%s",
					LocalAppData, FindData.cFileName, WIDGET_STATE_FILE_NAME) >= 0)
			{
				Found = TRUE;
				wcsncpy_s(CachedPath, _countof(CachedPath), PathOut, _TRUNCATE);
			}
			break;
		}
	} while (FindNextFileW(FindHandle, &FindData));

	FindClose(FindHandle);
	return(Found);
}

// Publishes the current pause state and paused process name so the Game Bar widget can
// reflect them. Writes a tiny UTF-16 file into the widget package's LocalState folder:
//   line 1: "0" (running) or "1" (paused)
//   line 2: the paused process's executable name (empty when running)
// The widget reads this via ApplicationData.Current.LocalFolder. Safe to call anytime;
// silently does nothing if the widget package isn't installed.
void UpdateWidgetState(void)
{
	if (gConfig.WidgetPause == FALSE)
	{
		return;
	}

	wchar_t FilePath[MAX_PATH];
	if (GetWidgetStateFilePath(FilePath, _countof(FilePath)) == FALSE)
	{
		return;
	}

	wchar_t Contents[160];
	int Length = swprintf_s(Contents, _countof(Contents), L"%d\n%s",
		gIsPaused ? 1 : 0, gIsPaused ? gPausedProcessName : L"");
	if (Length < 0)
	{
		return;
	}

	HANDLE File = CreateFileW(FilePath, GENERIC_WRITE, FILE_SHARE_READ, NULL,
		CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (File == INVALID_HANDLE_VALUE)
	{
		DbgPrint(L"Failed to write widget state file '%s'! Error 0x%08lx", FilePath, GetLastError());
		return;
	}

	// Write a UTF-16 LE BOM followed by the text so the widget can read it as Unicode.
	static const unsigned char Bom[2] = { 0xFF, 0xFE };
	DWORD Written = 0;
	WriteFile(File, Bom, sizeof(Bom), &Written, NULL);
	WriteFile(File, Contents, (DWORD)(Length * sizeof(wchar_t)), &Written, NULL);
	CloseHandle(File);
}

// Called periodically from the main loop. The widget state file lives in the widget
// package's LocalState folder, which only exists once the widget is installed. If the
// widget is installed (or reinstalled) after we start, that folder appears with no state
// file, so the widget would show "Start" until the next pause toggle. Republish it, but
// only when the file is actually missing, so we don't rewrite it on every tick while idle.
void RepublishWidgetStateIfMissing(void)
{
	if (gConfig.WidgetPause == FALSE)
	{
		return;
	}

	wchar_t FilePath[MAX_PATH];
	if (GetWidgetStateFilePath(FilePath, _countof(FilePath)) == FALSE)
	{
		return; // Widget not installed yet; nothing to publish.
	}

	if (GetFileAttributesW(FilePath) == INVALID_FILE_ATTRIBUTES)
	{
		UpdateWidgetState();
	}
}

// Deletes the widget state file (best effort) so the Game Bar widget shows the
// "not running / Start" state after the app exits.
void DeleteWidgetStateFile(void)
{
	wchar_t FilePath[MAX_PATH];
	if (GetWidgetStateFilePath(FilePath, _countof(FilePath)))
	{
		DeleteFileW(FilePath);
	}
}

// Looks up the executable name (e.g. "game.exe") for the given PID and copies it into
// Buffer. Buffer is set to an empty string if the process can't be found.
void GetProcessNameById(u32 ProcessId, wchar_t* Buffer, size_t BufferCount)
{
	HANDLE ProcessSnapshot = NULL;
	PROCESSENTRY32W ProcessEntry = { sizeof(PROCESSENTRY32W) };

	if (Buffer == NULL || BufferCount == 0)
	{
		return;
	}
	Buffer[0] = L'\0';

	ProcessSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (ProcessSnapshot == INVALID_HANDLE_VALUE)
	{
		return;
	}

	if (Process32FirstW(ProcessSnapshot, &ProcessEntry))
	{
		do
		{
			if (ProcessEntry.th32ProcessID == ProcessId)
			{
				wcsncpy_s(Buffer, BufferCount, ProcessEntry.szExeFile, _TRUNCATE);
				break;
			}
		} while (Process32NextW(ProcessSnapshot, &ProcessEntry));
	}

	CloseHandle(ProcessSnapshot);
}

void UnpausePreviouslyPausedProcess(void)
{	
	HANDLE ProcessHandle = NULL;
	DbgPrint(L"Pause key pressed. Attempting to un-pause previously paused PID %d.", gPreviouslyPausedProcessId);

	// NtResumeProcess only needs PROCESS_SUSPEND_RESUME (see PauseProcessById).
	ProcessHandle = OpenProcess(PROCESS_SUSPEND_RESUME, FALSE, gPreviouslyPausedProcessId);
	if (ProcessHandle == NULL)
	{
		// Maybe the previously paused process was killed, no longer exists?
		MsgBox(L"Failed to open process %d! Error 0x%08lx", APPNAME L" Error", MB_OK | MB_ICONERROR, gPreviouslyPausedProcessId, GetLastError());		
	}
	else
	{
		NtResumeProcess(ProcessHandle);
		DbgPrint(L"Un-pause successful.");
	}	
	gIsPaused = FALSE;
	gPreviouslyPausedProcessId = 0;
	gPausedProcessName[0] = L'\0';
	UpdateWidgetState();
	if (ProcessHandle)
	{
		CloseHandle(ProcessHandle);
	}
}

u32 LoadRegistrySettings(void)
{
	u32 Result = ERROR_SUCCESS;
	HKEY RegKey = NULL;

	typedef struct _REG_SETTING
	{
		wchar_t* Name;
		u32 DataType;
		void* DefaultValue;
		void* MinValue;
		void* MaxValue;
		void* Destination;
	} REG_SETTING;

	REG_SETTING Settings[] = {
		{	// Debug should always be the first setting loaded.
			.Name = L"Debug",
			.DataType = REG_DWORD,
			.DefaultValue = &(u32) { 0 },
			.MinValue = &(u32) { 0 },
			.MaxValue = &(u32) { 1 },
			.Destination = &gConfig.Debug
		},
		{
			.Name = L"TrayIcon",
			.DataType = REG_DWORD,
			.DefaultValue = &(u32) { 1 },
			.MinValue = &(u32) { 0 },
			.MaxValue = &(u32) { 1 },
			.Destination = &gConfig.TrayIcon
		},
		{
			.Name = L"PauseKey",
			.DataType = REG_DWORD,
			.DefaultValue = &(u32) { 'P' },
			.MinValue = &(u32) { 1 },
			.MaxValue = &(u32) { 0xFE },
			.Destination = &gConfig.PauseKey
		},
		{
			.Name = L"PauseKeyModifiers",
			.DataType = REG_DWORD,
			.DefaultValue = &(u32) { MOD_CONTROL | MOD_SHIFT },
			.MinValue = &(u32) { 0 },
			.MaxValue = &(u32) { 0x000F },  // MOD_ALT|MOD_CONTROL|MOD_SHIFT|MOD_WIN
			.Destination = &gConfig.PauseKeyModifiers
		},
		{
			.Name = L"ProcessNameToPause",
			.DataType = REG_SZ,
			.DefaultValue = &(wchar_t[128]) { L"" },
			.MinValue = NULL,
			.MaxValue = NULL,
			.Destination = &gConfig.ProcessNameToPause
		},
		{
			.Name = L"PauseOnSleep",
			.DataType = REG_DWORD,
			.DefaultValue = &(u32) { 1 },
			.MinValue = &(u32) { 0 },
			.MaxValue = &(u32) { 1 },
			.Destination = &gConfig.PauseOnSleep
		},
		{
			.Name = L"WidgetPause",
			.DataType = REG_DWORD,
			.DefaultValue = &(u32) { 1 },
			.MinValue = &(u32) { 0 },
			.MaxValue = &(u32) { 1 },
			.Destination = &gConfig.WidgetPause
		},
		{
			.Name = L"Autostart",
			.DataType = REG_DWORD,
			.DefaultValue = &(u32) { 0 },
			.MinValue = &(u32) { 0 },
			.MaxValue = &(u32) { 1 },
			.Destination = &gConfig.Autostart
		},
		{
			.Name = L"HideIdleCursor",
			.DataType = REG_DWORD,
			.DefaultValue = &(u32) { 1 },
			.MinValue = &(u32) { 0 },
			.MaxValue = &(u32) { 1 },
			.Destination = &gConfig.HideIdleCursor
		},
		{
			.Name = L"CursorIdleTimeoutMs",
			.DataType = REG_DWORD,
			.DefaultValue = &(u32) { 2000 },
			.MinValue = &(u32) { 200 },
			.MaxValue = &(u32) { 60000 },
			.Destination = &gConfig.CursorIdleTimeoutMs
		},
		{
			.Name = L"CursorMoveThreshold",
			.DataType = REG_DWORD,
			.DefaultValue = &(u32) { 0 },
			.MinValue = &(u32) { 0 },
			.MaxValue = &(u32) { 200 },
			.Destination = &gConfig.CursorMoveThreshold
		},
		{
			.Name = L"HideCursorOnAppSwitch",
			.DataType = REG_DWORD,
			.DefaultValue = &(u32) { 1 },
			.MinValue = &(u32) { 0 },
			.MaxValue = &(u32) { 1 },
			.Destination = &gConfig.HideCursorOnAppSwitch
		},
	};

	Result = RegCreateKeyExW(HKEY_CURRENT_USER, L"SOFTWARE\\" APPNAME, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &RegKey, NULL);
	if (Result != ERROR_SUCCESS)
	{
		MsgBox(L"Failed to load registry settings!\nError: 0x%08lx", L"Error", MB_OK | MB_ICONERROR, Result);
		goto Exit;
	}

	for (u32 s = 0; s < _countof(Settings); s++)
	{
		switch (Settings[s].DataType)
		{
			case REG_DWORD:
			{
				u32 BytesRead = sizeof(u32);
				Result = RegGetValueW(
					RegKey,
					NULL,
					Settings[s].Name,
					RRF_RT_DWORD,
					NULL,
					Settings[s].Destination,
					&BytesRead);
				if (Result != ERROR_SUCCESS)
				{
					if (Result == ERROR_FILE_NOT_FOUND)
					{
						Result = ERROR_SUCCESS;
						*(u32*)Settings[s].Destination = *(u32*)Settings[s].DefaultValue;
					}
					else
					{
						MsgBox(L"Failed to load registry value '%s'!\nError: 0x%08lx", L"Error", MB_OK | MB_ICONERROR, Settings[s].Name, Result);
						goto Exit;
					}
				}
				else
				{
					if (*(u32*)Settings[s].Destination < *(u32*)Settings[s].MinValue || *(u32*)Settings[s].Destination > *(u32*)Settings[s].MaxValue)
					{
						MsgBox(L"Registry value '%s' was out of range! Using default of %d.", L"Error", MB_OK | MB_ICONWARNING, Settings[s].Name, *(u32*)Settings[s].DefaultValue);
						*(u32*)Settings[s].Destination = *(u32*)Settings[s].DefaultValue;
					}
				}

				// Enable the debug console as early as possible if configured.
				// This is so the debug console can report on the other registry settings.
				if (Settings[s].Destination == &gConfig.Debug)
				{
					if (gConfig.Debug)
					{
						if (AllocConsole() == FALSE)
						{
							MsgBox(L"Failed to allocate debug console!\nError: 0x%08lx", L"Error", MB_OK | MB_ICONERROR, GetLastError());
							goto Exit;
						}
						gDbgConsole = GetStdHandle(STD_OUTPUT_HANDLE);
						if (gDbgConsole == INVALID_HANDLE_VALUE)
						{
							MsgBox(L"Failed to get stdout debug console handle!\nError: 0x%08lx", L"Error", MB_OK | MB_ICONERROR, GetLastError());
							goto Exit;
						}
						DbgPrint(L"%s version %s.", APPNAME, VERSION);
						DbgPrint(L"To disable this debug console, delete the 'Debug' reg setting at HKCU\\SOFTWARE\\%s", APPNAME);
					}
				}

				DbgPrint(L"Using value 0n%d (0x%x) for registry setting '%s'.", *(u32*)Settings[s].Destination, *(u32*)Settings[s].Destination, Settings[s].Name);

				break;
			}
			case REG_SZ:
			{
				u32 BytesRead = 128 * sizeof(wchar_t);
				Result = RegGetValueW(
					RegKey,
					NULL,
					Settings[s].Name,
					RRF_RT_REG_SZ,
					NULL,
					Settings[s].Destination,
					&BytesRead);
				if (Result != ERROR_SUCCESS)
				{
					if (Result == ERROR_FILE_NOT_FOUND)
					{
						Result = ERROR_SUCCESS;						
					}
					else
					{
						MsgBox(L"Failed to load registry value '%s'!\nError: 0x%08lx", L"Error", MB_OK | MB_ICONERROR, Settings[s].Name, Result);
						goto Exit;
					}
				}

				DbgPrint(L"Using value '%s' for registry setting '%s'.", Settings[s].Destination, Settings[s].Name);

				break;
			}
			default:
			{
				MsgBox(L"Registry value '%s' was not of the expected data type!", L"Error", MB_OK | MB_ICONERROR, Settings[s].Name);
				goto Exit;
			}
		}
	}

Exit:
	if (RegKey != NULL)
	{
		RegCloseKey(RegKey);
	}
	return(Result);
}

void MsgBox(const wchar_t* Message, const wchar_t* Caption, u32 Flags, ...)
{
	wchar_t FormattedMessage[1024] = { 0 };
	va_list Args = NULL;

	va_start(Args, Flags);
	_vsnwprintf_s(FormattedMessage, _countof(FormattedMessage), _TRUNCATE, Message, Args);
	va_end(Args);
	DbgPrint(FormattedMessage);
	MessageBoxW(NULL, FormattedMessage, Caption, Flags);
}

void DbgPrint(const wchar_t* Message, ...)
{
	if (gConfig.Debug == FALSE || gDbgConsole == INVALID_HANDLE_VALUE)
	{
		return;
	}

	wchar_t FormattedMessage[1024] = { 0 };
	u32 MsgLen = 0;
	wchar_t TimeString[64] = { 0 };
	SYSTEMTIME Time;
	va_list Args = NULL;

	va_start(Args, Message);
	_vsnwprintf_s(FormattedMessage, _countof(FormattedMessage), _TRUNCATE, Message, Args);
	va_end(Args);

	MsgLen = (u32)wcslen(FormattedMessage);
	FormattedMessage[MsgLen] = '\n';
	FormattedMessage[MsgLen + 1] = '\0';

	GetLocalTime(&Time);
	_snwprintf_s(TimeString, _countof(TimeString), _TRUNCATE, L"[%02d.%02d.%04d %02d.%02d.%02d.%03d] ", Time.wMonth, Time.wDay, Time.wYear, Time.wHour, Time.wMinute, Time.wSecond, Time.wMilliseconds);
	WriteConsoleW(gDbgConsole, TimeString, (u32)wcslen(TimeString), NULL, NULL);
	WriteConsoleW(gDbgConsole, FormattedMessage, (u32)wcslen(FormattedMessage), NULL, NULL);	
}

LRESULT CALLBACK SysTrayCallback(_In_ HWND Window, _In_ UINT Message, _In_ WPARAM WParam, _In_ LPARAM LParam)
{
	LRESULT Result = 0;
	static BOOL QuitMessageBoxIsShowing = FALSE;

	switch (Message)
	{
		case WM_TIMER:
		{
			if (WParam == IDT_CURSOR_IDLE_TIMER && gConfig.HideIdleCursor)
			{
				if (!gIsCursorHidden && (GetTickCount() - gLastRealMoveTick) >= gConfig.CursorIdleTimeoutMs)
				{
					HideCursorNow();
				}
			}
			break;
		}
		case WM_TRAYICON:
		{
			if (!QuitMessageBoxIsShowing && (LParam == WM_LBUTTONDOWN || LParam == WM_RBUTTONDOWN || LParam == WM_MBUTTONDOWN))
			{
				QuitMessageBoxIsShowing = TRUE;
				// Show the real cursor so the confirmation dialog is usable.
				ShowCursorNow();
				if (MessageBox(Window, L"Quit " APPNAME L"?", L"Are you sure?", MB_YESNO | MB_ICONQUESTION | MB_SYSTEMMODAL) == IDYES)
				{
					Shell_NotifyIconW(NIM_DELETE, &gTrayNotifyIconData);
					gIsRunning = FALSE;
					PostQuitMessage(0);
				}
				else
				{
					QuitMessageBoxIsShowing = FALSE;
				}
			}
			break;
		}
		case WM_POWERBROADCAST:
		{
			switch (WParam)
			{
				case PBT_APMSUSPEND:
				{
					// System is about to enter a low-power (sleep/suspend) state.
					HandleSystemSuspend();
					break;
				}
				case PBT_APMRESUMEAUTOMATIC:
				case PBT_APMRESUMESUSPEND:
				{
					// System has resumed from a low-power state.
					HandleSystemResume();
					break;
				}
				default:
				{
					break;
				}
			}
			Result = TRUE;
			break;
		}
		default:
		{
			Result = DefWindowProcW(Window, Message, WParam, LParam);
			break;
		}
	}
	return(Result);
}