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

        ContentFrame.Navigate(typeof(HomePage));
        RootNavigation.SelectedItem = RootNavigation.MenuItems[0];
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
