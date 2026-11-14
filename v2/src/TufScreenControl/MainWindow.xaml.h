#pragma once

// 主窗口（x:Class="TufScreenControl.MainWindow" 的代码后台声明）。
// 与官方 WinUI3 C++ 模板一致：include C++/WinRT 生成的 MainWindow.g.h
// （内含 MainWindow_base 基类），它会按 __has_include 自动带上 XAML 编译器
// 生成的 MainWindow.xaml.g.h（MainWindowT 模板 + x:Name 元素访问器）。
// 实现类定义在 winrt::TufScreenControl::implementation 命名空间。

#include "MainWindow.g.h"
#include "NativeTheme.h"
#include "PowerMonitor.h"
#include "TrayIcon.h"
#include "SingleInstance.h"

#include <memory>
#include <winrt/Windows.Graphics.Imaging.h>

namespace TufScreenControl::Core { class PlaybackController; }

namespace winrt::TufScreenControl::implementation
{
    struct MainWindow : MainWindowT<MainWindow>
    {
        MainWindow();
        ~MainWindow();

        int32_t MyProperty();
        void MyProperty(int32_t value);

        /// 当前实例（页面通过它访问播放器与共享 UI 句柄）。
        static MainWindow* Current() noexcept;

        /// 应用主题（跟随系统 / 浅色 / 深色）。persist = true 时同时写回配置。
        void ApplyTheme(::TufScreenControl::AppTheme theme, bool persist);

        /// 当前主题（供设置页回显）。
        ::TufScreenControl::AppTheme CurrentTheme() const noexcept;

        /// 懒创建播放控制器（并尝试打开设备）。
        ::TufScreenControl::Core::PlaybackController& Player();

        /// 把素材目录里的文件填进 ListView。
        void PopulateMediaList(winrt::Microsoft::UI::Xaml::Controls::ListView const& list,
                               std::wstring const& directory,
                               std::wstring const& extensionsCsv);

        // 页面注册自己的 UI 句柄：播放器的状态/预览回调统一写到这些句柄上。
        void AttachStatusText(winrt::Microsoft::UI::Xaml::Controls::TextBlock const& text);
        void AttachDeviceText(winrt::Microsoft::UI::Xaml::Controls::TextBlock const& text);
        void AttachPreview(winrt::Microsoft::UI::Xaml::Media::Imaging::SoftwareBitmapSource const& source,
                           winrt::Microsoft::UI::Xaml::Controls::TextBlock const& placeholder);
        void DetachPageUi();

    private:
        void SetupBackdropAndTitleBar();

        /// 当前窗口的原生 HWND（用于设置标题栏深色 / 系统菜单深色）。
        HWND GetHwnd() const noexcept;

        void ShutdownPlayer() noexcept;

        /// 注册电源 / 会话事件（睡眠、关机、注销）与窗口关闭处理。
        void SetupPowerEvents();

        /// 启动时把配置里的显示参数与屏幕状态应用到设备。
        void ApplyPersistedDeviceState();

        /// 按配置的频率在启动时检查固件更新（有新版弹窗）。
        void MaybeCheckFirmwareUpdate();

        /// 刷新预览框左上角的设备连接状态徽标。
        void RefreshDeviceBadge();

        // ---------------- 托盘 / 关闭行为 ----------------
        void SetupTrayAndCloseBehavior();
        /// 隐藏前把界面切成实色底并摘掉 Mica，避免关闭/隐藏瞬时白闪。
        void PrepareWindowForHide();
        void HideToTray();
        void ShowFromTray();
        void ShowTrayMenu();
        void ShowClosePrompt();
        void ExitApplication();

        /// 用一帧 BGRA 像素刷新预览图（必须在 UI 线程调用）。
        void UpdatePreview(std::vector<std::uint8_t> const& pixels, int width, int height);

        winrt::Microsoft::UI::Xaml::Controls::Frame m_contentFrame{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::Grid m_titleBar{ nullptr };
        ::TufScreenControl::AppTheme m_theme{ ::TufScreenControl::AppTheme::System };

        std::unique_ptr<::TufScreenControl::Core::PlaybackController> m_player;
        winrt::Microsoft::UI::Xaml::Controls::TextBlock m_statusText{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::TextBlock m_deviceText{ nullptr };
        winrt::Microsoft::UI::Xaml::Media::Imaging::SoftwareBitmapSource m_previewSource{ nullptr };
        winrt::Microsoft::UI::Xaml::Controls::TextBlock m_previewPlaceholder{ nullptr };
        /// 预览复用位图：避免每帧新建 SoftwareBitmap（内存持续增长的主因）
        winrt::Windows::Graphics::Imaging::SoftwareBitmap m_previewBitmap{ nullptr };
        /// 上一帧是否还在渲染，未完成则丢弃新帧，避免异步任务积压
        bool m_previewPending = false;
        std::wstring m_currentTag{ L"" };
        ::TufScreenControl::PowerMonitor m_powerMonitor;
        ::TufScreenControl::TrayIcon m_trayIcon;

        bool m_trayAvailable = false;
        bool m_firstNavigateDone = false;
        /// 首次导航是否已完成（放到窗口 Loaded 之后执行）
        bool m_initialNavigationDone = false;
        bool m_exiting = false;
        bool m_closePromptOpen = false;
    };
}

namespace winrt::TufScreenControl::factory_implementation
{
    struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow>
    {
    };
}
