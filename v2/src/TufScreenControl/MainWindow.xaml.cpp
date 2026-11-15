#include "pch.h"
#include "MainWindow.xaml.h"
#include <winrt/Microsoft.UI.Xaml.Interop.h>
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif

#include <winrt/Microsoft.UI.Windowing.h>

#include "AppConfig.h"
#include "PlaybackController.h"
#include "UiHelpers.h"
#include "App.xaml.h"
#include "SettingsPage.xaml.h"
#include "ScreenController.h"
#include "FirmwareUpdater.h"
#include "DisplayPage.xaml.h"
#include "SettingsPage.xaml.h"

#include <filesystem>
#include <vector>

#include <winrt/Windows.UI.Xaml.Interop.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Microsoft.UI.Xaml.Media.Animation.h>
#include <winrt/Microsoft.Windows.AppLifecycle.h>
#include <functional>
#include <fstream>
#include <memory>

#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
using namespace Microsoft::UI::Xaml::Media;

namespace winrt::TufScreenControl::implementation
{

static ::winrt::TufScreenControl::implementation::MainWindow* s_current = nullptr;


    MainWindow::MainWindow()
    {
        // 从 MainWindow.xaml 加载界面（RootGrid / AppTitleBar / NavView / ContentFrame）。
        InitializeComponent();

        s_current = this;

        // 标题栏与导航事件（布局全部在 XAML 里声明）。
        SetupBackdropAndTitleBar();

        NavView().SelectionChanged(
            [this](NavigationView const&, NavigationViewSelectionChangedEventArgs const& args)
        {
            auto item = args.SelectedItem().try_as<NavigationViewItem>();
            if (!item)
            {
                return;
            }
            NavigateTo(unbox_value<hstring>(item.Tag()));
        });

        // 启动时按配置恢复主题（含「跟随系统」）。
        m_theme = ::TufScreenControl::NativeTheme::Parse(::TufScreenControl::Ui::Config().Data().appTheme);
        ApplyTheme(m_theme, false);

        // 恢复上次的显示参数与屏幕开关状态
        ApplyPersistedDeviceState();

        // 设备插拔监听、单实例激活转发与窗口关闭处理。
        SetupSystemEvents();

        // 托盘图标 + 关闭行为（隐藏到托盘 / 退出 / 询问）
        SetupTrayAndCloseBehavior();

        // 首次导航放到窗口完成第一次布局之后。
        // 在构造函数里导航时 Frame 还是 0×0、窗口未显示，
        // EntranceNavigationTransitionInfo 没有可播放的目标，冷启动就没有入场动画。
        RootGrid().Loaded([this](auto&&, auto&&)
        {
            if (!m_initialNavigationDone)
            {
                m_initialNavigationDone = true;
                // 触发 SelectionChanged -> Frame.Navigate(Entrance...)
                NavView().SelectedItem(DisplayNavItem());
            }

            // 固件更新检查：此时 XamlRoot 才可用
            MaybeCheckFirmwareUpdate();
        });
    }

    void MainWindow::NavigateTo(hstring const& tag)
    {
        // 判重：同一个页面不重复导航
        if (!m_currentTag.empty() && tag == m_currentTag)
        {
            return;
        }
        m_currentTag = tag;

        // 导航切换动画：官方 EntranceNavigationTransitionInfo（淡入 + 从底部上浮）
        // 页面类型用官方的 winrt::xaml_typename<T>()（定义在 Windows.UI.Xaml.Interop.h），
        // 由 C++/WinRT 生成完整的 TypeName，不再手搓 Name + TypeKind。
        winrt::Windows::UI::Xaml::Interop::TypeName pageType =
            tag == L"settings" ? winrt::xaml_typename<TufScreenControl::SettingsPage>()
                               : winrt::xaml_typename<TufScreenControl::DisplayPage>();

        Microsoft::UI::Xaml::Media::Animation::EntranceNavigationTransitionInfo entrance;
        ContentFrame().Navigate(pageType, nullptr, entrance);

        // 关键：必须在 Navigate 之后清空。
        // Navigate 会把"上一个页面"压入 BackStack，先清再导航等于没清，
        // 旧页面（连同它的预览位图）会被一直引用，表现为每次切换内存增长。
        ContentFrame().BackStack().Clear();
    }

    void MainWindow::RefreshDeviceBadge()
    {
        if (m_deviceText)
        {
            const bool ready = m_player && m_player->DeviceReady();
            m_deviceText.Text(ready ? L"设备：已连接" : L"设备：未连接");
        }
    }

    void MainWindow::MaybeCheckFirmwareUpdate()
    {
        auto& config = ::TufScreenControl::Ui::Config().Data();
        if (!config.autoCheckUpdate)
        {
            return;
        }

        // 没枚举到设备就没有"当前固件"可比，直接跳过，避免误报更新。
        if (!m_player || !m_player->DeviceReady())
        {
            return;
        }

        const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();

        long long interval = 0;   // 每次启动：0，代表不限制
        if (config.updateFrequency == L"daily")
        {
            interval = 86400;
        }
        else if (config.updateFrequency == L"weekly")
        {
            interval = 604800;
        }
        else if (config.updateFrequency == L"monthly")
        {
            interval = 2592000;
        }
        else if (config.updateFrequency == L"halfyear")
        {
            interval = 15552000;
        }

        if (interval > 0 && config.lastUpdateCheckUnix > 0 &&
            now - config.lastUpdateCheckUnix < interval)
        {
            return;   // 还没到下次检查时间
        }

        // 先记录检查时间，避免失败后每次启动都重试
        config.lastUpdateCheckUnix = now;
        ::TufScreenControl::Ui::Config().Save();

        if (auto element = Content().try_as<FrameworkElement>())
        {
            auto root = element.XamlRoot();
            if (root)
            {
                auto queue = DispatcherQueue();
                std::thread([root, queue]()
                {
                    auto release = ::TufScreenControl::Ui::Firmware::FetchLatest();
                    if (!release.found)
                    {
                        return;
                    }

                    int current = 0;
                    try
                    {
                        current = std::stoi(::TufScreenControl::Device::ScreenController::FirmwareRevision());
                    }
                    catch (...)
                    {
                        current = 0;
                    }

                    if (release.version <= current)
                    {
                        return;
                    }

                    queue.TryEnqueue([root, release]()
                    {
                        ::TufScreenControl::Ui::Firmware::ShowDialog(root, release);
                    });
                }).detach();
            }
        }
    }

    void MainWindow::ApplyPersistedDeviceState()
    {
        auto& config = ::TufScreenControl::Ui::Config().Data();

        auto params = Player().Params();
        if (config.fitMode == L"cover")
        {
            params.fitMode = ::TufScreenControl::Imaging::FitMode::Cover;
        }
        else if (config.fitMode == L"stretch")
        {
            params.fitMode = ::TufScreenControl::Imaging::FitMode::Stretch;
        }
        else
        {
            params.fitMode = ::TufScreenControl::Imaging::FitMode::Contain;
        }

        switch (config.rotation)
        {
        case 90:  params.rotation = ::TufScreenControl::Imaging::Rotation::Deg90; break;
        case 180: params.rotation = ::TufScreenControl::Imaging::Rotation::Deg180; break;
        case 270: params.rotation = ::TufScreenControl::Imaging::Rotation::Deg270; break;
        default:  params.rotation = ::TufScreenControl::Imaging::Rotation::Deg0; break;
        }

        Player().SetParams(params);
        Player().SetPhotoFps(config.photoFps);

        // 恢复屏幕状态：用户最后一次设定的是开就开、是关就关。
        // 这样即便上次退出时被「电源联动」关了屏，本次启动也会恢复成用户设定，
        // 不需要再手动点两次开关。
        Player().SetBrightness(config.brightness);
        Player().SetScreenPower(config.screenOn);

        // 睡眠模式：0x8000011F（0 = 开启，1 = 关闭）
        Player().SetSleepMode(config.sleepMode);
    }


    void MainWindow::SetupTrayAndCloseBehavior()
    {
        // 拦截窗口关闭：按配置「隐藏到托盘 / 退出 / 询问」处理。
        if (auto appWindow = AppWindow())
        {
            appWindow.Closing([this](Microsoft::UI::Windowing::AppWindow const&,
                                     Microsoft::UI::Windowing::AppWindowClosingEventArgs const& args)
            {
                if (m_exiting)
                {
                    return;   // 真退出：放行，交给 Closed 处理
                }

                auto& config = ::TufScreenControl::Ui::Config().Data();
                std::wstring action = config.closeAction;
                if (!config.closePromptShown)
                {
                    action = L"ask";   // 首次关闭一律询问
                }

                if (action == L"tray")
                {
                    if (!m_trayAvailable)
                    {
                        // 托盘图标创建失败（例如进程令牌与 Explorer 不一致被拒绝）时，
                        // 隐藏窗口会让用户无处找回，退化为直接退出。
                        m_exiting = true;
                        return;
                    }
                    args.Cancel(true);
                    HideToTray();
                }
                else if (action == L"exit")
                {
                    // 不放行系统关闭：直接关闭会走 DWM 的窗口关闭合成，闪白。
                    // 统一走 ExitApplication（先隐藏、等一帧、再退出）。
                    args.Cancel(true);
                    ExitApplication();
                }
                else
                {
                    args.Cancel(true);
                    ShowClosePrompt();
                }
            });
        }

        m_trayAvailable = m_trayIcon.Add(GetHwnd(), L"TUF Screen Control",
                                         [this]() { ShowFromTray(); },
                                         [this]() { ShowTrayMenu(); });
    }

    void MainWindow::PrepareWindowForHide()
    {
        // 隐藏前先把界面换成"实色底 + 无 Mica"：
        // Mica 背衬在窗口隐藏的瞬间不再被绘制，根容器若还是 Transparent，
        // DWM 就会露出默认白色底，表现为"关闭时白闪"。
        if (auto root = Content().try_as<Panel>())
        {
            const auto background = ::TufScreenControl::NativeTheme::ResolveIsDark(m_theme)
                ? Windows::UI::ColorHelper::FromArgb(0xFF, 0x20, 0x20, 0x20)
                : Windows::UI::ColorHelper::FromArgb(0xFF, 0xF3, 0xF3, 0xF3);
            root.Background(Media::SolidColorBrush{ background });
        }
        SystemBackdrop(nullptr);
    }

    void MainWindow::HideToTray()
    {
        PrepareWindowForHide();

        auto appWindow = AppWindow();
        if (!appWindow)
        {
            return;
        }

        // 用 DwmFlush 等待实色底真正合成上屏，再隐藏窗口。
        // DwmFlush 阻塞到 DWM 完成当前帧合成，比定时器可靠。
        auto dispatcher = DispatcherQueue();
        if (dispatcher)
        {
            dispatcher.TryEnqueue([appWindow]()
            {
                ::DwmFlush();
                appWindow.Hide();
                ::DwmFlush();
            });
        }
        else
        {
            appWindow.Hide();
        }
    }
    void MainWindow::ShowFromTray()
    {
        if (auto appWindow = AppWindow())
        {
            appWindow.Show();
        }

        // 恢复 Mica 背衬：去掉隐藏前刷的实色底
        if (auto root = Content().try_as<Panel>())
        {
            root.Background(nullptr);
        }
        SystemBackdrop(MicaBackdrop{});

        Activate();
        if (HWND hwnd = GetHwnd())
        {
            ::SetForegroundWindow(hwnd);
            // 注意：这里不要再调 ShowWindow(SW_RESTORE)。
            // SW_RESTORE 的语义是「恢复成原始大小和位置」，会把最大化/贴边分屏
            // 的窗口打回普通尺寸——表现为从托盘回来时窗口位置和分屏状态丢了。
            // AppWindow.Show() + Activate() 已经足够，位置与最大/吸附状态由系统保留。
        }
    }

    void MainWindow::ShowClosePrompt()
    {
        if (m_closePromptOpen)
        {
            return;
        }

        auto root = Content().try_as<FrameworkElement>();
        if (!root || !root.XamlRoot())
        {
            HideToTray();
            return;
        }

        m_closePromptOpen = true;

        ContentDialog dialog;
        dialog.XamlRoot(root.XamlRoot());
        dialog.Title(box_value(L"关闭窗口时"));
        dialog.Content(box_value(L"关闭主窗口后，程序可以继续在系统托盘运行，水冷屏显示不会中断。"));
        dialog.PrimaryButtonText(L"隐藏到系统托盘");
        dialog.SecondaryButtonText(L"退出程序");
        dialog.CloseButtonText(L"取消");
        dialog.DefaultButton(ContentDialogButton::Primary);

        auto operation = dialog.ShowAsync();
        operation.Completed([this, dialog](auto const& op, Windows::Foundation::AsyncStatus status)
        {
            m_closePromptOpen = false;

            ContentDialogResult result = ContentDialogResult::None;
            if (status == Windows::Foundation::AsyncStatus::Completed)
            {
                result = op.GetResults();
            }

            auto& config = ::TufScreenControl::Ui::Config().Data();
            config.closePromptShown = true;


            if (result == ContentDialogResult::Primary)
            {
                config.closeAction = L"tray";
                ::TufScreenControl::Ui::Config().Save();
                if (m_trayAvailable)
                {
                    HideToTray();
                }
                else
                {
                    ExitApplication();   // 托盘不可用：直接退出，避免窗口丢失
                }
            }
            else if (result == ContentDialogResult::Secondary)
            {
                config.closeAction = L"exit";
                ::TufScreenControl::Ui::Config().Save();
                ExitApplication();
            }
            else
            {
                config.closeAction = L"ask";
                ::TufScreenControl::Ui::Config().Save();
            }
        });
    }

    void MainWindow::ExitApplication()
    {
        m_exiting = true;
        m_trayIcon.Remove();

        // 规范退出：关闭窗口（Microsoft.UI.Xaml.Window.Close），交给 Closed
        // 里的优雅收尾走完 WinUI 的正常生命周期，最后一个窗口关闭后
        // 框架结束消息循环，进程自然退出。
        Close();
    }

    void MainWindow::ShowTrayMenu()
    {
        // 托盘菜单用 Win32 HMENU（TrackPopupMenu）：
        // 不依赖任何 WinUI 宿主窗口，行为与系统托盘菜单一致，也不会被遮挡。
        enum Command : UINT
        {
            CommandOpen = 1,
            CommandTogglePlay,
            CommandStop,
            CommandExit,
        };

        HMENU menu = ::CreatePopupMenu();
        if (menu == nullptr)
        {
            return;
        }

        const bool paused = m_player && m_player->IsPaused();

        ::AppendMenuW(menu, MF_STRING, CommandOpen, L"打开");
        ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        ::AppendMenuW(menu, MF_STRING, CommandTogglePlay, paused ? L"播放" : L"暂停");
        ::AppendMenuW(menu, MF_STRING, CommandStop, L"停止显示");
        ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        ::AppendMenuW(menu, MF_STRING, CommandExit, L"退出");

        POINT cursor = ::TufScreenControl::TrayIcon::CursorPosition();
        HWND hwnd = GetHwnd();

        // 托盘菜单必须先把窗口前置，否则点击菜单外部时菜单不会消失
        ::SetForegroundWindow(hwnd);

        const UINT command = ::TrackPopupMenu(
            menu,
            TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN,
            cursor.x, cursor.y, 0, hwnd, nullptr);

        ::PostMessageW(hwnd, WM_NULL, 0, 0);
        ::DestroyMenu(menu);

        switch (command)
        {
        case CommandOpen:
            ShowFromTray();
            break;
        case CommandTogglePlay:
            if (m_player)
            {
                m_player->SetPaused(!m_player->IsPaused());
            }
            break;
        case CommandStop:
            if (m_player)
            {
                m_player->Stop();
            }
            break;
        case CommandExit:
            ExitApplication();
            break;
        default:
            break;
        }
    }
    void MainWindow::SetupSystemEvents()
    {
        HWND hwnd = GetHwnd();
        if (hwnd == nullptr)
        {
            return;
        }

        ::TufScreenControl::PowerMonitor::Handlers handlers;

        // 设备插拔：重新枚举设备（无需重启软件），并刷新界面上的连接状态
        handlers.deviceChanged = [this]()
        {
            if (m_player)
            {
                m_player->RefreshDevice();   // 立即重新枚举，状态才准确
            }
            RefreshDeviceBadge();
        };

        m_powerMonitor.Start(hwnd, handlers);

        // 单实例：第二实例的激活被系统重定向到本进程时，把窗口叫回来。
        // （Activated 在后台线程触发，必须封送到 UI 线程）
        try
        {
            auto const keyInstance = winrt::Microsoft::Windows::AppLifecycle::AppInstance::
                FindOrRegisterForKey(::TufScreenControl::kInstanceKey);
            if (keyInstance)
            {
                keyInstance.Activated([this](auto&&, auto&&)
                {
                    if (auto dispatcher = DispatcherQueue())
                    {
                        dispatcher.TryEnqueue([this]() { ShowFromTray(); });
                    }
                });
            }
        }
        catch (...)
        {
        }

        Closed([this](auto&&, auto&&)
        {
            // WinUI 默认生命周期：先摘掉电源/会话事件监听，再优雅停播放器
            // （请求停止 → join 工作线程 → 关闭 HID 句柄），
            // 最后一个窗口关闭后框架结束消息循环，进程正常退出。
            m_powerMonitor.Stop();
            ShutdownPlayer();
        });
    }
    HWND MainWindow::GetHwnd() const noexcept
    {
        if (auto appWindow = AppWindow())
        {
            return reinterpret_cast<HWND>(appWindow.Id().Value);
        }
        return nullptr;
    }

    void MainWindow::ApplyTheme(::TufScreenControl::AppTheme theme, bool persist)
    {
        m_theme = theme;

        // 1) XAML 层：设根元素的 RequestedTheme，WinUI 控件与 Mica 背衬一起切换。
        //    Default = 跟随系统；Light / Dark 为显式覆盖。
        if (auto root = Content().try_as<FrameworkElement>())
        {
            switch (theme)
            {
            case ::TufScreenControl::AppTheme::Light:
                root.RequestedTheme(ElementTheme::Light);
                break;
            case ::TufScreenControl::AppTheme::Dark:
                root.RequestedTheme(ElementTheme::Dark);
                break;
            default:
                root.RequestedTheme(ElementTheme::Default);
                break;
            }
        }

        // 2) 原生层：标题栏深色 + Win32 系统菜单（标题栏右键菜单）深色。
        //    这两处不受 XAML ElementTheme 影响，必须单独设置，否则会出现
        //    「控件已变深色、标题栏右键菜单仍是浅色」的割裂。
        ::TufScreenControl::NativeTheme::ApplyAll(theme, GetHwnd());

        // 3) 标题栏系统按钮的前景色必须自己跟随主题。
        //    ExtendsContentIntoTitleBar 之后三个按钮（最小化/最大化/关闭）由我们自己着色，
        //    在浅色主题下如果仍是白色前景，就会出现「按钮看不见、只有悬停才显形」的问题。
        if (auto appWindow = AppWindow())
        {
            if (auto titleBar = appWindow.TitleBar())
            {
                const bool dark = ::TufScreenControl::NativeTheme::ResolveIsDark(theme);
                auto const foreground = dark ? Windows::UI::Colors::White() : Windows::UI::Colors::Black();

                titleBar.ButtonForegroundColor(foreground);
                titleBar.ButtonInactiveForegroundColor(foreground);
                titleBar.ButtonHoverForegroundColor(foreground);
                titleBar.ButtonPressedForegroundColor(foreground);

                // 背景色也要跟随主题：否则浅色主题下悬停/按下会露出深色底块。
                auto const transparent = Windows::UI::Colors::Transparent();
                auto const hoverBackground = dark
                    ? Windows::UI::ColorHelper::FromArgb(0x1A, 0xFF, 0xFF, 0xFF)
                    : Windows::UI::ColorHelper::FromArgb(0x1A, 0x00, 0x00, 0x00);
                auto const pressedBackground = dark
                    ? Windows::UI::ColorHelper::FromArgb(0x33, 0xFF, 0xFF, 0xFF)
                    : Windows::UI::ColorHelper::FromArgb(0x33, 0x00, 0x00, 0x00);

                titleBar.ButtonBackgroundColor(transparent);
                titleBar.ButtonInactiveBackgroundColor(transparent);
                titleBar.ButtonHoverBackgroundColor(hoverBackground);
                titleBar.ButtonPressedBackgroundColor(pressedBackground);
            }
        }

        // 4) 持久化
        if (persist)
        {
            ::TufScreenControl::Ui::Config().Data().appTheme = ::TufScreenControl::NativeTheme::ToString(theme);
            ::TufScreenControl::Ui::Config().Save();
        }
    }

    void MainWindow::SetupBackdropAndTitleBar()
    {
        // 内容扩展进标题栏：标题栏与窗口内容融为一体。
        ExtendsContentIntoTitleBar(true);
        SetTitleBar(AppTitleBar());

        // 系统按钮透明，让 Mica 从按钮下方透出来。
        if (auto appWindow = AppWindow())
        {
            if (auto titleBar = appWindow.TitleBar())
            {
                titleBar.ButtonBackgroundColor(Windows::UI::Colors::Transparent());
                titleBar.ButtonInactiveBackgroundColor(Windows::UI::Colors::Transparent());
            }
        }

        // Mica 系统背衬。深浅色由根元素 RequestedTheme 驱动，Mica 自动跟随；
        // 系统不支持 Mica 时由 WinUI 自行降级，无需写分支。
        SystemBackdrop(MicaBackdrop{});
    }


    void MainWindow::PopulateMediaList(ListView const& list, std::wstring const& directory,
                                       std::wstring const& extensionsCsv)
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        if (!fs::exists(directory, ec))
        {
            return;
        }

        auto matches = [&extensionsCsv](std::filesystem::path const& file)
        {
            const std::wstring ext = file.extension().wstring();
            size_t start = 0;
            while (start <= extensionsCsv.size())
            {
                const size_t comma = extensionsCsv.find(L',', start);
                const std::wstring token = extensionsCsv.substr(
                    start, comma == std::wstring::npos ? std::wstring::npos : comma - start);
                if (!token.empty() && _wcsicmp(ext.c_str(), token.c_str()) == 0)
                {
                    return true;
                }
                if (comma == std::wstring::npos)
                {
                    break;
                }
                start = comma + 1;
            }
            return false;
        };

        for (auto const& entry : fs::directory_iterator(directory, ec))
        {
            if (!entry.is_regular_file() || !matches(entry.path()))
            {
                continue;
            }
            ListViewItem item;
            item.Content(box_value(entry.path().filename().wstring()));
            item.Tag(box_value(entry.path().wstring()));
            list.Items().Append(item);
        }
    }

    ::TufScreenControl::Core::PlaybackController& MainWindow::Player()
    {
        if (!m_player)
        {
            m_player = std::make_unique<::TufScreenControl::Core::PlaybackController>();
            m_player->OpenDevice();

            // 状态回调在工作线程触发，需封送到 UI 线程。
            m_player->SetStatusCallback([this](std::wstring const& message)
            {
                auto dispatcher = DispatcherQueue();
                if (!dispatcher)
                {
                    return;
                }
                const auto text = winrt::hstring(message);
                dispatcher.TryEnqueue([this, text]()
                {
                    if (m_statusText)
                    {
                        m_statusText.Text(text);
                    }
                });
            });

            // 预览回调同样在工作线程触发：复制像素后封送到 UI 线程渲染。
            m_player->SetPreviewCallback(
                [this](std::uint8_t const* pixels, int width, int height, int stride)
                {
                    if (pixels == nullptr || width <= 0 || height <= 0 || stride <= 0)
                    {
                        return;
                    }
                    auto frame = std::make_shared<std::vector<std::uint8_t>>(
                        pixels, pixels + static_cast<size_t>(stride) * static_cast<size_t>(height));

                    auto dispatcher = DispatcherQueue();
                    if (!dispatcher)
                    {
                        return;
                    }
                    dispatcher.TryEnqueue([this, frame, width, height]()
                    {
                        UpdatePreview(*frame, width, height);
                    });
                });
        }
        return *m_player;
    }

    void MainWindow::ShutdownPlayer() noexcept
    {
        if (m_player)
        {
            m_player->Stop();
            m_player->CloseDevice();
            m_player.reset();
        }
    }

    MainWindow::~MainWindow()
    {
        ShutdownPlayer();
    }

    void MainWindow::UpdatePreview(std::vector<std::uint8_t> const& pixels, int width, int height)
    {
        if (!m_previewSource || pixels.empty() || width <= 0 || height <= 0)
        {
            return;
        }
        if (pixels.size() < static_cast<size_t>(width) * 4 * static_cast<size_t>(height))
        {
            return;
        }

        // 上一帧还没渲染完就丢弃本帧：SetBitmapAsync 是异步的，帧率高于渲染时会积压，
        // 造成内存持续上涨（播放视频时尤其明显）。
        if (m_previewPending)
        {
            return;
        }

        try
        {
            Windows::Storage::Streams::InMemoryRandomAccessStream stream;
            Windows::Storage::Streams::DataWriter writer(stream);
            writer.WriteBytes(pixels);
            const auto buffer = writer.DetachBuffer();

            // 复用同一张 SoftwareBitmap：只拷贝像素，不再每帧分配新对象
            if (!m_previewBitmap ||
                m_previewBitmap.PixelWidth() != width ||
                m_previewBitmap.PixelHeight() != height)
            {
                m_previewBitmap = Windows::Graphics::Imaging::SoftwareBitmap::CreateCopyFromBuffer(
                    buffer,
                    Windows::Graphics::Imaging::BitmapPixelFormat::Bgra8,
                    width,
                    height,
                    Windows::Graphics::Imaging::BitmapAlphaMode::Premultiplied);
            }
            else
            {
                m_previewBitmap.CopyFromBuffer(buffer);
            }

            m_previewPending = true;
            auto operation = m_previewSource.SetBitmapAsync(m_previewBitmap);
            operation.Completed([this](auto const&, auto const&) { m_previewPending = false; });

            if (m_previewPlaceholder)
            {
                m_previewPlaceholder.Visibility(Visibility::Collapsed);
            }
        }
        catch (...)
        {
            // 预览只是辅助功能，失败不应影响播放主流程
        }
    }

    MainWindow* MainWindow::Current() noexcept
    {
        return s_current;
    }

    ::TufScreenControl::AppTheme MainWindow::CurrentTheme() const noexcept
    {
        return m_theme;
    }

    void MainWindow::AttachStatusText(TextBlock const& text)
    {
        m_statusText = text;
    }

    void MainWindow::AttachDeviceText(TextBlock const& text)
    {
        m_deviceText = text;
    }

    void MainWindow::AttachPreview(Media::Imaging::SoftwareBitmapSource const& source,
                                   TextBlock const& placeholder)
    {
        m_previewSource = source;
        m_previewPlaceholder = placeholder;
    }

    void MainWindow::DetachPageUi()
    {
        m_statusText = nullptr;
        m_deviceText = nullptr;
        m_previewSource = nullptr;
        m_previewPlaceholder = nullptr;
        m_previewBitmap = nullptr;
        m_previewPending = false;
    }
}
