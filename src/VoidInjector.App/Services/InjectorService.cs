using Microsoft.Win32;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.RegularExpressions;

namespace VoidInjector.Services;

public sealed record InjectionProgress(int Percent, string Status, string Detail);
public sealed record InjectionResult(bool Success, string Status, string Detail);

public sealed class InjectorService
{
    private const uint ProcessAllAccess = 0x001F0FFF;
    private const uint MemCommitReserve = 0x3000;
    private const uint PageReadWrite = 0x04;
    private const uint MemRelease = 0x8000;
    private const uint WaitObject0 = 0x00000000;
    private const uint WaitTimeout = 0x00000102;

    private const string Cs2RelativePath =
        @"steamapps\common\Counter-Strike Global Offensive\game\bin\win64\cs2.exe";

    public async Task<InjectionResult> RunAsync(
        IProgress<InjectionProgress> progress,
        CancellationToken cancellationToken = default)
    {
        LogService.BeginRun();
        LogService.Write("INJECT action started.");

        try
        {
            var dllPath = Path.Combine(AppContext.BaseDirectory, "void_internal.dll");
            LogService.Write($"Expected module path: {dllPath}");

            if (!File.Exists(dllPath))
                return Fail("Module file not found", "Place void_internal.dll next to VoidInjector.exe.");

            using var existingProcess = FindGameProcess();
            var targetProcess = existingProcess;

            if (targetProcess is not null)
            {
                LogService.Write($"CS2 is already running. PID={targetProcess.Id}");
            }
            else
            {
                progress.Report(new InjectionProgress(8, "Finding Counter-Strike 2",
                    "Checking your Steam libraries..."));
                LogService.Write("CS2 process not found. Searching Steam libraries.");

                var (steamRoot, gamePath) = FindGameInstallation();
                if (string.IsNullOrWhiteSpace(steamRoot) || string.IsNullOrWhiteSpace(gamePath))
                    return Fail("Game installation not found",
                        "Install Counter-Strike 2 through Steam, then try again.");

                LogService.Write($"Steam directory: {steamRoot}");
                LogService.Write($"CS2 executable: {gamePath}");

                var steamExe = Path.Combine(steamRoot, "steam.exe");
                if (!File.Exists(steamExe))
                    return Fail("Steam executable not found",
                        "The Steam installation was found, but steam.exe is missing.");

                progress.Report(new InjectionProgress(15, "Launching Counter-Strike 2",
                    "Steam is opening the game. Keep this window open."));
                LogService.Write("Starting Steam with AppID 730.");

                Process.Start(new ProcessStartInfo
                {
                    FileName = steamExe,
                    Arguments = "-applaunch 730",
                    WorkingDirectory = steamRoot,
                    UseShellExecute = true
                });

                for (var second = 0; second < 90; second++)
                {
                    cancellationToken.ThrowIfCancellationRequested();
                    await Task.Delay(1000, cancellationToken);

                    targetProcess = FindGameProcess();
                    if (targetProcess is not null)
                        break;

                    if (second % 5 == 0)
                    {
                        var percent = 16 + (second * 48 / 90);
                        progress.Report(new InjectionProgress(percent,
                            "Waiting for Counter-Strike 2", "Steam is starting the game..."));
                    }
                }

                if (targetProcess is null)
                    return Fail("CS2 did not start",
                        "The game process was not detected within 90 seconds.");

                LogService.Write($"CS2 process detected. PID={targetProcess.Id}");
                progress.Report(new InjectionProgress(72, "Preparing the game",
                    "The game is running. Waiting for startup to finish..."));
                await Task.Delay(5000, cancellationToken);
            }

            using (targetProcess)
            {
                if (IsModuleLoaded(targetProcess, "void_internal.dll"))
                {
                    LogService.Write("void_internal.dll is already present in the CS2 process.");
                    return new InjectionResult(true, "Already loaded",
                        "The module is already present in the game process.");
                }

                progress.Report(new InjectionProgress(86, "Loading component",
                    "Windows is processing the requested module load..."));

                var (success, detail) = await Task.Run(
                    () => LoadModule(targetProcess.Id, dllPath),
                    cancellationToken);

                if (!success)
                    return Fail("Load failed", detail);

                LogService.Write("SUCCESS: Windows reported that the module load completed.");
                progress.Report(new InjectionProgress(100, "Operation completed",
                    "Windows reported that the module loader finished successfully."));
                return new InjectionResult(true, "Operation completed",
                    "Windows reported that the module loader finished successfully.");
            }
        }
        catch (OperationCanceledException)
        {
            LogService.Write("Operation cancelled.");
            return Fail("Operation cancelled", "The operation was cancelled.");
        }
        catch (Exception ex)
        {
            LogService.Write($"UNHANDLED ERROR: {ex}");
            return Fail("Unexpected error", ex.Message);
        }
    }

    private static InjectionResult Fail(string status, string detail)
    {
        LogService.Write($"FAILED: {status} — {detail}");
        return new InjectionResult(false, status, detail);
    }

    private static Process? FindGameProcess()
    {
        try
        {
            return Process.GetProcessesByName("cs2").FirstOrDefault();
        }
        catch (Exception ex)
        {
            LogService.Write($"Could not enumerate game processes: {ex.Message}");
            return null;
        }
    }

    private static (string SteamRoot, string GamePath) FindGameInstallation()
    {
        string? root = null;
        try
        {
            using var key = Registry.CurrentUser.OpenSubKey(@"Software\Valve\Steam");
            root = key?.GetValue("SteamPath") as string;
        }
        catch (Exception ex)
        {
            LogService.Write($"Steam registry lookup failed: {ex.Message}");
        }

        if (string.IsNullOrWhiteSpace(root))
            return (string.Empty, string.Empty);

        root = root.Replace('/', Path.DirectorySeparatorChar);
        var direct = Path.Combine(root, Cs2RelativePath);
        if (File.Exists(direct))
            return (root, direct);

        var libraryFile = Path.Combine(root, "steamapps", "libraryfolders.vdf");
        if (!File.Exists(libraryFile))
            return (root, string.Empty);

        try
        {
            var contents = File.ReadAllText(libraryFile);
            var matches = Regex.Matches(contents, "\"path\"\s+\"(?<path>[^\"]+)\"");
            foreach (Match match in matches)
            {
                var library = match.Groups["path"].Value.Replace(@"\\", @"\");
                var candidate = Path.Combine(library, Cs2RelativePath);
                if (File.Exists(candidate))
                    return (root, candidate);
            }
        }
        catch (Exception ex)
        {
            LogService.Write($"Steam library scan failed: {ex.Message}");
        }

        return (root, string.Empty);
    }

    private static bool IsModuleLoaded(Process process, string moduleName)
    {
        try
        {
            return process.Modules.Cast<ProcessModule>()
                .Any(module => string.Equals(module.ModuleName, moduleName,
                    StringComparison.OrdinalIgnoreCase));
        }
        catch (Exception ex)
        {
            LogService.Write($"Could not inspect process modules: {ex.Message}");
            return false;
        }
    }

    private static (bool Success, string Detail) LoadModule(int processId, string dllPath)
    {
        LogService.Write($"Starting Windows module-load operation. Target PID={processId}");

        var process = OpenProcess(ProcessAllAccess, false, (uint)processId);
        if (process == IntPtr.Zero)
        {
            var error = Marshal.GetLastWin32Error();
            return (false, $"Windows could not open the game process (error {error}).");
        }

        IntPtr remotePath = IntPtr.Zero;
        IntPtr thread = IntPtr.Zero;

        try
        {
            var pathBytes = Encoding.Unicode.GetBytes(dllPath + "\0");
            remotePath = VirtualAllocEx(process, IntPtr.Zero,
                (UIntPtr)(uint)pathBytes.Length, MemCommitReserve, PageReadWrite);

            if (remotePath == IntPtr.Zero)
            {
                var error = Marshal.GetLastWin32Error();
                LogService.Write($"VirtualAllocEx failed. Win32 error={error}");
                return (false, $"Windows could not allocate target memory (error {error}).");
            }

            if (!WriteProcessMemory(process, remotePath, pathBytes,
                    (UIntPtr)(uint)pathBytes.Length, out var written) ||
                written.ToUInt64() != (ulong)pathBytes.Length)
            {
                var error = Marshal.GetLastWin32Error();
                LogService.Write($"WriteProcessMemory failed. Win32 error={error}");
                return (false, $"Windows could not write the module path (error {error}).");
            }

            var kernel32 = GetModuleHandleW("kernel32.dll");
            var loadLibrary = kernel32 == IntPtr.Zero
                ? IntPtr.Zero
                : GetProcAddress(kernel32, "LoadLibraryW");

            if (loadLibrary == IntPtr.Zero)
                return (false, "Windows could not resolve LoadLibraryW.");

            thread = CreateRemoteThread(process, IntPtr.Zero, UIntPtr.Zero,
                loadLibrary, remotePath, 0, out _);

            if (thread == IntPtr.Zero)
            {
                var error = Marshal.GetLastWin32Error();
                LogService.Write($"CreateRemoteThread failed. Win32 error={error}");
                return (false, $"Windows could not start the loader thread (error {error}).");
            }

            var wait = WaitForSingleObject(thread, 20000);
            if (wait != WaitObject0)
            {
                LogService.Write($"Loader thread wait returned {wait}.");
                return (false, wait == WaitTimeout
                    ? "Windows did not finish the module load within 20 seconds."
                    : "Windows returned an unexpected wait result.");
            }

            if (!GetExitCodeThread(thread, out var exitCode))
            {
                var error = Marshal.GetLastWin32Error();
                LogService.Write($"GetExitCodeThread failed. Win32 error={error}");
                return (false, $"Could not read the loader result (error {error}).");
            }

            if (exitCode == 0)
            {
                LogService.Write("LoadLibraryW returned a null module handle.");
                return (false, "Windows could not load the requested module.");
            }

            return (true, "Module loader completed.");
        }
        finally
        {
            if (thread != IntPtr.Zero)
                CloseHandle(thread);
            if (remotePath != IntPtr.Zero)
                VirtualFreeEx(process, remotePath, UIntPtr.Zero, MemRelease);
            CloseHandle(process);
        }
    }

    [DllImport("kernel32.dll", EntryPoint = "OpenProcess", ExactSpelling = true, SetLastError = true)]
    private static extern IntPtr OpenProcess(uint desiredAccess, bool inheritHandle, uint processId);

    [DllImport("kernel32.dll", EntryPoint = "VirtualAllocEx", ExactSpelling = true, SetLastError = true)]
    private static extern IntPtr VirtualAllocEx(IntPtr process, IntPtr address,
        UIntPtr size, uint allocationType, uint protect);

    [DllImport("kernel32.dll", EntryPoint = "WriteProcessMemory", ExactSpelling = true, SetLastError = true)]
    private static extern bool WriteProcessMemory(IntPtr process, IntPtr address,
        byte[] buffer, UIntPtr size, out UIntPtr written);

    [DllImport("kernel32.dll", EntryPoint = "GetModuleHandleW", ExactSpelling = true, CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr GetModuleHandleW(string moduleName);

    [DllImport("kernel32.dll", EntryPoint = "GetProcAddress", ExactSpelling = true, SetLastError = true)]
    private static extern IntPtr GetProcAddress(IntPtr module, string procedureName);

    [DllImport("kernel32.dll", EntryPoint = "CreateRemoteThread", ExactSpelling = true, SetLastError = true)]
    private static extern IntPtr CreateRemoteThread(IntPtr process, IntPtr threadAttributes,
        UIntPtr stackSize, IntPtr startAddress, IntPtr parameter, uint creationFlags,
        out uint threadId);

    [DllImport("kernel32.dll", EntryPoint = "WaitForSingleObject", ExactSpelling = true, SetLastError = true)]
    private static extern uint WaitForSingleObject(IntPtr handle, uint milliseconds);

    [DllImport("kernel32.dll", EntryPoint = "GetExitCodeThread", ExactSpelling = true, SetLastError = true)]
    private static extern bool GetExitCodeThread(IntPtr thread, out uint exitCode);

    [DllImport("kernel32.dll", EntryPoint = "VirtualFreeEx", ExactSpelling = true, SetLastError = true)]
    private static extern bool VirtualFreeEx(IntPtr process, IntPtr address,
        UIntPtr size, uint freeType);

    [DllImport("kernel32.dll", EntryPoint = "CloseHandle", ExactSpelling = true, SetLastError = true)]
    private static extern bool CloseHandle(IntPtr handle);
}
