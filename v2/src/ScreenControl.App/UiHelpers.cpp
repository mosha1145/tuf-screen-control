#include "pch.h"
#include "UiHelpers.h"

namespace ScreenControl::Ui
{
    /// 全局配置（首次访问时加载，含 v1 -> v2 迁移）。主题等设置经它持久化。
    ::ScreenControl::Core::ConfigStore& Config()
    {
        static ::ScreenControl::Core::ConfigStore store = []()
        {
            ::ScreenControl::Core::ConfigStore s;
            s.Load();
            return s;
        }();
        return store;
    }
}
