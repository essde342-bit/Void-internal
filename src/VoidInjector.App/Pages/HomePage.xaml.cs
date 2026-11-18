using Microsoft.UI;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using VoidInjector.Services;

namespace VoidInjector.Pages;

public sealed partial class HomePage : Page
{
    private readonly InjectorService _injector = new();

    public HomePage()
    {
        InitializeComponent();
    }

    private async void OnInjectClicked(object sender, RoutedEventArgs e)
    {
        LaunchButton.IsEnabled = false;
        BusyRing.Visibility = Visibility.Visible;
        BusyRing.IsActive = true;
        OperationProgress.Value = 2;
        StatusIndicator.Fill = new SolidColorBrush(ColorHelper.FromArgb(255, 148, 116, 255));
        StatusText.Text = "Preparing operation";
        DetailText.Text = "Checking files and preparing Counter-Strike 2...";

        var progress = new Progress<InjectionProgress>(update =>
        {
            OperationProgress.Value = update.Percent;
            StatusText.Text = update.Status;
            DetailText.Text = update.Detail;
        });

        try
        {
            var result = await _injector.RunAsync(progress);
            StatusText.Text = result.Status;
            DetailText.Text = result.Detail;
            OperationProgress.Value = result.Success ? 100 : 0;
            StatusIndicator.Fill = result.Success
                ? new SolidColorBrush(ColorHelper.FromArgb(255, 85, 214, 161))
                : new SolidColorBrush(ColorHelper.FromArgb(255, 255, 113, 133));
        }
        finally
        {
            BusyRing.IsActive = false;
            BusyRing.Visibility = Visibility.Collapsed;
            LaunchButton.IsEnabled = true;
        }
    }
}
