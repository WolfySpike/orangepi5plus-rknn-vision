#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

class MistKmbox {
private:
    int sockfd = -1;
    struct sockaddr_in servaddr{};
    uint32_t mac_uint = 0;
    uint32_t session_rand = 0;
    bool has_session_rand = false;
    uint32_t msg_index = 0;

    std::thread monitor_thread;
    std::atomic<bool> monitor_running{false};
    std::atomic<uint8_t> hw_buttons{0};
    std::atomic<int> virtual_buttons{0};

#pragma pack(push, 1)
    struct KmboxPacket {
        uint32_t mac;
        uint32_t rand;
        uint32_t indexpts;
        uint32_t cmd;
        int32_t button;
        int32_t x;
        int32_t y;
        int32_t wheel;
        int32_t point[4];
        char pad[976];
    };
#pragma pack(pop)

    static void put_u32_le(uint8_t* dst, uint32_t value) {
        dst[0] = static_cast<uint8_t>(value & 0xff);
        dst[1] = static_cast<uint8_t>((value >> 8) & 0xff);
        dst[2] = static_cast<uint8_t>((value >> 16) & 0xff);
        dst[3] = static_cast<uint8_t>((value >> 24) & 0xff);
    }

    static uint32_t get_u32_le(const char* src) {
        const auto* p = reinterpret_cast<const uint8_t*>(src);
        return static_cast<uint32_t>(p[0])
            | (static_cast<uint32_t>(p[1]) << 8)
            | (static_cast<uint32_t>(p[2]) << 16)
            | (static_cast<uint32_t>(p[3]) << 24);
    }

    uint32_t packet_rand() const {
        return has_session_rand ? session_rand : static_cast<uint32_t>(std::rand());
    }

    static constexpr uint32_t CMD_LOGICAL_LEFT = 0x9823AE8D;
    static constexpr uint32_t CMD_LOGICAL_RIGHT = 0x238d8212;
    static constexpr uint32_t CMD_LOGICAL_MIDDLE = 0x97a3AE8D;

    void send_short_packet(uint32_t cmd, uint32_t rand_field, const void* payload = nullptr, size_t payload_len = 0) {
        uint8_t pkt[64] = {};
        if (payload_len > sizeof(pkt) - 16) return;
        put_u32_le(pkt + 0, mac_uint);
        put_u32_le(pkt + 4, rand_field);
        put_u32_le(pkt + 8, msg_index++);
        put_u32_le(pkt + 12, cmd);
        if (payload && payload_len > 0) {
            std::memcpy(pkt + 16, payload, payload_len);
        }
        sendto(sockfd, pkt, 16 + payload_len, 0, (struct sockaddr *)&servaddr, sizeof(servaddr));
    }

    void send_legacy_packet(uint32_t cmd, uint32_t rand_field, int button, int x = 0, int y = 0, int wheel = 0) {
        KmboxPacket pkt;
        std::memset(&pkt, 0, sizeof(pkt));
        pkt.mac = mac_uint;
        pkt.rand = rand_field;
        pkt.indexpts = msg_index++;
        pkt.cmd = cmd;
        pkt.button = button;
        pkt.x = x;
        pkt.y = y;
        pkt.wheel = wheel;
        sendto(sockfd, &pkt, sizeof(pkt), 0, (struct sockaddr *)&servaddr, sizeof(servaddr));
    }

    void monitor_worker(int listen_port) {
        int listen_sock = socket(AF_INET, SOCK_DGRAM, 0);
        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(listen_port);
        addr.sin_addr.s_addr = INADDR_ANY;
        bind(listen_sock, (struct sockaddr*)&addr, sizeof(addr));

        while (monitor_running) {
            char buf[1024];
            if (recvfrom(listen_sock, buf, sizeof(buf), 0, nullptr, nullptr) > 0) {
                hw_buttons = static_cast<uint8_t>(buf[1]);
            }
        }
        close(listen_sock);
    }

public:
    MistKmbox(const std::string& ip, int port, const std::string& mac) {
        sockfd = socket(AF_INET, SOCK_DGRAM, 0);
        servaddr.sin_family = AF_INET;
        servaddr.sin_port = htons(port);
        inet_pton(AF_INET, ip.c_str(), &servaddr.sin_addr);
        mac_uint = std::stoul(mac, nullptr, 16);

        // Keep connect in the original legacy format. The monitor stream that
        // feeds physical aim/trigger buttons is sensitive on some KmboxNet
        // firmware, so do not disturb this path.
        send_legacy_packet(0xaf3c2828, 0x12345678, 0);
    }

    ~MistKmbox() {
        monitor_running = false;
        if (monitor_thread.joinable()) monitor_thread.join();
        if (sockfd >= 0) close(sockfd);
    }

    void start_monitor(int local_listen_port = 12345) {
        monitor_running = true;
        monitor_thread = std::thread(&MistKmbox::monitor_worker, this, local_listen_port);

        uint32_t monitor_rand = static_cast<uint32_t>(local_listen_port | (0xaa55 << 16));
        // Original monitor command format. This is what updates hw_buttons.
        send_legacy_packet(0x27388020, monitor_rand, 0);
        std::cout << "[INFO] Kmbox Monitor started successfully!" << std::endl;
    }

    void move_auto(int dx, int dy, int time_ms) {
        if (dx == 0 && dy == 0) return;
        send_legacy_packet(0xaede7346, static_cast<uint32_t>(time_ms), 0, dx, dy, 0);
    }

    void move(int dx, int dy) {
        if (dx == 0 && dy == 0) return;
        send_legacy_packet(0xaede7345, packet_rand(), 0, dx, dy, 0);
    }

    void send_mouse_packet(int button_mask, int dx = 0, int dy = 0, int wheel = 0) {
        (void)button_mask;
        send_legacy_packet(0xaede7345, packet_rand(), 0, dx, dy, wheel);
    }

    void send_button_cmd(uint32_t cmd, bool pressed) {
        uint8_t payload[4] = {};
        put_u32_le(payload, pressed ? 1u : 0u);

        // Some firmwares only honor the dedicated short click packet, while the
        // older code path only updates button state through mouse_move packets.
        // Send both forms; repeated down/up for the same button is harmless.
        send_short_packet(cmd, packet_rand(), payload, sizeof(payload));
    }

    void set_button_mask(int button_mask) {
        button_mask &= 0x07;
        int old_mask = virtual_buttons.exchange(button_mask);
        int changed = old_mask ^ button_mask;

        if (changed & 0x01) {
            std::cout << "\n[KMBTN] left=" << (((button_mask & 0x01) != 0) ? 1 : 0) << std::endl;
            send_button_cmd(CMD_LOGICAL_LEFT, (button_mask & 0x01) != 0);
        }
        if (changed & 0x02) {
            std::cout << "\n[KMBTN] right=" << (((button_mask & 0x02) != 0) ? 1 : 0) << std::endl;
            send_button_cmd(CMD_LOGICAL_RIGHT, (button_mask & 0x02) != 0);
        }
        if (changed & 0x04) {
            std::cout << "\n[KMBTN] middle=" << (((button_mask & 0x04) != 0) ? 1 : 0) << std::endl;
            send_button_cmd(CMD_LOGICAL_MIDDLE, (button_mask & 0x04) != 0);
        }

        send_mouse_packet(button_mask);
    }

    void force_button_mask(int button_mask) {
        button_mask &= 0x07;
        virtual_buttons = button_mask;

        // Safety resync for cases where the box/host missed a previous up
        // event. This is intentionally explicit, not based on our cached mask.
        send_button_cmd(CMD_LOGICAL_LEFT, (button_mask & 0x01) != 0); // left
        send_button_cmd(CMD_LOGICAL_RIGHT, (button_mask & 0x02) != 0); // right
        send_mouse_packet(button_mask);
    }

    void raw_button_cmd(uint32_t cmd, bool pressed) {
        send_button_cmd(cmd, pressed);
    }

    void raw_mouse_button_mask(int button_mask) {
        send_legacy_packet(0xaede7345, packet_rand(), button_mask, 0, 0, 0);
    }

    void click_left(int hold_ms = 18) {
        set_button_mask(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(std::max(1, hold_ms)));
        set_button_mask(0);
    }

    bool is_left_down() { return (hw_buttons & 0x01) != 0; }
    bool is_right_down() { return (hw_buttons & 0x02) != 0; }
    bool is_middle_down() { return (hw_buttons & 0x04) != 0; }
    bool is_side_up_down() { return (hw_buttons & 0x08) != 0; }
    bool is_side_down_down() { return (hw_buttons & 0x10) != 0; }
    bool is_left_or_right_down() { return (hw_buttons & 0x03) != 0; }
    uint8_t get_buttons_mask() { return hw_buttons.load(); }
};
