#include "pch.h"
#include "SettingsPage.xaml.h"
#include "UiHelpers.h"
#include "MainWindow.xaml.h"
#include "PlaybackController.h"
#include "ScreenController.h"
#include "FirmwareUpdater.h"
#if __has_include("SettingsPage.g.cpp")
#include "SettingsPage.g.cpp"
#endif

#include <thread>

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
using namespace Microsoft::UI::Xaml::Media;
using namespace ::TufScreenControl::Ui;

namespace winrt::TufScreenControl::implementation
{
    namespace
    {
        /// 主题枚举 -> 界面文字
        wchar_t const* ThemeLabelOf(::TufScreenControl::AppTheme value)
        {
            switch (value)
            {
            case ::TufScreenControl::AppTheme::Light: return L"浅色";
            case ::TufScreenControl::AppTheme::Dark: return L"深色";
            default: return L"使用系统设置";
            }
        }
    }

    SettingsPage::SettingsPage()
    {
        InitializeComponent();
        // 不开页面缓存：原因见 DisplayPage.xaml.cpp 里的说明（会导致退出崩溃）

        auto& config = Config().Data();

        // ---------------- 开机自启动 ----------------
        AutoStartStatus().Text(AutoStartSwitch().IsOn() ? L"开" : L"关");
        AutoStartSwitch().Toggled([status = AutoStartStatus()](IInspectable const& sender, RoutedEventArgs const&)
        {
            if (auto control = sender.try_as<ToggleSwitch>())
            {
                status.Text(control.IsOn() ? L"开" : L"关");
            }
        });

        // ---------------- 关闭窗口时 ----------------
        CloseChoice().SelectedIndex(config.closeAction == L"exit" ? 1 : 0);
        CloseChoice().SelectionChanged([](IInspectable const& sender, SelectionChangedEventArgs const&)
        {
            if (auto box = sender.try_as<ComboBox>())
            {
                Config().Data().closeAction = box.SelectedIndex() == 1 ? L"exit" : L"tray";
                Config().Save();
            }
        });

        // ---------------- 应用主题 ----------------
        const auto currentTheme = MainWindow::Current()->CurrentTheme();
        ThemeValue().Text(ThemeLabelOf(currentTheme));
        // 先回显再挂事件，避免初始化时触发一次多余的应用
        ThemeChoice().SelectedIndex(currentTheme == ::TufScreenControl::AppTheme::Light ? 0
                                    : (currentTheme == ::TufScreenControl::AppTheme::Dark ? 1 : 2));
        ThemeChoice().SelectionChanged([value = ThemeValue()](IInspectable const& sender, SelectionChangedEventArgs const&)
        {
            auto radio = sender.try_as<RadioButtons>();
            if (!radio)
            {
                return;
            }
            ::TufScreenControl::AppTheme theme = ::TufScreenControl::AppTheme::System;
            switch (radio.SelectedIndex())
            {
            case 0: theme = ::TufScreenControl::AppTheme::Light; break;
            case 1: theme = ::TufScreenControl::AppTheme::Dark; break;
            default: theme = ::TufScreenControl::AppTheme::System; break;
            }
            MainWindow::Current()->ApplyTheme(theme, true);
            value.Text(ThemeLabelOf(theme));
        });

        // ---------------- 主题色 ----------------
        auto colorLabelOf = [](std::wstring const& value) -> wchar_t const*
        {
            return value == L"tuf" ? L"TUF GAMING" : L"使用系统设置";
        };

        ThemeColorValue().Text(colorLabelOf(config.themeColor));
        ThemeColorChoice().SelectedIndex(config.themeColor == L"tuf" ? 0 : 1);
        ThemeColorChoice().SelectionChanged([value = ThemeColorValue()](IInspectable const& sender, SelectionChangedEventArgs const&)
        {
            auto radio = sender.try_as<RadioButtons>();
            if (!radio)
            {
                return;
            }
            const std::wstring chosen = radio.SelectedIndex() == 0 ? L"tuf" : L"system";
            Config().Data().themeColor = chosen;
            Config().Save();
            value.Text(chosen == L"tuf" ? L"TUF GAMING" : L"使用系统设置");
        });

        // ---------------- 睡眠模式 ----------------
        SleepToggle().IsOn(config.sleepMode);
        SleepStatus().Text(config.sleepMode ? L"开" : L"关");
        SleepToggle().Toggled([status = SleepStatus()](IInspectable const& sender, RoutedEventArgs const&)
        {
            if (auto control = sender.try_as<ToggleSwitch>())
            {
                const bool on = control.IsOn();
                Config().Data().sleepMode = on;
                Config().Save();
                status.Text(on ? L"开" : L"关");
                MainWindow::Current()->Player().SetSleepMode(on);
            }
        });

        // ---------------- 自动检测更新 ----------------
        std::wstring firmware = ::TufScreenControl::Device::ScreenController::FirmwareRevision();
        if (firmware.empty())
        {
            firmware = L"未知";
        }
        UpdateDesc().Text(L"固件版本 " + firmware);

        UpdateToggle().IsOn(config.autoCheckUpdate);
        UpdateStatus().Text(config.autoCheckUpdate ? L"开" : L"关");
        UpdateToggle().Toggled([status = UpdateStatus()](IInspectable const& sender, RoutedEventArgs const&)
        {
            if (auto control = sender.try_as<ToggleSwitch>())
            {
                const bool on = control.IsOn();
                Config().Data().autoCheckUpdate = on;
                Config().Save();
                status.Text(on ? L"开" : L"关");
            }
        });

        // ---------------- 检查频率 ----------------
        auto freqIndex = [](std::wstring const& value) -> int
        {
            if (value == L"daily") { return 1; }
            if (value == L"weekly") { return 2; }
            if (value == L"monthly") { return 3; }
            if (value == L"halfyear") { return 4; }
            return 0;
        };
        auto freqValueOf = [](int index) -> wchar_t const*
        {
            switch (index)
            {
            case 1: return L"daily";
            case 2: return L"weekly";
            case 3: return L"monthly";
            case 4: return L"halfyear";
            default: return L"startup";
            }
        };
        auto freqLabelOf = [](int index) -> wchar_t const*
        {
            switch (index)
            {
            case 1: return L"每天";
            case 2: return L"每周";
            case 3: return L"每月";
            case 4: return L"每半年";
            default: return L"每次启动";
            }
        };

        FreqBox().SelectedIndex(freqIndex(config.updateFrequency));
        FreqValue().Text(freqLabelOf(FreqBox().SelectedIndex()));
        FreqBox().SelectionChanged(
            [freqValueOf, freqLabelOf, value = FreqValue()](IInspectable const& sender, SelectionChangedEventArgs const&)
            {
                if (auto box = sender.try_as<ComboBox>())
                {
                    Config().Data().updateFrequency = freqValueOf(box.SelectedIndex());
                    Config().Save();
                    value.Text(freqLabelOf(box.SelectedIndex()));
                }
            });

        // ---------------- 立即检查 ----------------
        CheckNowButton().Click(
            [this, firmware,
             button = CheckNowButton(),
             ring = CheckRing(),
             bar = ResultBar()](IInspectable const&, RoutedEventArgs const&)
            {
                button.IsEnabled(false);
                ring.IsActive(true);
                ring.Visibility(Visibility::Visible);

                auto xamlRoot = this->XamlRoot();
                auto queue = this->DispatcherQueue();

                std::thread([button, ring, firmware, xamlRoot, queue, bar]()
                {
                    const auto release = ::TufScreenControl::Ui::Firmware::FetchLatest();

                    queue.TryEnqueue([button, ring, firmware, xamlRoot, release, bar]()
                    {
                        button.IsEnabled(true);
                        ring.IsActive(false);
                        ring.Visibility(Visibility::Collapsed);

                        if (!release.found)
                        {
                            bar.Severity(InfoBarSeverity::Error);
                            bar.Title(L"检查更新失败");
                            bar.Message(L"无法获取固件信息，请检查网络后重试。");
                            bar.IsOpen(true);
                            return;
                        }

                        int current = 0;
                        try
                        {
                            current = std::stoi(firmware);
                        }
                        catch (...)
                        {
                            current = 0;
                        }

                        if (release.version > current)
                        {
                            bar.Severity(InfoBarSeverity::Informational);
                            bar.Title(L"发现新固件 " + std::to_wstring(release.version));
                            bar.Message(L"当前版本 " + firmware + L"，安装包 " + release.fileSize + L"。");
                            bar.IsOpen(true);
                            if (xamlRoot)
                            {
                                ::TufScreenControl::Ui::Firmware::ShowDialog(xamlRoot, release);
                            }
                        }
                        else
                        {
                            bar.Severity(InfoBarSeverity::Success);
                            bar.Title(L"已是最新固件");
                            bar.Message(L"当前版本 " + firmware + L"。");
                            bar.IsOpen(true);
                        }
                    });
                }).detach();
            });
    }


    void SettingsPage::OnNavigatedFrom(Microsoft::UI::Xaml::Navigation::NavigationEventArgs const&)
    {
        // 页面已开启缓存，只需断开主窗口引用
    }
}
