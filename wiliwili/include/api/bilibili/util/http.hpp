//
// Created by fang on 2022/5/1.
//

#pragma once

#include <nlohmann/json.hpp>
#include <cpr/cpr.h>

#include "bilibili/util/md5.hpp"
#include "bilibili/util/json.hpp"
#include "bilibili/util/wbi.hpp"
#include "utils/number_helper.hpp"
#include "utils/string_helper.hpp"
#include <pystring.h>

#if defined(PLATFORM_XBOX360)
#include <borealis/core/logger.hpp>
#include <borealis/core/thread.hpp>
#include <borealis/platforms/xbox360/xbox360_perf.hpp>
#endif

namespace bilibili {

using Cookies = std::map<std::string, std::string>;

const std::string BILIBILI_APP_KEY    = "aa1e74ee4874176e";
const std::string BILIBILI_APP_SECRET = "54e6a9a31b911cd5fc0daa66ebf94bc4";
const std::string BILIBILI_BUILD      = "1001011000";

using ErrorCallback = std::function<void(const std::string&, int code)>;
#define ERROR_MSG(msg, ...) \
    if (error) error(msg, __VA_ARGS__)
#define HTTP_CALLBACK(data) \
    if (callback) callback(data)

#if defined(PLATFORM_XBOX360)
/**
 * P0 §5.5-5 埋点：给绕过 _cpr_get/_cpr_post 的直连 session 请求补 [x360-net] 行。
 * 用法：调用 GetCallback 前取 t_submit，回调第一行展开本宏。
 *   const std::uint64_t t_submit = brls::xbox360perf::nowUsec();
 *   session->GetCallback([... , t_submit](const cpr::Response& r) {
 *       BILI_X360_NETLOG("GET", <url>, session, r, t_submit);
 *       ...
 * 这类请求没有外层闸门（q1 恒为 0），q2 = 提交→回调进入 − curl total，
 * 实际包含内层闸门排队 + 回调调度。
 */
#define BILI_X360_NETLOG(method, url, session, resp, t_submit)                                    \
    bilibili::HTTP::RequestLog x360_reqlog(method, url, session, &(resp), t_submit,               \
                                           brls::xbox360perf::nowUsec(), 0.0,                      \
                                           brls::xbox360perf::msSince(t_submit))
#else
#define BILI_X360_NETLOG(method, url, session, resp, t_submit)
#endif

class CurlSharedObject {
public:
    CurlSharedObject() {
        share = curl_share_init();
        curl_share_setopt(share, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
        // TODO: 下列两个选线不支持多线程，需要实现自定义线程池，每个线程共享一个 curl share 对象
        // curl_share_setopt(share, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
        // curl_share_setopt(share, CURLSHOPT_SHARE, CURL_LOCK_DATA_CONNECT);
        curl_share_setopt(share, CURLSHOPT_LOCKFUNC, lock_callback);
        curl_share_setopt(share, CURLSHOPT_UNLOCKFUNC, unlock_callback);
        curl_share_setopt(share, CURLSHOPT_USERDATA, lock_array);
    }
    ~CurlSharedObject() {
        curl_share_cleanup(share);
    }

    static void lock_callback(CURL *handle, curl_lock_data data, curl_lock_access access, void *userptr) {
        auto *lock_array = (std::recursive_mutex *)userptr;
        lock_array[data].lock();
    }

    static void unlock_callback(CURL *handle, curl_lock_data data, void *userptr) {
        auto *lock_array = (std::recursive_mutex *)userptr;
        lock_array[data].unlock();
    }

    CURLSH* getShare() {
        return share;
    }

private:
    CURLSH* share;
    std::recursive_mutex lock_array[CURL_LOCK_DATA_LAST];
};

class HTTP {
public:
    static inline cpr::Cookies COOKIES = {false};
    static inline cpr::Header HEADERS  = {
        {"User-Agent", "wiliwili"},
        {"Referer", "https://www.bilibili.com/client"},
        {"Origin", "https://www.bilibili.com"},
#if defined(PLATFORM_XBOX360)
        // Xbox curl occasionally waits for keep-alive EOF when Bilibili omits
        // a content length. Force a close-delimited response for API calls.
        {"Connection", "close"},
#endif
    };
    static inline int TIMEOUT = 10000;
    static inline int CONNECTION_TIMEOUT = 0;
    static inline int DNS_CACHE_TIMEOUT = 60;
    static inline cpr::Proxies PROXIES;
    static inline cpr::VerifySsl VERIFY;
    static inline std::string PROTOCOL = "https:";
    static inline CurlSharedObject CURL_SHARE;

    static std::string getEncodedCookie(const cpr::Cookies& cookies);

    static std::shared_ptr<cpr::Session> createSession() {
        auto session = std::make_shared<cpr::Session>();
        CURL* curl = session->GetCurlHolder()->handle;
#if !defined(PLATFORM_XBOX360)
        curl_easy_setopt(curl, CURLOPT_SHARE, HTTP::CURL_SHARE.getShare());
#endif
        curl_easy_setopt(curl, CURLOPT_DNS_CACHE_TIMEOUT, HTTP::DNS_CACHE_TIMEOUT);
        session->SetTimeout(cpr::Timeout{bilibili::HTTP::TIMEOUT});
#if defined(PLATFORM_XBOX360)
        // A zero value is treated as an immediate timeout by the Xbox curl
        // fork. Keep the connection phase bounded without allowing 0 ms.
        const int connectionTimeout = HTTP::CONNECTION_TIMEOUT > 0 ? HTTP::CONNECTION_TIMEOUT : 10000;
        session->SetConnectTimeout(cpr::ConnectTimeout{connectionTimeout});
#else
        session->SetConnectTimeout(cpr::ConnectTimeout{bilibili::HTTP::CONNECTION_TIMEOUT});
#endif
        session->SetHeader(bilibili::HTTP::HEADERS);
#if defined(PLATFORM_XBOX360)
        // The Xbox curl fork has incomplete HTTP/1.1 end-of-body handling for
        // chunked keep-alive responses. HTTP/1.0 makes the server delimit the
        // body with connection close, which the transport handles reliably.
        curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_0);
        // 该请求头只是让服务端不主动压缩 JSON 接口。注意：本 fork 未编译 zlib
        // （没有 Curl_unencode_* 符号），即使响应带 Content-Encoding 也不会在 curl
        // 内解压；而 /x/v1/dm/list.so 等老接口无视该请求头恒返回 deflate。这类响应
        // 统一交给 HTTP::decodeText() 在应用层解压。
        curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "identity");
#endif
        session->SetProxies(bilibili::HTTP::PROXIES);
        session->SetVerifySsl(bilibili::HTTP::VERIFY);
        return session;
    }

    /**
     * 返回按 Content-Encoding 解压后的响应正文。
     * Xbox 的 curl fork 未编译 zlib，压缩响应会原样透传；而 bilibili 的
     * /x/v1/dm/list.so 恒返回 deflate（无视 Accept-Encoding: identity），
     * 直接交给 XML/JSON 解析必然失败（tinyxml2 报 XML_ERROR_PARSING_*）。
     * 无该响应头、正文已是明文或解压失败时原样返回，其他平台行为不变。
     */
    static std::string decodeText(const cpr::Response& response) {
        auto it = response.header.find("Content-Encoding");
        if (it == response.header.end()) return response.text;
        return wiliwili::inflateContent(response.text, it->second);
    }

#if defined(PLATFORM_XBOX360)
    /**
     * P0 测量埋点（每请求一行 [x360-net]）：
     *   q1   提交到 cpr::async → 外层工作线程开始执行（建线程 + 闸门排队）
     *   q2   GetCallback 提交 → 回调进入，减去 curl 的 total（内层建线程 + 闸门排队）
     *   dns/conn/tls/ttfb/total  curl 自身阶段计时（conn 含 dns，tls 含 conn）
     *   cb   回调内处理耗时（decode + JSON 解析 + 业务回调）
     *   wall 提交 → 本行输出
     * 用 RAII 析构打点，回调里提前 return 也不会漏。
     */
    struct RequestLog {
        const char* method;
        std::string url;
        std::shared_ptr<cpr::Session> session;
        const cpr::Response* response;
        std::uint64_t t_submit;
        std::uint64_t t_entry;
        double queue_ms;
        double inner_ms;

        // P0 §5.5-4 测量：cb 阶段拆分。
        //   dec  decodeText() 解压耗时（_cpr_get/_cpr_post 在解压后立即赋值）
        //   biz  = cb - dec（含 JSON 解析 + 业务回调）
        // JSON 解析耗时单独由 parseJson 打 [x360-parse] 行（跨函数传状态会引入
        // 线程安全问题，thread_local 在 XDK 上不可靠，故拆成两行日志）。
        double dec = 0;

        RequestLog(const char* method, const std::string& url, std::shared_ptr<cpr::Session> session,
                   const cpr::Response* response, std::uint64_t t_submit, std::uint64_t t_entry, double queue_ms,
                   double inner_ms)
            : method(method), url(url), session(std::move(session)), response(response), t_submit(t_submit),
              t_entry(t_entry), queue_ms(queue_ms), inner_ms(inner_ms) {
            // §5.6-4：回调期间打上接口标签，本回调里 brls::sync() 入队的主线程任务
            // 会在 [x360-perf] slow sync task 里带上它（如 "playurl" / "reply" / "*.jpg"）。
            // 派生：去掉 query，取最后一段路径。
            std::string tag = url.substr(0, url.find('?'));
            size_t slash    = tag.find_last_of('/');
            if (slash != std::string::npos) tag = tag.substr(slash + 1);
            if (tag.size() > 40) tag = tag.substr(tag.size() - 40);
            brls::Threading::setSyncEnqueueTag(tag.c_str());
        }

        ~RequestLog() {
            brls::Threading::setSyncEnqueueTag("");  // 回调结束，清标签
            double dns = 0, conn = 0, tls = 0, ttfb = 0, total = 0;
            if (session) {
                CURL* handle = session->GetCurlHolder()->handle;
                curl_easy_getinfo(handle, CURLINFO_NAMELOOKUP_TIME, &dns);
                curl_easy_getinfo(handle, CURLINFO_CONNECT_TIME, &conn);
                curl_easy_getinfo(handle, CURLINFO_APPCONNECT_TIME, &tls);
                curl_easy_getinfo(handle, CURLINFO_STARTTRANSFER_TIME, &ttfb);
                curl_easy_getinfo(handle, CURLINFO_TOTAL_TIME, &total);
            }
            double q2 = inner_ms - total * 1000.0;
            if (q2 < 0) q2 = 0;
            const double cb   = brls::xbox360perf::msSince(t_entry);
            const double wall = brls::xbox360perf::msSince(t_submit);
            double biz        = cb - dec;
            if (biz < 0) biz = 0;
            brls::Logger::info(
                "[x360-net] {} {} | q1 {:.1f} q2 {:.1f} dns {:.1f} conn {:.1f} tls {:.1f} ttfb {:.1f} total {:.1f} cb {:.1f} dec {:.1f} biz {:.1f} wall {:.1f} ms code {} err {}",
                method, parseLink(url), queue_ms, q2, dns * 1000.0, conn * 1000.0, tls * 1000.0, ttfb * 1000.0,
                total * 1000.0, cb, dec, biz, wall, response ? response->status_code : 0L,
                response ? (int)response->error.code : 0);
        }
    };
#endif

    static void _cpr_post(const std::string& url, const cpr::Parameters& parameters = {},
                           const cpr::Payload& payload                               = {},
                           const std::function<void(const cpr::Response&)>& callback = nullptr,
                           const ErrorCallback& error                                = nullptr) {
#if defined(PLATFORM_XBOX360)
        const std::uint64_t t_submit = brls::xbox360perf::nowUsec();
        cpr::async([url, parameters, payload, callback, error, t_submit]() {
        const double queue_ms = brls::xbox360perf::msSince(t_submit);
        auto session = createSession();
        session->SetUrl(cpr::Url{parseLink(url)});
        session->SetParameters(parameters);
        session->SetPayload(payload);

        const std::uint64_t t_dispatch = brls::xbox360perf::nowUsec();
        session->PostCallback(
            [session, url, t_submit, t_dispatch, queue_ms, callback, error](const cpr::Response& raw) {
                RequestLog reqlog("POST", url, session, &raw, t_submit, brls::xbox360perf::nowUsec(), queue_ms,
                                  brls::xbox360perf::msSince(t_dispatch));
                cpr::Response r = raw;
                {
                    const std::uint64_t t_dec = brls::xbox360perf::nowUsec();
                    r.text                    = HTTP::decodeText(raw);
                    reqlog.dec                = brls::xbox360perf::msSince(t_dec);
                }
                if (r.error) {
                    ERROR_MSG(r.error.message, -1);
                    return;
                } else if (r.status_code != 200) {
                    ERROR_MSG("Network error. [Status code: " + std::to_string(r.status_code) + " ]", r.status_code);
                    return;
                }
                if (callback) callback(r);
            });
        });
#else
        auto session = createSession();
        session->SetUrl(cpr::Url{parseLink(url)});
        session->SetParameters(parameters);
        session->SetPayload(payload);
        session->PostCallback(
            [callback, error](const cpr::Response& r) {
                if (r.error) { ERROR_MSG(r.error.message, -1); return; }
                if (r.status_code != 200) { ERROR_MSG("Network error. [Status code: " + std::to_string(r.status_code) + " ]", r.status_code); return; }
                if (callback) callback(r);
            });
#endif
    }

    static void _cpr_get(const std::string& url, const cpr::Parameters& parameters = {},
                          const std::function<void(const cpr::Response&)>& callback = nullptr,
                          const ErrorCallback& error                                = nullptr) {
#if defined(PLATFORM_XBOX360)
        const std::uint64_t t_submit = brls::xbox360perf::nowUsec();
        cpr::async([url, parameters, callback, error, t_submit]() {
        const double queue_ms = brls::xbox360perf::msSince(t_submit);
        auto session = createSession();
        session->SetUrl(cpr::Url{parseLink(url)});
        session->SetParameters(parameters);

        const std::uint64_t t_dispatch = brls::xbox360perf::nowUsec();
        session->GetCallback(
            [session, url, t_submit, t_dispatch, queue_ms, callback, error](const cpr::Response& raw) {
                RequestLog reqlog("GET", url, session, &raw, t_submit, brls::xbox360perf::nowUsec(), queue_ms,
                                  brls::xbox360perf::msSince(t_dispatch));
                cpr::Response r = raw;
                {
                    const std::uint64_t t_dec = brls::xbox360perf::nowUsec();
                    r.text                    = HTTP::decodeText(raw);
                    reqlog.dec                = brls::xbox360perf::msSince(t_dec);
                }
                if (r.error) {
                    ERROR_MSG(r.error.message, -1);
                    return;
                }
                if (r.status_code != 200) {
                    ERROR_MSG("Network error. [Status code: " + std::to_string(r.status_code) + " ]", r.status_code);
                    return;
                }
                if (callback) callback(r);
            });
        });
#else
        auto session = createSession();
        session->SetUrl(cpr::Url{parseLink(url)});
        session->SetParameters(parameters);
        session->GetCallback(
            [callback, error](const cpr::Response& r) {
                if (r.error) { ERROR_MSG(r.error.message, -1); return; }
                if (r.status_code != 200) { ERROR_MSG("Network error. [Status code: " + std::to_string(r.status_code) + " ]", r.status_code); return; }
                if (callback) callback(r);
            });
#endif
    }

    template <typename ReturnType>
    static int parseJson(const cpr::Response& r, const std::function<void(ReturnType)>& callback = nullptr,
                          const ErrorCallback& error = nullptr) {
#if !defined(PLATFORM_XBOX360)
        try {
#endif
#if defined(PLATFORM_XBOX360)
            // P0 §5.5-4 测量：JSON 解析+结构转换耗时与 body 大小。
            // 与 [x360-net] cb 拆行输出（避免跨函数状态的线程安全问题），
            // json >= 100ms 或 body >= 64KB 才打，用于判断 cb 大头是解析还是业务回调。
            const std::uint64_t t_json = brls::xbox360perf::nowUsec();
            nlohmann::json res = nlohmann::json::parse(r.text, nullptr, false);
            const double jsonMs  = brls::xbox360perf::msSince(t_json);
            if (jsonMs >= 100.0 || r.text.size() >= 65536) {
                brls::Logger::info("[x360-parse] {} json {:.1f} ms body {}", parseLink(r.url.str()), jsonMs,
                                   r.text.size());
            }
            if (res.is_discarded() || !res.is_object() || !res.contains("code") || !res.at("code").is_number_integer()) {
                ERROR_MSG("Api error: invalid JSON response", -1);
                return 1;
            }
#else
            nlohmann::json res = nlohmann::json::parse(r.text);
#endif
            int code           = res.at("code").get<int>();
            if (code == 0) {
                if (res.contains("data") && (res.at("data").is_object() || res.at("data").is_array())) {
#if defined(PLATFORM_XBOX360)
                    // 结构转换（get<ReturnType>()）单列：大列表时它可能比 parse 更贵
                    const std::uint64_t t_conv = brls::xbox360perf::nowUsec();
                    ReturnType data_val        = res.at("data").get<ReturnType>();
                    const double convMs        = brls::xbox360perf::msSince(t_conv);
                    if (convMs >= 100.0)
                        brls::Logger::info("[x360-parse] {} conv {:.1f} ms", parseLink(r.url.str()), convMs);
                    HTTP_CALLBACK(std::move(data_val));
#else
                    HTTP_CALLBACK(res.at("data").get<ReturnType>());
#endif
                    return 0;
                } else if (res.contains("result") && res.at("result").is_object()) {
#if defined(PLATFORM_XBOX360)
                    const std::uint64_t t_conv = brls::xbox360perf::nowUsec();
                    ReturnType data_val        = res.at("result").get<ReturnType>();
                    const double convMs        = brls::xbox360perf::msSince(t_conv);
                    if (convMs >= 100.0)
                        brls::Logger::info("[x360-parse] {} conv {:.1f} ms", parseLink(r.url.str()), convMs);
                    HTTP_CALLBACK(std::move(data_val));
#else
                    HTTP_CALLBACK(res.at("result").get<ReturnType>());
#endif
                    return 0;
                } else {
                    printf("data: %s\n", r.text.c_str());
                    ERROR_MSG("Cannot find data", -1);
                }
            } else if (res.contains("message") && res.at("message").is_string()) {
                ERROR_MSG(res.at("message").get<std::string>(), code);
            } else {
                ERROR_MSG("Param error", -1);
            }
#if !defined(PLATFORM_XBOX360)
        } catch (const std::exception& e) {
            ERROR_MSG("Api error. \n" + std::string{e.what()}, 200);
            printf("data: %s\n", r.text.c_str());
            printf("ERROR: %s\n", e.what());
        }
#endif
        return 1;
    }

    static void signParameters(cpr::Parameters& parameters);

    template <typename ReturnType>
    static void getResultAsync(const std::string& url,
                               cpr::Parameters parameters                      = {},
                               const std::function<void(ReturnType)>& callback = nullptr,
                               const ErrorCallback& error                      = nullptr,
                               bool needSign                                   = false) {
        if (needSign) {
            signParameters(parameters);
        }
        _cpr_get(
            url,
            parameters,
            [callback, error](const cpr::Response& r) {
                parseJson<ReturnType>(r, callback, error);
            },
            error);
    }

    template <typename ReturnType>
    static void getResultWithWbiAsync(const std::string& url,
                                      cpr::Parameters parameters                      = {},
                                      const std::function<void(ReturnType)>& callback = nullptr,
                                      const ErrorCallback& error                      = nullptr,
                                      bool needSign                                   = false) {
        wbi::updateWbiKeys([url, parameters, callback, error, needSign]() mutable {
            if (needSign) {
                signParameters(parameters);
            }
            wbi::encWbi(parameters);
            _cpr_get(
                url,
                parameters,
                [callback, error](const cpr::Response& r) {
                    parseJson<ReturnType>(r, callback, error);
                },
                error);
        }, error);
    }

    template <typename ReturnType>
    static void postResultAsync(const std::string& url,
                                cpr::Parameters parameters                      = {},
                                const cpr::Payload& payload                     = {},
                                const std::function<void(ReturnType)>& callback = nullptr,
                                const ErrorCallback& error                      = nullptr,
                                bool needSign                                   = false) {
        if (needSign) {
            signParameters(parameters);
        }
        _cpr_post(
            url, parameters, payload,
            [callback, error](const cpr::Response& r) {
#if !defined(PLATFORM_XBOX360)
                try {
#endif
                    nlohmann::json res = nlohmann::json::parse(r.text);
                    const int code     = res.at("code").get<int>();
                    if (code == 0) {
                        if (res.contains("data") && res.at("data").is_object()) {
                            HTTP_CALLBACK(res.at("data").get<ReturnType>());
                        } else if (res.contains("result") && res.at("result").is_object()) {
                            HTTP_CALLBACK(res.at("result").get<ReturnType>());
                        } else {
                            printf("data: %s\n", r.text.c_str());
                            ERROR_MSG("Cannot find data", -1);
                        }
                        return;
                    }
                    ERROR_MSG(res.at("message").get<std::string>(), code);
#if !defined(PLATFORM_XBOX360)
                } catch (const std::exception& e) {
                    ERROR_MSG(std::string(e.what()), r.status_code);
                    printf("data: %s\n", r.text.c_str());
                    printf("ERROR: %s\n", e.what());
                }
#endif
            },
            error);
    }

    static void postResultAsync(const std::string& url,
                                const cpr::Parameters& parameters     = {},
                                const cpr::Payload& payload           = {},
                                const std::function<void()>& callback = nullptr,
                                const ErrorCallback& error            = nullptr) {
        _cpr_post(
            url, parameters, payload,
            [callback, error](const cpr::Response& r) {
#if !defined(PLATFORM_XBOX360)
                try {
#endif
                    nlohmann::json res = nlohmann::json::parse(r.text);
                    const int code     = res.at("code").get<int>();
                    if (code == 0) {
                        if (callback) callback();
                        return;
                    }
                    ERROR_MSG(res.at("message").get<std::string>(), code);
#if !defined(PLATFORM_XBOX360)
                } catch (const std::exception& e) {
                    ERROR_MSG(std::string(e.what()), r.status_code);
                    printf("data: %s\n", r.text.c_str());
                    printf("ERROR: %s\n", e.what());
                }
#endif
            },
            error);
    }
};

}  // namespace bilibili
