using System.Text;

namespace VoidInjector.Services;

public static class LogService
{
    private static readonly object Sync = new();
    private static readonly UTF8Encoding Utf8 = new(false);

    public static string LogPath => Path.Combine(AppContext.BaseDirectory, "last_run.txt");

    public static void BeginRun()
    {
        lock (Sync)
        {
            var header = new StringBuilder()
                .AppendLine("VOID Injector — diagnostic log")
                .AppendLine($"Started: {DateTimeOffset.Now:yyyy-MM-dd HH:mm:ss zzz}")
                .AppendLine(new string('-', 58))
                .ToString();

            File.WriteAllText(LogPath, header, Utf8);
        }
    }

    public static void Write(string message)
    {
        lock (Sync)
        {
            File.AppendAllText(
                LogPath,
                $"[{DateTimeOffset.Now:yyyy-MM-dd HH:mm:ss.fff zzz}] {message}{Environment.NewLine}",
                Utf8);
        }
    }

    public static async Task<string> ReadAsync()
    {
        if (!File.Exists(LogPath))
            return "No log has been created yet. Start an operation from Home to create one.";

        return await File.ReadAllTextAsync(LogPath, Utf8);
    }
}
