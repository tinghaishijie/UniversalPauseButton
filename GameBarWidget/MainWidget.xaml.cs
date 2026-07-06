using System;
using System.Runtime.InteropServices;
using Windows.Storage;
using Windows.System;
using Windows.UI.Core;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;
using Windows.UI.Xaml.Input;
using Windows.UI.Xaml.Media;

namespace UniversalPauseButton.Widget
{
    /// <summary>
    /// The pause/resume widget page.
    ///
    /// Toggle (widget -> app): opens the named event UniversalPauseButton.exe creates
    /// (Global\UniversalPauseButtonToggle) and signals it, which makes the desktop app
    /// toggle its pause state — exactly like pressing the pause hotkey.
    ///
    /// State (app -> widget): the desktop app writes a tiny file (state.dat) into this
    /// widget's own ApplicationData LocalState folder. A non-elevated desktop process
    /// can't create a Global\ shared-memory section (needs SeCreateGlobalPrivilege) but
    /// it can write to this per-user folder, and the sandboxed widget reads it with no
    /// capability. We poll it so the button shows Pause vs Resume and the paused process.
    ///
    /// This is the workaround for the Xbox full-screen experience: while it is active the
    /// desktop app's XInput polling can no longer see controller input, but a Game Bar
    /// widget still receives gamepad navigation, so the user can toggle pausing from here.
    /// </summary>
    public sealed partial class MainWidget : Page
    {
        // Must match PAUSE_SIGNAL_EVENT_NAME / WIDGET_STATE_FILE_NAME / WIDGET_LAUNCH_PROTOCOL in Main.h.
        private const string EventName = "Global\\UniversalPauseButtonToggle";
        private const string StateFileName = "state.dat";
        private const string LaunchUri = "universalpausebutton:launch";

        private const uint EVENT_MODIFY_STATE = 0x0002;

        // Segoe MDL2 Assets glyphs.
        private const string GlyphPause = "\uE769";   // pause bars
        private const string GlyphPlay = "\uE768";    // play triangle
        private const string GlyphLaunch = "\uE7B5";  // power / open

        [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
        private static extern IntPtr OpenEventW(uint dwDesiredAccess, bool bInheritHandle, string lpName);

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool SetEvent(IntPtr hEvent);

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool CloseHandle(IntPtr hObject);

        private readonly DispatcherTimer _timer = new DispatcherTimer();
        private readonly string _stateFilePath;
        private bool _refreshing;

        public MainWidget()
        {
            this.InitializeComponent();
            _stateFilePath = System.IO.Path.Combine(ApplicationData.Current.LocalFolder.Path, StateFileName);

            _timer.Interval = TimeSpan.FromMilliseconds(400);
            _timer.Tick += (s, e) => RefreshState();
            this.Loaded += (s, e) =>
            {
                RefreshState();
                _timer.Start();
                // Land keyboard focus on the button so the controller starts on it and the
                // focus rectangle shows (Game Bar's first focus entry otherwise doesn't).
                FocusButtonKeyboard();
            };
            this.GettingFocus += OnGettingFocus;
            this.Unloaded += (s, e) => _timer.Stop();
        }

        /// <summary>
        /// When focus enters the widget from outside, defer to the dispatcher and give the
        /// button keyboard focus so the focus rectangle shows and gamepad A can invoke it.
        /// </summary>
        private void OnGettingFocus(UIElement sender, GettingFocusEventArgs args)
        {
            var old = args.OldFocusedElement as DependencyObject;
            bool fromOutside = old == null || !IsDescendant(old);
            if (fromOutside)
            {
                FocusButtonKeyboard();
            }
        }

        private void FocusButtonKeyboard()
        {
            var ignore = Dispatcher.RunAsync(CoreDispatcherPriority.Normal, () =>
            {
                try { PauseButton.Focus(FocusState.Keyboard); } catch { }
            });
        }

        private bool IsDescendant(DependencyObject node)
        {
            while (node != null)
            {
                if (ReferenceEquals(node, this)) { return true; }
                node = VisualTreeHelper.GetParent(node);
            }
            return false;
        }


        /// <summary>
        /// Reads the state file the desktop app writes and updates the button and status.
        /// </summary>
        private void RefreshState()
        {
            if (_refreshing)
            {
                return;
            }
            _refreshing = true;
            try
            {
                bool exists;
                bool isPaused = false;
                string processName = string.Empty;

                try
                {
                    string text = System.IO.File.ReadAllText(_stateFilePath);
                    exists = true;
                    string[] lines = text.Replace("\r", string.Empty).Split('\n');
                    if (lines.Length > 0)
                    {
                        isPaused = lines[0].Trim() == "1";
                    }
                    if (lines.Length > 1)
                    {
                        processName = lines[1].Trim();
                    }
                }
                catch (System.IO.FileNotFoundException)
                {
                    exists = false;
                }
                catch (System.IO.DirectoryNotFoundException)
                {
                    exists = false;
                }
                catch
                {
                    // Transient read (file being rewritten): keep the current UI this tick.
                    return;
                }

                string status;
                PauseButton.IsEnabled = true;
                if (!exists)
                {
                    ButtonIcon.Glyph = GlyphLaunch;
                    ButtonText.Text = "Start";
                    status = "UniversalPauseButton is not running \u2014 press to start it.";
                }
                else if (isPaused)
                {
                    ButtonIcon.Glyph = GlyphPlay;
                    ButtonText.Text = "Resume";
                    status = string.IsNullOrEmpty(processName) ? "Paused." : "Paused: " + processName;
                }
                else
                {
                    ButtonIcon.Glyph = GlyphPause;
                    ButtonText.Text = "Pause";
                    status = "Running \u2014 press to pause the foreground game.";
                }

                StatusText.Text = status;
            }
            finally
            {
                _refreshing = false;
            }
        }

        private async void PauseButton_Click(object sender, RoutedEventArgs e)
        {
            IntPtr handle = OpenEventW(EVENT_MODIFY_STATE, false, EventName);
            if (handle == IntPtr.Zero)
            {
                // The app isn't running: start it via its registered launch protocol.
                await LaunchAppAsync();
                return;
            }

            try
            {
                if (!SetEvent(handle))
                {
                    StatusText.Text = "Failed to send toggle (error " + Marshal.GetLastWin32Error() + ").";
                    return;
                }
            }
            finally
            {
                CloseHandle(handle);
            }

            // Give the desktop app a moment to apply the toggle and rewrite the state file,
            // then reflect the new state immediately instead of waiting for the next tick.
            await System.Threading.Tasks.Task.Delay(150);
            RefreshState();
        }

        /// <summary>
        /// Starts UniversalPauseButton.exe through its registered "universalpausebutton:"
        /// URI scheme. The desktop app registers this scheme (pointing at its own exe) each
        /// time it runs, so it must have run at least once for this to succeed.
        /// </summary>
        private async System.Threading.Tasks.Task LaunchAppAsync()
        {
            StatusText.Text = "Starting UniversalPauseButton\u2026";
            try
            {
                bool launched = await Launcher.LaunchUriAsync(new Uri(LaunchUri));
                if (!launched)
                {
                    StatusText.Text = "Couldn't start UniversalPauseButton. Run it once on the desktop first.";
                }
                // If it launched, the app writes the state file and the timer will flip the
                // button to Pause on the next tick.
            }
            catch (Exception)
            {
                StatusText.Text = "Couldn't start UniversalPauseButton. Run it once on the desktop first.";
            }
        }
    }
}
