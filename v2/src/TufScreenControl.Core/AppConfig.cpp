#include "pch.h"
#include "AppConfig.h"
#include "MinimalJson.h"

#include <shlobj.h>

#include <fstream>
#include <sstream>

#pragma comment(lib, "shell32.lib")

namespace TufScreenControl::Core
{
    namespace
    {
        /// v1 仓库根的判定标志：同时存在这些文件才认为是仓库根。
        bool LooksLikeRepoRoot(std::filesystem::path const& dir)
        {
            std::error_code ec;
            return std::filesystem::exists(dir / L"tuf_gui.py", ec) &&
                   std::filesystem::exists(dir / L"config_store.py", ec);
        }

        /// 从当前模块位置向上找仓库根（v2 构建产物在 <repo>\v2\out\...，需要上溯 3 层）。
        std::filesystem::path FindRepoRoot()
        {
            wchar_t modulePath[MAX_PATH]{};
            if (GetModuleFileNameW(nullptr, modulePath, MAX_PATH) == 0)
            {
                return {};
            }
            std::filesystem::path dir = std::filesystem::path(modulePath).parent_path();
            for (int depth = 0; depth < 8 && !dir.empty(); ++depth)
            {
                if (LooksLikeRepoRoot(dir))
                {
                    return dir;
                }
                const std::filesystem::path parent = dir.parent_path();
                if (parent == dir)
                {
                    break;
                }
                dir = parent;
            }
            return {};
        }

        std::string ReadAllBytes(std::filesystem::path const& path)
        {
            std::ifstream in(path, std::ios::binary);
            if (!in)
            {
                return {};
            }
            std::ostringstream buffer;
            buffer << in.rdbuf();
            return buffer.str();
        }

        /// UTF-8 -> UTF-16
        std::wstring WidenUtf8(std::string const& text)
        {
            if (text.empty())
            {
                return {};
            }
            const int needed = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                                   static_cast<int>(text.size()), nullptr, 0);
            std::wstring out(static_cast<std::size_t>(needed > 0 ? needed : 0), L'\0');
            if (needed > 0)
            {
                MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                    out.data(), needed);
            }
            return out;
        }

        /// UTF-16 -> UTF-8
        std::string NarrowUtf8(std::wstring const& text)
        {
            if (text.empty())
            {
                return {};
            }
            const int needed = WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                                   static_cast<int>(text.size()), nullptr, 0,
                                                   nullptr, nullptr);
            std::string out(static_cast<std::size_t>(needed > 0 ? needed : 0), '\0');
            if (needed > 0)
            {
                WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                    out.data(), needed, nullptr, nullptr);
            }
            return out;
        }

        std::string JsonEscape(std::string const& text)
        {
            std::string out;
            out.reserve(text.size() + 8);
            for (char const c : text)
            {
                switch (c)
                {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20)
                    {
                        char buf[8]{};
                        sprintf_s(buf, "\\u%04x", static_cast<unsigned char>(c));
                        out += buf;
                    }
                    else
                    {
                        out.push_back(c);
                    }
                    break;
                }
            }
            return out;
        }

        std::wstring ToLower(std::wstring value)
        {
            for (wchar_t& c : value)
            {
                c = static_cast<wchar_t>(::towlower(c));
            }
            return value;
        }
    }

    std::filesystem::path ConfigStore::DefaultConfigPath()
    {
        PWSTR localAppData = nullptr;
        std::filesystem::path base;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &localAppData)) &&
            localAppData != nullptr)
        {
            base = localAppData;
            CoTaskMemFree(localAppData);
        }
        if (base.empty())
        {
            base = std::filesystem::temp_directory_path();
        }
        return base / L"TufScreenControl" / L"config.json";
    }

    std::filesystem::path ConfigStore::DefaultLegacyConfigPath()
    {
        // 优先从模块位置推导仓库根，避免硬编码绝对路径（目录改名/移动后仍可用）。
        const std::filesystem::path repoRoot = FindRepoRoot();
        if (!repoRoot.empty())
        {
            return repoRoot / L"config.json";
        }
        return {};   // 找不到就当作没有 v1 配置
    }

    ConfigStore::ConfigStore(std::filesystem::path configPath)
        : m_path(std::move(configPath))
    {
        // 默认路径可能因为环境原因不可写（例如受限令牌、漫游配置只读、
        // 或某些安全软件拦截 AppData 写入）。这里在真正使用前先探一次可写性，
        // 不可写就回退到 exe 同目录，保证配置一定能落盘（便携模式）。
        if (m_path.empty())
        {
            m_path = ResolveWritablePath();
        }
    }

    std::filesystem::path ConfigStore::ExecutableDirectory()
    {
        wchar_t modulePath[MAX_PATH]{};
        if (GetModuleFileNameW(nullptr, modulePath, MAX_PATH) == 0)
        {
            return {};
        }
        return std::filesystem::path(modulePath).parent_path();
    }

    bool ConfigStore::IsDirectoryWritable(std::filesystem::path const& directory)
    {
        std::error_code ec;
        std::filesystem::create_directories(directory, ec);
        if (ec)
        {
            return false;
        }

        const std::filesystem::path probe = directory / L".dsh_write_probe";
        {
            std::ofstream test(probe, std::ios::binary | std::ios::trunc);
            if (!test)
            {
                return false;
            }
            test << "probe";
        }
        std::filesystem::remove(probe, ec);
        return true;
    }

    std::filesystem::path ConfigStore::ResolveWritablePath()
    {
        // 1) 首选：%LOCALAPPDATA%\TufScreenControl\config.json
        const std::filesystem::path preferred = DefaultConfigPath();
        if (!preferred.parent_path().empty() && IsDirectoryWritable(preferred.parent_path()))
        {
            return preferred;
        }

        // 2) 回退：exe 同目录（便携模式）
        const std::filesystem::path exeDir = ExecutableDirectory();
        if (!exeDir.empty() && IsDirectoryWritable(exeDir))
        {
            return exeDir / L"config.json";
        }

        // 3) 最后兜底：临时目录（至少不会崩，重启后可能丢失）
        return std::filesystem::temp_directory_path() / L"TufScreenControl" / L"config.json";
    }

    std::filesystem::path ConfigStore::AssetRoot() const
    {
        const std::filesystem::path repoRoot = FindRepoRoot();
        return repoRoot.empty() ? std::filesystem::path{} : repoRoot / L"assets";
    }

    bool ConfigStore::Load()
    {
        std::error_code ec;

        if (std::filesystem::exists(m_path, ec))
        {
            const Json::Value root = Json::Parse(ReadAllBytes(m_path));
            if (root.IsObject())
            {
                ApplyJson(root);
                return false;
            }
            // 配置损坏：保留一份 .broken 便于排查，然后退回默认值。
            std::filesystem::path broken = m_path;
            broken += L".broken";
            std::filesystem::copy_file(m_path, broken,
                                       std::filesystem::copy_options::overwrite_existing, ec);
            return false;
        }

        const std::filesystem::path legacy = DefaultLegacyConfigPath();
        if (!legacy.empty() && std::filesystem::exists(legacy, ec))
        {
            return MigrateFromLegacy(legacy);
        }
        return false;
    }

    bool ConfigStore::MigrateFromLegacy(std::filesystem::path const& legacyPath)
    {
        const Json::Value root = Json::Parse(ReadAllBytes(legacyPath));
        if (!root.IsObject())
        {
            return false;
        }

        ApplyJson(root);

        // 注意：绝不删除/修改 v1 的 config.json —— 用户可随时回退到 v1。
        Save();
        return true;
    }

    void ConfigStore::ApplyJson(Json::Value const& root)
    {
        // screen_on / brightness 等默认值沿用 v1 代码默认值，缺键就不动。
        m_config.screenOn = root[L"screen_on"].AsBool(m_config.screenOn);
        m_config.brightness = root[L"brightness"].AsInt(m_config.brightness);
        m_config.fitMode = root[L"fit_mode"].AsString(m_config.fitMode);
        m_config.rotation = root[L"rotation"].AsInt(m_config.rotation);
        m_config.photoFps = root[L"photo_fps"].AsDouble(m_config.photoFps);

        m_config.autoStart = root[L"auto_start"].AsBool(m_config.autoStart);
        m_config.closeAction = ToLower(root[L"close_action"].AsString(m_config.closeAction));
        m_config.closePromptShown = root[L"close_prompt_shown"].AsBool(m_config.closePromptShown);
        m_config.appTheme = ToLower(root[L"app_theme"].AsString(m_config.appTheme));
        m_config.themeColor = ToLower(root[L"theme_color"].AsString(m_config.themeColor));

        m_config.sleepMode = root[L"sleep_mode"].AsBool(m_config.sleepMode);
        m_config.autoCheckUpdate = root[L"auto_check_update"].AsBool(m_config.autoCheckUpdate);
        m_config.updateFrequency = root[L"update_frequency"].AsString(m_config.updateFrequency);
        m_config.lastUpdateCheckUnix =
            static_cast<long long>(root[L"last_update_check"].AsDouble(
                static_cast<double>(m_config.lastUpdateCheckUnix)));

        m_config.schemaVersion = root[L"schema_version"].AsInt(m_config.schemaVersion);
    }

    bool ConfigStore::Save() const
    {
        std::error_code ec;

        if (!m_path.parent_path().empty())
        {
            std::filesystem::create_directories(m_path.parent_path(), ec);
        }

        std::ostringstream out;
        out << "{\n";
        out << "  \"schema_version\": " << m_config.schemaVersion << ",\n";
        out << "  \"screen_on\": " << (m_config.screenOn ? "true" : "false") << ",\n";
        out << "  \"brightness\": " << m_config.brightness << ",\n";
        out << "  \"fit_mode\": \"" << JsonEscape(NarrowUtf8(m_config.fitMode)) << "\",\n";
        out << "  \"photo_fps\": " << m_config.photoFps << ",\n";
        out << "  \"rotation\": " << m_config.rotation << ",\n";
        out << "  \"auto_start\": " << (m_config.autoStart ? "true" : "false") << ",\n";
        out << "  \"app_theme\": \"" << JsonEscape(NarrowUtf8(m_config.appTheme)) << "\",\n";
        out << "  \"theme_color\": \"" << JsonEscape(NarrowUtf8(m_config.themeColor)) << "\",\n";
        out << "  \"close_action\": \"" << JsonEscape(NarrowUtf8(m_config.closeAction)) << "\",\n";
        out << "  \"close_prompt_shown\": " << (m_config.closePromptShown ? "true" : "false") << ",\n";
        out << "  \"sleep_mode\": " << (m_config.sleepMode ? "true" : "false") << ",\n";
        out << "  \"auto_check_update\": " << (m_config.autoCheckUpdate ? "true" : "false") << ",\n";
        out << "  \"update_frequency\": \"" << JsonEscape(NarrowUtf8(m_config.updateFrequency)) << "\",\n";
        out << "  \"last_update_check\": " << m_config.lastUpdateCheckUnix << "\n";
        out << "}\n";

        // 原子写：先写临时文件，再替换目标，避免崩溃留下半截 JSON（v1 的隐患）。
        std::filesystem::path temp = m_path;
        temp += L".tmp";
        {
            std::ofstream file(temp, std::ios::binary | std::ios::trunc);
            if (!file)
            {
                return false;
            }
            const std::string text = out.str();
            file.write(text.data(), static_cast<std::streamsize>(text.size()));
            if (!file)
            {
                return false;
            }
        }

        if (!MoveFileExW(temp.c_str(), m_path.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            std::filesystem::remove(temp, ec);
            return false;
        }
        return true;
    }
}
