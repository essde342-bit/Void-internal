using Microsoft.UI;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using VoidInjector.Pages;

namespace VoidInjector;

public sealed partial class MainWindow : Window
{
    public MainWindow()
    {
        InitializeComponent();

        ExtendsContentIntoTitleBar = true;
        SetTitleBar(AppTitleBar);

        try
        {
            SystemBackdrop = new MicaBackdrop();
        }
        catch
        {
            // The solid dark background remains as a graceful fallback.
        }

        try
        {
            ContentFrame.Navigate(typeof(HomePage));
            RootNavigation.SelectedItem = RootNavigation.MenuItems[0];
            StartupDiagnostics.Write("Home page loaded successfully.");
        }
        catch (Exception ex)
        {
            StartupDiagnostics.Write("Home page failed to load.", ex);
            ShowHomePageFailure(ex);
        }
    }

    private void ShowHomePageFailure(Exception exception)
    {
        var panel = new StackPanel
        {
            Spacing = 12,
            Margin = new Thickness(32),
            VerticalAlignment = VerticalAlignment.Center
        };

        panel.Children.Add(new TextBlock
        {
            Text = "VOID interface could not load",
            FontSize = 25,
            FontWeight = Microsoft.UI.Text.FontWeights.SemiBold,
            Foreground = new SolidColorBrush(ColorHelper.FromArgb(255, 242, 243, 250))
        });

        panel.Children.Add(new TextBlock
        {
            Text = "The application started, but its Home page reported an error. The details are shown below and saved to the startup log.",
            FontSize = 13,
            TextWrapping = TextWrapping.Wrap,
            Foreground = new SolidColorBrush(ColorHelper.FromArgb(255, 150, 160, 180))
        });

        panel.Children.Add(new TextBlock
        {
            Text = exception.ToString(),
            FontFamily = new Microsoft.UI.Xaml.Media.FontFamily("Consolas"),
            FontSize = 11,
            TextWrapping = TextWrapping.Wrap,
            Foreground = new SolidColorBrush(ColorHelper.FromArgb(255, 235, 180, 190))
        });

        ContentFrame.Content = new ScrollViewer
        {
            Content = panel,
            VerticalScrollBarVisibility = ScrollBarVisibility.Auto
        };
    }

    private void OnNavigationSelectionChanged(
        NavigationView sender,
        NavigationViewSelectionChangedEventArgs args)
    {
        if (args.SelectedItemContainer?.Tag is not string page)
            return;

        Type? pageType = page switch
        {
            "home" => typeof(HomePage),
            "logs" => typeof(LogsPage),
            "settings" => typeof(SettingsPage),
            "about" => typeof(AboutPage),
            _ => null
        };

        if (pageType is not null && ContentFrame.CurrentSourcePageType != pageType)
            ContentFrame.Navigate(pageType);
    }
}