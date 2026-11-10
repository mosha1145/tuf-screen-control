#pragma once
#include "SettingsPage.g.h"
namespace winrt::ScreenControl::implementation
{
    struct SettingsPage : SettingsPageT<SettingsPage>
    {
        SettingsPage();
    void OnNavigatedFrom(Microsoft::UI::Xaml::Navigation::NavigationEventArgs const&);
    };
}
namespace winrt::ScreenControl::factory_implementation
{
    struct SettingsPage : SettingsPageT<SettingsPage, implementation::SettingsPage>
    {
    };
}