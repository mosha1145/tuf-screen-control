#pragma once
#include <windows.h>
#include <string>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include "AppConfig.h"

namespace ScreenControl::Ui
{
    /// 全局配置（首次访问时加载，含 v1 -> v2 迁移）。主题等设置经它持久化。
    ::ScreenControl::Core::ConfigStore& Config();
}
