using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using System.IO;
using VoidInjector.Services;
using Windows.Storage;
using Windows.Storage.Pickers;
using WinRT.Interop;

namespace VoidInjector.Pages;

public sealed partial class LogsPage : Page
{
    public LogsPage()
    {
        InitializeComponent();
        Loaded += async (_, _) => await RefreshLogAsync();
    }

    private async Task RefreshLogAsync()
    {
        try
        {
            var text = await LogService.ReadAsync();
            LogPreview.Text = text;
            FileStateText.Text = File.Exists(LogService.LogPath)
                ? $"Updated {File.GetLastWriteTime(LogService.LogPath):yyyy-MM-dd HH:mm:ss}  •  {new FileInfo(LogService.LogPath).Length:N0} bytes"
                : "No session file yet";
        }
        catch (Exception ex)
        {
            LogPreview.Text = $"Could not read the log file: {ex.Message}";
            FileStateText.Text = "Read error";
        }
    }

    private async void OnRefreshClicked(object sender, RoutedEventArgs e)
    {
        await RefreshLogAsync();
    }

    private async void OnDownloadClicked(object sender, RoutedEventArgs e)
    {
        try
        {
            if (!File.Exists(LogService.LogPath))
            {
                var noLog = new ContentDialog
                {
                    Title = "No log available",
                    Content = "Run an operation from Home first. Its diagnostic log will appear here.",
                    CloseButtonText = "OK",
                    XamlRoot = XamlRoot
                };
                await noLog.ShowAsync();
                return;
            }

            var picker = new FileSavePicker
            {
                SuggestedFileName = "last_run.txt",
                SuggestedStartLocation = PickerLocationId.Downloads
            };
            picker.FileTypeChoices.Add("Text document", new List<string> { ".txt" });

            var hwnd = WindowNative.GetWindowHandle(App.CurrentWindow!);
            InitializeWithWindow.Initialize(picker, hwnd);

            StorageFile? destination = await picker.PickSaveFileAsync();
            if (destination is null)
                return;

            var contents = await File.ReadAllTextAsync(LogService.LogPath);
            await FileIO.WriteTextAsync(destination, contents);

            var saved = new ContentDialog
            {
                Title = "Log saved",
                Content = $"Your log was saved to {destination.Path}",
                CloseButtonText = "Done",
                XamlRoot = XamlRoot
            };
            await saved.ShowAsync();
        }
        catch (Exception ex)
        {
            var error = new ContentDialog
            {
                Title = "Download failed",
                Content = ex.Message,
                CloseButtonText = "Close",
                XamlRoot = XamlRoot
            };
            await error.ShowAsync();
        }
    }
}
