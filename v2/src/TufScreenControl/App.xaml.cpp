#include "pch.h"
#include "App.xaml.h"
#include "MainWindow.xaml.h"

#include "AppConfig.h"
#include "FirmwareUpdater.h"
#include "NativeTheme.h"
#include "StartupTrace.h"

#include <filesystem>
#include <fstream>

#include <winrt/Microsoft.Windows.AppLifecycle.h>

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::Windows::AppLifecycle;

namespace winrt::TufScreenControl::implementation
{
    App::App()
    {
        ::TufScreenControl::TraceStartup("App ctor enter");

        // 与官方 WinUI 3 C++ 模板一致：在构造函数里加载 App.xaml
        // （XamlControlsResources + 应用级样式表）。生成的 InitializeComponent
        // 自带 _contentLoaded 幂等保护，重复调用无副作用。
        InitializeComponent();
        ::TufScreenControl::TraceStartup("App ctor: InitializeComponent done");

        // 全局兜底：把未处理异常写进 exe 同目录的 crash.log，
        // 否则 0xC000027B（stowed exception）在事件日志里只有模块偏移，无法定位。
        UnhandledException([](IInspectable const&, UnhandledExceptionEventArgs const& e)
        {
            try
            {
                wchar_t modulePath[MAX_PATH]{};
                GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
                const std::filesystem::path logPath =
                    std::filesystem::path(modulePath).parent_path() / L"crash.log";

                std::wstring text = L"=== 未处理异常 ===\r\n消息: ";
                text += std::wstring(e.Message());
                text += L"\r\n\r\n";

                // 用 UTF-8 写文件：直接 wofstream 会受 locale 影响，中文会写不出来。
                const int bytes = ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                                                        static_cast<int>(text.size()),
                                                        nullptr, 0, nullptr, nullptr);
                std::string utf8(static_cast<size_t>(bytes), '\0');
                ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                      utf8.data(), bytes, nullptr, nullptr);

                std::ofstream log(logPath, std::ios::binary | std::ios::app);
                log.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
                log.flush();
            }
            catch (...)
            {
            }

            if (IsDebuggerPresent())
            {
                __debugbreak();
            }
        });
    }

    namespace
    {
        /// 单实例守卫（命名互斥量）。
        ///
        /// 为什么用互斥量而不是 AppLifecycle 的 key 判单实例：
        /// AppLifecycle 的 key 在进程**崩溃**后仍会挂在这个已死进程上，
        /// 后续启动的进程都判定「已有实例」，把激活重定向给死人后挂住、
        /// 什么都不显示 —— 表现为「双击两三次才打得开」。
        /// 命名互斥量由内核持有，进程无论正常退出还是崩溃，句柄一关就释放，
        /// 不会留下这种残留状态。
        HANDLE AcquireInstanceMutex()
        {
            HANDLE mutex = ::CreateMutexW(nullptr, FALSE, L"Local\\TufScreenControl.SingleInstance");
            if (mutex == nullptr)
            {
                return nullptr;
            }
            if (::GetLastError() == ERROR_ALREADY_EXISTS)
            {
                ::CloseHandle(mutex);
                return nullptr;   // 已有存活实例
            }
            return mutex;         // 本进程是唯一实例
        }

        /// 把已有实例的窗口叫到前面，交接完成后退出本进程。
        ///
        /// 只在互斥量确认「已有存活实例」时才调用，所以对端一定在跑，
        /// co_await 正常几十毫秒就完成——不需要任何超时兜底：
        ///  - 同步等（WaitForSingleObject）会卡死：本线程是 STA，完成回调要靠 COM
        ///    封送回该套间，而同步等待不派发 COM 调用，回调永远排不上队。
        ///  - co_await 把续体挂到当前套间的 DispatcherQueue 上，OnLaunched 返回后
        ///    消息循环已经在跑，完成即退出。
        /// 注意这里**不是**单实例判定的依据（互斥量才是），只是「顺便把窗口置前」，
        /// 所以即便这次交接没送达，也不会多开、也不会残留状态。
        winrt::fire_and_forget RaiseExistingInstanceAndExit(AppActivationArguments args,
                                                            AppInstance target)
        {
            try
            {
                co_await target.RedirectActivationToAsync(args);
                ::TufScreenControl::TraceStartup("raise: co_await completed");
            }
            catch (...)
            {
                ::TufScreenControl::TraceStartup("raise: co_await threw");
            }
            ::TufScreenControl::TraceStartup("raise: exiting process");
            ::ExitProcess(0);
        }

        /// 把强调色注入到应用级资源。
        ///
        /// 这是 WinUI 的「系统强调色」机制：控件（开关、复选框、选中态、
        /// 按钮等）的强调色画刷最终都引用 SystemAccentColor 系列资源，
        /// 因此只需要在创建任何界面元素之前覆盖这一组资源，
        /// 所有原生控件就会自动跟随，不需要逐个控件刷色。
        ///
        /// 注意：资源需要在界面创建前注入，所以修改主题色后需重启生效。
        void ApplyAccentResources(std::wstring const& themeColor)
        {
            auto resources = Application::Current().Resources();

            const bool tufGaming = themeColor == L"tuf";

            if (tufGaming)
            {
                // TUF GAMING 品牌橙。这里只定义一次，其余明暗变化由下面派生，
                // 避免到处散落硬编码颜色值。
                const auto accent = Windows::UI::ColorHelper::FromArgb(0xFF, 0xFF, 0x9E, 0x1B);

                auto lighten = [](Windows::UI::Color const& color, double factor)
                {
                    return Windows::UI::ColorHelper::FromArgb(
                        color.A,
                        static_cast<std::uint8_t>(color.R + (255 - color.R) * factor),
                        static_cast<std::uint8_t>(color.G + (255 - color.G) * factor),
                        static_cast<std::uint8_t>(color.B + (255 - color.B) * factor));
                };
                auto darken = [](Windows::UI::Color const& color, double factor)
                {
                    return Windows::UI::ColorHelper::FromArgb(
                        color.A,
                        static_cast<std::uint8_t>(color.R * (1.0 - factor)),
                        static_cast<std::uint8_t>(color.G * (1.0 - factor)),
                        static_cast<std::uint8_t>(color.B * (1.0 - factor)));
                };

                resources.Insert(box_value(L"SystemAccentColor"), box_value(accent));
                resources.Insert(box_value(L"SystemAccentColorLight1"), box_value(lighten(accent, 0.15)));
                resources.Insert(box_value(L"SystemAccentColorLight2"), box_value(lighten(accent, 0.30)));
                resources.Insert(box_value(L"SystemAccentColorLight3"), box_value(lighten(accent, 0.45)));
                resources.Insert(box_value(L"SystemAccentColorDark1"), box_value(darken(accent, 0.10)));
                resources.Insert(box_value(L"SystemAccentColorDark2"), box_value(darken(accent, 0.20)));
                resources.Insert(box_value(L"SystemAccentColorDark3"), box_value(darken(accent, 0.30)));
            }
            // 跟随系统：不覆盖任何资源，WinUI 默认就使用系统强调色。
        }
    }

    void App::OnLaunched([[maybe_unused]] LaunchActivatedEventArgs const& e)
    {
        // 诊断模式：TufScreenControl.exe --check-update
        // 不创建界面，直接把固件接口的结果写到 update.log，便于排查。
        if (std::wstring(::GetCommandLineW()).find(L"--check-update") != std::wstring::npos)
        {
            ::TufScreenControl::Ui::Firmware::RunSelfTest();
            ::ExitProcess(0);
        }

        // 单实例判定：命名互斥量（崩溃安全，见 AcquireInstanceMutex 的注释）。
        // 已有存活实例时，只做「把它的窗口叫到前面」这个附加动作，然后退出。
        ::TufScreenControl::TraceStartup("OnLaunched: instance mutex check");
        static HANDLE const instanceMutex = AcquireInstanceMutex();
        if (instanceMutex == nullptr)
        {
            ::TufScreenControl::TraceStartup("OnLaunched: another instance alive, raising it");
            try
            {
                RaiseExistingInstanceAndExit(AppInstance::GetCurrent().GetActivatedEventArgs(),
                                             AppInstance::FindOrRegisterForKey(
                                                 ::TufScreenControl::kInstanceKey));
            }
            catch (...)
            {
            }
            ::TufScreenControl::TraceStartup("OnLaunched: exiting (second instance)");
            ::ExitProcess(0);
        }
        ::TufScreenControl::TraceStartup("OnLaunched: primary instance (continue)");

        // 强调色必须在创建任何控件之前注入，否则已经实例化的控件不会跟随。
        // 因此主题色的改动是「重启后生效」。
        try
        {
            ::TufScreenControl::Core::ConfigStore config;
            config.Load();
            ApplyAccentResources(config.Data().themeColor);

            // 深色系统菜单（标题栏右键原生菜单）：必须在创建窗口之前设置，
            // 否则窗口创建时的菜单主题不会跟随（参考实现也放在 App 启动早期）。
            ::TufScreenControl::NativeTheme::ApplyPreferredAppMode(
                ::TufScreenControl::NativeTheme::Parse(config.Data().appTheme));
        }
        catch (...)
        {
        }

        ::TufScreenControl::TraceStartup("OnLaunched: creating MainWindow");
        window = make<MainWindow>();
        ::TufScreenControl::TraceStartup("OnLaunched: MainWindow created");
        window.Activate();
        ::TufScreenControl::TraceStartup("OnLaunched: Activate returned");
    }
}
