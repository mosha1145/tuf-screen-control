#pragma once

// ============================================================================
//  DeviceWatcher.h — 设备插拔监听
// ----------------------------------------------------------------------------
//  只监听 WM_DEVICECHANGE：水冷屏插拔后立即重新枚举，无需重启软件。
//
//  早期这里还接管过电源/会话事件（睡眠、关机、注销），配合 v1 的「智能关屏」
//  联动；该功能已整体移除，相关分支与回调一并删掉了。
//
//  实现方式：用 SetWindowLongPtr(GWLP_WNDPROC) 做经典子类化，兼容性最好
//  （刻意不用 SetWindowSubclass，那需要 comctl32 v6 激活上下文）。
// ============================================================================

#include <windows.h>

#include <functional>

namespace TufScreenControl
{
    class DeviceWatcher
    {
    public:
        using Handler = std::function<void()>;

        struct Handlers
        {
            /// 设备插拔（WM_DEVICECHANGE）：用于重新枚举水冷屏。
            Handler deviceChanged;
        };

        DeviceWatcher() = default;
        ~DeviceWatcher();

        DeviceWatcher(DeviceWatcher const&) = delete;
        DeviceWatcher& operator=(DeviceWatcher const&) = delete;

        /// 开始监听指定窗口。重复调用会先停止上一次。
        bool Start(HWND hwnd, Handlers handlers);

        /// 停止监听并恢复原始窗口过程。
        void Stop() noexcept;


    private:
        static LRESULT CALLBACK ForwardedWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

        HWND m_hwnd = nullptr;
        WNDPROC m_originalProc = nullptr;
        Handlers m_handlers;
    };
}
