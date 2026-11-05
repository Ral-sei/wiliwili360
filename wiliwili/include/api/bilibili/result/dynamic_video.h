//
// Created by fang on 2022/8/18.
//

#pragma once

#include <cstdlib>

#include "bilibili/util/json.hpp"
#include "user_result.h"
#include "home_result.h"
#include "dynamic_article.h"

namespace bilibili {

class DynamicVideoResult {
public:
    uint64_t aid = 0;
    std::string bvid;
    std::string pic;
    std::string title;
    int duration = 0;
    int pubdate  = 0;
    UserSimpleResult owner;
    VideoSimpleStateResult stat;
};
inline void to_json(nlohmann::json& nlohmann_json_j, const DynamicVideoResult& nlohmann_json_t) {
    NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(NLOHMANN_JSON_TO, aid, bvid, pic, title, duration, pubdate, stat));
}
inline void from_json(const nlohmann::json& nlohmann_json_j, DynamicVideoResult& nlohmann_json_t) {
    if (nlohmann_json_j.contains("owner")) {
        nlohmann_json_j.at("owner").get_to(nlohmann_json_t.owner);
    } else if (nlohmann_json_j.contains("author")) {
        nlohmann_json_j.at("author").get_to(nlohmann_json_t.owner);
    }
    if (nlohmann_json_j.at("duration").is_number()) {
        nlohmann_json_j.at("duration").get_to(nlohmann_json_t.duration);
    }
    if (nlohmann_json_j.at("pubdate").is_number()) {
        nlohmann_json_j.at("pubdate").get_to(nlohmann_json_t.pubdate);
    }
    NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(NLOHMANN_JSON_FROM, aid, bvid, pic, title, stat));
}

typedef std::vector<DynamicVideoResult> DynamicVideoListResult;
typedef std::vector<DynamicArticleResult> DynamicArticleListResult;

template <typename Item>
class DynamicListResultWrapper {
public:
    Item items;
    bool has_more{};
    std::string offset;
    std::string update_baseline;
    unsigned int update_num{};
    unsigned int page{};
};
template <typename Item>
inline void from_json(const nlohmann::json& nlohmann_json_j, DynamicListResultWrapper<Item>& nlohmann_json_t) {
    if (nlohmann_json_j.contains("items") && nlohmann_json_j.at("items").is_array()) {
        nlohmann_json_j.at("items").get_to(nlohmann_json_t.items);
    }
    NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(NLOHMANN_JSON_FROM, has_more, offset, update_baseline, update_num));
}

class DynamicUpResult {
public:
    bool has_update = false;
    std::string face;
    std::string uname;
    uint64_t mid = 0;
};

// The uplist endpoint is not consistent about `mid`: it can arrive as an
// unsigned/signed number, a decimal string, or null.  The Xbox 360 build is
// compiled with -fno-exceptions (toolchain.cmake), so a strict
// get<uint64_t>() on a mismatched type aborts the whole title instead of
// failing the request — measured via KDNet: DynamicUpResult::from_json ->
// nlohmann get_arithmetic_value<unsigned long long> -> abort -> bugcheck 0.
// Parse defensively here; malformed values fall back to 0 and the rest of the
// item is still usable.
inline void from_json(const nlohmann::json& j, DynamicUpResult& t) {
    if (j.contains("has_update") && j.at("has_update").is_boolean())
        t.has_update = j.at("has_update").get<bool>();
    if (j.contains("face") && j.at("face").is_string())
        t.face = j.at("face").get<std::string>();
    if (j.contains("uname") && j.at("uname").is_string())
        t.uname = j.at("uname").get<std::string>();
    if (j.contains("mid")) {
        const auto& m = j.at("mid");
        if (m.is_number_unsigned()) {
            t.mid = m.get<uint64_t>();
        } else if (m.is_number_integer()) {
            const int64_t v = m.get<int64_t>();
            if (v > 0) t.mid = static_cast<uint64_t>(v);
        } else if (m.is_string()) {
            t.mid = std::strtoull(m.get<std::string>().c_str(), nullptr, 10);
        }
    }
}
inline void to_json(nlohmann::json& j, const DynamicUpResult& t) {
    j = nlohmann::json{{"has_update", t.has_update}, {"face", t.face}, {"uname", t.uname}, {"mid", t.mid}};
}

typedef std::vector<DynamicUpResult> DynamicUpListResult;

class DynamicUpListResultWrapper {
public:
    DynamicUpListResult items;
};
inline void from_json(const nlohmann::json& nlohmann_json_j, DynamicUpListResultWrapper& nlohmann_json_t) {
    if (nlohmann_json_j.contains("items") && nlohmann_json_j.at("items").is_array()) {
        nlohmann_json_j.at("items").get_to(nlohmann_json_t.items);
    }
}
};  // namespace bilibili