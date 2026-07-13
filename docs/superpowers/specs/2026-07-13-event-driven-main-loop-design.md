# Event-driven main loop — design

- **Date:** 2026-07-13
- **Status:** Approved design, not yet implemented
- **Scope:** `Main.c` `wWinMain` message loop and foreground tracking
- **Author note:** To be implemented on a Windows machine with a Visual Studio build; the design was written on macOS where it cannot be compiled or tested.

## Problem

The `wWinMain` loop is a busy-poll: every iteration it drains the message queue, polls the foreground window, checks the widget event, maybe republishes the widget state file, then `Sleep(5)`. That means ~200 wake-ups per second forever, even when the app is completely idle. It doesn't burn much CPU percentage, but it prevents the CPU from reaching deep idle states — a real battery/power cost for a background tray app.

This is the last remaining continuous-polling source. Two others have already been removed:

- The Xbox controller pause combo (background `XInputGetState` polling) — removed.
- The web server (`serve_request` non-blocking `accept` poll) — removed.

With those gone, the loop's remaining per-iteration work is:

1. **Message pump** (`PeekMessageW`) — `WM_HOTKEY` (keyboard hotkey), `WM_POWERBROADCAST` (sleep/wake), `WM_TRAYICON` (tray quit). Already event-capable; it only polls because `PeekMessage` is non-blocking.
2. **Foreground tracking** (`GetForegroundWindow`) — remembers the last real foreground process so sleep can auto-pause the right one. The only genuinely poll-based source.
3. **Pause signal event** (`WaitForSingleObject(gPauseSignalEvent, 0)`) — the Game Bar widget's toggle. Already a kernel event handle; polled with timeout 0.
4. **Widget state republish** (every 5s) — already a timer, not a busy poll.

## Goal / success criteria

- When idle, the thread blocks instead of spinning: **≈0 wake-ups/second** (only the 5s widget-state check, or fully blocked when `WidgetPause` is off).
- No change to observable behavior: keyboard hotkey, sleep/wake auto-pause, and the Game Bar widget all keep working with the same responsiveness.
- No new external dependencies; only standard `user32` APIs already available.

## Design

Replace `Sleep(5)` + non-blocking polls with a single blocking wait via **`MsgWaitForMultipleObjectsEx`**, and move foreground tracking from a per-iteration poll to an event callback via **`SetWinEventHook(EVENT_SYSTEM_FOREGROUND, …)`**.

### Wait set

`MsgWaitForMultipleObjectsEx(nCount, handles, timeout, QS_ALLINPUT, MWMO_INPUTAVAILABLE)`:

- **handles** — `[gPauseSignalEvent]` when `WidgetPause` is on (0 handles otherwise). `QS_ALLINPUT` covers `WM_HOTKEY` (`QS_HOTKEY`), `WM_POWERBROADCAST` / `WM_TRAYICON` (posted/sent message bits), and the out-of-context WinEvent callbacks (delivered during message retrieval).
- **timeout** — milliseconds until the next 5s widget-state check when `WidgetPause` is on; otherwise `INFINITE`. Compute as `max(0, 5000 - (GetTickCount() - LastWidgetCheckTick))`.
- **`MWMO_INPUTAVAILABLE`** — so a message already sitting in the queue (e.g. `WM_HOTKEY` posted between iterations) wakes the wait instead of being missed.

Return value handling:

- `WAIT_OBJECT_0 + nCount` → messages available: run the existing `PeekMessageW` drain loop (keep the `WM_HOTKEY → HandlePauseKeyPress()` special-case and `DispatchMessageW` for everything else). Dispatching also delivers the `WM_POWERBROADCAST` handler (`SysTrayCallback`) and the WinEvent foreground callback.
- `WAIT_OBJECT_0` (when `nCount == 1`) → the pause signal event fired: `DbgPrint` + `HandlePauseKeyPress()`.
- `WAIT_TIMEOUT` → time to run `RepublishWidgetStateIfMissing()` (via the same 5s gate).
- `WAIT_FAILED` → `DbgPrint` the error; to avoid a hot error-loop, fall back to a short `Sleep` (e.g. 50 ms) for that iteration.

After any wake, unconditionally: (a) drain pending messages if any, (b) run the 5s widget-state check if `WidgetPause` and the interval elapsed. `gIsRunning` is re-checked at the top of the loop; the tray "quit" path (`gIsRunning = FALSE` + `PostQuitMessage`) posts a message that wakes the wait so the loop exits promptly.

### Foreground tracking via WinEvent hook (replaces poll #2)

Install once, only when `PauseOnSleep` is on, before the loop:

```c
HWINEVENTHOOK gForegroundHook =
    SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
                    NULL, ForegroundChangeProc, 0, 0,
                    WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
```

Callback (same selection logic the poll used today):

```c
void CALLBACK ForegroundChangeProc(HWINEVENTHOOK hook, DWORD event, HWND hwnd,
                                   LONG idObject, LONG idChild,
                                   DWORD idEventThread, DWORD dwmsEventTime)
{
    if (event != EVENT_SYSTEM_FOREGROUND || hwnd == NULL || gIsPaused)
        return;
    u32 pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != 0 &&
        pid != GetCurrentProcessId() &&        // redundant with SKIPOWNPROCESS, kept for clarity
        pid != gLastForegroundProcessId &&
        !IsGameBarProcessId(pid))
    {
        gLastForegroundProcessId = pid;
    }
}
```

`WINEVENT_OUTOFCONTEXT` means the callback runs on our thread while it pumps messages — no injected DLL, no separate thread. `WINEVENT_SKIPOWNPROCESS` drops events for our own process. Call `UnhookWinEvent(gForegroundHook)` in the `Exit:` cleanup.

The per-iteration `GetForegroundWindow` block (current lines ~197–219) is deleted entirely.

### Timeout / widget republish (poll #4 unchanged in intent)

Keep the existing 5s `RepublishWidgetStateIfMissing()` behavior, but drive its cadence from the wait timeout rather than a busy tick. The `LastWidgetCheckTick` gate stays; the wait simply wakes at (or before) the next due time.

## Loop skeleton (illustrative)

```c
while (gIsRunning)
{
    DWORD nCount = 0;
    HANDLE handles[1];
    if (gPauseSignalEvent != NULL) handles[nCount++] = gPauseSignalEvent;

    DWORD timeout = INFINITE;
    if (gConfig.WidgetPause)
    {
        u32 elapsed = GetTickCount() - LastWidgetCheckTick;
        timeout = (elapsed >= 5000) ? 0 : (5000 - elapsed);
    }

    DWORD r = MsgWaitForMultipleObjectsEx(nCount, handles, timeout,
                                          QS_ALLINPUT, MWMO_INPUTAVAILABLE);

    if (r == WAIT_OBJECT_0 + nCount)          // window/thread messages
    {
        while (PeekMessageW(&WndMsg, NULL, 0, 0, PM_REMOVE))
        {
            if (WndMsg.message == WM_HOTKEY) HandlePauseKeyPress();
            DispatchMessageW(&WndMsg);        // delivers WM_POWERBROADCAST + WinEvent callback
        }
    }
    else if (nCount == 1 && r == WAIT_OBJECT_0)   // widget pause signal
    {
        DbgPrint(L"Pause signal received from Game Bar widget.");
        HandlePauseKeyPress();
    }
    else if (r == WAIT_FAILED)
    {
        DbgPrint(L"MsgWaitForMultipleObjectsEx failed! Error 0x%08lx", GetLastError());
        Sleep(50);
    }
    // WAIT_TIMEOUT falls through

    if (gConfig.WidgetPause)
    {
        u32 now = GetTickCount();
        if (now - LastWidgetCheckTick >= 5000)
        {
            LastWidgetCheckTick = now;
            RepublishWidgetStateIfMissing();
        }
    }
}
```

## Error handling / fallback

- **`SetWinEventHook` returns NULL** (hook install failed): `DbgPrint` a warning and continue without event-based foreground tracking. Degraded behavior: on sleep, `HandleSystemSuspend` pauses whatever `GetForegroundWindow()` returns at that moment, which may be the lock screen. This is a graceful degradation, not a crash. (Optional stronger fallback: keep a low-frequency foreground poll driven by a short wait timeout when the hook failed. Decide during implementation; the simple degrade is acceptable for a first cut.)
- **`MsgWaitForMultipleObjectsEx` returns `WAIT_FAILED`:** log and `Sleep(50)` for that iteration to avoid a hot loop.

## Edge cases

- **Both `WidgetPause` and `PauseOnSleep` off:** `nCount == 0`, `timeout == INFINITE` → the thread blocks purely on messages (hotkey/tray). Truly idle. `WidgetPause` defaults to 1, so in practice the timeout is 5s.
- **Hidden window requirement:** `WM_POWERBROADCAST` needs the hidden top-level window, already created when `TrayIcon || PauseOnSleep`. The message wait works because the thread owns a message queue regardless.
- **Quit path:** tray-click sets `gIsRunning = FALSE` and calls `PostQuitMessage(0)`; the posted `WM_QUIT` wakes the wait, the drain loop runs, and the `while (gIsRunning)` test exits.
- **PID reuse:** unchanged from today — `gLastForegroundProcessId` can in theory point at a recycled PID; acceptable edge case, out of scope here.

## Code changes summary

- `Main.c`
  - Add `ForegroundChangeProc` callback and a `HWINEVENTHOOK gForegroundHook` (file-scope or local-to-`wWinMain`).
  - Install the hook before the loop when `PauseOnSleep` is on; `UnhookWinEvent` in `Exit:`.
  - Replace the loop body with the `MsgWaitForMultipleObjectsEx` skeleton above.
  - Delete the per-iteration `GetForegroundWindow` foreground block and the `Sleep(5)`.
  - Lift `LastWidgetCheckTick` out of the loop body to a `wWinMain` local (it must persist across iterations and be readable for the timeout computation).
- No changes to `Main.h` config, `HandlePauseKeyPress`, `HandleSystemSuspend/Resume`, `IsGameBarProcessId`, or the widget helpers.
- Links only `user32` (already linked). No new dependency.

## Non-goals

- No change to the pause/suspend mechanism, the widget protocol, or registry settings.
- Not pursuing a fully hand-tuned waitable-timer; the message-wait timeout is sufficient.

## Testing (on Windows)

1. Build Release x64 in Visual Studio; confirm it compiles and links.
2. Idle CPU / wake-ups: with the app idle, confirm via Process Explorer / `powercfg` that context switches / wake-ups drop from ~200/s to near 0.
3. Keyboard hotkey still pauses/un-pauses the foreground app immediately.
4. Sleep/wake: put a game in the foreground, sleep the PC, wake it — the game is auto-paused on sleep and auto-resumed on wake (confirms the WinEvent hook recorded the foreground process before sleep).
5. Game Bar widget: press the widget's Pause/Resume button; confirm the app toggles within the expected latency.
6. Quit from the tray icon exits cleanly (no hang), and `UnhookWinEvent` runs.

## Risks

- `MsgWaitForMultipleObjectsEx` + `MWMO_INPUTAVAILABLE` + `PeekMessage` has subtle message-drain semantics; verify no message is missed or the wait doesn't spin (a spin would show as high CPU — catch it in testing).
- WinEvent hook callback runs on the UI thread during message pumping; `IsGameBarProcessId` takes a process snapshot, but only fires on actual foreground changes (infrequent), same as today.
- Cannot be validated on the authoring machine (macOS); the Windows build/test steps above are required before merging any implementation.
