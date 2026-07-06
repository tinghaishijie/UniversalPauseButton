using Microsoft.Gaming.XboxGameBar;
using Windows.ApplicationModel;
using Windows.ApplicationModel.Activation;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;
using Windows.UI.Xaml.Navigation;

namespace UniversalPauseButton.Widget
{
    /// <summary>
    /// Minimal Xbox Game Bar widget host. Game Bar activates the widget through the
    /// ms-gamebarwidget: protocol; we build a Frame and navigate to <see cref="MainWidget"/>.
    /// </summary>
    sealed partial class App : Application
    {
        private XboxGameBarWidget _widget;

        public App()
        {
            this.InitializeComponent();
            this.Suspending += OnSuspending;
        }

        protected override void OnActivated(IActivatedEventArgs args)
        {
            XboxGameBarWidgetActivatedEventArgs widgetArgs = null;

            if (args.Kind == ActivationKind.Protocol)
            {
                var protocolArgs = args as IProtocolActivatedEventArgs;
                if (protocolArgs != null && protocolArgs.Uri.Scheme.Equals("ms-gamebarwidget"))
                {
                    widgetArgs = args as XboxGameBarWidgetActivatedEventArgs;
                }
            }

            if (widgetArgs == null)
            {
                return;
            }

            if (widgetArgs.IsLaunchActivation)
            {
                var rootFrame = new Frame();
                rootFrame.NavigationFailed += OnNavigationFailed;
                Window.Current.Content = rootFrame;

                _widget = new XboxGameBarWidget(
                    widgetArgs,
                    Window.Current.CoreWindow,
                    rootFrame);

                rootFrame.Navigate(typeof(MainWidget));

                Window.Current.Closed += Window_Closed;
                Window.Current.Activate();
            }
        }

        private void Window_Closed(object sender, Windows.UI.Core.CoreWindowEventArgs e)
        {
            _widget = null;
            Window.Current.Closed -= Window_Closed;
        }

        private void OnNavigationFailed(object sender, NavigationFailedEventArgs e)
        {
            throw new System.Exception("Failed to load Page " + e.SourcePageType.FullName);
        }

        private void OnSuspending(object sender, SuspendingEventArgs e)
        {
            var deferral = e.SuspendingOperation.GetDeferral();
            _widget = null;
            deferral.Complete();
        }
    }
}
