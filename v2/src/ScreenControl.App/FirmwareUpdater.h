#pragma once

// 固件更新检查（header-only）
// ---------------------------------------------------------------------------
// 1) WinHTTP 同步 GET ASUS 支持页接口（返回 JSON）
// 2) 用项目自带的 MinimalJson 解析（不引入第三方依赖）
// 3) 与设备当前固件版本（USB 实例 ID 里的 REV_xxxx）比较
// 4) 有新版时弹 ContentDialog，点「下载」用系统浏览器打开官方链接
//
// 接口返回结构（已实测）：
//   Result.Obj[0].Files[0] = { Version:"51", FileSize:"1.27 MB",
//                              DownloadUrl:{ China:..., Global:... } }
// ---------------------------------------------------------------------------

#include <string>
#include <functional>
#include <thread>
#include <cctype>

#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.System.h>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>

#include "MinimalJson.h"

namespace ScreenControl::Ui
{
    /// 一次更新检查的结果。
    struct FirmwareRelease
    {
        bool found = false;
        int version = 0;
        std::wstring fileSize;
        std::wstring title;
        std::wstring downloadUrl;
    };

    namespace Firmware
    {
        /// 官方固件查询接口（TUF Gaming LC III 360 ARGB LCD）。
        inline constexpr wchar_t kApiHost[] = L"www.asus.com";
        inline constexpr wchar_t kApiPath[] =
            L"/support/api/product.asmx/GetPDBIOS"
            L"?website=cn&model=tuf-gaming-lc-iii-360-argb-lcd"
            L"&pdhashedid=vuxiprtwlnip2fc9&pdid=34029&osid=52&siteID=www";

        /// 把 URL 里的空格编码为 %20（接口返回的 ?model=... 带空格，
        /// 直接交给 Windows::Foundation::Uri 会解析失败）。
        inline std::wstring EncodeUrl(std::wstring const& url)
        {
            std::wstring out;
            out.reserve(url.size() + 8);
            for (wchar_t ch : url)
            {
                if (ch == L' ')
                {
                    out += L"%20";
                }
                else
                {
                    out += ch;
                }
            }
            return out;
        }

        /// 同步 HTTP GET，返回原始响应体（UTF-8）。失败返回空串。
        inline std::string HttpGet(std::wstring const& host, std::wstring const& path)
        {
            std::string body;

            HINTERNET session = ::WinHttpOpen(L"TUF Screen Control/1.0",
                                              WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                              WINHTTP_NO_PROXY_NAME,
                                              WINHTTP_NO_PROXY_BYPASS, 0);
            if (session == nullptr)
            {
                return body;
            }

            // WinHTTP 默认不启用 TLS 1.2，访问 ASUS（仅 HTTPS）会直接失败。
            DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
            ::WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS,
                               &protocols, sizeof(protocols));

            // 部分 CDN 对空 User-Agent 返回 403，这里显式补上。
            ::WinHttpSetOption(session, WINHTTP_OPTION_USER_AGENT,
                               const_cast<wchar_t*>(L"TUF Screen Control/1.0"), 0);

            if (HINTERNET connect = ::WinHttpConnect(session, host.c_str(),
                                                     INTERNET_DEFAULT_HTTPS_PORT, 0))
            {
                if (HINTERNET request = ::WinHttpOpenRequest(
                        connect, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                        WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE))
                {
                    const BOOL sent = ::WinHttpSendRequest(request,
                                                           WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                                           WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
                    if (sent && ::WinHttpReceiveResponse(request, nullptr))
                    {
                        for (;;)
                        {
                            DWORD available = 0;
                            if (!::WinHttpQueryDataAvailable(request, &available) || available == 0)
                            {
                                break;
                            }
                            std::string chunk(available, '\0');
                            DWORD read = 0;
                            if (!::WinHttpReadData(request, chunk.data(), available, &read))
                            {
                                break;
                            }
                            body.append(chunk.data(), read);
                        }
                    }
                    ::WinHttpCloseHandle(request);
                }
                ::WinHttpCloseHandle(connect);
            }

            ::WinHttpCloseHandle(session);
            return body;
        }

        /// 拉取官方最新固件信息。网络失败或解析失败时返回 found == false。
        inline FirmwareRelease FetchLatest()
        {
            FirmwareRelease release;

            const std::string raw = HttpGet(kApiHost, kApiPath);
            if (raw.empty())
            {
                return release;
            }

            const auto root = ::ScreenControl::Core::Json::Parse(raw);
            const auto& objects = root[L"Result"][L"Obj"];
            if (!objects.IsArray() || objects.items.empty())
            {
                return release;
            }

            const auto& files = objects.items.front()[L"Files"];
            if (!files.IsArray() || files.items.empty())
            {
                return release;
            }

            const auto& file = files.items.front();
            release.version = file[L"Version"].AsInt(0);
            release.fileSize = file[L"FileSize"].AsString(L"未知");
            release.title = file[L"Title"].AsString(L"");
            release.downloadUrl = file[L"DownloadUrl"][L"China"].AsString(L"");
            if (release.downloadUrl.empty())
            {
                // 国内渠道拿不到时退回全球渠道
                release.downloadUrl = file[L"DownloadUrl"][L"Global"].AsString(L"");
            }
            release.found = release.version > 0;
            return release;
        }

        /// 弹「固件更新」对话框。点「下载」打开浏览器。
        inline void ShowDialog(winrt::Microsoft::UI::Xaml::XamlRoot const& root,
                               FirmwareRelease const& release)
        {
            using namespace winrt::Microsoft::UI::Xaml::Controls;

            std::wstring message = L"新固件已发布，是否下载。\n版本 ";
            message += std::to_wstring(release.version);
            message += L"，大小 ";
            message += release.fileSize;
            message += L"，";

            ContentDialog dialog;
            dialog.XamlRoot(root);
            dialog.Title(winrt::box_value(L"固件更新"));
            dialog.Content(winrt::box_value(winrt::hstring(message)));
            dialog.PrimaryButtonText(L"下载");
            dialog.CloseButtonText(L"否");
            dialog.DefaultButton(ContentDialogButton::Primary);

            auto const url = EncodeUrl(release.downloadUrl);
            dialog.ShowAsync().Completed(
                [url](auto const& operation, auto const&)
                {
                    if (operation.GetResults() != ContentDialogResult::Primary || url.empty())
                    {
                        return;
                    }
                    try
                    {
                        // 用系统默认浏览器打开官方下载页
                        winrt::Windows::System::Launcher::LaunchUriAsync(
                            winrt::Windows::Foundation::Uri{ url });
                    }
                    catch (...)
                    {
                        // 打开失败不影响主流程
                    }
                });
        }
    }
}
