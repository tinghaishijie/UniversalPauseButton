# Universal Pause Button — Xbox Game Bar widget

This is a small [Xbox Game Bar](https://learn.microsoft.com/gaming/game-bar/) widget that
adds a single **Pause / Resume** button to Game Bar. It lets you pause/resume with a
**controller** in the **Xbox full-screen experience (Xbox Mode / FSE)**.

A background app can't receive controller input while the FSE shell or Game Bar is in the
foreground — that shell takes exclusive control of the controller. Game Bar widgets,
however, still receive gamepad navigation in FSE. So you can open Game Bar, focus this
widget, and press **A** to toggle pausing. (The keyboard pause hotkey also keeps working in
FSE, since it's a system-level global hotkey.)

## How it talks to the main app

The main `UniversalPauseButton.exe` creates a named auto-reset event
`Global\UniversalPauseButtonToggle` with a security descriptor that grants
`EVENT_MODIFY_STATE` to `ALL APPLICATION PACKAGES`, so this sandboxed UWP widget can open
and signal it (see `CreatePauseSignalEvent` in `Main.c` and the `WidgetPause` registry
setting). When the button is pressed, the widget calls `OpenEventW` + `SetEvent`, and the
main app's message loop toggles the pause state — exactly as if you had pressed the pause
hotkey.

**The main Universal Pause Button app must be running** (with `WidgetPause` enabled, which
is the default) for the widget to do anything.

## Prerequisites

Xbox Game Bar widgets must be **classic UWP** apps (they rely on a `CoreWindow`); WinUI 3 /
Windows App SDK is **not** supported by the Xbox Game Bar SDK. You therefore need a Visual
Studio with the classic **Universal Windows Platform development** tooling.

> **Note:** Recent Visual Studio 2022 builds (2026 / 17.14+) have **removed** the classic
> UWP C# workload. If your VS 2022 no longer offers "Universal Windows Platform tools",
> build this project with **Visual Studio 2019** (Community is free and installs
> side-by-side with VS 2022) using its **Universal Windows Platform development** workload.
> This project has been built successfully with VS 2019 + Windows SDK 10.0.19041.0.

- Visual Studio 2019 (or an older VS 2022 that still has the UWP workload) with the
  **Universal Windows Platform development** workload.
- Windows 10 SDK **10.0.19041.0** (the project targets it; installed automatically with the
  UWP workload). Adjust `TargetPlatformVersion` in the `.csproj` if you use a different SDK.
- NuGet packages restore automatically:
  - `Microsoft.Gaming.XboxGameBar`
  - `Microsoft.NETCore.UniversalWindowsPlatform`

The placeholder logos in `Assets\` are plain generated PNGs — replace them with your own
artwork if you like.

## Build and sideload (command line)

The following was verified with VS 2019. Adjust the MSBuild path to your install.

1. Enable **Developer Mode** in Windows Settings → Privacy & security → For developers.

2. Restore and build a **signed** package. A self-signed test certificate whose subject
   matches the package `Publisher` (`CN=UniversalPauseButton`) is required for sideloading:

   ```powershell
   $msbuild = "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\MSBuild\Current\Bin\MSBuild.exe"
   $proj    = "UniversalPauseButton.Widget.csproj"

   # Restore NuGet packages (first run can take several minutes).
   & $msbuild $proj /t:Restore /p:Configuration=Release /p:Platform=x64

   # Create a self-signed test cert (once). Subject MUST match the manifest Publisher.
   $cert = New-SelfSignedCertificate -Type Custom -Subject "CN=UniversalPauseButton" `
       -KeyUsage DigitalSignature -CertStoreLocation "Cert:\CurrentUser\My" `
       -TextExtension @("2.5.29.37={text}1.3.6.1.5.5.7.3.3")
   $pwd = ConvertTo-SecureString -String "upbwidget" -Force -AsPlainText
   Export-PfxCertificate -Cert "Cert:\CurrentUser\My\$($cert.Thumbprint)" `
       -FilePath "UniversalPauseButton.Widget_TemporaryKey.pfx" -Password $pwd

   # Build the signed .msix (use Platform=ARM64 on a handheld running Windows on ARM).
   & $msbuild $proj /t:Build /p:Configuration=Release /p:Platform=x64 `
       /p:AppxPackageSigningEnabled=true `
       /p:PackageCertificateKeyFile=UniversalPauseButton.Widget_TemporaryKey.pfx `
       /p:PackageCertificatePassword=upbwidget
   ```

3. The signed package and its dependencies are produced under:

   ```
   AppPackages\UniversalPauseButton.Widget_1.1.6.0_x64_Test\
   ```

4. Copy that whole `..._Test` folder to the target PC/handheld and run
   **`Add-AppDevPackage.ps1`** from inside it in an **elevated** PowerShell. It trusts the
   test certificate and installs the widget together with the .NET Native / VCLibs
   dependencies. (Alternatively, import the `.cer` into
   `Cert:\LocalMachine\TrustedPeople` and run `Add-AppxPackage` on the `.msix`.)

## Enable the widget in Game Bar

1. Open Game Bar (**Xbox button** on the controller, or **Win + G**).
2. Open the **Widget menu** and enable **Universal Pause Button**.
3. Pin/favorite it so it is easy to reach in the Xbox full-screen experience.
4. Focus the widget with the controller and press **A** on the **Pause / Resume** button.

## Notes

- The event name `Global\UniversalPauseButtonToggle` and the state file name `state.dat`
  in `MainWidget.xaml.cs` must stay in sync with `PAUSE_SIGNAL_EVENT_NAME` and
  `WIDGET_STATE_FILE_NAME` in `..\Main.h`.
- The button reflects live state published by the app: it shows **Pause** while running,
  **Resume** (with the paused process name) while paused, and **Start** when the app isn't
  running.
- **Start button:** when the app isn't running, pressing the button launches it through the
  private `universalpausebutton:` URI scheme the app registers under
  `HKCU\Software\Classes`. Because the app registers that scheme on startup, the app must
  have run at least once on the desktop before the widget can relaunch it.
- **How state is shared:** a non-elevated desktop process cannot create a `Global\`
  shared-memory section (that needs `SeCreateGlobalPrivilege`), so instead the app writes
  the state to `state.dat` inside this widget package's own `LocalState` folder
  (`%LOCALAPPDATA%\Packages\<PackageFamilyName>\LocalState\`), which the current user can
  write and the sandboxed widget reads with no capability. The widget package name prefix
  is referenced by `WIDGET_PACKAGE_NAME_PREFIX` in `..\Main.h`; if you change the widget's
  `Package.appxmanifest` Identity Name, update that constant too.
- This widget only sends a *toggle*; the main app owns the actual pause/resume logic and
  the current paused/unpaused state.
