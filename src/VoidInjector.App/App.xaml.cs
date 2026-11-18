using Microsoft.UI.Xaml;

namespace VoidInjector;

public partial class App : Application
{
    public static Window? CurrentWindow { get; private set; }

    public App()
    {
        StartupDiagnostics.Initialize();
        StartupDiagnostics.Write("App constructor entered.");

        try
        {
            InitializeComponent();
            StartupDiagnostics.Write("Application XAML initialized.");
            RequestedTheme = ApplicationTheme.Dark;
        }
        catch (Exception ex)
        {
            StartupDiagnostics.Write("Application constructor failed.", ex);
            throw;
        }

        UnhandledException += (_, e) =>
            StartupDiagnostics.Write("Unhandled WinUI exception.", e.Exception);
    }

    protected override void OnLaunched(LaunchActivatedEventArgs args)
    {
        StartupDiagnostics.Write("OnLaunched entered.");

        try
        {
            CurrentWindow = new MainWindow();
            StartupDiagnostics.Write("MainWindow constructed.");
            CurrentWindow.Activate();
            StartupDiagnostics.Write("MainWindow activated.");
        }
        catch (Exception ex)
        {
            StartupDiagnostics.Write("Window startup failed.", ex);
            throw;
        }
    }
}