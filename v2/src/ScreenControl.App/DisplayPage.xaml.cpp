#include "pch.h"
#include "DisplayPage.xaml.h"
#include "UiHelpers.h"
#include "PlaybackController.h"
#include "PageTracker.h"
#if __has_include("DisplayPage.g.cpp")
#include "DisplayPage.g.cpp"
#endif

#include <filesystem>

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
using namespace Microsoft::UI::Xaml::Media;
using namespace ::ScreenControl::Ui;

namespace winrt::ScreenControl::implementation
{
    DisplayPage::DisplayPage()
    {
        InitializeComponent();
        ::ScreenControl::TrackPage(L"Display", 1);

        // 页面缓存：切换页面时复用同一实例，避免每次重建整棵 UI 树（内存持续上涨的主因）
        NavigationCacheMode(Microsoft::UI::Xaml::Navigation::NavigationCacheMode::Required);

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
            case 1: params.fitMode = ::ScreenControl::Imaging::FitMode::Cover; break;
            case 2: params.fitMode = ::ScreenControl::Imaging::FitMode::Stretch; break;
            default: params.fitMode = ::ScreenControl::Imaging::FitMode::Contain; break;
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
            case 1: params.rotation = ::ScreenControl::Imaging::Rotation::Deg90; break;
            case 2: params.rotation = ::ScreenControl::Imaging::Rotation::Deg180; break;
            case 3: params.rotation = ::ScreenControl::Imaging::Rotation::Deg270; break;
            default: params.rotation = ::ScreenControl::Imaging::Rotation::Deg0; break;
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

        // ---------------- 交给主窗口做实时更新 ----------------
        MainWindow::Current()->AttachPreview(m_previewSource, m_previewPlaceholder);
        MainWindow::Current()->AttachStatusText(m_statusText);
        MainWindow::Current()->AttachDeviceText(m_deviceText);
    }

    DisplayPage::~DisplayPage()
    {
        ::ScreenControl::TrackPage(L"Display", -1);
    }

    void DisplayPage::OnNavigatedFrom(Microsoft::UI::Xaml::Navigation::NavigationEventArgs const&)
    {
        MainWindow::Current()->DetachPageUi();

        // 页面已开启 NavigationCacheMode.Required（复用实例），
        // 因此这里不能清空 Content，否则再次进入时页面是空白的。
        // 只需断开主窗口对页面控件的引用即可。
    }
}
