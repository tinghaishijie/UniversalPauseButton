# Universal Pause Button

<img src="https://github.com/ryanries/UniversalPauseButton/blob/master/NiceLogo.png" width="128" height="128" alt="Universal Pause Button">

<img alt="GitHub all releases" src="https://img.shields.io/github/downloads/ryanries/UniversalPauseButton/total">

Pause **any** game or app — even during those "un-pausable" cutscenes — by pressing **Ctrl+Shift+P**. Press again to resume.

A tiny Windows tray app: it suspends the foreground window's process (or a named process) using `NtSuspendProcess`, and resumes it later. Originally written in 2015 and rewritten in 2023.

## Usage

1. Run `UniversalPauseButton.exe`. It lives in the system tray (pause-button icon).
2. Focus your game and press **Ctrl+Shift+P** to freeze it; press again to un-freeze.
3. Click the tray icon to quit.

Other ways to toggle pause: a custom hotkey, automatically on sleep/wake, or via the Xbox Game Bar widget.

> ⚠️ Best for **single-player** games. Pausing a multiplayer game will get you kicked. Since suspending a process is something only debuggers normally do, results vary by app — test before relying on it.

## Registry settings

All values live under `HKCU\Software\UniversalPauseButton`. Out-of-range values fall back to the default.

| Setting | Type | Default | Range / values | Description |
| --- | --- | --- | --- | --- |
| `Debug` | DWORD | `0` | 0–1 | Spawn a console showing internal debug messages. |
| `TrayIcon` | DWORD | `1` | 0–1 | Show the tray icon. `0` runs fully invisibly (for shells with no tray). |
| `PauseKey` | DWORD | `0x50` | 0x1–0xFE | [Virtual-key code](https://learn.microsoft.com/en-us/windows/win32/inputdev/virtual-key-codes) of the hotkey (default = `P`). |
| `PauseKeyModifiers` | DWORD | `0x6` | 0–0xF | Modifier bitmask: ALT=1, CTRL=2, SHIFT=4, WIN=8 (combine as needed). Default = CTRL+SHIFT. |
| `ProcessNameToPause` | String | `""` | — | Always pause this process by name (e.g. `game.exe`), ignoring the foreground window. First match only. |
| `PauseOnSleep` | DWORD | `1` | 0–1 | Auto-pause when the PC sleeps and auto-resume on wake. |
| `WidgetPause` | DWORD | `1` | 0–1 | Create the shared event (`Global\UniversalPauseButtonToggle`) the Game Bar widget uses. |
| `Autostart` | DWORD | `0` | 0–1 | Register in the per-user Run key to launch at sign-in. |

![Registry](https://github.com/ryanries/UniversalPauseButton/blob/master/registry.png)

Notes: if `ProcessNameToPause` is set, that named process is paused (never `svchost.exe`, `lsass.exe`, etc.). On sleep, if it's unset, the last foreground process is paused; a process you paused manually is left untouched. The Xbox Game Bar overlay is excluded from foreground tracking.

## Xbox full-screen experience / Game Bar widget

The keyboard hotkey keeps working in the Xbox full-screen experience (FSE) because it's a system-level global hotkey. If you play with a controller, use the **Game Bar widget** in the [`GameBarWidget`](GameBarWidget) folder: it adds a Pause/Resume button to the Xbox Game Bar that you can focus and press A on (Game Bar stays controller-navigable in FSE). It signals the main app through the shared event, so the main app must be running. See [`GameBarWidget/README.md`](GameBarWidget/README.md) to build and sideload it.

---

# 中文使用说明

一键暂停**任意**游戏或程序——即便是那些无法暂停的过场动画。按下 **Ctrl+Shift+P** 暂停，再按一次恢复。

这是一个极小的 Windows 托盘程序：用 `NtSuspendProcess` 挂起当前前台窗口对应的进程（或指定名字的进程），之后再恢复。最初写于 2015 年，2023 年重写。

## 使用方法

1. 运行 `UniversalPauseButton.exe`，程序常驻系统托盘（暂停按钮图标）。
2. 切到游戏窗口，按 **Ctrl+Shift+P** 冻结它；再按一次解冻。
3. 点击托盘图标可退出。

其他触发方式：自定义热键、睡眠/唤醒时自动暂停、或使用 Xbox Game Bar 小组件。

> ⚠️ 仅适合**单人游戏**。暂停多人游戏会被踢出房间。挂起进程通常只有调试器才会做，效果因程序而异，请先测试再依赖。

## 注册表设置

所有设置位于 `HKCU\Software\UniversalPauseButton`，超出范围的值会回退到默认值。

| 设置项 | 类型 | 默认 | 取值范围 | 说明 |
| --- | --- | --- | --- | --- |
| `Debug` | DWORD | `0` | 0–1 | 弹出控制台显示内部调试信息。 |
| `TrayIcon` | DWORD | `1` | 0–1 | 是否显示托盘图标；`0` 表示完全隐藏运行（适合无托盘的 shell）。 |
| `PauseKey` | DWORD | `0x50` | 0x1–0xFE | 热键的[虚拟键码](https://learn.microsoft.com/en-us/windows/win32/inputdev/virtual-key-codes)（默认为 `P`）。 |
| `PauseKeyModifiers` | DWORD | `0x6` | 0–0xF | 修饰键位掩码：ALT=1、CTRL=2、SHIFT=4、WIN=8（可组合相加）。默认为 CTRL+SHIFT。 |
| `ProcessNameToPause` | 字符串 | `""` | — | 始终按名字暂停该进程（如 `game.exe`），忽略前台窗口；只匹配第一个。 |
| `PauseOnSleep` | DWORD | `1` | 0–1 | 睡眠时自动暂停、唤醒时自动恢复。 |
| `WidgetPause` | DWORD | `1` | 0–1 | 创建 Game Bar 小组件所用的共享事件（`Global\UniversalPauseButtonToggle`）。 |
| `Autostart` | DWORD | `0` | 0–1 | 写入当前用户的 Run 键，登录时自动启动。 |

说明：设置了 `ProcessNameToPause` 时只暂停该进程（切勿填 `svchost.exe`、`lsass.exe` 等系统进程）。睡眠时若未设置该项，则暂停最后的前台进程；你手动暂停的进程不会被自动处理。Xbox Game Bar 覆盖层已从前台跟踪中排除。

## Xbox 全屏体验 / Game Bar 小组件

键盘热键在 Xbox 全屏体验（FSE）下仍然有效（它是系统级全局热键）。如果你用手柄游玩，请使用 [`GameBarWidget`](GameBarWidget) 文件夹中的 **Game Bar 小组件**：它在 Xbox Game Bar 中加一个"暂停/恢复"按钮，可用手柄聚焦并按 A 触发（Game Bar 在 FSE 下仍支持手柄导航）。它通过共享事件通知主程序，因此主程序必须处于运行状态。构建与旁加载方法见 [`GameBarWidget/README.md`](GameBarWidget/README.md)。

---

Joseph Ryan Ries, 2015–2023 · ryanries09@gmail.com · [GitHub](https://github.com/ryanries/UniversalPauseButton/)
