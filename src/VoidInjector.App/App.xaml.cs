using System.Runtime.InteropServices;
using Microsoft.UI.Xaml;

namespace VoidInjector;

public partial class App : Application
{
    private const uint ErrorDialogFlags = 0x00000010 | 0x00001000;

    [DllImport("user32.dll", CharSet = CharSet.Unicode, EntryPoint = "MessageBoxW")]
    private static extern int ShowMessageBox(IntPtr window, string text, string caption, uint type);

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
            ShowStartupError("Application initialization", ex);
            throw;
        }

        UnhandledException += (_, e) =>
        {
            StartupDiagnostics.Write("Unhandled WinUI exception.", e.Exception);
            ShowStartupError("Unhandled WinUI exception", e.Exception);
        };
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
            ShowStartupError("Window startup", ex);
        }
    }

    private static void ShowStartupError(string stage, Exception exception)
    {
        try
        {
            var message =
                $"VOID Internal could not start at: {stage}.\r\n\r\n" +
                $"{exception.Message}\r\n\r\n" +
                $"Full details were saved to:\r\n{StartupDiagnostics.LogFilePath}";

            ShowMessageBox(IntPtr.Zero, message, "VOID Internal — startup error", ErrorDialogFlags);
        }
        catch
        {
            // The diagnostic file remains available if a dialog cannot be shown.
        }
    }
}