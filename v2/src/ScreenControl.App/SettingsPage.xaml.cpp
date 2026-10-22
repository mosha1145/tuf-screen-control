#include "pch.h"
#include "SettingsPage.xaml.h"
#include "UiHelpers.h"
#include "PlaybackController.h"
#include <winrt/Microsoft.UI.Xaml.Media.Animation.h>
#if __has_include("SettingsPage.g.cpp")
#include "SettingsPage.g.cpp"
#endif
using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
using namespace Microsoft::UI::Xaml::Media;
using namespace ::ScreenControl::Ui;
namespace winrt::ScreenControl::implementation
{
    SettingsPage::SettingsPage()
    {
        InitializeComponent();

        StackPanel page;
        page.Spacing(4);
        page.Margin(Thickness{ 24, 8, 24, 24 });

        page.Children().Append(MakeTitle(L"设置"));

        // 开机自启动：左侧文本、右侧开关。
        // 开关旁不再用 OnContent/OffContent（它们渲染在开关右侧），
        // 改为在开关左侧放"开/关"文字，垂直居中对齐。
        ToggleSwitch autoStart;
        autoStart.MinWidth(0);
        autoStart.VerticalAlignment(VerticalAlignment::Center);
        autoStart.HorizontalAlignment(HorizontalAlignment::Right);

        Grid autoStartRow;
        autoStartRow.ColumnDefinitions().Append([] { ColumnDefinition c; c.Width(GridLengthHelper::Auto()); return c; }());
        autoStartRow.ColumnDefinitions().Append([] { ColumnDefinition c; c.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star)); return c; }());
        autoStartRow.ColumnDefinitions().Append([] { ColumnDefinition c; c.Width(GridLengthHelper::Auto()); return c; }());
        autoStartRow.ColumnDefinitions().Append([] { ColumnDefinition c; c.Width(GridLengthHelper::Auto()); return c; }());

        FontIcon autoStartIcon;
        autoStartIcon.Glyph(L"\xE7E8");
        autoStartIcon.FontSize(16);
        autoStartIcon.VerticalAlignment(VerticalAlignment::Center);
        Grid::SetColumn(autoStartIcon, 0);
        autoStartRow.Children().Append(autoStartIcon);

        TextBlock autoStartLabel;
        autoStartLabel.Text(L"开机自启动");
        autoStartLabel.VerticalAlignment(VerticalAlignment::Center);
        autoStartLabel.Margin(Thickness{ 16, 0, 0, 0 });
        Grid::SetColumn(autoStartLabel, 1);
        autoStartRow.Children().Append(autoStartLabel);

        TextBlock autoStartStatus;
        autoStartStatus.FontSize(12);
        autoStartStatus.Text(autoStart.IsOn() ? L"开" : L"关");
        autoStartStatus.VerticalAlignment(VerticalAlignment::Center);
        autoStartStatus.Margin(Thickness{ 0, 0, 12, 0 });
        Grid::SetColumn(autoStartStatus, 2);
        autoStartRow.Children().Append(autoStartStatus);

        autoStart.Toggled([autoStartStatus](IInspectable const& sender, RoutedEventArgs const&)
        {
            if (auto control = sender.try_as<ToggleSwitch>())
            {
                autoStartStatus.Text(control.IsOn() ? L"开" : L"关");
            }
        });

        Grid::SetColumn(autoStart, 3);
        autoStartRow.Children().Append(autoStart);
        // 关闭窗口时的行为（下拉选择，与开机自启动同一张卡）
        Grid closeRow;
        closeRow.ColumnDefinitions().Append([] { ColumnDefinition c; c.Width(GridLengthHelper::Auto()); return c; }());
        closeRow.ColumnDefinitions().Append([] { ColumnDefinition c; c.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star)); return c; }());
        closeRow.ColumnDefinitions().Append([] { ColumnDefinition c; c.Width(GridLengthHelper::Auto()); return c; }());

        FontIcon closeIcon;
        closeIcon.Glyph(L"\xE8BB");
        closeIcon.FontSize(16);
        closeIcon.VerticalAlignment(VerticalAlignment::Center);
        Grid::SetColumn(closeIcon, 0);
        closeRow.Children().Append(closeIcon);

        TextBlock closeLabel;
        closeLabel.Text(L"关闭窗口时");
        closeLabel.VerticalAlignment(VerticalAlignment::Center);
        closeLabel.Margin(Thickness{ 16, 0, 0, 0 });
        Grid::SetColumn(closeLabel, 1);
        closeRow.Children().Append(closeLabel);

        ComboBox closeChoice;
        closeChoice.MinWidth(180);
        closeChoice.VerticalAlignment(VerticalAlignment::Center);
        closeChoice.Items().Append(box_value(L"隐藏到系统托盘"));
        closeChoice.Items().Append(box_value(L"退出程序"));


        auto const& closeConfig = ::ScreenControl::Ui::Config().Data();
        closeChoice.SelectedIndex(closeConfig.closeAction == L"exit" ? 1 : 0);

        closeChoice.SelectionChanged([this, closeChoice](IInspectable const&, SelectionChangedEventArgs const&)
        {
            const std::wstring value = closeChoice.SelectedIndex() == 1 ? L"exit" : L"tray";
            auto& config = ::ScreenControl::Ui::Config().Data();
            config.closeAction = value;
            ::ScreenControl::Ui::Config().Save();
        });

        Grid::SetColumn(closeChoice, 2);
        closeRow.Children().Append(closeChoice);

        // 主题：参考系统设置页的做法 —— 可展开条目，标题行左侧图标 + 名称、
        // 右侧显示当前选项；展开后三选一。顺序按参考图：浅色 / 深色 / 使用系统设置。
        auto themeLabelOf = [](::ScreenControl::AppTheme value) -> wchar_t const*
        {
            switch (value)
            {
            case ::ScreenControl::AppTheme::Light: return L"浅色";
            case ::ScreenControl::AppTheme::Dark: return L"深色";
            default: return L"使用系统设置";
            }
        };

        TextBlock themeValue;
        themeValue.FontSize(12);
        themeValue.VerticalAlignment(VerticalAlignment::Center);
        themeValue.Text(themeLabelOf(MainWindow::Current()->CurrentTheme()));

        RadioButtons themeChoice;
        themeChoice.Items().Append(box_value(L"浅色"));
        themeChoice.Items().Append(box_value(L"深色"));
        themeChoice.Items().Append(box_value(L"使用系统设置"));

        // 先回显再挂事件，避免初始化时触发一次多余的应用。
        themeChoice.SelectedIndex(
            MainWindow::Current()->CurrentTheme() == ::ScreenControl::AppTheme::Light ? 0
            : (MainWindow::Current()->CurrentTheme() == ::ScreenControl::AppTheme::Dark ? 1 : 2));

        themeChoice.SelectionChanged([this, themeValue, themeLabelOf](IInspectable const& sender, SelectionChangedEventArgs const&)
        {
            auto radio = sender.try_as<RadioButtons>();
            if (!radio)
            {
                return;
            }
            ::ScreenControl::AppTheme theme = ::ScreenControl::AppTheme::System;
            switch (radio.SelectedIndex())
            {
            case 0: theme = ::ScreenControl::AppTheme::Light; break;
            case 1: theme = ::ScreenControl::AppTheme::Dark; break;
            default: theme = ::ScreenControl::AppTheme::System; break;
            }
            MainWindow::Current()->ApplyTheme(theme, true);
            themeValue.Text(themeLabelOf(theme));
        });


        // 「常规」与「个性化」拆成两张卡：开机自启属于启动行为，主题属于外观，不应混在一起。
        page.Children().Append(MakeSectionTitle(L"应用行为"));
        page.Children().Append(MakeBehaviorCard(autoStartRow));
        page.Children().Append(MakeBehaviorCard(closeRow));
        page.Children().Append(MakeSectionTitle(L"个性化"));

        // Expander 自身就是一张卡片，不再额外套一层 MakeCard，
        // 否则会出现双层边框与多余内边距（也就是"多包了一层"）。
        page.Children().Append(MakeSettingsCard(L"\xE706", L"应用主题", themeValue, themeChoice));

        // ---------------- 主题色 ----------------
        auto& config = ::ScreenControl::Ui::Config().Data();

        auto colorLabelOf = [](std::wstring const& value) -> wchar_t const*
        {
            return value == L"tuf" ? L"TUF GAMING" : L"使用系统设置";
        };

        TextBlock colorValue;
        colorValue.FontSize(12);
        colorValue.VerticalAlignment(VerticalAlignment::Center);
        colorValue.Text(colorLabelOf(config.themeColor));


        RadioButtons colorChoice;
        colorChoice.Items().Append(box_value(L"TUF GAMING"));
        colorChoice.Items().Append(box_value(L"使用系统设置"));
        colorChoice.SelectedIndex(config.themeColor == L"tuf" ? 0 : 1);
        colorChoice.SelectionChanged([this, colorValue, colorLabelOf](IInspectable const& sender, SelectionChangedEventArgs const&)
        {
            auto radio = sender.try_as<RadioButtons>();
            if (!radio)
            {
                return;
            }
            const std::wstring value = radio.SelectedIndex() == 0 ? L"tuf" : L"system";
            ::ScreenControl::Ui::Config().Data().themeColor = value;
            ::ScreenControl::Ui::Config().Save();
            colorValue.Text(colorLabelOf(value));
        });

        page.Children().Append(MakeSettingsCard(L"\xE790", L"主题色", colorValue, colorChoice));

// ---------------- 屏幕设置 ----------------
        page.Children().Append(MakeSectionTitle(L"屏幕设置"));

        ToggleSwitch sleepToggle;
        sleepToggle.IsOn(config.sleepMode);
        sleepToggle.MinWidth(0);
        sleepToggle.VerticalAlignment(VerticalAlignment::Center);
        sleepToggle.HorizontalAlignment(HorizontalAlignment::Right);

        Grid sleepRow;
        sleepRow.ColumnDefinitions().Append([] { ColumnDefinition c; c.Width(GridLengthHelper::Auto()); return c; }());
        sleepRow.ColumnDefinitions().Append([] { ColumnDefinition c; c.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star)); return c; }());
        sleepRow.ColumnDefinitions().Append([] { ColumnDefinition c; c.Width(GridLengthHelper::Auto()); return c; }());
        sleepRow.ColumnDefinitions().Append([] { ColumnDefinition c; c.Width(GridLengthHelper::Auto()); return c; }());

        FontIcon sleepIcon;
        sleepIcon.Glyph(L"\xE708");
        sleepIcon.FontSize(16);
        sleepIcon.VerticalAlignment(VerticalAlignment::Center);
        Grid::SetColumn(sleepIcon, 0);
        sleepRow.Children().Append(sleepIcon);

        StackPanel sleepText;
        sleepText.Spacing(2);
        sleepText.VerticalAlignment(VerticalAlignment::Center);
        sleepText.Margin(Thickness{ 16, 0, 0, 0 });
        TextBlock sleepTitle;
        sleepTitle.Text(L"睡眠模式");
        TextBlock sleepDesc;
        sleepDesc.Text(L"无内容播放时关闭屏幕");
        sleepDesc.FontSize(12);
        sleepDesc.Foreground(Application::Current().Resources()
                                 .Lookup(box_value(L"TextFillColorSecondaryBrush")).as<Brush>());
        sleepText.Children().Append(sleepTitle);
        sleepText.Children().Append(sleepDesc);
        Grid::SetColumn(sleepText, 1);
        sleepRow.Children().Append(sleepText);

        TextBlock sleepStatus;
        sleepStatus.FontSize(12);
        sleepStatus.Text(config.sleepMode ? L"开" : L"关");
        sleepStatus.VerticalAlignment(VerticalAlignment::Center);
        sleepStatus.Margin(Thickness{ 0, 0, 12, 0 });
        Grid::SetColumn(sleepStatus, 2);
        sleepRow.Children().Append(sleepStatus);

        sleepToggle.Toggled([sleepStatus](IInspectable const& sender, RoutedEventArgs const&)
        {
            if (auto control = sender.try_as<ToggleSwitch>())
            {
                const bool on = control.IsOn();
                ::ScreenControl::Ui::Config().Data().sleepMode = on;
                ::ScreenControl::Ui::Config().Save();
                sleepStatus.Text(on ? L"开" : L"关");
                MainWindow::Current()->Player().SetSleepMode(on);
            }
        });

        Grid::SetColumn(sleepToggle, 3);
        sleepRow.Children().Append(sleepToggle);

        page.Children().Append(MakeBehaviorCard(sleepRow));
        Content(MakeScrollable(page));
    }
}