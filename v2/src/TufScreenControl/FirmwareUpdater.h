#pragma once

// 固件更新检查（header-only）
// ---------------------------------------------------------------------------
// 1) Windows.Web.Http.HttpClient GET ASUS 支持页接口（返回 JSON）
// 2) Windows.Data.Json 解析
// 3) 与设备当前固件版本（USB 实例 ID 里的 REV_xxxx）比较
// 4) 有新版时弹 ContentDialog，点「下载」在应用内下载并做 SHA-256 校验
//
// 全部使用 WinRT 官方 API（网络 / JSON / 加密 / 流），不直接调 WinHTTP、
// BCrypt 或自写 JSON 解析器。
//
// 接口返回结构（已实测）：
//   Result.Obj[0].Files[0] = { Version:"51", FileSize:"1.27 MB",
//                              DownloadUrl:{ China:..., Global:... } }
// ---------------------------------------------------------------------------

#include <string>
#include <functional>
#include <thread>
#include <optional>
#include <type_traits>
#include <cctype>
#include <atomic>
#include <fstream>
#include <filesystem>
#include <memory>
#include <sstream>
#include <algorithm>
#include <vector>

#include <shlobj.h>
#include <shellapi.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Web.Http.h>
#include <winrt/Windows.Web.Http.Headers.h>
#include <winrt/Windows.Security.Cryptography.Core.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.System.h>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>

namespace TufScreenControl::Ui
{
    /// 一次更新检查的结果。
    struct FirmwareRelease
    {
        bool found = false;
        int version = 0;
        std::wstring fileSize;
        std::wstring title;
        std::wstring downloadUrl;
        std::wstring sha256;   ///< 接口给出的安装包 SHA-256（大写十六进制）
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
            DWORD error = 0;    ///< 失败时的 HRESULT
        };

        /// 在临时 MTA 线程上执行 fn 并等它结束。
        ///
        /// 这些 WinRT 异步操作（HttpClient / DataReader / 加密）都要在套间里跑。
        /// 调用方在 UI 线程（STA）时必须靠消息循环派发才能完成——而这里的调用点
        /// 是同步语义（RunSelfTest 直接在 OnLaunched 里同步调），在 STA 上阻塞等
        /// 一定死锁。统一丢到临时 MTA 线程执行，调用方保持同步，行为可预测。
        template <typename Fn>
        auto RunInMta(Fn&& fn)
        {
            using Result = std::invoke_result_t<Fn>;

            std::optional<Result> result;
            std::thread worker([&]()
            {
                try
                {
                    winrt::init_apartment(winrt::apartment_type::multi_threaded);
                    result.emplace(fn());
                    winrt::uninit_apartment();
                }
                catch (...)
                {
                }
            });
            worker.join();

            return result.has_value() ? std::move(*result) : Result{};
        }

        /// 等一个 WinRT 异步操作完成，超时则取消并返回 false。
        /// WinRT 的 HttpClient 没有 .NET 那种 Timeout 属性，超时只能靠
        /// Completed + 等待句柄 + Cancel() 自己组合。
        template <typename Async>
        bool WaitWithTimeout(Async const& operation, std::chrono::milliseconds timeout)
        {
            winrt::handle completed{ ::CreateEventW(nullptr, FALSE, FALSE, nullptr) };
            operation.Completed([handle = completed.get()](auto&&, auto&&)
            {
                ::SetEvent(handle);
            });

            if (::WaitForSingleObject(completed.get(), static_cast<DWORD>(timeout.count())) ==
                WAIT_OBJECT_0)
            {
                return true;
            }

            try
            {
                operation.Cancel();
            }
            catch (...)
            {
            }
            return false;
        }

        /// 同步 HTTP GET（内部用 Windows.Web.Http.HttpClient，跟随重定向、
        /// 自动协商 TLS 与解压）。失败时 body 为空，error/status 说明原因。
        inline HttpResult HttpGetEx(std::wstring const& host, std::wstring const& path)
        {
            return RunInMta([&]() -> HttpResult
            {
                HttpResult result;
                try
                {
                    // Windows.Foundation.Uri 要完整 URI，必须显式带 https:// 前缀
                    // （WinHTTP 那套是 host / path 分开传的，不能直接拼过来）
                    winrt::Windows::Foundation::Uri const uri{ L"https://" + host + path };

                    using winrt::Windows::Web::Http::HttpClient;
                    HttpClient client;

                    const auto operation = client.GetAsync(uri);
                    if (!WaitWithTimeout(operation, std::chrono::milliseconds(15000)))
                    {
                        result.error = ERROR_TIMEOUT;
                        return result;
                    }

                    const auto response = operation.GetResults();
                    result.status = static_cast<DWORD>(response.StatusCode());

                    const auto body = response.Content().ReadAsStringAsync().get();
                    result.body = winrt::to_string(body);
                }
                catch (winrt::hresult_error const& ex)
                {
                    result.error = static_cast<DWORD>(ex.code());
                }
                catch (...)
                {
                    result.error = ERROR_GEN_FAILURE;
                }
                return result;
            });
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
            using winrt::Windows::Data::Json::JsonObject;
            using winrt::Windows::Data::Json::JsonValueType;

            FirmwareRelease release;

            const std::string raw = HttpGet(kApiHost, kApiPath);
            if (raw.empty())
            {
                return release;
            }

            // 从 JsonObject 里安全取字符串：键不存在或类型不对都退回默认值。
            const auto text = [](JsonObject const& object, wchar_t const* key,
                                 std::wstring const& fallback) -> std::wstring
            {
                try
                {
                    if (object.HasKey(key))
                    {
                        return std::wstring(object.GetNamedString(key).c_str());
                    }
                }
                catch (...)
                {
                }
                return fallback;
            };

            try
            {
                const auto root = JsonObject::Parse(winrt::to_hstring(raw));

                const auto objects = root.GetNamedObject(L"Result").GetNamedArray(L"Obj");
                if (objects.Size() == 0)
                {
                    return release;
                }

                const auto files = objects.GetObjectAt(0).GetNamedArray(L"Files");
                if (files.Size() == 0)
                {
                    return release;
                }

                const auto file = files.GetObjectAt(0);

                // 注意：接口把 Version 返回成【字符串】（"51"）。但历史上也出现过
                // 数字类型，两种都兼容，避免整段解析因为类型变化而失效。
                int version = 0;
                switch (file.GetNamedValue(L"Version").ValueType())
                {
                case JsonValueType::Number:
                    version = static_cast<int>(file.GetNamedNumber(L"Version", 0));
                    break;
                case JsonValueType::String:
                    try
                    {
                        version = std::stoi(std::wstring(file.GetNamedString(L"Version").c_str()));
                    }
                    catch (...)
                    {
                        version = 0;
                    }
                    break;
                default:
                    break;
                }

                release.version = version;
                release.fileSize = text(file, L"FileSize", L"未知");
                release.title = text(file, L"Title", L"");
                release.sha256 = text(file, L"sha256", L"");

                release.downloadUrl = text(file.GetNamedObject(L"DownloadUrl"), L"China", L"");
                if (release.downloadUrl.empty())
                {
                    // 国内渠道拿不到时退回全球渠道
                    release.downloadUrl = text(file.GetNamedObject(L"DownloadUrl"), L"Global", L"");
                }
                release.found = release.version > 0;
            }
            catch (...)
            {
                return FirmwareRelease{};
            }

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

            /// 计算文件 SHA-256（大写十六进制）。失败返回空串。
            /// 用 Windows.Security.Cryptography.Core 的官方实现（原来的 BCrypt 直调已移除）。
            inline std::wstring FileSha256(std::filesystem::path const& path)
            {
                std::ifstream in(path, std::ios::binary);
                if (!in)
                {
                    return {};
                }

                return RunInMta([&]() -> std::wstring
                {
                    using winrt::Windows::Security::Cryptography::CryptographicBuffer;
                    using winrt::Windows::Security::Cryptography::Core::HashAlgorithmNames;
                    using winrt::Windows::Security::Cryptography::Core::HashAlgorithmProvider;

                    try
                    {
                        const auto provider =
                            HashAlgorithmProvider::OpenAlgorithm(HashAlgorithmNames::Sha256());
                        auto hash = provider.CreateHash();

                        std::vector<std::uint8_t> buffer(64 * 1024);
                        while (in)
                        {
                            in.read(reinterpret_cast<char*>(buffer.data()),
                                    static_cast<std::streamsize>(buffer.size()));
                            const auto got = in.gcount();
                            if (got <= 0)
                            {
                                break;
                            }
                            hash.Append(CryptographicBuffer::CreateFromByteArray(
                                winrt::array_view<std::uint8_t const>(
                                    buffer.data(), buffer.data() + got)));
                        }

                        const auto digest = hash.GetValueAndReset();
                        const std::wstring hex =
                            CryptographicBuffer::EncodeToHexString(digest).c_str();

                        // 接口给的是大写十六进制，统一转大写便于直接比较
                        std::wstring upper = hex;
                        for (wchar_t& ch : upper)
                        {
                            ch = static_cast<wchar_t>(towupper(ch));
                        }
                        return upper;
                    }
                    catch (...)
                    {
                        return std::wstring{};
                    }
                });
            }

            /// 同步下载。progress(已收字节, 总字节)，总字节未知时为 0。
            /// 用 HttpClient + DataReader 流式读取，不把整个文件读进内存。
            inline bool DownloadTo(std::wstring const& url,
                                   std::filesystem::path const& target,
                                   std::function<void(unsigned long long, unsigned long long)> const& progress)
            {
                std::error_code ec;
                std::filesystem::create_directories(target.parent_path(), ec);

                return RunInMta([&]() -> bool
                {
                    using winrt::Windows::Web::Http::HttpClient;
                    using winrt::Windows::Web::Http::HttpCompletionOption;
                    using winrt::Windows::Web::Http::HttpStatusCode;
                    using winrt::Windows::Storage::Streams::DataReader;

                    const auto removeTarget = [&]()
                    {
                        std::error_code removeEc;
                        std::filesystem::remove(target, removeEc);
                    };

                    try
                    {
                        winrt::Windows::Foundation::Uri const uri{ EncodeUrl(url) };

                        HttpClient client;
                        // 只要响应头就开始读，避免把大文件整体缓冲到内存
                        const auto response = client.GetAsync(uri, HttpCompletionOption::ResponseHeadersRead).get();
                        if (response.StatusCode() != HttpStatusCode::Ok)
                        {
                            return false;
                        }

                        const auto content = response.Content();
                        unsigned long long total = 0;
                        if (const auto length = content.Headers().ContentLength(); length)
                        {
                            total = length.Value();
                        }

                        std::ofstream out(target, std::ios::binary | std::ios::trunc);
                        if (!out)
                        {
                            return false;
                        }

                        const auto stream = content.ReadAsInputStreamAsync().get();
                        DataReader reader(stream);

                        unsigned long long received = 0;
                        for (;;)
                        {
                            const std::uint32_t loaded = reader.LoadAsync(64 * 1024).get();
                            if (loaded == 0)
                            {
                                break;
                            }

                            std::vector<std::uint8_t> chunk(loaded);
                            reader.ReadBytes(chunk);
                            out.write(reinterpret_cast<char const*>(chunk.data()),
                                      static_cast<std::streamsize>(loaded));

                            received += loaded;
                            if (progress)
                            {
                                progress(received, total);
                            }
                        }

                        out.flush();
                        if (received == 0)
                        {
                            out.close();
                            removeTarget();
                            return false;
                        }
                        return true;
                    }
                    catch (...)
                    {
                        removeTarget();
                        return false;
                    }
                });
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
                if (wchar_t* downloads = nullptr;
                    SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &downloads)) &&
                    downloads != nullptr)
                {
                    // 用户默认下载文件夹，不写死路径
                    targetDir = std::filesystem::path(downloads);
                    ::CoTaskMemFree(downloads);
                }
                if (targetDir.empty())
                {
                    targetDir = std::filesystem::temp_directory_path();
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

                std::thread([bar, label, queue, dialog, root, url, target, targetDir, cancel,
                             sha256 = release.sha256]() mutable
                {
                    bool ok = false;
                    for (int attempt = 1; attempt <= 3; ++attempt)
                    {
                        if (cancel->load())
                        {
                            break;
                        }

                        if (attempt > 1)
                        {
                            queue.TryEnqueue([label, attempt]()
                            {
                                label.Text(L"正在重新下载固件…（第 " +
                                           winrt::hstring(std::to_wstring(attempt)) + L" 次）");
                            });
                        }

                        ok = DownloadTo(url, target,
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

                        if (!ok)
                        {
                            continue;
                        }

                        // 静默校验：接口给了 SHA-256 就比对，不一致则删除重下
                        if (!sha256.empty())
                        {
                            const std::wstring actual = FileSha256(target);
                            if (actual.empty() || _wcsicmp(actual.c_str(), sha256.c_str()) != 0)
                            {
                                std::error_code removeEc;
                                std::filesystem::remove(target, removeEc);
                                ok = false;
                                continue;
                            }
                        }
                        break;
                    }

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

            std::wstring message = L"新固件已发布，是否下载？\n版本 ";
            message += std::to_wstring(release.version);
            message += L"，大小 ";
            message += release.fileSize;
            message += L"。";

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
