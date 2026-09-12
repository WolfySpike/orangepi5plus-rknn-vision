#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <opencv2/opencv.hpp>

namespace {

struct Options {
    std::string host = "127.0.0.1";
    int port = 9999;
    std::string device = "/dev/video0";
    int src_width = 1920;
    int src_height = 1080;
    int src_fps = 240;
    std::string src_format = "BGR";
    int width = 416;
    int height = 416;
    int fps = 60;
    int quality = 70;
    int min_quality = 35;
    int max_packet = 65000;
    bool swap_rb = false;
    bool gst_crop = true;
};

void print_usage(const char* argv0) {
    std::cerr
        << "Usage:\n"
        << "  " << argv0 << " --host <pc_ip> --port <port> [options]\n\n"
        << "Options:\n"
        << "  --device /dev/video0\n"
        << "  --width 416 --height 416 --fps 60 --quality 70\n"
        << "  --src-width 1920 --src-height 1080 --src-fps 240 --src-format BGR\n"
        << "  --min-quality 35 --max-packet 65000\n"
        << "  --swap-rb     swap red/blue before JPEG encode if receiver color is wrong\n"
        << "  --no-gst-crop receive full frame in OpenCV, then crop in C++\n";
}

bool parse_int_arg(const char* value, int& out) {
    try {
        size_t used = 0;
        int parsed = std::stoi(value, &used, 10);
        if (used != std::strlen(value)) {
            return false;
        }
        out = parsed;
        return true;
    } catch (...) {
        return false;
    }
}

bool parse_args(int argc, char** argv, Options& opt) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto need_value = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for " << name << "\n";
                return nullptr;
            }
            return argv[++i];
        };

        if (arg == "--host") {
            const char* value = need_value("--host");
            if (!value) return false;
            opt.host = value;
        } else if (arg == "--port") {
            const char* value = need_value("--port");
            if (!value || !parse_int_arg(value, opt.port)) return false;
        } else if (arg == "--device") {
            const char* value = need_value("--device");
            if (!value) return false;
            opt.device = value;
        } else if (arg == "--width") {
            const char* value = need_value("--width");
            if (!value || !parse_int_arg(value, opt.width)) return false;
        } else if (arg == "--height") {
            const char* value = need_value("--height");
            if (!value || !parse_int_arg(value, opt.height)) return false;
        } else if (arg == "--fps") {
            const char* value = need_value("--fps");
            if (!value || !parse_int_arg(value, opt.fps)) return false;
        } else if (arg == "--quality") {
            const char* value = need_value("--quality");
            if (!value || !parse_int_arg(value, opt.quality)) return false;
        } else if (arg == "--min-quality") {
            const char* value = need_value("--min-quality");
            if (!value || !parse_int_arg(value, opt.min_quality)) return false;
        } else if (arg == "--max-packet") {
            const char* value = need_value("--max-packet");
            if (!value || !parse_int_arg(value, opt.max_packet)) return false;
        } else if (arg == "--src-width") {
            const char* value = need_value("--src-width");
            if (!value || !parse_int_arg(value, opt.src_width)) return false;
        } else if (arg == "--src-height") {
            const char* value = need_value("--src-height");
            if (!value || !parse_int_arg(value, opt.src_height)) return false;
        } else if (arg == "--src-fps") {
            const char* value = need_value("--src-fps");
            if (!value || !parse_int_arg(value, opt.src_fps)) return false;
        } else if (arg == "--src-format") {
            const char* value = need_value("--src-format");
            if (!value) return false;
            opt.src_format = value;
            if (opt.src_format == "BGR3") {
                opt.src_format = "BGR";
            }
        } else if (arg == "--swap-rb") {
            opt.swap_rb = true;
        } else if (arg == "--no-gst-crop") {
            opt.gst_crop = false;
        } else if (arg == "-h" || arg == "--help") {
            return false;
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            return false;
        }
    }

    opt.width = std::max(16, opt.width);
    opt.height = std::max(16, opt.height);
    opt.fps = std::max(1, opt.fps);
    opt.quality = std::clamp(opt.quality, 1, 100);
    opt.min_quality = std::clamp(opt.min_quality, 1, opt.quality);
    opt.max_packet = std::clamp(opt.max_packet, 1200, 65507);
    return !opt.host.empty() && opt.port > 0 && opt.port <= 65535;
}

std::string build_pipeline(const Options& opt) {
    std::string pipeline = "v4l2src device=" + opt.device + " ! "
        + "video/x-raw,format=" + opt.src_format
        + ",width=" + std::to_string(opt.src_width)
        + ",height=" + std::to_string(opt.src_height)
        + ",framerate=" + std::to_string(opt.src_fps) + "/1 ! "
        + "queue max-size-buffers=1 max-size-time=0 max-size-bytes=0 leaky=downstream ! ";

    if (opt.gst_crop && opt.width <= opt.src_width && opt.height <= opt.src_height) {
        int left = (opt.src_width - opt.width) / 2;
        int right = opt.src_width - opt.width - left;
        int top = (opt.src_height - opt.height) / 2;
        int bottom = opt.src_height - opt.height - top;
        pipeline += "videocrop left=" + std::to_string(left)
            + " right=" + std::to_string(right)
            + " top=" + std::to_string(top)
            + " bottom=" + std::to_string(bottom)
            + " ! video/x-raw,format=" + opt.src_format
            + ",width=" + std::to_string(opt.width)
            + ",height=" + std::to_string(opt.height)
            + " ! ";
    }

    pipeline += "videorate drop-only=true ! video/x-raw,framerate="
        + std::to_string(opt.fps) + "/1 ! "
        + "appsink drop=true max-buffers=1 sync=false";
    return pipeline;
}

cv::Rect center_rect(const cv::Mat& frame, int width, int height) {
    int crop_w = std::min(width, frame.cols);
    int crop_h = std::min(height, frame.rows);
    int x = std::max(0, (frame.cols - crop_w) / 2);
    int y = std::max(0, (frame.rows - crop_h) / 2);
    return cv::Rect(x, y, crop_w, crop_h);
}

bool encode_jpeg_under_limit(
    const cv::Mat& image,
    int preferred_quality,
    int min_quality,
    int max_packet,
    std::vector<uchar>& encoded,
    int& used_quality
) {
    std::vector<int> params = {cv::IMWRITE_JPEG_QUALITY, preferred_quality};
    for (int q = preferred_quality; q >= min_quality; q -= 5) {
        params[1] = q;
        encoded.clear();
        if (!cv::imencode(".jpg", image, encoded, params)) {
            return false;
        }
        if (static_cast<int>(encoded.size()) <= max_packet) {
            used_quality = q;
            return true;
        }
    }
    used_quality = min_quality;
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parse_args(argc, argv, opt)) {
        print_usage(argv[0]);
        return 2;
    }

    int sock = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        std::cerr << "socket() failed: " << std::strerror(errno) << "\n";
        return 1;
    }

    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(static_cast<uint16_t>(opt.port));
    if (::inet_pton(AF_INET, opt.host.c_str(), &dst.sin_addr) != 1) {
        std::cerr << "Invalid host IP: " << opt.host << "\n";
        ::close(sock);
        return 1;
    }

    std::string pipeline = build_pipeline(opt);
    std::cout << "Opening HDMI RX pipeline:\n  " << pipeline << "\n";
    cv::VideoCapture cap;
    cap.open(pipeline, cv::CAP_GSTREAMER);
    if (!cap.isOpened()) {
        std::cerr << "Failed to open HDMI RX pipeline.\n";
        ::close(sock);
        return 1;
    }

    std::cout << "Streaming center " << opt.width << "x" << opt.height
              << " JPEG UDP to " << opt.host << ":" << opt.port
              << " target_fps=" << opt.fps
              << " quality=" << opt.quality
              << " max_packet=" << opt.max_packet
              << (opt.swap_rb ? " swap_rb=1" : "")
              << "\n";

    cv::Mat frame;
    cv::Mat crop;
    cv::Mat encoded_source;
    std::vector<uchar> jpeg;

    using clock = std::chrono::steady_clock;
    const auto frame_interval = std::chrono::duration_cast<clock::duration>(
        std::chrono::duration<double>(1.0 / opt.fps)
    );
    auto next_send = clock::now();
    auto stat_time = clock::now();
    int captured = 0;
    int sent = 0;
    int dropped_big = 0;
    int last_quality = opt.quality;
    size_t last_size = 0;

    while (true) {
        if (!cap.read(frame) || frame.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        ++captured;

        auto now = clock::now();
        if (now < next_send) {
            continue;
        }
        next_send = now + frame_interval;

        cv::Rect roi = center_rect(frame, opt.width, opt.height);
        crop = frame(roi);
        if (crop.cols != opt.width || crop.rows != opt.height) {
            cv::resize(crop, encoded_source, cv::Size(opt.width, opt.height), 0, 0, cv::INTER_LINEAR);
        } else {
            encoded_source = crop;
        }

        if (opt.swap_rb) {
            cv::cvtColor(encoded_source, encoded_source, cv::COLOR_BGR2RGB);
        }

        int used_quality = opt.quality;
        bool ok = encode_jpeg_under_limit(encoded_source, opt.quality, opt.min_quality, opt.max_packet, jpeg, used_quality);
        last_quality = used_quality;
        last_size = jpeg.size();
        if (!ok || jpeg.empty() || static_cast<int>(jpeg.size()) > opt.max_packet) {
            ++dropped_big;
            continue;
        }

        ssize_t n = ::sendto(sock, jpeg.data(), jpeg.size(), 0, reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
        if (n < 0) {
            std::cerr << "sendto() failed: " << std::strerror(errno) << "\n";
            break;
        }
        ++sent;

        now = clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - stat_time);
        if (elapsed.count() >= 1000) {
            double seconds = elapsed.count() / 1000.0;
            std::cout << "capture=" << (captured / seconds)
                      << "fps send=" << (sent / seconds)
                      << "fps jpeg=" << (last_size / 1024.0)
                      << "KiB q=" << last_quality
                      << " dropped_big=" << dropped_big
                      << "\n";
            captured = 0;
            sent = 0;
            dropped_big = 0;
            stat_time = now;
        }
    }

    ::close(sock);
    return 0;
}
