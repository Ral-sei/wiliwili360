//
// Created for wiliwili based on bilibili-API-collect documentation
//

#include "bilibili/util/wbi.hpp"

#include <array>
#include <string>
#include <vector>
#include <algorithm>
#include <map>
#include <cpr/cpr.h>
#include <pystring.h>
#include <borealis/core/xbox360_mutex.hpp>

#include "bilibili/util/md5.hpp"
#include "bilibili/util/http.hpp"
#include "bilibili/util/json.hpp"
#include "bilibili/api.h"

namespace bilibili::wbi {
// 重排映射表，用于计算 mixin_key
constexpr std::array<int, 64> MIXIN_KEY_ENC_TAB = {46, 47, 18, 2, 53, 8, 23, 32, 15, 50, 10, 31, 58, 3, 45, 35,
                                                   27, 43, 5, 49, 33, 9, 42, 19, 29, 28, 14, 39, 12, 38, 41, 13,
                                                   37, 48, 7, 16, 24, 55, 40, 61, 26, 17, 0, 1, 60, 51, 30, 4,
                                                   22, 25, 54, 21, 56, 59, 6, 63, 57, 62, 11, 36, 20, 34, 44, 52};

// 存储的 WBI 实时口令
static std::time_t g_last_update_time = 0;

// 缓存的 mixin_key
static std::string g_mixin_key;
static brls::PlatformMutex g_mixin_key_mutex;

/**
 * 从URL中提取key
 * 例如：从 https://i0.hdslb.com/bfs/wbi/7cd084941338484aae1ad9425b84077c.png
 * 提取出 7cd084941338484aae1ad9425b84077c
 */
std::string extractKeyFromUrl(const std::string& url) {
    const size_t lastSlash = url.find_last_of('/');
    const size_t lastDot   = url.find_last_of('.');

    if (lastSlash != std::string::npos && lastDot != std::string::npos && lastSlash < lastDot) {
        return url.substr(lastSlash + 1, lastDot - lastSlash - 1);
    }

    return "";
}

/**
 * 通过混淆算法计算 mixin_key
 * 将 img_key 和 sub_key 拼接后，根据映射表重新排列得到新的字符串
 * 并截取前32位作为 mixin_key
 */
std::string getMixinKey(const std::string& img_key, const std::string& sub_key) {
    const std::string raw_key = img_key + sub_key;
    std::string key;
    key.reserve(32);

    for (size_t i = 0; i < 32; ++i) {
        key.push_back(raw_key[MIXIN_KEY_ENC_TAB[i]]);
    }

    return key;
}

void updateWbiKeys(const std::function<void()>& success, const ErrorCallback& error) {
    const std::time_t now = std::time(nullptr);

    // 如果距离上次更新时间少于1小时，则不更新
    if (now - g_last_update_time < 3600 && !g_mixin_key.empty()) {
        success();
        return;
    }

    auto session = HTTP::createSession();
    session->SetUrl(cpr::Url{parseLink(Api::Nav)});
    session->GetCallback([success, error, now](const cpr::Response& r) {
        if (r.status_code != 200) {
            ERROR_MSG("WBI签名获取失败", -412);
            return;
        }
#if defined(__cpp_exceptions)
        try {
            nlohmann::json res = nlohmann::json::parse(r.text);
#else
        nlohmann::json res = nlohmann::json::parse(r.text, nullptr, false);
        if (!res.is_discarded()) {
#endif
            if (res.contains("data") && res["data"].contains("wbi_img")) {
                const std::string img_key = extractKeyFromUrl(res["data"]["wbi_img"]["img_url"]);
                const std::string sub_key = extractKeyFromUrl(res["data"]["wbi_img"]["sub_url"]);

                // 计算并缓存 mixin_key
                {
                    brls::PlatformLockGuard lock(g_mixin_key_mutex);
                    g_mixin_key        = getMixinKey(img_key, sub_key);
                    g_last_update_time = now;
                }

                // 继续执行网络请求
                success();
                return;
            }
#if defined(__cpp_exceptions)
        } catch (...) {
        }
#else
        }
#endif
        ERROR_MSG("WBI签名失败", -412);
    });
}

void encWbi(cpr::Parameters& params) {
    // 添加 wts 时间戳
    std::time_t wts = std::time(nullptr);
    params.Add({"wts", std::to_string(wts)});

    // 获取所有参数并解析
    std::string paramContent = params.GetContent(cpr::CurlHolder());
    std::vector<std::string> paramPairs;
    pystring::split(paramContent, paramPairs, "&");

    // 解析参数并过滤特殊字符
    std::map<std::string, std::string> sorted_params;
    for (const auto& pair : paramPairs) {
        std::vector<std::string> kv;
        pystring::split(pair, kv, "=");
        if (kv.size() == 2) {
            const std::string& key = kv[0];
            std::string value      = kv[1];

            // 过滤 value 中的 "!'()*" 字符
            value.erase(
                std::remove_if(value.begin(), value.end(),
                               [](char c) { return c == '!' || c == '\'' || c == '(' || c == ')' || c == '*'; }),
                value.end());

            sorted_params[key] = value;
        }
    }

    // 构建待签名字符串
    std::string query;
    for (const auto& pair : sorted_params) {
        if (!query.empty()) {
            query += "&";
        }
        query += pair.first + "=" + pair.second;
    }

    // 计算 w_rid
    std::string w_rid = websocketpp::md5::md5_hash_hex(query + g_mixin_key);

    // 添加 w_rid 参数
    params.Add({"w_rid", w_rid});
}
}
