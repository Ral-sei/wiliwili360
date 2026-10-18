//
// Created by fang on 2023/4/25.
//

#include <vector>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <zlib.h>
#include <cpr/cpr.h>
#include <mongoose.h>
#include <borealis/core/logger.hpp>

#include "utils/string_helper.hpp"

namespace wiliwili {

std::string urlEncode(const std::string &in) { return cpr::util::urlEncode(in); }

std::string base64Encode(const std::string &data) {
    size_t in_len  = data.size();
    size_t out_len = ((in_len / 3) + (in_len % 3 ? 1 : 0)) * 4 + 1;
    std::unique_ptr<char[]> buf(new char[out_len]);
    const auto *in = reinterpret_cast<const unsigned char*>(data.c_str());
    size_t encoded_len = mg_base64_encode(in, in_len, buf.get(), out_len);
    std::string out;
    if (encoded_len > 0) {
        out.assign(buf.get(), encoded_len);
    }
    return out;
}

int base64Decode(const std::string &input, std::string &out) {
    out.clear();
    size_t in_len = input.size();
    if (in_len % 4 != 0) { // "Input data size is not a multiple of 4"
        return -1;
    }
    size_t out_len = in_len / 4 * 3 + 1;
    std::unique_ptr<char[]> buf(new char[out_len]);
    size_t decoded_len = mg_base64_decode(input.c_str(), input.size(), buf.get(), out_len);
    if (decoded_len > 0) {
        out.assign(buf.get(), decoded_len);
        return 0;
    }
    return -1;
}

std::string decompressGzipData(const std::string &compressedData) {
    z_stream zs;
    memset(&zs, 0, sizeof(zs));

    if (inflateInit2(&zs, 16 + MAX_WBITS) != Z_OK)
        return {};

    zs.next_in  = (Bytef *)compressedData.data();
    zs.avail_in = compressedData.size();

    int ret;
    char outbuffer[4096];
    std::string decompressedData;

    do {
        zs.next_out  = reinterpret_cast<Bytef *>(outbuffer);
        zs.avail_out = sizeof(outbuffer);

        ret = inflate(&zs, 0);

        if (decompressedData.size() < zs.total_out) {
            decompressedData.append(outbuffer, zs.total_out - decompressedData.size());
        }

    } while (ret == Z_OK);

    inflateEnd(&zs);

    if (ret != Z_STREAM_END)
        return {};

    return decompressedData;
}

namespace {

// 严格模式解压：windowBits < 0 为裸 deflate（RFC1951），> 0 为 zlib 包装，
// 16 + MAX_WBITS 为 gzip。流不完整或数据非法时返回空串。
std::string inflateOnce(const std::string &in, int windowBits) {
    if (in.empty()) return {};

    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    if (inflateInit2(&zs, windowBits) != Z_OK) return {};

    zs.next_in  = reinterpret_cast<Bytef *>(const_cast<char *>(in.data()));
    zs.avail_in = static_cast<uInt>(in.size());

    std::string out;
    char buffer[4096];
    int ret = Z_OK;
    while (true) {
        zs.next_out  = reinterpret_cast<Bytef *>(buffer);
        zs.avail_out = sizeof(buffer);

        ret = inflate(&zs, Z_NO_FLUSH);
        out.append(buffer, sizeof(buffer) - zs.avail_out);

        if (ret == Z_STREAM_END) break;
        if (ret != Z_OK) { out.clear(); break; }       // Z_DATA_ERROR / Z_BUF_ERROR 等
        if (zs.avail_out != 0) { out.clear(); break; } // 输入耗尽仍未结束 → 流被截断
    }

    inflateEnd(&zs);
    return out;
}

} // namespace

std::string inflateContent(const std::string &data, const std::string &contentEncoding) {
    if (data.empty() || contentEncoding.empty()) return data;

    // 明文判定：跳过空白与 UTF-8 BOM 后仍是 XML/JSON 开头则无需解压。
    // 也保证了即使 curl 将来编入 zlib 先解压过一次，也不会被二次解压。
    size_t i = 0;
    if (data.size() >= 3 && static_cast<unsigned char>(data[0]) == 0xEF &&
        static_cast<unsigned char>(data[1]) == 0xBB && static_cast<unsigned char>(data[2]) == 0xBF)
        i = 3;
    while (i < data.size() && (data[i] == ' ' || data[i] == '\t' || data[i] == '\r' || data[i] == '\n')) i++;
    if (i < data.size() && (data[i] == '<' || data[i] == '{' || data[i] == '[')) return data;

    std::string encoding = contentEncoding;
    for (auto &c : encoding) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));

    std::string out;
    if (encoding.find("gzip") != std::string::npos) {
        out = inflateOnce(data, 16 + MAX_WBITS);
    } else if (encoding.find("deflate") != std::string::npos) {
        // bilibili 的 /x/v1/dm/list.so 等接口返回裸 deflate，个别 CDN 会加 zlib 头
        out = inflateOnce(data, -MAX_WBITS);
        if (out.empty()) out = inflateOnce(data, MAX_WBITS);
    } else {
        return data; // identity / br 等不处理
    }

    if (out.empty()) {
        brls::Logger::error("Content-Encoding: {} but inflate failed ({} bytes), use raw body",
                            contentEncoding, data.size());
        return data;
    }
    brls::Logger::debug("Decompressed Content-Encoding: {} ({} -> {} bytes)", contentEncoding, data.size(),
                        out.size());
    return out;
}

std::string toUpper( const std::string & str, std::string::size_type length )
{
    std::string::size_type len = std::min(str.size(), length), i;
    std::string s = str.substr(0, len);

    for ( i = 0; i < len; ++i )
    {
        if ( ::islower( s[i] ) ) s[i] = (char) ::toupper( s[i] );
    }

    return s;
}

bool parseHexColor(const std::string& hex, uint8_t& r, uint8_t& g, uint8_t& b) {
    // Accept both "RRGGBB" (6 chars) and "#RRGGBB" (7 chars with leading #)
    const std::string& digits = (hex.size() == 7 && hex[0] == '#') ? hex.substr(1) : hex;
    if (digits.size() != 6) return false;
    char* end = nullptr;
    unsigned long value = std::strtoul(digits.c_str(), &end, 16);
    if (end == digits.c_str() || *end != '\0' || value > 0xFFFFFF)
        return false;
    r = (value >> 16) & 0xFF;
    g = (value >> 8) & 0xFF;
    b = value & 0xFF;
    return true;
}

};  // namespace wiliwili
