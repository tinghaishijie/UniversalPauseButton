#pragma once

#pragma warning(disable:4820) // padding in structures

#define VERSION L"1.1.6"
#define APPNAME L"UniversalPauseButton"

// Name of the named event that the Xbox Game Bar widget sets to toggle the pause
// state. It lives in the Global namespace so the widget's UWP AppContainer and this
// desktop process can share it within the interactive session.
#define PAUSE_SIGNAL_EVENT_NAME L"Global\\UniversalPauseButtonToggle"

// The app publishes its current pause state (and paused process name) to a small file
// inside the Game Bar widget's own AppData LocalState folder, which the sandboxed UWP
// widget can read via ApplicationData.Current.LocalFolder without any capability. This
// is used instead of shared memory because a non-elevated desktop process cannot create
// a Global\ file-mapping section (that needs SeCreateGlobalPrivilege), whereas writing to
// the widget package's LocalState folder works for the interactive user.
// WIDGET_PACKAGE_NAME_PREFIX must match the <Identity Name="..."> in the widget's
// Package.appxmanifest; the actual folder has a publisher hash suffix we resolve at
// runtime. WIDGET_STATE_FILE_NAME must match the reader in MainWidget.xaml.cs.
#define WIDGET_PACKAGE_NAME_PREFIX L"49ea3fb1-1591-434f-99ae-afec90b1a17c_"
#define WIDGET_STATE_FILE_NAME L"state.dat"

// Private URI scheme the app registers (pointing at its own exe) so the Game Bar widget
// can start the app via Launcher.LaunchUriAsync when it isn't already running. Must match
// the scheme used in the widget (MainWidget.xaml.cs).
#define WIDGET_LAUNCH_PROTOCOL L"universalpausebutton"

// The Lord's data types.
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned long u32;
typedef unsigned long long u64;
typedef signed char i8;
typedef signed short i16;
typedef signed long i32;
typedef signed long long i64;
typedef float f32;
typedef double f64;

// WARNING: Undocumented Win32 API functions!
typedef LONG(NTAPI* _NtSuspendProcess) (IN HANDLE ProcessHandle);
typedef LONG(NTAPI* _NtResumeProcess) (IN HANDLE ProcessHandle);
typedef HWND(NTAPI* _HungWindowFromGhostWindow) (IN HWND GhostWindowHandle);

_NtSuspendProcess NtSuspendProcess;
_NtResumeProcess NtResumeProcess;
_HungWindowFromGhostWindow HungWindowFromGhostWindow;

// Configurable registry settings.
typedef struct _CONFIG
{
	u32 Debug;
	u32 TrayIcon;
	u32 PauseKey;
	u32 PauseKeyModifiers;
	wchar_t ProcessNameToPause[128];
	u32 PauseOnSleep;
	u32 WidgetPause;
	u32 Autostart;
} CONFIG;

// Function declarations.
u32 LoadRegistrySettings(void);
void MsgBox(const wchar_t* Message, const wchar_t* Caption, u32 Flags, ...);
void DbgPrint(const wchar_t* Message, ...);
LRESULT CALLBACK SysTrayCallback(_In_ HWND Window, _In_ UINT Message, _In_ WPARAM WParam, _In_ LPARAM LParam);
void HandlePauseKeyPress(void);
void UnpausePreviouslyPausedProcess(void);
u32 FindProcessIdByName(const wchar_t* ProcessName);
BOOL IsGameBarProcessId(u32 ProcessId);
void PauseProcessById(u32 ProcessId);
void HandleSystemSuspend(void);
void HandleSystemResume(void);
HANDLE CreatePauseSignalEvent(void);
void RegisterWidgetLaunchProtocol(void);
void UpdateAutostartRegistration(void);
void UpdateWidgetState(void);
void RepublishWidgetStateIfMissing(void);
void DeleteWidgetStateFile(void);
void GetProcessNameById(u32 ProcessId, wchar_t* Buffer, size_t BufferCount);