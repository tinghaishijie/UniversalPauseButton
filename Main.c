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

#include <Windows.h>
#include <stdio.h>
#include <TlHelp32.h>
#include <Xinput.h>
#include "Main.h"
#include "resource.h"
#include "Server.h"

#pragma comment(lib, "Xinput9_1_0.lib")

CONFIG gConfig;
HANDLE gDbgConsole = INVALID_HANDLE_VALUE;
BOOL gIsRunning = TRUE;
HANDLE gMutex;
NOTIFYICONDATA gTrayNotifyIconData;
BOOL gIsPaused;
u32 gPreviouslyPausedProcessId;
u32 gLastForegroundProcessId;
BOOL gPausedBySleep;


int WINAPI wWinMain(_In_ HINSTANCE Instance, _In_opt_ HINSTANCE PrevInstance, _In_ PWSTR CmdLine, _In_ int CmdShow)
{	
	UNREFERENCED_PARAMETER(PrevInstance);
	UNREFERENCED_PARAMETER(CmdLine);
	UNREFERENCED_PARAMETER(CmdShow);

	HMODULE NtDll = NULL;
	MSG WndMsg = { 0 };
	//HHOOK KeyboardHook = NULL;

	bool runningServer = false;

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
	// notifications. So we create the window if EITHER the tray icon OR the
	// PauseOnSleep feature is enabled. The system tray icon itself is only added
	// when TrayIcon is enabled.
	if (gConfig.TrayIcon || gConfig.PauseOnSleep)
	{
		WNDCLASSW WndClass = { 0 };
		HWND HWnd = NULL;

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

	runningServer = (gConfig.WebPort != 0);

	if (runningServer)
	{
		DbgPrint(L"Starting Server!");

		if (serve_start(gConfig.WebPort))
		{
			MsgBox(L"Failed to start webserver!", APPNAME L" Error", MB_OK | MB_ICONERROR);
			runningServer = false;
		}

		openWelcomePageInBrowser(gConfig.WebPort);
	}


	while (gIsRunning)
	{
		while (PeekMessageW(&WndMsg, NULL, 0, 0, PM_REMOVE))
		{
			if (WndMsg.message == WM_HOTKEY)
			{
				HandlePauseKeyPress();
			}

			DispatchMessageW(&WndMsg);
		}

		// Continuously remember the last "real" foreground process (excluding our own),
		// so that if the system sleeps we know which game process to auto-pause even if
		// the foreground window has already changed to the lock screen / LogonUI.
		if (gConfig.PauseOnSleep && !gIsPaused)
		{
			HWND ForegroundWindow = GetForegroundWindow();
			if (ForegroundWindow != NULL)
			{
				u32 ForegroundProcessId = 0;
				GetWindowThreadProcessId(ForegroundWindow, &ForegroundProcessId);
				// Only re-evaluate when the foreground process actually changes, since this
				// loop runs every ~5ms and the Game Bar check below takes a process snapshot.
				// Skip our own process and the Xbox Game Bar overlay (Win+G), so we don't
				// end up auto-pausing Game Bar instead of the game underneath it.
				if (ForegroundProcessId != 0 &&
					ForegroundProcessId != GetCurrentProcessId() &&
					ForegroundProcessId != gLastForegroundProcessId &&
					!IsGameBarProcessId(ForegroundProcessId))
				{
					gLastForegroundProcessId = ForegroundProcessId;
				}
			}
		}

		if (runningServer && serve_request(gIsPaused))
			HandlePauseKeyPress();

		// Poll the Xbox controller(s) for the pause combo (Back + Start + LT + RT).
		if (gConfig.ControllerPause && PollGamepadForPauseCombo())
		{
			HandlePauseKeyPress();
		}

		Sleep(5);
	}

Exit:
	if(runningServer)
		serve_stop();

	return(0);
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
	HANDLE ProcessHandle = OpenProcess(PROCESS_ALL_ACCESS, FALSE, ProcessId);
	if (ProcessHandle == NULL)
	{
		MsgBox(L"Failed to open process %d! Error 0x%08lx", APPNAME L" Error", MB_OK | MB_ICONERROR, ProcessId, GetLastError());
		return;
	}
	NtSuspendProcess(ProcessHandle);
	gIsPaused = TRUE;
	gPreviouslyPausedProcessId = ProcessId;
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

// Polls all connected Xbox controllers for the pause combo: Back + Start + LT + RT
// held simultaneously. Uses edge detection so it fires only once per press (on the
// transition from "not held" to "held"), not continuously while the buttons are down.
// Returns TRUE exactly once each time the combo becomes newly pressed.
BOOL PollGamepadForPauseCombo(void)
{
	static BOOL WasPressed = FALSE;
	BOOL IsPressedNow = FALSE;

	for (u32 i = 0; i < XUSER_MAX_COUNT; i++)
	{
		XINPUT_STATE State = { 0 };
		if (XInputGetState(i, &State) == ERROR_SUCCESS)
		{
			WORD Buttons = State.Gamepad.wButtons;
			BOOL BackAndStart = (Buttons & XINPUT_GAMEPAD_BACK) && (Buttons & XINPUT_GAMEPAD_START);
			BOOL BothTriggers = (State.Gamepad.bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD) &&
			                    (State.Gamepad.bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD);
			if (BackAndStart && BothTriggers)
			{
				IsPressedNow = TRUE;
				break;
			}
		}
	}

	BOOL JustPressed = (IsPressedNow && !WasPressed);
	if (JustPressed)
	{
		DbgPrint(L"Controller pause combo (Back+Start+LT+RT) detected.");
	}
	WasPressed = IsPressedNow;
	return(JustPressed);
}

void UnpausePreviouslyPausedProcess(void)
{	
	HANDLE ProcessHandle = NULL;
	DbgPrint(L"Pause key pressed. Attempting to un-pause previously paused PID %d.", gPreviouslyPausedProcessId);

	ProcessHandle = OpenProcess(PROCESS_ALL_ACCESS, FALSE, gPreviouslyPausedProcessId);
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
			.DefaultValue = &(u32) { VK_PAUSE },
			.MinValue = &(u32) { 1 },
			.MaxValue = &(u32) { 0xFE },
			.Destination = &gConfig.PauseKey
		},
		{
			.Name = L"PauseKeyModifiers",
			.DataType = REG_DWORD,
			.DefaultValue = &(u32) { 0 },
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
			.Name = L"WebPort",
			.DataType = REG_DWORD,
			.DefaultValue = &(u32) { 0 },
			.MinValue = &(u32) { 0 },
			.MaxValue = &(u32) { 65535 },
			.Destination = &gConfig.WebPort
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
			.Name = L"ControllerPause",
			.DataType = REG_DWORD,
			.DefaultValue = &(u32) { 1 },
			.MinValue = &(u32) { 0 },
			.MaxValue = &(u32) { 1 },
			.Destination = &gConfig.ControllerPause
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
		case WM_TRAYICON:
		{
			if (!QuitMessageBoxIsShowing && (LParam == WM_LBUTTONDOWN || LParam == WM_RBUTTONDOWN || LParam == WM_MBUTTONDOWN))
			{
				QuitMessageBoxIsShowing = TRUE;
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