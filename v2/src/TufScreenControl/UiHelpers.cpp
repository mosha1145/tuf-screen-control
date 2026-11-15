#include "pch.h"
#include "UiHelpers.h"
#include "NativeTheme.h"

namespace TufScreenControl::Ui
{
    /// 全局配置（首次访问时加载，含 v1 -> v2 迁移）。主题等设置经它持久化。
    ::TufScreenControl::Core::ConfigStore& Config()
    {
        static ::TufScreenControl::Core::ConfigStore store = []()
        {
            ::TufScreenControl::Core::ConfigStore s;
            s.Load();
            return s;
        }();
        return store;
    }

    winrt::Microsoft::UI::Xaml::ElementTheme ToElementTheme(
        ::TufScreenControl::AppTheme theme) noexcept
    {
        using winrt::Microsoft::UI::Xaml::ElementTheme;
        switch (theme)
        {
        case ::TufScreenControl::AppTheme::Light: return ElementTheme::Light;
        case ::TufScreenControl::AppTheme::Dark: return ElementTheme::Dark;
        default:
            // 跟随系统：弹出层不会自己解析，这里取系统实际深浅
            return ::TufScreenControl::NativeTheme::ResolveIsDark(theme) ? ElementTheme::Dark
                                                                        : ElementTheme::Light;
        }
    }
}
