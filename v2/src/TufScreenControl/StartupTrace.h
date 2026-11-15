#pragma once

// ============================================================================
//  StartupTrace.h — 启动/生命周期检查点日志
// ----------------------------------------------------------------------------
//  排查「冷启动偶发崩溃」用：崩溃时进程直接消失，没有调试器就看不出走到哪一步。
//  这里在关键节点往 exe 同目录的 startup.log 追加一行并立即 flush，
//  崩溃后读日志即可知道最后一个成功到达的检查点。
//
//  已知限制：访问冲突（0xC0000005）不会走 UnhandledException，所以只能靠这种
//  埋点定位阶段；排查完即可整体移除。
// ============================================================================

#include <windows.h>

#include <cstdio>
#include <string>

namespace TufScreenControl
{
    inline void TraceStartup(char const* stage) noexcept
    {
        static const unsigned long long startTick = ::GetTickCount64();

        static const std::wstring logPath = []()
        {
            wchar_t module[MAX_PATH]{};
            ::GetModuleFileNameW(nullptr, module, MAX_PATH);
            std::wstring path{ module };
            const auto slash = path.find_last_of(L'\\');
            if (slash != std::wstring::npos)
            {
                path.resize(slash + 1);
            }
            path += L"startup.log";
            return path;
        }();

        FILE* file = nullptr;
        if (::_wfopen_s(&file, logPath.c_str(), L"a") != 0 || file == nullptr)
        {
            return;
        }
        std::fprintf(file, "[%6llu ms] pid=%5lu tid=%5lu  %s\n",
                     ::GetTickCount64() - startTick,
                     ::GetCurrentProcessId(),
                     ::GetCurrentThreadId(),
                     stage);
        std::fflush(file);
        std::fclose(file);
    }
}
