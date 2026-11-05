//
// Created by fang on 2023/11/14.
//

#pragma once

#include <nlohmann/json.hpp>

#include <climits>
#include <string>
#include <utility>
#include <vector>

namespace bilibili {

class LiveDanmakuHostinfo {
public:
    std::string host;
    int ws_port  = 0;
    int wss_port = 0;
    int port     = 0;
};

// 容错解析：B 站偶尔会省略某个端口字段，用 at() 会让整个 getDanmuInfo
// 反序列化失败，表现就是直播间完全没有弹幕。
//
// value() 只容忍“字段缺失”：字段存在但为 null 或类型不对时它照样抛
// type_error，而 Xbox 360 构建是 -fno-exceptions，抛异常会直接 abort。
// 这里改成显式类型检查，坏值回退到默认值。
namespace live_danmaku_json {

inline std::string stringOr(const nlohmann::json& j, const char* key) {
    const auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

inline int intOr(const nlohmann::json& j, const char* key) {
    const auto it = j.find(key);
    if (it == j.end()) return 0;
    if (it->is_number_unsigned()) {
        const auto value = it->get<unsigned long long>();
        return value <= static_cast<unsigned long long>(INT_MAX) ? static_cast<int>(value) : 0;
    }
    if (it->is_number_integer()) {
        const auto value = it->get<long long>();
        return value >= INT_MIN && value <= INT_MAX ? static_cast<int>(value) : 0;
    }
    return 0;
}

}  // namespace live_danmaku_json

inline void from_json(const nlohmann::json& j, LiveDanmakuHostinfo& h) {
    h.host     = live_danmaku_json::stringOr(j, "host");
    h.ws_port  = live_danmaku_json::intOr(j, "ws_port");
    h.wss_port = live_danmaku_json::intOr(j, "wss_port");
    h.port     = live_danmaku_json::intOr(j, "port");
}

class LiveDanmakuinfo {
public:
    std::vector<LiveDanmakuHostinfo> host_list;
    std::string token = "";
};

inline void from_json(const nlohmann::json& j, LiveDanmakuinfo& info) {
    info.token = live_danmaku_json::stringOr(j, "token");
    info.host_list.clear();
    const auto it = j.find("host_list");
    if (it == j.end() || !it->is_array()) return;
    for (const auto& item : *it) {
        // 单个坏元素不应让整份 host_list 反序列化失败。
        if (!item.is_object()) continue;
        LiveDanmakuHostinfo host;
        from_json(item, host);
        info.host_list.push_back(std::move(host));
    }
}

class LiveHistoryDanmakuData {
public:
    std::vector<nlohmann::json> admin;
    std::vector<nlohmann::json> room;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(LiveHistoryDanmakuData, admin, room);

};  // namespace bilibili