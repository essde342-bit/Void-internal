using System.Text;

namespace VoidInjector;

internal static class StartupDiagnostics
{
    private static readonly object Sync = new();
    private static readonly string LogPath = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
        "VOID Internal",
        "startup.log");

    public static void Initialize()
    {
        AppDomain.CurrentDomain.UnhandledException += (_, args) =>
        {
            var exception = args.ExceptionObject as Exception;
            Write($"Process-level unhandled exception. IsTerminating={args.IsTerminating}.", exception);
        };

        TaskScheduler.UnobservedTaskException += (_, args) =>
            Write("Unobserved task exception.", args.Exception);

        Write("Startup diagnostics initialized.");
    }

    public static void Write(string message, Exception? exception = null)
    {
        try
        {
            lock (Sync)
            {
                var directory = Path.GetDirectoryName(LogPath);
                if (string.IsNullOrWhiteSpace(directory))
                    return;

                Directory.CreateDirectory(directory);

                using var writer = new StreamWriter(
                    LogPath,
                    append: true,
                    encoding: new UTF8Encoding(encoderShouldEmitUTF8Identifier: false));

                writer.WriteLine($"[{DateTimeOffset.Now:yyyy-MM-dd HH:mm:ss.fff zzz}] {message}");

                if (exception is not null)
                    writer.WriteLine(exception);

                writer.Flush();
            }
        }
        catch
        {
            // Diagnostics must never prevent application startup.
        }
    }
}