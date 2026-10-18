//
// Created by fang on 2022/9/13.
//

#pragma once

#include "bilibili/result/home_live_result.h"
#include "bilibili/result/live_danmaku_result.h"
#include "presenter/presenter.h"

class LiveDataRequest : public Presenter {
public:
    virtual void onLiveData(const bilibili::LiveRoomPlayInfo& result) {}

    virtual void onError(const std::string& error) {}

    virtual void onNeedPay(const std::string& msg, const std::string& link, const std::string& startTime,
                           const std::string& endTime) {}

    virtual void onDanmakuInfo(int roomid, const bilibili::LiveDanmakuinfo& info) {}

    virtual void onAnchorInfo(const std::string& face, const std::string& uname) {}

    virtual void onAnchorTitleInfo(const std::string& title) {}

    void requestData(int roomid);

    void reportHistory(int roomid);

    void requestPayLiveInfo(int roomid);

    void requestLiveDanmakuToken(int roomid);

    void requestLiveAnchorInfo(int roomid);

    void requestLiveAnchorTitle(int roomid);

    std::string getQualityDescription(int qn);

#ifdef __PSV__
    // 720P
    static inline int defaultQuality = 150;
#elif defined(PLATFORM_XBOX360)
    // 超清 (720P): Xenon 软解上限为 720p, 1080p 流 (蓝光400/原画10000) 解码
    // 必然卡顿, 因此在请求直播地址时就限档 (与 PSV 同一模式)。
    // qn 档位: 80 流畅 / 150 高清 / 250 超清720P / 400 蓝光1080P / 10000 原画
    static inline int defaultQuality = 250;
#else
    static inline int defaultQuality = 0;
#endif
    bilibili::LiveRoomPlayInfo liveRoomPlayInfo{};
    bilibili::LiveStreamFormatCodec liveUrl{};
    std::unordered_map<int, std::string> qualityDescriptionMap{};
};