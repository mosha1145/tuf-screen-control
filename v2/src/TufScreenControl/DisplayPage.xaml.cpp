#include "pch.h"
#include "DisplayPage.xaml.h"
#include "UiHelpers.h"
#include "MainWindow.xaml.h"
#include "PlaybackController.h"
#if __has_include("DisplayPage.g.cpp")
#include "DisplayPage.g.cpp"
#endif

#include <filesystem>

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
using namespace Microsoft::UI::Xaml::Media;
using namespace ::TufScreenControl::Ui;

namespace winrt::TufScreenControl::implementation
{
    DisplayPage::DisplayPage()
    {
        InitializeComponent();
        // 页面缓存：切换页面时复用同一实例，避免每次重建整棵 UI 树（内存持续上涨的主因）
        // 注意：**不要**开启 NavigationCacheMode::Required（页面缓存）。
        // 实测：开了之后进程退出时必崩，退出码固定 0xC0000005，
        // dump 显示 DisplayPage 的最后一个引用在 Microsoft.UI.Xaml.dll 里析构时炸掉；
        // 关掉缓存（保持默认 Disabled）退出码为 0。省下的那点切页重建开销
        // 不值得换一个必现的退出崩溃。

        // ---------------- 预览 ----------------
        m_previewSource = Media::Imaging::SoftwareBitmapSource{};
        PreviewImage().Source(m_previewSource);
        m_previewImage = PreviewImage();
        m_previewPlaceholder = PreviewPlaceholder();
        m_deviceText = DeviceText();
        m_statusText = StatusText();

        m_deviceText.Text(MainWindow::Current()->Player().DeviceReady()
                              ? L"设备：已连接"
                              : L"设备：未连接");

        // ---------------- 屏幕开关 ----------------
        ScreenSwitch().IsOn(Config().Data().screenOn);
        ScreenSwitch().Toggled([this](IInspectable const& sender, RoutedEventArgs const&)
        {
            if (auto toggle = sender.try_as<ToggleSwitch>())
            {
                // 记住用户设定，下次启动按此恢复
                Config().Data().screenOn = toggle.IsOn();
                Config().Save();
                MainWindow::Current()->Player().SetScreenPower(toggle.IsOn());
            }
        });

        // ---------------- 亮度 ----------------
        BrightnessSlider().Value(Config().Data().brightness);
        // 拖动时实时下发命令；松手（失焦）时才写配置，避免拖一次写上百次文件。
        BrightnessSlider().ValueChanged(
            [this](IInspectable const&, Controls::Primitives::RangeBaseValueChangedEventArgs const& args)
            {
                MainWindow::Current()->Player().SetBrightness(static_cast<int>(args.NewValue()));
            });
        BrightnessSlider().LostFocus([this](IInspectable const&, RoutedEventArgs const&)
        {
            Config().Data().brightness = static_cast<int>(BrightnessSlider().Value());
            Config().Save();
        });

        // ---------------- 图像适应 ----------------
        {
            const std::wstring stored = Config().Data().fitMode;
            FitModeBox().SelectedIndex(stored == L"cover" ? 1 : (stored == L"stretch" ? 2 : 0));
        }
        FitModeBox().SelectionChanged([this](IInspectable const&, SelectionChangedEventArgs const&)
        {
            auto params = MainWindow::Current()->Player().Params();
            switch (FitModeBox().SelectedIndex())
            {
            case 1: params.fitMode = ::TufScreenControl::Imaging::FitMode::Cover; break;
            case 2: params.fitMode = ::TufScreenControl::Imaging::FitMode::Stretch; break;
            default: params.fitMode = ::TufScreenControl::Imaging::FitMode::Contain; break;
            }
            MainWindow::Current()->Player().SetParams(params);

            const int index = FitModeBox().SelectedIndex();
            Config().Data().fitMode = index == 1 ? L"cover" : (index == 2 ? L"stretch" : L"contain");
            Config().Save();
        });

        // ---------------- 画面旋转 ----------------
        {
            const int storedRotation = Config().Data().rotation;
            RotationBox().SelectedIndex(storedRotation == 90 ? 1
                                       : (storedRotation == 180 ? 2
                                                                : (storedRotation == 270 ? 3 : 0)));
        }
        RotationBox().SelectionChanged([this](IInspectable const&, SelectionChangedEventArgs const&)
        {
            auto params = MainWindow::Current()->Player().Params();
            const int index = RotationBox().SelectedIndex();
            switch (index)
            {
            case 1: params.rotation = ::TufScreenControl::Imaging::Rotation::Deg90; break;
            case 2: params.rotation = ::TufScreenControl::Imaging::Rotation::Deg180; break;
            case 3: params.rotation = ::TufScreenControl::Imaging::Rotation::Deg270; break;
            default: params.rotation = ::TufScreenControl::Imaging::Rotation::Deg0; break;
            }
            MainWindow::Current()->Player().SetParams(params);

            Config().Data().rotation = index == 1 ? 90 : (index == 2 ? 180 : (index == 3 ? 270 : 0));
            Config().Save();
        });

        // ---------------- 素材列表 ----------------
        MainWindow::Current()->PopulateMediaList(
            PhotoList(), (Config().AssetRoot() / L"photos").wstring(), L".jpg,.jpeg,.png,.bmp");
        MainWindow::Current()->PopulateMediaList(
            VideoList(), (Config().AssetRoot() / L"videos").wstring(), L".mp4,.avi,.mov,.mkv,.webm,.m4v");

        PlayPhotoButton().Click([this](auto const&, RoutedEventArgs const&)
        {
            // SelectedItem() 返回 ListViewItem 本身而非 hstring，路径存在 Tag 里。
            if (auto item = PhotoList().SelectedItem().try_as<ListViewItem>())
            {
                const std::wstring path = unbox_value<hstring>(item.Tag()).c_str();
                MainWindow::Current()->Player().Play(std::filesystem::path(path));
            }
        });

        PlayVideoButton().Click([this](auto const&, RoutedEventArgs const&)
        {
            if (auto item = VideoList().SelectedItem().try_as<ListViewItem>())
            {
                const std::wstring path = unbox_value<hstring>(item.Tag()).c_str();
                MainWindow::Current()->Player().Play(std::filesystem::path(path));
            }
        });

        StopButton().Click([](auto const&, RoutedEventArgs const&)
        {
            MainWindow::Current()->Player().Stop();
        });

        // 首次进入时挂上句柄（之后的每次导航见 OnNavigatedTo）
        AttachToMainWindow();
    }

    void DisplayPage::AttachToMainWindow()
    {
        // ---------------- 交给主窗口做实时更新 ----------------
        // 页面是缓存的（NavigationCacheMode::Required），构造函数只会跑一次；
        // 离开页面时 MainWindow::DetachPageUi() 会把句柄清空，所以每次导航进来
        // 都要重新挂上，否则切一次页面后预览/状态/设备徽标就再也不更新。
        MainWindow::Current()->AttachPreview(m_previewSource, m_previewPlaceholder);
        MainWindow::Current()->AttachStatusText(m_statusText);
        MainWindow::Current()->AttachDeviceText(m_deviceText);
        MainWindow::Current()->RefreshDeviceBadge();
    }

    void DisplayPage::OnNavigatedTo(Microsoft::UI::Xaml::Navigation::NavigationEventArgs const&)
    {
        AttachToMainWindow();
    }


    void DisplayPage::OnNavigatedFrom(Microsoft::UI::Xaml::Navigation::NavigationEventArgs const&)
    {
        MainWindow::Current()->DetachPageUi();
        // 页面已开启缓存，不能清空 Content（否则再次进入是空白页）
    }
}
