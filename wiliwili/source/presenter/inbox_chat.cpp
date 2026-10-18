#include <borealis/core/thread.hpp>

#include "bilibili.h"
#include "presenter/inbox_chat.hpp"
#include "utils/number_helper.hpp"
#include "utils/config_helper.hpp"

void InboxChatRequest::onChatList(const bilibili::InboxChatListResult& result, bool refresh) {}

void InboxChatRequest::onError(const std::string& error) {}

/*
 * 线程约定（务必保持）：
 * HTTP 回调运行在 cpr 的工作线程上，此时 InboxView 随时可能已被用户按返回销毁
 * （进入私信页的请求、以及聊天页关闭时触发的刷新都可能还在网络往返中）。
 * 同一个页面的多个请求也可能并发，多个工作线程会同时碰 user_map。
 * 因此回调里只允许把结果搬进 brls::sync，任何成员访问都必须放在
 * ASYNC_RELEASE 判定存活之后，否则会在已释放/正在 rehash 的 user_map 上查找。
 */
void InboxChatRequest::requestData(bool refresh) {
    ASYNC_RETAIN
    BILI::new_inbox_sessions(
        refresh ? 0 : this->last_time,
        [ASYNC_TOKEN, refresh](const bilibili::InboxChatResultWrapper& result) {
            brls::sync([ASYNC_TOKEN, result, refresh]() {
                if (*token) {
                    // 对象已销毁：归还引用（release 为 true，宏内直接 return）
                    ASYNC_RELEASE
                }

                std::vector<std::string> uids;
                for (auto& s : result.session_list) {
                    if (s.account_info.name.empty()) {
                        auto it = user_map.find(s.talker_id);
                        if (it == user_map.end()) {
                            uids.push_back(std::to_string(s.talker_id));
                        }
                    }
                }
                this->last_time = wiliwili::unix_time() * 1000000;

                // 不需要请求头像
                if (uids.empty()) {
                    auto list = result.session_list;
                    for (auto& s : list) {
                        auto it = user_map.find(s.talker_id);
                        if (it != user_map.end()) {
                            s.account_info.name    = it->second.name;
                            s.account_info.pic_url = it->second.face;
                        }
                    }
                    this->onChatList(list, refresh);
                    ASYNC_RELEASE // 本次请求的唯一一次归还
                    return;
                }

                /*
                 * 还要再请求一次用户卡片：这里不归还引用，把唯一一次
                 * ASYNC_RELEASE 留给嵌套回调，token 在嵌套请求期间始终存活，
                 * 嵌套请求结束（成功或失败）时才归还。
                 */
                BILI::get_user_cards(
                    uids,
                    [ASYNC_TOKEN, result, refresh](const bilibili::UserCardListResult& users) {
                        brls::sync([ASYNC_TOKEN, users, result, refresh]() {
                            ASYNC_RELEASE
                            for (auto& u : users) user_map[u.mid] = u;

                            std::vector<bilibili::InboxChatResult> list;
                            std::copy_if(result.session_list.begin(), result.session_list.end(),
                                         std::back_inserter(list),
                                         [](auto i){ return i.session_type == 1;});
                            for (auto& s : list) {
                                auto it = user_map.find(s.talker_id);
                                if (it != user_map.end()) {
                                    s.account_info.name    = it->second.name;
                                    s.account_info.pic_url = it->second.face;
                                }
                            }
                            this->onChatList(list, refresh);
                        });
                    },
                    [ASYNC_TOKEN](BILI_ERR) {
                        brls::sync([ASYNC_TOKEN, error]() {
                            ASYNC_RELEASE
                            this->onError(error);
                        });
                    });
            });
        },
        [ASYNC_TOKEN](BILI_ERR) {
            brls::sync([ASYNC_TOKEN, error]() {
                ASYNC_RELEASE
                this->onError(error);
            });
        });
}
