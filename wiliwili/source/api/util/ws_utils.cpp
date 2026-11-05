#include "live/ws_utils.hpp"

#include <cstddef>
#include <utility>
#include <iostream>
#include <cstring>
#if defined(PLATFORM_XBOX360)
// Xenon is big-endian, so host and network byte order are identical.
#define ntohl(value) (value)
#define ntohs(value) (value)
#define htonl(value) (value)
#define htons(value) (value)
#elif defined(_WIN32)
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif
#include <zlib.h>
//#include <brotli/decode.h>

// 解析数据包
std::vector<std::string> parse_packet(const std::vector<uint8_t>& data) {
    std::vector<std::string> messages;
    messages.reserve(128);

    size_t data_len = data.size();
    size_t offset   = 0;

    uint32_t packet_length;
    uint16_t header_length;
    uint16_t protocol_version;
    uint32_t operation;

    // 至少要有完整的 16 字节协议头
    while (offset + 16 <= data_len) {
        std::memcpy(&packet_length, data.data() + offset, sizeof(uint32_t));
        std::memcpy(&header_length, data.data() + offset + 4, sizeof(uint16_t));
        std::memcpy(&protocol_version, data.data() + offset + 6, sizeof(uint16_t));
        std::memcpy(&operation, data.data() + offset + 8, sizeof(uint32_t));

        packet_length    = ntohl(packet_length);
        header_length    = ntohs(header_length);
        protocol_version = ntohs(protocol_version);
        operation        = ntohl(operation);

        // 丢弃截断/畸形包，避免越界读取
        if (header_length < 16 || packet_length < header_length ||
            packet_length > data_len - offset) {
            break;
        }

        const uint8_t* body = data.data() + offset + header_length;
        size_t body_len     = packet_length - header_length;

        //| 5 | 服务器 | 数据类型为JSON纯文本 | 通知 | 弹幕、广播等全部信息 |
        if (operation == 5) {
            if (protocol_version == 0) {
                messages.emplace_back(reinterpret_cast<const char*>(body), body_len);
            } else if (protocol_version == 2 && body_len > 0) {
                // 每个压缩包单独解压。原实现复用跨包的 decompressed 缓冲区，
                // 同一帧里出现多个 zlib 包时会把上一包内容重复解析一遍。
                std::vector<uint8_t> decompressed;
                decompressed.reserve(body_len * 4);

                z_stream strm;
                std::memset(&strm, 0, sizeof(strm));
                strm.next_in  = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(body));
                strm.avail_in = static_cast<uInt>(body_len);

                if (inflateInit(&strm) == Z_OK) {
                    int ret;
                    do {
                        uint8_t buffer[16384];
                        strm.next_out  = buffer;
                        strm.avail_out = sizeof(buffer);
                        ret            = inflate(&strm, Z_NO_FLUSH);
                        if (ret != Z_OK && ret != Z_STREAM_END) break;
                        size_t produced = sizeof(buffer) - strm.avail_out;
                        decompressed.insert(decompressed.end(), buffer, buffer + produced);
                    } while (strm.avail_out == 0);
                    inflateEnd(&strm);

                    if (!decompressed.empty()) {
                        auto nested_messages = parse_packet(decompressed);
                        messages.insert(messages.end(), nested_messages.begin(), nested_messages.end());
                    }
                }
            }
        }

        offset += packet_length;
    }

    return messages;
}

// 编码数据包
std::vector<uint8_t> encode_packet(uint16_t protocol_version, uint32_t operation, const std::string& body) {
    std::vector<uint8_t> packet;
    uint32_t packet_length = 16 + body.size();
    uint16_t header_length = 16;
    uint32_t sequence_id   = 1;

    // Packet Length
    uint32_t packet_length_n = htonl(packet_length);
    packet.insert(packet.end(), reinterpret_cast<uint8_t*>(&packet_length_n),
                  reinterpret_cast<uint8_t*>(&packet_length_n) + 4);

    // Header Length
    uint16_t header_length_n = htons(header_length);
    packet.insert(packet.end(), reinterpret_cast<uint8_t*>(&header_length_n),
                  reinterpret_cast<uint8_t*>(&header_length_n) + 2);

    // Protocol Version
    uint16_t protocol_version_n = htons(protocol_version);
    packet.insert(packet.end(), reinterpret_cast<uint8_t*>(&protocol_version_n),
                  reinterpret_cast<uint8_t*>(&protocol_version_n) + 2);

    // Operation
    uint32_t operation_n = htonl(operation);
    packet.insert(packet.end(), reinterpret_cast<uint8_t*>(&operation_n), reinterpret_cast<uint8_t*>(&operation_n) + 4);

    // Sequence Id
    uint32_t sequence_id_n = htonl(sequence_id);
    packet.insert(packet.end(), reinterpret_cast<uint8_t*>(&sequence_id_n),
                  reinterpret_cast<uint8_t*>(&sequence_id_n) + 4);

    // Body
    packet.insert(packet.end(), body.begin(), body.end());

    return packet;
}
