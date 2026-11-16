// ============================================================================
//  RenderParams.h — 一帧的渲染参数（不可变快照）
// ----------------------------------------------------------------------------
//  对应 v1: tuf_gui.py 里 4 个流线程通过 6 个 getter 回调逐帧读取 UI 状态
//  （get_mode / get_rotation / get_overlay / get_fps / get_quality / get_screen）。
//
//  v1 那种做法让 UI 与线程强耦合、无法单测。v2 改为「不可变快照 + 原子替换」：
//  后台线程每帧读一份快照，参数变更时整份替换，既保留「改参数不用重启线程、
//  视频不闪位置不跳」的优点，又把状态从 UI 里解耦出来。
// ============================================================================
#pragma once

#include <cstdint>

namespace TufScreenControl::Imaging
{
    /// 图像适配模式（对应 v1 asset_lib.FIT_MODES）。
    enum class FitMode
    {
        Contain,   ///< 适应：长边填满，短边补黑边（等比，不变形）
        Cover,     ///< 填充：短边填满，长边居中裁剪
        Stretch,   ///< 拉伸：直接拉到 320x320
    };

    /// 画面旋转（适配水冷安装方向）。
    enum class Rotation
    {
        Deg0 = 0,
        Deg90 = 90,
        Deg180 = 180,
        Deg270 = 270,
    };

    /// 渲染参数快照。
    struct RenderParams
    {
        FitMode fitMode = FitMode::Contain;
        Rotation rotation = Rotation::Deg270;   ///< 实测默认 270° 适配多数安装方向
        std::uint8_t jpegQuality = 60;          ///< 视频建议 50-60，静态图可用 95
    };
}
