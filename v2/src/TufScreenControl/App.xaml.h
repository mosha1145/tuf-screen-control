#pragma once

#include "App.xaml.g.h"

namespace TufScreenControl
{
    /// 单实例 key（App 注册 / 第二实例重定向 / MainWindow 订阅激活都引用它）。
    inline constexpr wchar_t kInstanceKey[] = L"TufScreenControl.MainInstance";
}

namespace winrt::TufScreenControl::implementation
{
    // 应用对象（x:Class="TufScreenControl.App" 的代码后台声明）。
    // AppT<App> 由 XAML 编译器生成的 App.xaml.g.h 提供。
    struct App : AppT<App>
    {
        App();

        void OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const&);

    private:
        winrt::Microsoft::UI::Xaml::Window window{ nullptr };
    };
}
