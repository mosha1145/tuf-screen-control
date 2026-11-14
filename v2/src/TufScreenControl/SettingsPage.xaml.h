#pragma once
#include "SettingsPage.g.h"
namespace winrt::TufScreenControl::implementation
{
    struct SettingsPage : SettingsPageT<SettingsPage>
    {
        SettingsPage();
    void OnNavigatedFrom(Microsoft::UI::Xaml::Navigation::NavigationEventArgs const&);
    };
}
namespace winrt::TufScreenControl::factory_implementation
{
    struct SettingsPage : SettingsPageT<SettingsPage, implementation::SettingsPage>
    {
    };
}