#pragma once

// 页面生命周期诊断（临时）
// ---------------------------------------------------------------------------
// 目的：确认「反复切换页面时内存上涨」到底是页面没被释放（真泄漏），
// 还是 XAML 堆高水位（假泄漏）。
// 每次页面构造/析构往 %LOCALAPPDATA%\TufScreenControl\page.log 追加一行，
// 记录当前存活页面数与进程内存。日志文件很小，可在验证完后整体删除。
// ---------------------------------------------------------------------------

#include <windows.h>
#include <psapi.h>
#include <shlobj.h>

#pragma comment(lib, "psapi.lib")

#include <atomic>
#include <fstream>
#include <string>

namespace ScreenControl
{
    inline std::atomic<int>& LivePageCount() noexcept
    {
        static std::atomic<int> count{ 0 };
        return count;
    }

    inline std::wstring PageLogPath()
    {
        static std::wstring cached;
        if (!cached.empty())
        {
            return cached;
        }

        wchar_t* localAppData = nullptr;
        if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &localAppData)) &&
            localAppData != nullptr)
        {
            std::wstring folder = localAppData;
            ::CoTaskMemFree(localAppData);

            std::error_code ec;
            std::filesystem::create_directories(folder + L"\\TufScreenControl", ec);
            cached = folder + L"\\TufScreenControl\\page.log";
        }
        return cached;
    }

    /// name: Display / Settings；delta: +1 构造，-1 析构
    inline void TrackPage(wchar_t const* name, int delta)
    {
        const int live = LivePageCount().fetch_add(delta) + delta;

        const std::wstring path = PageLogPath();
        if (path.empty())
        {
            return;
        }

        PROCESS_MEMORY_COUNTERS_EX counters{};
        counters.cb = sizeof(counters);
        unsigned long long workingSet = 0;
        unsigned long long privateBytes = 0;
        if (::GetProcessMemoryInfo(::GetCurrentProcess(),
                                   reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                                   sizeof(counters)))
        {
            workingSet = counters.WorkingSetSize / 1024 / 1024;
            privateBytes = counters.PrivateUsage / 1024 / 1024;
        }

        std::wofstream out(path, std::ios::app);
        out << (delta > 0 ? L"+" : L"-") << name
            << L" live=" << live
            << L" ws=" << workingSet << L"MB"
            << L" private=" << privateBytes << L"MB\n";
    }
}
