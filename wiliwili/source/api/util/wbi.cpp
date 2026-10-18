//
// Created for wiliwili based on bilibili-API-collect documentation
//

#include "bilibili/util/wbi.hpp"

#include <array>
#include <chrono>
#include <string>
#include <vector>
#include <algorithm>
#include <map>
#include <cpr/cpr.h>
#include <pystring.h>
#include <borealis/core/xbox360_mutex.hpp>
#include <borealis/core/logger.hpp>

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

// 当前 Unix 秒（§5.7-1 修复）：XDK 上 `std::time(nullptr)` 返回形如
// `(epoch << 32) | x` 的 64 位打包值（实测 now=7692297454196417448，高 32 位才是
// 真实 epoch），导致 `now - last < 3600` 恒为假 → 每次开视频都强制刷新 nav，
// 且 `wts` 也是垃圾值。这里统一改用 chrono（与 brls logger 的时间路径一致）。
static std::time_t wbiNow() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// 缓存的 mixin_key
static std::string g_mixin_key;
static brls::PlatformMutex g_mixin_key_mutex;

// P0 §5.5-2 测量：正在飞的 nav 刷新数与一次性探测标记。
static volatile long g_nav_inflight       = 0;
static volatile long g_wbi_probe_logged   = 0;

// §5.5-2 修复：并发 refresh 请求合并到同一条 nav。
// 成对的 updateWbiKeys（videoUrl + pageDetail 背靠背调用）在 key 为空时各发一条
// 全套 DNS+TCP+TLS 的 nav，首播延迟被放大一倍；这里让后到的调用方挂起等待，
// 复用第一条 nav 的结果。保护 g_nav_fetching 与 waiters 的锁与 mixin_key 共用。
static bool g_nav_fetching = false;
static std::vector<std::pair<std::function<void()>, ErrorCallback>> g_nav_waiters;

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
    // §5.7-1：改用 wbiNow()（原 std::time(nullptr) 在 XDK 返回打包值 → TTL 恒过期）
    const std::time_t now = wbiNow();

    // P0 §5.5-2 测量：分支诊断。保持原逻辑的无锁读（写在 107-109 行持锁），
    // 故意不加锁——若加锁会掩盖"并发读到中间态(空 key)"的竞态假设。
    // refresh 分支打印 age/empty 即可区分"TTL 真过期"与"key 被读成空"。
    // valid 分支用 debug 级避免刷屏（每次签名请求都会走到）。
    const bool key_empty     = g_mixin_key.empty();
    const std::time_t last   = g_last_update_time;
    const long age           = (long)(now - last);

    // 如果距离上次更新时间少于1小时，则不更新
    if (now - last < 3600 && !key_empty) {
        brls::Logger::debug("[wbi] valid age={}s", age);
        success();
        return;
    }

    // refresh 分支：日志级别 info。inflight>1 表示并发刷新未合并（成对 nav 的直接证据）。
    // §5.6-4 新增原始值：三轮实测出现 age=200/0s(<3600) 仍走 refresh、启动 age=1879368616s
    // 等与逻辑矛盾的现象，打印 now/last 64 位原值以区分
    //   (a) `(long)(now-last)` 32 位截断（XDK long=32 位 vs time_t=64 位）
    //   (b) `g_last_update_time` 无锁撕裂读
    //   (c) devkit 时钟跳变
    const long inflight = ++g_nav_inflight;
    brls::Logger::info("[wbi] refresh age={}s empty={} inflight={} now={} last={} raw_dt={}",
                       age, (int)key_empty, inflight,
                       (long long)now, (long long)last, (long long)(now - last));

    // 已有一条 nav 在飞：本次调用不发第二条，挂起等其结果复用。
    {
        brls::PlatformLockGuard lock(g_mixin_key_mutex);
        if (g_nav_fetching) {
            g_nav_waiters.emplace_back(success, error);
            brls::Logger::info("[wbi] refresh merged into in-flight nav, waiters={}",
                               (long)g_nav_waiters.size());
            --g_nav_inflight;  // 合并调用不经过 cpr 回调，InflightGuard 不会替它归还
            return;
        }
        g_nav_fetching = true;
    }

    // 完成（成功或失败）后统一唤醒合并等待者，并复位 fetching 标志。
    auto flushNavWaiters = [](bool ok, const std::string& msg) {
        std::vector<std::pair<std::function<void()>, ErrorCallback>> waiters;
        {
            brls::PlatformLockGuard lock(g_mixin_key_mutex);
            waiters.swap(g_nav_waiters);
            g_nav_fetching = false;
        }
        for (auto& w : waiters) {
            if (ok) {
                if (w.first) w.first();
            } else {
                if (w.second) w.second(msg, -412);
            }
        }
        if (!waiters.empty())
            brls::Logger::info("[wbi] nav waiters flushed: {} ({})", waiters.size(), ok ? "ok" : "fail");
    };
    // 首次 refresh 时同时打出 valid 分支的一行 info，确认诊断日志本身在跑。
    if (inflight == 1 && g_wbi_probe_logged == 0) {
        g_wbi_probe_logged = 1;
        brls::Logger::info("[wbi] probe ok (refresh/valid branch logging active)");
    }

    cpr::async([success, error, now, flushNavWaiters]() {
    auto session = HTTP::createSession();
    session->SetUrl(cpr::Url{parseLink(Api::Nav)});
#if defined(PLATFORM_XBOX360)
    const std::uint64_t t_submit = brls::xbox360perf::nowUsec();
#endif
    session->GetCallback([success, error, now, flushNavWaiters
#if defined(PLATFORM_XBOX360)
                          , session, t_submit
#endif
    ](const cpr::Response& r) {
#if defined(PLATFORM_XBOX360)
        // P0 §5.5-5：nav 走直连 session，此前只有 [nav] 总耗时，没有
        // dns/conn/tls 拆分，无法判断 5-21s 慢在握手还是排队。
        BILI_X360_NETLOG("GET", Api::Nav, session, r, t_submit);
#endif
        // 无论成败都必须归还 in-flight 计数（用 RAII 防止早退路径漏减）。
        struct InflightGuard {
            ~InflightGuard() { --g_nav_inflight; }
        } inflightGuard;

        if (r.status_code != 200) {
            /* 失败现场：status/ip/耗时/错误文本。ip 空 = 根本没连上，
               ip 与 api.bilibili.com 对不上 = DNS 并发覆盖。 */
            brls::Logger::error("[nav] status={} ip={} t={}ms code={} err={} inflight={}",
                                r.status_code, r.ip_address,
                                (int)(r.elapsed * 1000), (int)r.error.code, r.error.message,
                                (long)g_nav_inflight);
            ERROR_MSG("WBI签名获取失败", -412);
            flushNavWaiters(false, "WBI签名获取失败");
            return;
        }
        // P0 测量：WBI Nav 串行预热请求（缓存过期后首个 API 调用会等它），
        // 提升到 info 以便在正式日志中看到它何时发生、耗时多少。
        brls::Logger::info("[nav] ok ip={} t={}ms inflight={}", r.ip_address, (int)(r.elapsed * 1000),
                           (long)g_nav_inflight);
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
                flushNavWaiters(true, "");
                return;
            }
#if defined(__cpp_exceptions)
        } catch (...) {
        }
#else
        }
#endif
        ERROR_MSG("WBI签名失败", -412);
        flushNavWaiters(false, "WBI签名失败");
    });
    });
}

void encWbi(cpr::Parameters& params) {
    // 添加 wts 时间戳（§5.7-1：原 std::time(nullptr) 产生垃圾 wts）
    std::time_t wts = wbiNow();
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
