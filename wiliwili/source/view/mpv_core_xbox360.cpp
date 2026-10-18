// NOT COMPILED. Kept only as the M3-era fallback if the libmpv route (M5) is
// ever rolled back — do not read this file as the current Xbox behaviour.
//
// This was the "libmpv not wired up yet" MPVCore: a header-shaped stub that
// answers isValid() == false and swallows every playback call. Since M5, the
// real wiliwili/source/view/mpv_core.cpp is what runs on Xbox (it drives libmpv
// through the MPV_SW_RENDER software renderer), and this file would define the
// exact same MPVCore members — so building both is a duplicate symbol.
// CMakeLists.txt therefore list(REMOVE_ITEM)s it out of MAIN_SRC on Xbox. If
// you re-enable it, you must remove the real mpv_core.cpp from the Xbox build
// in the same change, or the link fails.
//
// Historical note: because it is excluded from the build, the
// #if defined(PLATFORM_XBOX360) guard below is inert — it exists only so the
// file still parses if someone compiles it by hand on another platform.

#include "view/mpv_core.hpp"

#if defined(PLATFORM_XBOX360)

MPVCore::MPVCore() {
    volume = VIDEO_VOLUME;
    video_speed = 1.0;
}

MPVCore::~MPVCore() = default;

bool MPVCore::isStopped() const { return video_stopped; }
bool MPVCore::isPlaying() const { return video_playing; }
bool MPVCore::isPaused() const { return video_paused; }
double MPVCore::getSpeed() const { return video_speed; }
double MPVCore::getPlaybackTime() const { return playback_time; }
std::string MPVCore::getCacheSpeed() const { return std::to_string(cache_speed); }
int64_t MPVCore::getVolume() const { return volume; }
bool MPVCore::isValid() { return false; }
std::string MPVCore::getString(const std::string&) { return {}; }
double MPVCore::getDouble(const std::string&) { return 0.0; }
int64_t MPVCore::getInt(const std::string&) { return 0; }
std::unordered_map<std::string, mpv_node> MPVCore::getNodeMap(const std::string&) { return {}; }

void MPVCore::setUrl(const std::string& url, const std::string&, const std::string&) {
    filepath = url;
    video_stopped = false;
    video_playing = false;
}
void MPVCore::setBackupUrl(const std::string&, const std::string&) {}
void MPVCore::setVolume(int64_t value) { volume = value; }
void MPVCore::setVolume(const std::string&) {}
void MPVCore::resume() { video_paused = false; video_playing = true; video_stopped = false; }
void MPVCore::pause() { video_paused = true; video_playing = false; }
void MPVCore::stop() { video_stopped = true; video_playing = false; }
void MPVCore::seek(int64_t p) { playback_time = static_cast<double>(p); }
void MPVCore::seek(const std::string&) {}
void MPVCore::seekRelative(int64_t p) { playback_time += static_cast<double>(p); }
void MPVCore::seekPercent(double p) { percent_pos = p; }
void MPVCore::setSpeed(double value) { video_speed = value; }
void MPVCore::showOsdText(const std::string&, int) {}
void MPVCore::setAspect(const std::string&) {}
void MPVCore::setMirror(bool value) { VIDEO_MIRROR = value; }
void MPVCore::setBrightness(int value) { video_brightness = value; }
void MPVCore::setContrast(int value) { video_contrast = value; }
void MPVCore::setSaturation(int value) { video_saturation = value; }
void MPVCore::setGamma(int value) { video_gamma = value; }
void MPVCore::setHue(int value) { video_hue = value; }
int MPVCore::getBrightness() const { return static_cast<int>(video_brightness); }
int MPVCore::getContrast() const { return static_cast<int>(video_contrast); }
int MPVCore::getSaturation() const { return static_cast<int>(video_saturation); }
int MPVCore::getGamma() const { return static_cast<int>(video_gamma); }
int MPVCore::getHue() const { return static_cast<int>(video_hue); }
void MPVCore::setHwdecCopyMode(bool) {}
void MPVCore::disableDimming(bool) {}
void MPVCore::draw(brls::Rect, float) {}
mpv_render_context* MPVCore::getContext() { return mpv_context; }
mpv_handle* MPVCore::getHandle() { return mpv; }
MPVEvent* MPVCore::getEvent() { return &mpvCoreEvent; }
void MPVCore::restart() { reset(); }
void MPVCore::reset() {
    duration = 0;
    playback_time = 0;
    percent_pos = 0;
    video_progress = 0;
    video_eof = false;
    video_paused = false;
    video_stopped = true;
    video_playing = false;
}
void MPVCore::setShader(const std::string&, const std::string&, const std::vector<std::vector<std::string>>&, bool) {}
void MPVCore::clearShader(bool) {}
void MPVCore::_command_async(const std::vector<std::string>&) {}
void MPVCore::eventMainLoop() {}
void MPVCore::initializeVideo() {}
void MPVCore::uninitializeVideo() {}
void MPVCore::init() {}
void MPVCore::clean() {}
void MPVCore::setFrameSize(brls::Rect value) { rect = value; }
void MPVCore::on_update(void*) {}
void MPVCore::on_wakeup(void*) {}

#endif
