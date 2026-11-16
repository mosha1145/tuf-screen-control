// ============================================================================
//  AppConfig.h — 应用配置（对应 v1 config_store.py）
// ----------------------------------------------------------------------------
//  v1 行为（必须兼容）：
//    * DEFAULT_CONFIG 打底 + 文件值覆盖，未知键保留 -> 天然向前兼容
//    * set() = 改内存 + 立即整文件重写（滑杆拖动时疯狂写盘，v2 要改成去抖 + 原子写）
//    * 素材路径相对仓库根存储，软件目录搬移后仍有效
//
//  v2 变更（已确认）：
//    * 配置位置改为 %LOCALAPPDATA%\TufScreenControl\config.json
//    * 首次启动从仓库根 config.json 迁移，并把相对路径重定位为绝对路径
//      （因为配置基目录变了，v1 存储的相对路径会失效）
//    * 绝不修改/删除 v1 的 config.json，用户可随时回退到 v1
// ============================================================================
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace TufScreenControl::Core::Json
{
    class Value;
}

namespace TufScreenControl::Core
{
    /// 与 v1 config_store.DEFAULT_CONFIG 一一对应。
    /// 注意：默认值必须沿用 v1 的「代码默认值」，否则老用户升级后观感会变。
    struct AppConfig
    {
        // ---- 屏幕 ----
        bool screenOn = true;                ///< 默认开屏
        int brightness = 100;                ///< 0-100（默认满亮度）
        std::wstring fitMode = L"contain";   ///< contain / cover / stretch
        int rotation = 0;                    ///< 0 / 90 / 180 / 270

        // ---- 播放 ----
        double photoFps = 10.0;              ///< 0.1 - 30

        // ---- 启动 ----
        bool autoStart = false;

        /// 关闭主窗口时的行为：tray = 隐藏到系统托盘 / exit = 退出程序 / ask = 每次询问
        std::wstring closeAction = L"ask";

        /// 是否已经问过用户（首次关闭时弹一次）
        bool closePromptShown = false;

        // ---- 界面（v2 新增） ----
        std::wstring appTheme = L"system";   ///< system / light / dark
        std::wstring themeColor = L"system"; ///< system / tuf（TUF GAMING 橙色 #FF9E1B）

        /// 睡眠模式：开启后无内容播放时关闭屏幕（命令 0x8000011F = 0）。
        bool sleepMode = true;

        /// 自动检测固件更新。
        bool autoCheckUpdate = true;
        /// 检查频率：startup / daily / weekly / monthly / halfyear。
        std::wstring updateFrequency = L"startup";
        /// 上次检查时间（Unix 秒，0 表示从未检查）。
        long long lastUpdateCheckUnix = 0;

        /// 配置结构版本，用于将来做版本化升级。
        int schemaVersion = 2;
    };

    /// 配置存储：加载 / 保存 / 迁移。
    class ConfigStore
    {
    public:
        /// v1 配置位置（仓库根，只读，不修改）。
        static std::filesystem::path DefaultLegacyConfigPath();

        /// v2 配置位置（%LOCALAPPDATA%\TufScreenControl\config.json）。
        static std::filesystem::path DefaultConfigPath();

        /// 传空路径时自动探测可写位置（LocalAppData -> exe 目录 -> 临时目录）。
        explicit ConfigStore(std::filesystem::path configPath = {});

        /// 实际会被使用的可写配置路径。
        static std::filesystem::path ResolveWritablePath();

        /// 目录是否可写（内部会创建目录并写一个探针文件）。
        static bool IsDirectoryWritable(std::filesystem::path const& directory);

        /// 当前 exe 所在目录。
        static std::filesystem::path ExecutableDirectory();

        /// 加载配置。若 v2 配置不存在而 v1 配置存在，则执行一次性迁移。
        /// 返回值表示是否发生了迁移。
        bool Load();

        /// 保存配置（原子写：临时文件 + 替换）。
        bool Save() const;

        AppConfig& Data() noexcept { return m_config; }
        AppConfig const& Data() const noexcept { return m_config; }

        /// 素材库根目录（v2 仍指向仓库内的 assets\，素材是用户数据不搬走）。
        std::filesystem::path AssetRoot() const;

    private:
        bool MigrateFromLegacy(std::filesystem::path const& legacyPath);

        /// 把解析好的 JSON 合并进 m_config（缺键保持默认值 -> 向前兼容）。
        void ApplyJson(Json::Value const& root);

        std::filesystem::path m_path;
        AppConfig m_config;
    };
}
