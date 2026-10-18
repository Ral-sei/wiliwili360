//
// Created by fang on 2023/4/1.
//

#include <borealis/core/application.hpp>
#include <borealis/views/label.hpp>

#include "view/video_profile.hpp"
#include "view/mpv_core.hpp"

VideoProfile::VideoProfile() {
    this->inflateFromXMLRes("xml/views/video_profile.xml");
    brls::Logger::debug("View VideoProfile: create");
}

void VideoProfile::update() {
    auto mpvCore = &MPVCore::instance();
    if (!mpvCore->isValid()) {
        constexpr const char *unavailable = "Unavailable";
        labelUrl->setText(unavailable);
        labelSize->setText(unavailable);
        labelFormat->setText(unavailable);
        labelCache->setText(unavailable);
        labelVideoRes->setText(unavailable);
        labelVideoCodec->setText(unavailable);
        labelVideoPixel->setText(unavailable);
        labelVideoHW->setText(unavailable);
        labelVideoBitrate->setText(unavailable);
        labelVideoDrop->setText(unavailable);
        labelVideoSync->setText(unavailable);
        labelAudioChannel->setText(unavailable);
        labelAudioCodec->setText(unavailable);
        labelAudioSampleRate->setText(unavailable);
        labelAudioBitrate->setText(unavailable);
        return;
    }

    // file
    if (mpvCore->filepath != labelUrl->getFullText()) labelUrl->setText(mpvCore->filepath);
    auto fileSize = mpvCore->getInt("file-size");
    labelSize->setText(fileSize > 0 ? fmt::format("{:.2f}MB", fileSize / 1048576.0) : "Unavailable");
    auto format = mpvCore->getString("file-format");
    labelFormat->setText(format.empty() ? "Unavailable" : format);
    auto cache = mpvCore->getNodeMap("demuxer-cache-state");
    if (cache.count("fw-bytes")) {
        labelCache->setText(fmt::format("{:.2f}MB ({:.1f} sec)", cache["fw-bytes"].u.int64 / 1048576.0,
                                        mpvCore->getDouble("demuxer-cache-duration")));
    } else {
        labelCache->setText("Unavailable");
    }
    brls::Logger::debug(
        "total-bytes: {:.2f}MB; cache-duration: "
        "{:.2f}; "
        "underrun: {}; fw-bytes: {:.2f}MB; bof-cached: "
        "{}; eof-cached: {}; file-cache-bytes: {}; "
        "raw-input-rate: {:.2f};",
        cache.count("total-bytes") ? cache["total-bytes"].u.int64 / 1048576.0 : 0.0,
        cache.count("cache-duration") ? cache["cache-duration"].u.double_ : 0.0,
        cache.count("underrun") ? cache["underrun"].u.flag : 0,
        cache.count("fw-bytes") ? cache["fw-bytes"].u.int64 / 1048576.0 : 0.0,
        cache.count("bof-cached") ? cache["bof-cached"].u.flag : 0,
        cache.count("eof-cached") ? cache["eof-cached"].u.flag : 0,
        cache.count("file-cache-bytes") ? cache["file-cache-bytes"].u.int64 / 1048576.0 : 0.0,
        cache.count("raw-input-rate") ? cache["raw-input-rate"].u.int64 / 1048576.0 : 0.0);

    // video
    auto videoWidth = mpvCore->getInt("video-params/w");
    auto videoHeight = mpvCore->getInt("video-params/h");
    auto fps = mpvCore->getDouble("container-fps");
    labelVideoRes->setText(videoWidth > 0 && videoHeight > 0
                               ? fmt::format("{} x {}@{:.2f} (window: {} x {} framebuffer: {} x {})",
                                             videoWidth, videoHeight, fps, brls::Application::contentWidth,
                                             brls::Application::contentHeight, brls::Application::windowWidth,
                                             brls::Application::windowHeight)
                               : "Unavailable");
    auto videoCodec = mpvCore->getString("video-codec");
    labelVideoCodec->setText(videoCodec.empty() ? "Unavailable" : videoCodec);
    auto pixelFormat = mpvCore->getString("video-params/pixelformat");
    labelVideoPixel->setText(pixelFormat.empty() ? "Unavailable" : pixelFormat);
    auto hwdec = mpvCore->getString("hwdec-current");
    labelVideoHW->setText(hwdec.empty() ? "no" : hwdec);
    auto videoBitrate = mpvCore->getInt("video-bitrate");
    labelVideoBitrate->setText(videoBitrate > 0 ? std::to_string(videoBitrate / 1024) + "kbps" : "Unavailable");
    labelVideoDrop->setText(fmt::format("{} (decoder) {} (output)",
                                        mpvCore->getInt("decoder-frame-drop-count"),
                                        mpvCore->getInt("frame-drop-count")));
    labelVideoSync->setText(fmt::format("{:.5f}", mpvCore->getDouble("avsync")));

    // audio
    // "Unavailable" used to hide two very different situations: a stream with
    // no audio track at all, and a property mpv has not reported yet.  On a
    // player whose audio output is missing entirely both looked identical, so
    // surface the AO name and the track presence separately.
    auto currentAO = mpvCore->getString("current-ao");
    // Xbox builds read resources/ from disk (-fno-exceptions), so a console
    // still running an older resource pack would terminate() on the missing
    // row.  Resolve this one label optionally: absent row just means no text.
    if (auto *outputLabel = labelAudioOutput.getViewIfPresent())
        outputLabel->setText(currentAO.empty() ? "-" : currentAO);

    // current-tracks/audio is unavailable (empty map) whenever no audio track
    // is selected, which is different from a selected track not being decoded yet.
    if (mpvCore->getNodeMap("current-tracks/audio").empty()) {
        labelAudioCodec->setText("No audio track");
        labelAudioChannel->setText("-");
        labelAudioSampleRate->setText("-");
        labelAudioBitrate->setText("-");
    } else {
        auto audioCodec = mpvCore->getString("audio-codec");
        labelAudioCodec->setText(audioCodec.empty() ? "Unavailable" : audioCodec);
        auto audioChannels = mpvCore->getString("audio-params/channel-count");
        labelAudioChannel->setText(audioChannels.empty() ? "Unavailable" : audioChannels);
        auto sampleRate = mpvCore->getInt("audio-params/samplerate");
        labelAudioSampleRate->setText(sampleRate > 0 ? std::to_string(sampleRate / 1000) + "kHz" : "Unavailable");
        auto audioBitrate = mpvCore->getInt("audio-bitrate");
        labelAudioBitrate->setText(audioBitrate > 0 ? std::to_string(audioBitrate / 1024) + "kbps" : "Unavailable");
    }
}

void VideoProfile::draw(NVGcontext *vg, float x, float y, float width, float height, brls::Style style,
                        brls::FrameContext *ctx) {
    static brls::Time time = 0;
    brls::Time now         = brls::getCPUTimeUsec();
    if ((now - time) > 1000000) {
        this->update();
        time = now;
    }

    Box::draw(vg, x, y, width, height, style, ctx);
}

VideoProfile::~VideoProfile() { brls::Logger::debug("View VideoProfile: delete"); }

brls::View *VideoProfile::create() { return new VideoProfile(); }
