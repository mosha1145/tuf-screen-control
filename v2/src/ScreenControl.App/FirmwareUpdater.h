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
#include <atomic>
#include <fstream>
#include <filesystem>
#include <memory>
#include <sstream>
#include <algorithm>

#include <shlobj.h>
#include <shellapi.h>

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

        /// 同步 HTTP GET 的详细结果，便于定位失败原因。
        struct HttpResult
        {
            std::string body;
            DWORD status = 0;   ///< HTTP 状态码
            DWORD error = 0;    ///< 首个失败的 Win32 错误码
        };

        /// 同步 HTTP GET。失败时 body 为空，error/status 说明原因。
        inline HttpResult HttpGetEx(std::wstring const& host, std::wstring const& path)
        {
            HttpResult result;

            HINTERNET session = ::WinHttpOpen(L"TUF Screen Control/1.0",
                                              WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                              WINHTTP_NO_PROXY_NAME,
                                              WINHTTP_NO_PROXY_BYPASS, 0);
            if (session == nullptr)
            {
                result.error = ::GetLastError();
                return result;
            }

            // WinHTTP 默认不启用 TLS 1.2，访问 ASUS（仅 HTTPS）会直接失败。
            DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
            ::WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS,
                               &protocols, sizeof(protocols));

            // 关键：服务器可能 302 到区域站点，WinHTTP 默认不跟随重定向。
            DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
            ::WinHttpSetOption(session, WINHTTP_OPTION_REDIRECT_POLICY,
                               &redirect, sizeof(redirect));

            // 关键：服务器可能返回 gzip，默认不自动解压 -> JSON 解析失败。
            DWORD decompress = WINHTTP_DECOMPRESSION_FLAG_ALL;
            ::WinHttpSetOption(session, WINHTTP_OPTION_DECOMPRESSION,
                               &decompress, sizeof(decompress));

            int timeoutMs = 15000;
            ::WinHttpSetTimeouts(session, timeoutMs, timeoutMs, timeoutMs, timeoutMs);

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
                    if (!sent)
                    {
                        result.error = ::GetLastError();
                    }
                    else if (!::WinHttpReceiveResponse(request, nullptr))
                    {
                        result.error = ::GetLastError();
                    }
                    else
                    {
                        DWORD statusCode = 0;
                        DWORD statusSize = sizeof(statusCode);
                        ::WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                              WINHTTP_HEADER_NAME_BY_INDEX, &statusCode,
                                              &statusSize, WINHTTP_NO_HEADER_INDEX);
                        result.status = statusCode;

                        for (;;)
                        {
                            DWORD available = 0;
                            if (!::WinHttpQueryDataAvailable(request, &available))
                            {
                                result.error = ::GetLastError();
                                break;
                            }
                            if (available == 0)
                            {
                                break;
                            }
                            std::string chunk(available, '\0');
                            DWORD read = 0;
                            if (!::WinHttpReadData(request, chunk.data(), available, &read))
                            {
                                result.error = ::GetLastError();
                                break;
                            }
                            result.body.append(chunk.data(), read);
                        }
                    }
                    ::WinHttpCloseHandle(request);
                }
                else
                {
                    result.error = ::GetLastError();
                }
                ::WinHttpCloseHandle(connect);
            }
            else
            {
                result.error = ::GetLastError();
            }

            ::WinHttpCloseHandle(session);
            return result;
        }

        /// 同步 HTTP GET，返回原始响应体（UTF-8）。失败返回空串。
        inline std::string HttpGet(std::wstring const& host, std::wstring const& path)
        {
            return HttpGetEx(host, path).body;
        }

        /// 拉取官方最新固件信息（定义见下方）。
        inline FirmwareRelease FetchLatest();

        /// 诊断模式：把接口结果写到 %LOCALAPPDATA%\TufScreenControl\update.log。
        /// 用 `TufScreenControl.exe --check-update` 触发，排查网络/解析问题。
        inline void RunSelfTest()
        {
            std::wstring folder;
            if (wchar_t* localAppData = nullptr;
                SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &localAppData)) &&
                localAppData != nullptr)
            {
                folder = localAppData;
                ::CoTaskMemFree(localAppData);
            }
            if (folder.empty())
            {
                folder = std::filesystem::temp_directory_path().wstring();
            }
            const std::filesystem::path logPath =
                std::filesystem::path(folder) / L"TufScreenControl" / L"update.log";

            std::error_code ec;
            std::filesystem::create_directories(logPath.parent_path(), ec);

            std::wostringstream log;
            const auto http = HttpGetEx(kApiHost, kApiPath);
            log << L"GET " << kApiHost << kApiPath << L"\n";
            log << L"status=" << http.status << L" error=" << http.error
                << L" bodyLen=" << http.body.size() << L"\n";

            if (!http.body.empty())
            {
                std::wstring preview(http.body.begin(),
                                     http.body.begin() + (std::min)(http.body.size(), std::size_t{ 400 }));
                log << L"body=" << preview << L"\n";
            }

            const FirmwareRelease release = FetchLatest();
            log << L"parsed: found=" << (release.found ? 1 : 0)
                << L" version=" << release.version
                << L" fileSize=" << release.fileSize
                << L" url=" << release.downloadUrl << L"\n";

            std::wofstream out(logPath, std::ios::trunc);
            out << log.str();
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

            // 注意：接口把 Version 返回成【字符串】（"51"），不是数字。
            // Value::AsInt 只对数字类型生效，直接用它会被解析成 0。
            const auto& versionValue = file[L"Version"];
            int version = 0;
            if (versionValue.IsNumber())
            {
                version = static_cast<int>(versionValue.number);
            }
            else if (versionValue.IsString())
            {
                try
                {
                    version = std::stoi(versionValue.text);
                }
                catch (...)
                {
                    version = 0;
                }
            }

            release.version = version;
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

        /// 弹「固件更新」对话框。点「下载」在应用内下载固件包。
        inline void ShowDialog(winrt::Microsoft::UI::Xaml::XamlRoot const& root,
                               FirmwareRelease const& release);

        // ------------------------------------------------------------------
        // 应用内下载
        // ------------------------------------------------------------------
        namespace Download
        {
            /// 从 https URL 拆出 host 与 path。
            inline bool SplitUrl(std::wstring const& url, std::wstring& host, std::wstring& path)
            {
                const std::wstring prefix = L"https://";
                if (url.rfind(prefix, 0) != 0)
                {
                    return false;
                }
                const std::size_t slash = url.find(L'/', prefix.size());
                if (slash == std::wstring::npos)
                {
                    host = url.substr(prefix.size());
                    path = L"/";
                    return true;
                }
                host = url.substr(prefix.size(), slash - prefix.size());
                path = url.substr(slash);
                return true;
            }

            /// 从 URL 末段取文件名。
            inline std::wstring FileNameOf(std::wstring const& url)
            {
                std::wstring clean = url;
                const std::size_t query = clean.find(L'?');
                if (query != std::wstring::npos)
                {
                    clean = clean.substr(0, query);
                }
                const std::size_t slash = clean.find_last_of(L'/');
                return slash == std::wstring::npos ? clean : clean.substr(slash + 1);
            }

            /// 同步下载。progress(已收字节, 总字节)，总字节未知时为 0。
            inline bool DownloadTo(std::wstring const& url,
                                   std::filesystem::path const& target,
                                   std::function<void(unsigned long long, unsigned long long)> const& progress)
            {
                std::wstring host;
                std::wstring path;
                if (!SplitUrl(url, host, path))
                {
                    return false;
                }

                std::error_code ec;
                std::filesystem::create_directories(target.parent_path(), ec);

                std::ofstream out(target, std::ios::binary | std::ios::trunc);
                if (!out)
                {
                    return false;
                }

                HINTERNET session = ::WinHttpOpen(L"TUF Screen Control/1.0",
                                                  WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                                  WINHTTP_NO_PROXY_NAME,
                                                  WINHTTP_NO_PROXY_BYPASS, 0);
                if (session == nullptr)
                {
                    return false;
                }
                DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
                ::WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));

                bool ok = false;
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
                            // 先问内容长度，用于进度条
                            unsigned long long total = 0;
                            wchar_t lengthText[64]{};
                            DWORD lengthSize = sizeof(lengthText);
                            if (::WinHttpQueryHeaders(request,
                                                      WINHTTP_QUERY_CONTENT_LENGTH,
                                                      WINHTTP_HEADER_NAME_BY_INDEX,
                                                      lengthText, &lengthSize,
                                                      WINHTTP_NO_HEADER_INDEX))
                            {
                                total = _wcstoui64(lengthText, nullptr, 10);
                            }

                            unsigned long long received = 0;
                            ok = true;
                            for (;;)
                            {
                                DWORD available = 0;
                                if (!::WinHttpQueryDataAvailable(request, &available))
                                {
                                    ok = false;
                                    break;
                                }
                                if (available == 0)
                                {
                                    break;
                                }

                                std::string chunk(available, '\0');
                                DWORD read = 0;
                                if (!::WinHttpReadData(request, chunk.data(), available, &read))
                                {
                                    ok = false;
                                    break;
                                }
                                if (read == 0)
                                {
                                    break;
                                }

                                out.write(chunk.data(), static_cast<std::streamsize>(read));
                                received += read;
                                if (progress)
                                {
                                    progress(received, total);
                                }
                            }

                            out.flush();
                            if (received == 0)
                            {
                                ok = false;
                            }
                        }
                        ::WinHttpCloseHandle(request);
                    }
                    ::WinHttpCloseHandle(connect);
                }
                ::WinHttpCloseHandle(session);

                if (!ok)
                {
                    out.close();
                    std::filesystem::remove(target, ec);
                }
                return ok;
            }

            /// 下载对话框：进度条 + 取消。完成后询问是否打开文件夹。
            inline void ShowDownloadDialog(winrt::Microsoft::UI::Xaml::XamlRoot const& root,
                                           winrt::Microsoft::UI::Dispatching::DispatcherQueue const& queue,
                                           FirmwareRelease const& release)
            {
                using namespace winrt::Microsoft::UI::Xaml::Controls;

                const std::wstring fileName = FileNameOf(release.downloadUrl);
                const std::wstring url = EncodeUrl(release.downloadUrl);

                std::filesystem::path targetDir;
                if (wchar_t* localAppData = nullptr;
                    SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &localAppData)) &&
                    localAppData != nullptr)
                {
                    targetDir = std::filesystem::path(localAppData) / L"TufScreenControl" / L"firmware";
                    ::CoTaskMemFree(localAppData);
                }
                if (targetDir.empty())
                {
                    targetDir = std::filesystem::temp_directory_path() / L"TufScreenControl" / L"firmware";
                }
                const std::filesystem::path target = targetDir / fileName;

                ProgressBar bar;
                bar.Minimum(0);
                bar.Maximum(100);
                bar.Value(0);
                bar.Width(360);

                TextBlock label;
                label.Text(L"正在下载固件…");

                StackPanel panel;
                panel.Spacing(10);
                panel.Children().Append(label);
                panel.Children().Append(bar);

                ContentDialog dialog;
                dialog.XamlRoot(root);
                dialog.Title(winrt::box_value(L"固件更新"));
                dialog.Content(panel);
                dialog.CloseButtonText(L"取消");

                auto cancel = std::make_shared<std::atomic<bool>>(false);
                dialog.Closing([cancel](ContentDialog const&, ContentDialogClosingEventArgs const&)
                {
                    cancel->store(true);
                });

                dialog.ShowAsync();

                std::thread([bar, label, queue, dialog, root, url, target, targetDir, cancel]() mutable
                {
                    const bool ok = DownloadTo(url, target,
                        [bar, label, queue, cancel](unsigned long long received, unsigned long long total)
                        {
                            queue.TryEnqueue([bar, label, received, total, cancel]()
                            {
                                if (cancel->load())
                                {
                                    return;
                                }
                                if (total > 0)
                                {
                                    const double percent =
                                        100.0 * static_cast<double>(received) / static_cast<double>(total);
                                    bar.Value(percent);
                                    label.Text(L"正在下载固件… " +
                                               winrt::hstring(std::to_wstring(static_cast<int>(percent))) + L"%");
                                }
                                else
                                {
                                    label.Text(L"正在下载固件… " +
                                               winrt::hstring(std::to_wstring(received / 1024)) + L" KB");
                                }
                            });
                        });

                    queue.TryEnqueue([dialog, root, ok, target, targetDir, cancel]()
                    {
                        if (cancel->load())
                        {
                            return;
                        }
                        dialog.Hide();

                        ContentDialog done;
                        done.XamlRoot(root);
                        done.Title(winrt::box_value(L"固件更新"));
                        if (ok)
                        {
                            done.Content(winrt::box_value(winrt::hstring(
                                L"固件已下载到：\n" + target.wstring())));
                            done.PrimaryButtonText(L"打开文件夹");
                            done.DefaultButton(ContentDialogButton::Primary);
                        }
                        else
                        {
                            done.Content(winrt::box_value(L"下载失败，请检查网络连接后重试。"));
                        }
                        done.CloseButtonText(L"关闭");

                        const std::wstring folder = targetDir.wstring();
                        done.ShowAsync().Completed([folder](auto const& op, auto const&)
                        {
                            if (op.GetResults() == ContentDialogResult::Primary)
                            {
                                ::ShellExecuteW(nullptr, L"open", folder.c_str(),
                                                nullptr, nullptr, SW_SHOWNORMAL);
                            }
                        });
                    });
                }).detach();
            }
        }

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

            auto queue = winrt::Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();
            dialog.ShowAsync().Completed(
                [root, queue, release](auto const& operation, auto const&)
                {
                    if (operation.GetResults() != ContentDialogResult::Primary)
                    {
                        return;
                    }
                    // 在应用内下载（无需跳转浏览器）
                    Download::ShowDownloadDialog(root, queue, release);
                });
        }
    }
}
