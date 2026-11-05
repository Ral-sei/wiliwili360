//
// Created by maye174 on 2023/4/6.
//

#include "live/danmaku_live.hpp"
#include "bilibili.h"
#include "bilibili/api.h"
#include "bilibili/result/live_danmaku_result.h"
#include "bilibili/util/http.hpp"
#include "live/ws_utils.hpp"
#include "utils/config_helper.hpp"

#include <cstddef>
#include <ctime>
#include <string>

#if defined(PLATFORM_XBOX360)
#include <xtl.h>
// 亲和性/优先级复核探针（round-8，§15.5）；xboxkrnl 导出，OXDK/XDK 头无声明。
// 声明必须在文件作用域：lambda 体内不允许出现 linkage-specification。
extern "C" void DbgPrint(const char*, ...);
#endif

namespace bilibili {
void BilibiliClient::get_live_danmaku_info(int roomid, const std::function<void(LiveDanmakuinfo)> &callback,
                                           const ErrorCallback &error) {
    HTTP::getResultWithWbiAsync<LiveDanmakuinfo>(Api::LiveDanmakuInfo,
                                                 {{"type", "0"}, {"id", std::to_string(roomid)}},
                                                 callback,
                                                 error);
}
}  // namespace bilibili

static void mongoose_event_handler(struct mg_connection *nc, int ev, void *ev_data);

// 心跳定时器回调函数
// mg_timer_poll() 在 mg_mgr_poll() 内部执行，回调本身就在 mongoose 线程，
// 因此可以直接发送，无需再经 mg_wakeup 绕一圈。
static void heartbeat_timer(void *param) {
    auto liveDanmaku = static_cast<LiveDanmaku *>(param);
    if (liveDanmaku->is_connected() && liveDanmaku->is_evOK()) {
        liveDanmaku->send_heartbeat();
    }
}

// 添加消息到队列的辅助函数
void add_message(LiveDanmaku* live, std::string&& msg) {
    if (live) {
        live->msg_queue.push(std::move(msg));
    }
}

// MessageQueue实现
void LiveDanmaku::MessageQueue::push(std::string&& msg) {
    std::lock_guard<std::mutex> lock(mutex);
    queue.emplace(std::move(msg));
    cv.notify_one();
}

bool LiveDanmaku::MessageQueue::pop(std::string& msg, bool wait, const std::atomic<bool>& running) {
    std::unique_lock<std::mutex> lock(mutex);
    
    if (wait) {
        cv.wait(lock, [this, &running] { 
            return !queue.empty() || !running.load(std::memory_order_acquire); 
        });
    }
    
    if (!running.load(std::memory_order_acquire) && queue.empty()) {
        return false;
    }
    
    if (!queue.empty()) {
        msg = std::move(queue.front());
        queue.pop();
        return true;
    }
    
    return false;
}

void LiveDanmaku::MessageQueue::clear() {
    std::lock_guard<std::mutex> lock(mutex);
    std::queue<std::string> empty;
    std::swap(queue, empty);
}

void LiveDanmaku::MessageQueue::notify_all() {
    cv.notify_all();
}

// LiveDanmaku实现
LiveDanmaku::LiveDanmaku() = default;

LiveDanmaku::~LiveDanmaku() {
    disconnect();
}

bool LiveDanmaku::is_connected() const {
    return connected.load(std::memory_order_acquire);
}

bool LiveDanmaku::is_evOK() const {
    return ms_ev_ok.load(std::memory_order_acquire);
}

void LiveDanmaku::open_connection() {
    if (mgr == nullptr || this->info.host_list.empty()) {
        brls::Logger::error("(LiveDanmaku) 没有可用的弹幕服务器");
        nc = nullptr;
        return;
    }

    // 建立WebSocket连接。轮换 host_list，避免固定使用某一个不可达节点。
    size_t count         = this->info.host_list.size();
    const auto& hostInfo = this->info.host_list[this->hostIndex % count];
    this->hostIndex      = (this->hostIndex + 1) % count;
    int port             = hostInfo.ws_port > 0 ? hostInfo.ws_port : 2244;
    std::string host     = "ws://" + hostInfo.host + ":" + std::to_string(port) + "/sub";

    brls::Logger::info("(LiveDanmaku) connecting {}", host);
    this->ms_ev_ok.store(false, std::memory_order_release);
    nc = mg_ws_connect(mgr, host.c_str(), mongoose_event_handler, this, nullptr);
    if (nc == nullptr) {
        brls::Logger::error("(LiveDanmaku) 无法创建WebSocket连接: {}", host);
    }
}

void LiveDanmaku::connect(int room_id, uint64_t uid, const bilibili::LiveDanmakuinfo &info) {
    if (is_connected()) {
        return;
    }
    
    // 初始化成员变量
    this->room_id = room_id;
    this->uid = uid;
    this->info = info;
    this->heartbeatTimerAdded = false;
    this->reconnectNeeded.store(false, std::memory_order_release);
    this->hostIndex = 0;

    // 在这里（调用线程）解析 buvid3，而不是在 mongoose 线程的 join 回调里。
    // getCookie() 会惰性写入 buvid3/DedeUserID，从网络线程修改共享的
    // std::map 会和 UI/HTTP 线程的读取并发，导致堆损坏。
    {
        Cookie cookies = ProgramConfig::instance().getCookie();
        auto buvidIt   = cookies.find("buvid3");
        this->buvid =
            buvidIt != cookies.end() ? buvidIt->second : ProgramConfig::instance().getUuID();
    }
    
    // 创建并配置Mongoose
    mgr = new mg_mgr;
    mg_log_set(MG_LL_NONE);
    mg_mgr_init(mgr);
    mg_wakeup_init(mgr);  // 初始化wakeup功能

    // 建立WebSocket连接
    this->open_connection();

    if (nc == nullptr) {
        brls::Logger::error("(LiveDanmaku) 无法创建WebSocket连接");
        mg_mgr_free(mgr);
        delete mgr;
        mgr = nullptr;
        return;
    }

    // 设置连接标志
    connected.store(true, std::memory_order_release);
    this->lastReconnectMs = mg_millis();

    // 启动Mongoose事件循环线程
    mongoose_thread = std::thread([this]() {
#if defined(PLATFORM_XBOX360)
        // 静态线程分区（对齐 Xbox 360 YouTube 客户端的"每线程钉硬件线程"与
        // XDK "后台 CPU 密集线程降级"指引）：网络轮询属于后台工作，钉到 hw5
        // （libmpv worker spread 表 {2,4,1,3} 不使用 hw5）并降到主线程之下，
        // 避免弹幕洪峰在 hw0 与出帧/上传逐帧抢时间片。
        XSetThreadProcessor(GetCurrentThread(), 5);
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#if X360_RUNTIME_PROBES
        DbgPrint("[x360-thread] danmaku/mongoose hw=5 prio=below");
#endif
#endif
        while (this->is_connected()) {
            if (mgr) {
                mg_mgr_poll(mgr, wait_time);

                // 直播服务器会定期断开长连接，重连后弹幕/在线人数才会继续更新。
                if (this->reconnectNeeded.load(std::memory_order_acquire) &&
                    this->is_connected() &&
                    mg_millis() - this->lastReconnectMs >= 5000) {
                    this->lastReconnectMs = mg_millis();
                    this->reconnectNeeded.store(false, std::memory_order_release);
                    brls::Logger::info("(LiveDanmaku) 连接已断开，正在重连");
                    this->open_connection();
                    if (this->nc == nullptr)
                        this->reconnectNeeded.store(true, std::memory_order_release);
                }
            } else {
                break;
            }
        }
        
        // 在线程结束前安全释放资源
        if (mgr) {
            mg_mgr_free(mgr);
            delete mgr;
            mgr = nullptr;
            nc = nullptr;
        }
    });

    // 启动消息处理线程
    task_thread = std::thread([this]() {
#if defined(PLATFORM_XBOX360)
        // 同上：消息解析属于后台工作，与 mongoose 一起留在 hw5。
        XSetThreadProcessor(GetCurrentThread(), 5);
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#if X360_RUNTIME_PROBES
        DbgPrint("[x360-thread] danmaku/task hw=5 prio=below");
#endif
#endif
        std::string msg;
        while (connected.load(std::memory_order_acquire)) {
            if (msg_queue.pop(msg, true, connected) && onMessage) {
                onMessage(msg);
            }
        }
        
        // 直接清空剩余消息
        msg_queue.clear();
    });

    brls::Logger::info("(LiveDanmaku) connect step finish");
}

void LiveDanmaku::disconnect() {
    if (!is_connected()) {
        return;
    }

    // 标记为断开连接
    connected.store(false, std::memory_order_release);

    // 通知消息队列退出等待
    msg_queue.notify_all();
    
    // 等待线程结束
    if (mongoose_thread.joinable()) {
        mongoose_thread.join();
    }

    if (task_thread.joinable()) {
        task_thread.join();
    }
    brls::Logger::info("(LiveDanmaku) close step finish");
}

void LiveDanmaku::set_wait_time(size_t time) { 
    wait_time = time; 
}

void LiveDanmaku::send_join_request(const int room_id, const uint64_t uid) {
    if (!is_connected() || !mgr || !nc) {
        return;
    }

    // 协议里的 buvid 字段对应 buvid3（不是 _uuid）；匿名用户也必须带一个
    // 有效的 buvid3，否则部分弹幕节点会直接关闭连接。
    // buvid 已在 connect() 里（调用线程）解析好，这里只读成员变量：
    // 本函数运行在 mongoose 线程，调用 getCookie() 会并发修改 cookie map。
    const std::string& buvid = this->buvid;

    json join_request = {{"uid", uid},
                         {"roomid", room_id},
                         {"protover", 2},
                         {"buvid", buvid},
                         {"platform", "web"},
                         {"type", 2},
                         {"key", this->info.token}};
    std::string join_request_str = join_request.dump();
    brls::Logger::info("(LiveDanmaku) join_request:{}", join_request_str);
    std::vector<uint8_t> packet = encode_packet(0, 7, join_request_str);

    // 本函数只会在 mongoose 线程（MG_EV_WS_OPEN）中调用，可以直接发送。
    // 之前绕 mg_wakeup + UDP 回环 socketpair 的路径在 Xbox 上多一个失败点：
    // wakeup 初始化失败时 join 包永远不会发出，服务器也就不会推送任何消息。
    mg_ws_send(nc, packet.data(), packet.size(), WEBSOCKET_OP_BINARY);
}

void LiveDanmaku::send_heartbeat() {
    if (!is_connected() || !mgr || !nc) {
        return;
    }

    brls::Logger::debug("(LiveDanmaku) send_heartbeat");
    std::vector<uint8_t> packet = encode_packet(0, 2, "");

    // 同上：心跳定时器在 mg_mgr_poll 内触发，属于 mongoose 线程。
    mg_ws_send(nc, packet.data(), packet.size(), WEBSOCKET_OP_BINARY);
}

void LiveDanmaku::send_text_message(const std::string &message) {
    // 暂时不用
}

void LiveDanmaku::setonMessage(MessageCallback callback) {
    onMessage = std::move(callback);
}

static void mongoose_event_handler(struct mg_connection *nc, int ev, void *ev_data) {
    auto *liveDanmaku = static_cast<LiveDanmaku *>(nc->fn_data);
    if (!liveDanmaku) return;
    
    if (ev == MG_EV_OPEN) {
        MG_DEBUG(("%p %s", nc->fd, (char *)ev_data));
#ifdef MONGOOSE_HEX_DUMPS
        nc->is_hexdumping = 1;
#else
        nc->is_hexdumping = 0;
#endif
    } else if (ev == MG_EV_ERROR) {
        MG_ERROR(("%p %s", nc->fd, (char *)ev_data));
        brls::Logger::error("(LiveDanmaku) WebSocket error: {}", (const char *)ev_data);
        liveDanmaku->ms_ev_ok.store(false, std::memory_order_release);
        if (liveDanmaku->is_connected())
            liveDanmaku->reconnectNeeded.store(true, std::memory_order_release);
    } else if (ev == MG_EV_WS_OPEN) {
        MG_DEBUG(("%p %s", nc->fd, (char *)ev_data));
        brls::Logger::info("(LiveDanmaku) WebSocket connected");
        liveDanmaku->ms_ev_ok.store(true, std::memory_order_release);
        liveDanmaku->send_join_request(liveDanmaku->room_id, liveDanmaku->uid);
        if (!liveDanmaku->heartbeatTimerAdded) {
            liveDanmaku->heartbeatTimerAdded = true;
            mg_timer_add(liveDanmaku->mgr, 30000, MG_TIMER_REPEAT, heartbeat_timer, nc->fn_data);
        }
    } else if (ev == MG_EV_WS_MSG) {
        MG_DEBUG(("%p %s", nc->fd, (char *)ev_data));
        struct mg_ws_message *wm = (struct mg_ws_message *)ev_data;
        add_message(liveDanmaku, std::string(wm->data.buf, wm->data.len));
    } else if (ev == MG_EV_CLOSE) {
        MG_DEBUG(("%p %s", nc->fd, (char *)ev_data));
        brls::Logger::info("(LiveDanmaku) WebSocket closed");
        liveDanmaku->ms_ev_ok.store(false, std::memory_order_release);
        // 允许事件循环线程在稍后重建连接；断开时 nc 已不可用。
        if (liveDanmaku->nc == nc) liveDanmaku->nc = nullptr;
        if (liveDanmaku->is_connected())
            liveDanmaku->reconnectNeeded.store(true, std::memory_order_release);
    } else if (ev == MG_EV_WAKEUP) {
        // 处理唤醒事件
        struct mg_str *data = (struct mg_str *)ev_data;
        if (data != nullptr && data->len >= sizeof(int)) {
            int *cmd = (int *)data->buf;
            if (*cmd == 1) {
                // 心跳命令
                liveDanmaku->send_heartbeat();
            } else {
                // 数据包指针
                std::string **packet_ptr = (std::string **)data->buf;
                if (*packet_ptr != nullptr) {
                    // 发送数据包
                    mg_ws_send(nc, (*packet_ptr)->data(), (*packet_ptr)->size(), WEBSOCKET_OP_BINARY);
                    // 释放内存
                    delete *packet_ptr;
                    *packet_ptr = nullptr;
                }
            }
        }
    }
}