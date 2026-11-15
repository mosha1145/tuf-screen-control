#pragma once
#include <windows.h>
#include <string>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include "AppConfig.h"
#include "NativeTheme.h"

namespace TufScreenControl::Ui
{
    /// 全局配置（首次访问时加载，含 v1 -> v2 迁移）。主题等设置经它持久化。
    ::TufScreenControl::Core::ConfigStore& Config();

    /// 把应用主题解析成可直接赋给 RequestedTheme 的 ElementTheme。
    ///
    /// 为什么必须给对话框显式设置：ContentDialog / Flyout 这类**弹出层**有自己的
    /// 视觉树，不继承父元素上的 RequestedTheme，于是「应用深色、系统浅色」时
    /// 它们仍是浅色。官方对此的说明是「代码里创建的 ContentDialog 需要自己设
    /// RequestedTheme」（microsoft/microsoft-ui-xaml#1582），Flyout 同理（#6622）。
    /// 这里把 System 解析成实际深浅，保证弹窗与主界面一致。
    winrt::Microsoft::UI::Xaml::ElementTheme ToElementTheme(::TufScreenControl::AppTheme theme) noexcept;
}
