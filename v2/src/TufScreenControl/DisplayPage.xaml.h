#pragma once
#include "DisplayPage.g.h"
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
namespace winrt::TufScreenControl::implementation
{
    struct DisplayPage : DisplayPageT<DisplayPage>
    {
        DisplayPage();
        void OnNavigatedTo(Microsoft::UI::Xaml::Navigation::NavigationEventArgs const& e);
        void OnNavigatedFrom(Microsoft::UI::Xaml::Navigation::NavigationEventArgs const& e);
    private:
        /// 把预览/状态/设备徽标句柄重新挂到主窗口（每帧预览都走这些句柄）。
        void AttachToMainWindow();
    private:
        winrt::Microsoft::UI::Xaml::Media::Imaging::SoftwareBitmapSource m_previewSource{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::TextBlock m_previewPlaceholder{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::TextBlock m_statusText{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::TextBlock m_deviceText{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::Image m_previewImage{ nullptr };
    };
}
namespace winrt::TufScreenControl::factory_implementation
{
    struct DisplayPage : DisplayPageT<DisplayPage, implementation::DisplayPage>
    {
    };
}