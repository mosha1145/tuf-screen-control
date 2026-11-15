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
        /// 把激活交给已运行的实例。
        ///
        /// 返回值 = 交接是否真的完成：
        ///   true  → 对端收下了，本进程应当退出；
        ///   false → 对方没响应（多半是它已经崩了，单实例 key 还挂在死进程上），
        ///           此时**不能**傻等，也不能直接退出——否则用户双击后什么都不会出现，
        ///           表现为「要点两三次才打得开」。由调用方落下去接管成为主实例。
        ///
        /// 等待用 CoWaitForMultipleHandles 而不是 WaitForSingleObject：
        /// 本线程是 STA，C++/WinRT 的完成回调要靠 COM 封送回该套间，
        /// 普通等待不派发 COM 调用，回调永远排不上队（之前就栽在这里）。
        bool TryRedirectActivation(AppActivationArguments const& args, AppInstance const& target)
        {
            constexpr DWORD kOwnerRespondTimeoutMs = 5000;

            winrt::handle ready{ ::CreateEventW(nullptr, FALSE, FALSE, nullptr) };
            target.RedirectActivationToAsync(args).Completed(
                [handle = ready.get()](auto&&, auto&&)
                {
                    ::SetEvent(handle);
                });

            HANDLE handles[1]{ ready.get() };
            DWORD index = 0;
            const DWORD wait = ::CoWaitForMultipleHandles(
                COWAIT_DISPATCH_CALLS | COWAIT_DISPATCH_WINDOW_MESSAGES,
                kOwnerRespondTimeoutMs, 1, handles, &index);

            if (wait == WAIT_OBJECT_0)
            {
                ::TufScreenControl::TraceStartup("redirect: owner accepted activation");
                return true;
            }

            ::TufScreenControl::TraceStartup("redirect: owner did not respond (taking over)");
            return false;
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

        // 单实例（Windows App SDK 官方 AppLifecycle API）：
        //   FindOrRegisterForKey 注册 key；若当前进程不是 key 的拥有者，
        //   说明已有实例在跑，把本次激活重定向过去后退出。
        try
        {
            ::TufScreenControl::TraceStartup("OnLaunched: single-instance check begin");
            auto const activationArgs = AppInstance::GetCurrent().GetActivatedEventArgs();
            auto const keyInstance = AppInstance::FindOrRegisterForKey(
                ::TufScreenControl::kInstanceKey);
            ::TufScreenControl::TraceStartup(keyInstance.IsCurrent()
                                                 ? "OnLaunched: is key owner (continue)"
                                                 : "OnLaunched: not key owner (redirect)");

            if (!keyInstance.IsCurrent())
            {
                // 第二实例：把激活交给已运行的实例。
                if (TryRedirectActivation(activationArgs, keyInstance))
                {
                    ::TufScreenControl::TraceStartup("OnLaunched: handoff done, exiting");
                    ::ExitProcess(0);
                }

                // 对端没响应（几乎可以肯定它已经崩溃，key 还挂在死进程上）。
                // 这里不能退出，否则用户双击后屏幕上什么都不会出现 —— 正是
                // 「要点两三次才打开」的成因。落下去自己开窗口，接管主实例。
                ::TufScreenControl::TraceStartup("OnLaunched: taking over as primary");
            }
        }
        catch (...)
        {
            // AppLifecycle 不可用时退化为允许多开，不影响主功能。
            ::TufScreenControl::TraceStartup("OnLaunched: applifecycle threw, continue");
        }

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
