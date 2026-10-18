//
// Created by fang on 2022/7/16.
//

#include <borealis/core/singleton.hpp>
#include <borealis/core/application.hpp>
#include <borealis/core/cache_helper.hpp>
#include <borealis/core/thread.hpp>
#include <borealis/platforms/xbox360/xbox360_perf.hpp>
#include <stb_image.h>

#include "utils/image_helper.hpp"
#include "utils/string_helper.hpp"
#include "api/bilibili/util/http.hpp"

#ifdef USE_WEBP
#include <webp/decode.h>
#endif

#ifdef BOREALIS_USE_GXM
#ifndef MAX
#define MAX(a,b) (((a)>(b))?(a):(b))
#endif
#ifndef MIN
#define MIN(a,b) (((a)<(b))?(a):(b))
#endif
#define STB_DXT_IMPLEMENTATION
#include <borealis/extern/nanovg/stb_dxt.h>
#include <borealis/extern/nanovg/nanovg_gxm.h>

static inline __attribute__((always_inline)) uint32_t nearest_po2(uint32_t val) {
    val--;
    val |= val >> 1;
    val |= val >> 2;
    val |= val >> 4;
    val |= val >> 8;
    val |= val >> 16;
    val++;

    return val;
}

static inline __attribute__((always_inline)) uint64_t morton_1(uint64_t x) {
    x = x & 0x5555555555555555;
    x = (x | (x >> 1)) & 0x3333333333333333;
    x = (x | (x >> 2)) & 0x0F0F0F0F0F0F0F0F;
    x = (x | (x >> 4)) & 0x00FF00FF00FF00FF;
    x = (x | (x >> 8)) & 0x0000FFFF0000FFFF;
    x = (x | (x >> 16)) & 0xFFFFFFFFFFFFFFFF;
    return x;
}

static inline __attribute__((always_inline)) void d2xy_morton(uint64_t d, uint64_t *x, uint64_t *y) {
    *x = morton_1(d);
    *y = morton_1(d >> 1);
}

static inline __attribute__((always_inline)) void extract_block(const uint8_t *src, uint32_t width, uint8_t *block) {
    for (int j = 0; j < 4; j++) {
        memcpy(&block[j * 4 * 4], src, 16);
        src += width * 4;
    }
}

/**
 * Compress RGBA data to DXT1 or DXT5
 * @param dst DXT data
 * @param src RGBA data
 * @param w source width
 * @param h source height
 * @param stride source stride
 * @param last_size max block size in pixel of last compression round, default is 64
 * @param isdxt5 false for DXT1, true for DXT5
 */
static void dxt_compress_ext(uint8_t *dst, uint8_t *src, uint32_t w, uint32_t h, uint32_t stride, uint32_t last_size, bool isdxt5) {
    uint8_t block[64];
    uint32_t align_w = MAX(nearest_po2(w), last_size);
    uint32_t align_h = MAX(nearest_po2(h), last_size);
    uint32_t s = MIN(align_w, align_h);
    uint32_t num_blocks = s * s / 16;
    const uint32_t block_size = isdxt5 ? 16 : 8;
    uint64_t d, offs_x, offs_y;

    for (d = 0; d < num_blocks; d++, dst += block_size) {
        d2xy_morton(d, &offs_x, &offs_y);
        if (offs_x * 4 >= h || offs_y * 4 >= w)
            continue;
        extract_block(src + offs_y * 16 + offs_x * stride * 16, stride, block);
        stb_compress_dxt_block(dst, block, isdxt5, STB_DXT_NORMAL);
    }
    if (align_w > align_h)
        return dxt_compress_ext(dst, src + s * 4, w - s, h, stride, s, isdxt5);
    if (align_w < align_h)
        return dxt_compress_ext(dst, src + stride * s * 4, w, h - s, stride, s, isdxt5);
}

static void dxt_compress(uint8_t *dst, uint8_t *src, uint32_t w, uint32_t h, bool isdxt5) {
    dxt_compress_ext(dst, src, w, h, w, 64, isdxt5);
}
#endif

#if defined(PLATFORM_XBOX360)
// CPR's Xbox worker uses an XDK thread so image transfer and decoding stay
// off the UI thread. Texture creation remains in the brls::sync callback.
class ImageThreadPool : public brls::Singleton<ImageThreadPool> {
public:
    ImageThreadPool() = default;

    template <typename Fn>
    void Submit(Fn&& fn) {
        cpr::async(std::forward<Fn>(fn));
    }

    CURLSH* getShare() { return share.getShare(); }

private:
    bilibili::CurlSharedObject share;
};
#else
class ImageThreadPool : public cpr::ThreadPool, public brls::Singleton<ImageThreadPool> {
public:
    ImageThreadPool() : cpr::ThreadPool(1, ImageHelper::REQUEST_THREADS, std::chrono::milliseconds(5000)) {
        brls::Logger::info("max_thread_num: {}", this->max_thread_num);
        this->Start();
    }

    ~ImageThreadPool() override { this->Stop(); }

    CURLSH* getShare() {
        return share.getShare();
    }

private:
    bilibili::CurlSharedObject share;
};
#endif

ImageHelper::ImageHelper(brls::Image* view) : imageView(view) {}

ImageHelper::~ImageHelper() { brls::Logger::verbose("delete ImageHelper {}", (size_t)this); }

std::shared_ptr<ImageHelper> ImageHelper::with(brls::Image* view) {
    brls::PlatformLockGuard lock(requestMutex);
    std::shared_ptr<ImageHelper> item;

    if (!requestPool.empty() && (*requestPool.begin())->getImageView() == nullptr) {
        // 复用请求，挪到队尾
        item = *requestPool.begin();
        item->setImageView(view);
        requestPool.splice(requestPool.end(), requestPool, requestPool.begin());
    } else {
        // 新建 ImageHelper 实例
        item = std::make_shared<ImageHelper>(view);
        requestPool.emplace_back(item);
    }

    auto iter         = --requestPool.end();
    requestMap[view]  = iter;
    item->currentIter = iter;
    // 重置 "取消" 标记位
    item->isCancel = false;
    // 禁止图片组件销毁
    item->imageView->ptrLock();
    // 设置图片组件不处理纹理的销毁，由缓存统一管理纹理销毁
    item->imageView->setFreeTexture(false);

    brls::Logger::verbose("with view: {} {} {}", (size_t)view, (size_t)item.get(), (size_t)(*iter).get());

    return item;
}

void ImageHelper::load(const std::string &url) {
#if defined(PLATFORM_XBOX360)
    // The Xbox transport supports HTTPS without certificate verification. A
    // number of Bilibili image CDNs reject the HTTP fallback, so keep the
    // original URL even while TLS verification is disabled.
    this->imageUrl = url;
#else
    this->imageUrl = bilibili::HTTP::VERIFY.verify ? url : pystring::replace(url, "https", "http", 1);
#endif

#ifdef BOREALIS_USE_GXM
    std::vector<std::string> urls = pystring::rsplit(this->imageUrl, "@", 1);
    if (pystring::endswith(urls[0], "jpg")) {
        this->imageFlag = NVG_IMAGE_DXT1;
    } else {
        this->imageFlag = NVG_IMAGE_DXT5;
    }
#endif

    brls::Logger::verbose("load view: {} {}", (size_t)this->imageView, (size_t)this);

    //    std::unique_lock<std::mutex> lock(this->loadingMutex);

    // 检查是否存在缓存
    int tex = brls::TextureCache::instance().getCache(this->imageUrl);
    if (tex > 0) {
        brls::Logger::verbose("cache hit: {}", this->imageUrl);
        this->imageView->innerSetImage(tex);
        this->clean();
        return;
    }

    //todo: 可能会发生同时请求多个重复链接的情况，此种情况下最好合并为一个请求

    // 缓存网络图片
    brls::Logger::verbose("request Image 1: {} {}", this->imageUrl, this->isCancel);
    ImageThreadPool::instance().Submit([this]() {
        brls::Logger::verbose("Submit view: {} {} {} {}", (size_t)this->imageView, (size_t)this, this->imageUrl,
                              this->isCancel);
        if (this->isCancel) {
            this->clean();
            return;
        }
        this->requestImage();
    });
}

static inline void freeImageData(uint8_t* imageData, bool isWebp) {
    if (!imageData) return;
#ifdef USE_WEBP
    if (isWebp)
        WebPFree(imageData);
    else
#endif
        stbi_image_free(imageData);
}

void ImageHelper::requestImage() {
    brls::Logger::verbose("request Image 2: {} {}", this->imageUrl, this->isCancel);

    // 请求图片
    cpr::Session session;
    CURL* curl = session.GetCurlHolder()->handle;
    curl_easy_setopt(curl, CURLOPT_SHARE, ImageThreadPool::instance().getShare());
    curl_easy_setopt(curl, CURLOPT_DNS_CACHE_TIMEOUT, bilibili::HTTP::DNS_CACHE_TIMEOUT);
    session.SetTimeout(cpr::Timeout{bilibili::HTTP::TIMEOUT});
    session.SetConnectTimeout(cpr::ConnectTimeout{bilibili::HTTP::CONNECTION_TIMEOUT});
    session.SetHeader(bilibili::HTTP::HEADERS);
#if defined(PLATFORM_XBOX360)
    // Match HTTP::createSession(): the Xbox curl fork handles close-delimited
    // HTTP/1.0 responses and identity encoding more reliably than chunked,
    // compressed HTTP/1.1 image responses.
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_0);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "identity");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
#endif
    session.SetVerifySsl(bilibili::HTTP::VERIFY);
    session.SetProxies(bilibili::HTTP::PROXIES);
    session.SetUrl(cpr::Url{this->imageUrl});
    session.SetProgressCallback(cpr::ProgressCallback([this](...) -> bool { return !this->isCancel; }));
    cpr::Response r = session.Get();

    // 图片请求失败或取消请求
    const size_t responseBytes = r.text.size();
    if (r.status_code < 200 || r.status_code >= 300 || responseBytes == 0) {
        // P0 排查「刷新后封面全不显示」：非取消场景下的 HTTP 失败要看得见
        brls::Logger::info("[img] http fail code={} bytes={} dl={} err={} cancel={} url={}", (int)r.status_code,
                           responseBytes, (long long)r.downloaded_bytes, (int)r.error.code, this->isCancel,
                           r.url.str());
        this->clean();
        return;
    }
    if (this->isCancel) {
        brls::Logger::verbose("request undone (cancel): {}", r.url.str());
        this->clean();
        return;
    }

    brls::Logger::verbose("load pic:{} size:{} bytes by{}", r.url.str(), responseBytes, (size_t)this);

    uint8_t* imageData = nullptr;
    int imageW = 0, imageH = 0;
    bool isWebp = false;

#ifdef USE_WEBP
    if (imageUrl.size() > 5 && imageUrl.substr(imageUrl.size() - 5, 5) == ".webp") {
        imageData = WebPDecodeRGBA((const uint8_t*)r.text.data(), responseBytes, &imageW, &imageH);
        isWebp    = true;
    } else {
#endif
        int n;
        imageData =
            stbi_load_from_memory((unsigned char*)r.text.data(), (int)responseBytes, &imageW, &imageH, &n, 4);
#ifdef USE_WEBP
    }
#endif

    // P0 排查「刷新后封面全不显示」：HTTP 2xx 且有 body 却解码失败时，
    // 打印 stbi 失败原因（"out of memory" = 内存耗尽 / "bad jpeg" 等 = 响应体被截断）
    // 以及响应体前 16 字节（FFD8.. = 真 JPEG，"<" 开头 = 服务器返回了 HTML 错误页）。
    // 另附一次堆内存探针：从 64MB 往下试分配并立即释放，报告仍能拿到的最大连续块。
    std::string failWhy, failHead;
    size_t failHeap = 0;
    if (!imageData) {
        const char* why = isWebp ? "webp-decode" : stbi_failure_reason();
        failWhy         = why ? why : "unknown";
        static const char* hexd = "0123456789abcdef";
        for (size_t i = 0; i < 16 && i < responseBytes; i++) {
            unsigned char c = (unsigned char)r.text[i];
            failHead.push_back(hexd[c >> 4]);
            failHead.push_back(hexd[c & 15]);
        }
        static const size_t kProbe[] = {64u << 20, 32u << 20, 16u << 20, 8u << 20, 4u << 20, 2u << 20, 1u << 20};
        for (size_t s : kProbe) {
            void* p = malloc(s);
            if (p) {
                free(p);
                failHeap = s;
                break;
            }
        }
    }

#ifdef BOREALIS_USE_GXM
    if (imageData) {
        bool dxt5 = this->imageFlag & NVG_IMAGE_DXT5;
        size_t size = nearest_po2(imageW) * nearest_po2(imageH);
        if (!dxt5)
            size >> 1;
        auto *compressed = (uint8_t *)malloc(size);
        dxt_compress(compressed, imageData, imageW, imageH, dxt5);
        freeImageData(imageData, isWebp);
        imageData = compressed;
    }
#endif

    // tag "img"：来源辨识用（纹理创建走 image 桶，任务标签补足慢任务日志）
    brls::sync([this, r, imageData, imageW, imageH, isWebp, failWhy, failHead, failHeap]() {
        // P0 §3.1 测量：归类到 sync split 的 image 桶（纹理创建 + 缓存查询全耗时）
        brls::xbox360perf::SyncScope syncScope(brls::xbox360perf::SYNC_IMAGE);
        // 再检查一遍缓存
        int tex = brls::TextureCache::instance().getCache(this->imageUrl);
        if (tex > 0) {
            brls::Logger::verbose("cache hit 2: {}", this->imageUrl);
            this->imageView->innerSetImage(tex);
        } else {
            NVGcontext* vg = brls::Application::getNVGContext();
            if (imageData) {
#ifdef BOREALIS_USE_GXM
                bool dxt5 = this->imageFlag & NVG_IMAGE_DXT5;
                tex = nvgCreateImageRGBA(vg, imageW, imageH, (dxt5 ? NVG_IMAGE_DXT5 : NVG_IMAGE_DXT1) | NVG_IMAGE_LPDDR, imageData);
#else
                tex = nvgCreateImageRGBA(vg, imageW, imageH, 0, imageData);
#endif
            } else {
                brls::Logger::error("[img] decode fail: why={} bytes={} head={} heap>={} url={}", failWhy,
                                    r.text.size(), failHead, failHeap, this->imageUrl);
            }

            if (tex > 0) {
                brls::TextureCache::instance().addCache(this->imageUrl, tex);
                if (!this->isCancel) {
                    brls::Logger::verbose("load image: {}", this->imageUrl);
                    this->imageView->innerSetImage(tex);
                }
            } else if (imageData) {
                // P0 §5.11：解码成功但纹理创建失败 = 显存/堆耗尽，是内存问题的直接信号
                brls::Logger::error("[img] texture create failed {}x{} url={}", imageW, imageH, this->imageUrl);
            }
        }
        if (imageData) {
#ifdef BOREALIS_USE_GXM
            free(imageData);
#else
            freeImageData(imageData, isWebp);
#endif
        }
        this->clean();
    }, "img");
}

void ImageHelper::clean() {
    brls::PlatformLockGuard lock(requestMutex);

    // 允许图片组件销毁
    if (this->imageView) this->imageView->ptrUnlock();
    // 移除请求，复用 ImageHelper (挪到队首)
    requestPool.splice(requestPool.begin(), requestPool, this->currentIter);
    this->imageView   = nullptr;
    this->currentIter = requestPool.end();
}

void ImageHelper::clear(brls::Image* view) {
    brls::TextureCache::instance().removeCache(view->getTexture());
    view->clear();

    brls::PlatformLockGuard lock(requestMutex);

    // 请求不存在
    if (requestMap.find(view) == requestMap.end()) return;

    brls::Logger::verbose("clear view: {} {}", (size_t)view, (size_t)(*requestMap[view]).get());

    // 请求没结束，取消请求
    if ((*requestMap[view])->imageView == view) {
        (*requestMap[view])->cancel();
    }
    requestMap.erase(view);
}

void ImageHelper::cancel() {
    brls::Logger::verbose("Cancel request: {}", this->imageUrl);
    this->isCancel = true;
}

void ImageHelper::setRequestThreads(size_t num) {
    REQUEST_THREADS                            = num;
#if !defined(PLATFORM_XBOX360)
    ImageThreadPool::instance().min_thread_num = 1;
    ImageThreadPool::instance().max_thread_num = num;
#else
    (void)num;
#endif
}

void ImageHelper::setImageView(brls::Image* view) { this->imageView = view; }

brls::Image* ImageHelper::getImageView() { return this->imageView; }

std::string ImageHelper::parseGifImageUrl(const std::string& url, const std::string& ext) {
#ifdef USE_WEBP
    std::string image_url = url;
    if (pystring::endswith(url, "gif")) {
        // gif 图片暂时按照 jpg 来解析
        image_url += pystring::replace(ext, ".webp", ".jpg");
    } else {
        image_url += ext;
    }
    return image_url;
#else
    return url + ext;
#endif
}

std::string ImageHelper::parseNoteImageUrl(const std::string& url, size_t w, size_t h) {
    return parseGifImageUrl(url, wiliwili::format(note_custom_ext, w, h));
}
