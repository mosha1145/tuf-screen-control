#include "pch.h"
#include "DeviceWatcher.h"

namespace TufScreenControl
{
    namespace
    {
        /// 每个窗口只允许一个 DeviceWatcher 实例，用窗口属性挂接。
        constexpr wchar_t kDeviceWatcherProp[] = L"TufScreenControl.DeviceWatcher";

        void Invoke(::TufScreenControl::DeviceWatcher::Handler const& handler) noexcept
        {
            if (!handler)
            {
                return;
            }
            try
            {
                handler();
            }
            catch (...)
            {
                // 窗口过程里绝不能因为回调异常打断系统消息处理
            }
        }
    }

    DeviceWatcher::~DeviceWatcher()
    {
        Stop();
    }

    bool DeviceWatcher::Start(HWND hwnd, Handlers handlers)
    {
        Stop();

        if (hwnd == nullptr || !IsWindow(hwnd))
        {
            return false;
        }

        m_handlers = std::move(handlers);
        m_hwnd = hwnd;

        // 保存原始窗口过程，替换成我们的转发过程。
        m_originalProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
            m_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&DeviceWatcher::ForwardedWndProc)));
        if (m_originalProc == nullptr)
        {
            m_hwnd = nullptr;
            return false;
        }

        // 把 this 挂到窗口属性上（避免占用 USERDATA，WinUI 自己也要用）。
        SetPropW(m_hwnd, kDeviceWatcherProp, reinterpret_cast<HANDLE>(this));
        return true;
    }

    void DeviceWatcher::Stop() noexcept
    {
        if (m_hwnd == nullptr)
        {
            return;
        }

        RemovePropW(m_hwnd, kDeviceWatcherProp);

        if (m_originalProc != nullptr && IsWindow(m_hwnd))
        {
            SetWindowLongPtrW(m_hwnd, GWLP_WNDPROC,
                              reinterpret_cast<LONG_PTR>(m_originalProc));
        }

        m_originalProc = nullptr;
        m_hwnd = nullptr;
        m_handlers = {};
    }

    LRESULT CALLBACK DeviceWatcher::ForwardedWndProc(HWND hwnd, UINT message,
                                                     WPARAM wParam, LPARAM lParam)
    {
        auto* self = reinterpret_cast<DeviceWatcher*>(GetPropW(hwnd, kDeviceWatcherProp));

        if (self != nullptr && message == WM_DEVICECHANGE)
        {
            // 设备插拔：不消费消息，交给默认处理，只做通知。
            Invoke(self->m_handlers.deviceChanged);
        }

        // 转回原始窗口过程。注意：self 可能在回调里被 Stop() 掉，
        // 所以这里用成员保存的原始过程地址（self 可能已失效）。
        if (self != nullptr && self->m_originalProc != nullptr)
        {
            return CallWindowProcW(self->m_originalProc, hwnd, message, wParam, lParam);
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
}
