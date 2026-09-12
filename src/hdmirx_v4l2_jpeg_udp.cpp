#include <arpa/inet.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
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
    int width = 416;
    int height = 416;
    int quality = 70;
    int min_quality = 35;
    int max_packet = 65000;
    int buffers = 4;
    bool swap_rb = false;
    bool blocking = false;
};

struct MappedBuffer {
    void* start = nullptr;
    size_t length = 0;
};

int xioctl(int fd, unsigned long request, void* arg) {
    int r = 0;
    do {
        r = ioctl(fd, request, arg);
    } while (r == -1 && errno == EINTR);
    return r;
}

bool parse_int(const char* value, int& out) {
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

void usage(const char* argv0) {
    std::cerr
        << "Usage:\n"
        << "  " << argv0 << " --host <pc_ip> --port <port> [options]\n\n"
        << "Options:\n"
        << "  --device /dev/video0\n"
        << "  --width 416 --height 416 --quality 70\n"
        << "  --src-width 1920 --src-height 1080 --src-fps 240\n"
        << "  --buffers 4 --min-quality 35 --max-packet 65000\n"
        << "  --swap-rb\n"
        << "  --blocking   use blocking VIDIOC_DQBUF instead of poll + nonblocking DQBUF\n";
}

bool parse_args(int argc, char** argv, Options& opt) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto val = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for " << name << "\n";
                return nullptr;
            }
            return argv[++i];
        };

        if (arg == "--host") {
            const char* v = val("--host");
            if (!v) return false;
            opt.host = v;
        } else if (arg == "--port") {
            const char* v = val("--port");
            if (!v || !parse_int(v, opt.port)) return false;
        } else if (arg == "--device") {
            const char* v = val("--device");
            if (!v) return false;
            opt.device = v;
        } else if (arg == "--width") {
            const char* v = val("--width");
            if (!v || !parse_int(v, opt.width)) return false;
        } else if (arg == "--height") {
            const char* v = val("--height");
            if (!v || !parse_int(v, opt.height)) return false;
        } else if (arg == "--quality") {
            const char* v = val("--quality");
            if (!v || !parse_int(v, opt.quality)) return false;
        } else if (arg == "--min-quality") {
            const char* v = val("--min-quality");
            if (!v || !parse_int(v, opt.min_quality)) return false;
        } else if (arg == "--max-packet") {
            const char* v = val("--max-packet");
            if (!v || !parse_int(v, opt.max_packet)) return false;
        } else if (arg == "--src-width") {
            const char* v = val("--src-width");
            if (!v || !parse_int(v, opt.src_width)) return false;
        } else if (arg == "--src-height") {
            const char* v = val("--src-height");
            if (!v || !parse_int(v, opt.src_height)) return false;
        } else if (arg == "--src-fps") {
            const char* v = val("--src-fps");
            if (!v || !parse_int(v, opt.src_fps)) return false;
        } else if (arg == "--buffers") {
            const char* v = val("--buffers");
            if (!v || !parse_int(v, opt.buffers)) return false;
        } else if (arg == "--swap-rb") {
            opt.swap_rb = true;
        } else if (arg == "--blocking") {
            opt.blocking = true;
        } else if (arg == "-h" || arg == "--help") {
            return false;
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            return false;
        }
    }

    opt.width = std::max(16, opt.width);
    opt.height = std::max(16, opt.height);
    opt.quality = std::clamp(opt.quality, 1, 100);
    opt.min_quality = std::clamp(opt.min_quality, 1, opt.quality);
    opt.max_packet = std::clamp(opt.max_packet, 1200, 65507);
    opt.buffers = std::clamp(opt.buffers, 2, 16);
    return !opt.host.empty() && opt.port > 0 && opt.port <= 65535;
}

cv::Rect center_rect(int src_w, int src_h, int width, int height) {
    int crop_w = std::min(width, src_w);
    int crop_h = std::min(height, src_h);
    int x = std::max(0, (src_w - crop_w) / 2);
    int y = std::max(0, (src_h - crop_h) / 2);
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

void print_dv_timings(const char* label, const v4l2_dv_timings& timings) {
    if (timings.type == V4L2_DV_BT_656_1120) {
        const auto& bt = timings.bt;
        int total_width = static_cast<int>(bt.width + bt.hfrontporch + bt.hsync + bt.hbackporch);
        int total_height = static_cast<int>(bt.height + bt.vfrontporch + bt.vsync + bt.vbackporch);
        double fps = 0.0;
        if (total_width > 0 && total_height > 0) {
            fps = static_cast<double>(bt.pixelclock) / static_cast<double>(total_width) / static_cast<double>(total_height);
        }
        std::cout << label << ": "
                  << bt.width << "x" << bt.height
                  << " total=" << total_width << "x" << total_height
                  << " pixelclock=" << bt.pixelclock
                  << " approx_fps=" << fps
                  << "\n";
    } else {
        std::cout << label << ": type=" << timings.type << "\n";
    }
}

void setup_input_if_available(int fd) {
    int input = 0;
    if (xioctl(fd, VIDIOC_G_INPUT, &input) == 0) {
        std::cout << "Current V4L2 input: " << input << "\n";
    } else {
        std::cerr << "VIDIOC_G_INPUT skipped: " << std::strerror(errno) << "\n";
        input = 0;
    }

    v4l2_input in{};
    in.index = static_cast<uint32_t>(std::max(0, input));
    if (xioctl(fd, VIDIOC_ENUMINPUT, &in) == 0) {
        std::cout << "Input[" << in.index << "]: " << reinterpret_cast<const char*>(in.name)
                  << " type=" << in.type
                  << " status=0x" << std::hex << in.status
                  << " capabilities=0x" << in.capabilities << std::dec
                  << "\n";
    }

    if (xioctl(fd, VIDIOC_S_INPUT, &input) == 0) {
        std::cout << "V4L2 input set: " << input << "\n";
    } else {
        std::cerr << "VIDIOC_S_INPUT skipped: " << std::strerror(errno) << "\n";
    }
}

bool setup_format(int fd, Options& opt, int& bytes_per_line, int& num_planes) {
    v4l2_format fmt{};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    fmt.fmt.pix_mp.width = opt.src_width;
    fmt.fmt.pix_mp.height = opt.src_height;
    fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_BGR24;
    fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
    fmt.fmt.pix_mp.num_planes = 1;

    if (xioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
        std::cerr << "VIDIOC_S_FMT BGR3 failed: " << std::strerror(errno) << "\n";
        return false;
    }

    opt.src_width = static_cast<int>(fmt.fmt.pix_mp.width);
    opt.src_height = static_cast<int>(fmt.fmt.pix_mp.height);
    num_planes = std::max(1, static_cast<int>(fmt.fmt.pix_mp.num_planes));
    bytes_per_line = static_cast<int>(fmt.fmt.pix_mp.plane_fmt[0].bytesperline);
    if (bytes_per_line <= 0) {
        bytes_per_line = opt.src_width * 3;
    }

    std::cout << "V4L2 format: "
              << opt.src_width << "x" << opt.src_height
              << " BGR3 planes=" << num_planes
              << " stride=" << bytes_per_line
              << " sizeimage=" << fmt.fmt.pix_mp.plane_fmt[0].sizeimage
              << "\n";
    return true;
}

void setup_dv_timings_if_available(int fd) {
    v4l2_dv_timings current{};
    bool have_current = false;
    if (xioctl(fd, VIDIOC_G_DV_TIMINGS, &current) == 0) {
        print_dv_timings("Current DV timings", current);
        have_current = true;
    } else {
        std::cerr << "VIDIOC_G_DV_TIMINGS skipped: " << std::strerror(errno) << "\n";
    }

    v4l2_dv_timings queried{};
    if (xioctl(fd, VIDIOC_QUERY_DV_TIMINGS, &queried) < 0) {
        std::cerr << "VIDIOC_QUERY_DV_TIMINGS skipped: " << std::strerror(errno) << "\n";
        return;
    }
    print_dv_timings("Queried DV timings", queried);

    // Some Rockchip HDMI RX kernels reject S_DV_TIMINGS even when G/QUERY work.
    // If current timings already match the queried signal, keep the current mode.
    if (have_current && std::memcmp(&current, &queried, sizeof(v4l2_dv_timings)) == 0) {
        std::cout << "DV timings already active.\n";
        return;
    }

    if (xioctl(fd, VIDIOC_S_DV_TIMINGS, &queried) < 0) {
        std::cerr << "VIDIOC_S_DV_TIMINGS failed: " << std::strerror(errno) << "\n";
    } else {
        print_dv_timings("DV timings set", queried);
    }
}

void try_set_fps(int fd, const Options& opt) {
    v4l2_streamparm parm{};
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    parm.parm.capture.timeperframe.numerator = 1;
    parm.parm.capture.timeperframe.denominator = opt.src_fps;
    if (xioctl(fd, VIDIOC_S_PARM, &parm) == 0) {
        auto n = parm.parm.capture.timeperframe.numerator;
        auto d = parm.parm.capture.timeperframe.denominator;
        if (n != 0) {
            std::cout << "V4L2 timeperframe: " << n << "/" << d << "\n";
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parse_args(argc, argv, opt)) {
        usage(argv[0]);
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

    int open_flags = O_RDWR;
    if (!opt.blocking) {
        open_flags |= O_NONBLOCK;
    }
    int fd = ::open(opt.device.c_str(), open_flags, 0);
    if (fd < 0) {
        std::cerr << "open(" << opt.device << ") failed: " << std::strerror(errno) << "\n";
        ::close(sock);
        return 1;
    }

    v4l2_capability cap{};
    if (xioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
        std::cerr << "VIDIOC_QUERYCAP failed: " << std::strerror(errno) << "\n";
        ::close(fd);
        ::close(sock);
        return 1;
    }
    uint32_t caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;
    if (!(caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE)) {
        std::cerr << "Device does not report VIDEO_CAPTURE_MPLANE. device_caps=0x"
                  << std::hex << cap.device_caps
                  << " capabilities=0x" << cap.capabilities << std::dec << "\n";
        ::close(fd);
        ::close(sock);
        return 1;
    }
    if (!(caps & V4L2_CAP_STREAMING)) {
        std::cerr << "Device does not report STREAMING mmap support. device_caps=0x"
                  << std::hex << cap.device_caps
                  << " capabilities=0x" << cap.capabilities << std::dec << "\n";
        ::close(fd);
        ::close(sock);
        return 1;
    }

    int stride = 0;
    int num_planes = 1;
    setup_input_if_available(fd);
    setup_dv_timings_if_available(fd);
    if (!setup_format(fd, opt, stride, num_planes)) {
        ::close(fd);
        ::close(sock);
        return 1;
    }
    try_set_fps(fd, opt);

    v4l2_requestbuffers req{};
    req.count = opt.buffers;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
        std::cerr << "VIDIOC_REQBUFS failed: " << std::strerror(errno) << "\n";
        ::close(fd);
        ::close(sock);
        return 1;
    }
    if (req.count < 2) {
        std::cerr << "Insufficient V4L2 buffers.\n";
        ::close(fd);
        ::close(sock);
        return 1;
    }

    std::vector<MappedBuffer> buffers(req.count);
    for (uint32_t i = 0; i < req.count; ++i) {
        v4l2_plane planes[VIDEO_MAX_PLANES]{};
        v4l2_buffer buf{};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        buf.length = num_planes;
        buf.m.planes = planes;
        if (xioctl(fd, VIDIOC_QUERYBUF, &buf) < 0) {
            std::cerr << "VIDIOC_QUERYBUF failed: " << std::strerror(errno) << "\n";
            return 1;
        }

        buffers[i].length = planes[0].length;
        buffers[i].start = mmap(nullptr, planes[0].length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, planes[0].m.mem_offset);
        if (buffers[i].start == MAP_FAILED) {
            std::cerr << "mmap failed: " << std::strerror(errno) << "\n";
            return 1;
        }

        if (xioctl(fd, VIDIOC_QBUF, &buf) < 0) {
            std::cerr << "VIDIOC_QBUF failed: " << std::strerror(errno) << "\n";
            return 1;
        }
    }

    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (xioctl(fd, VIDIOC_STREAMON, &type) < 0) {
        std::cerr << "VIDIOC_STREAMON failed: " << std::strerror(errno) << "\n";
        return 1;
    }

    std::cout << "Streaming V4L2 mmap center " << opt.width << "x" << opt.height
              << " JPEG UDP to " << opt.host << ":" << opt.port
              << " quality=" << opt.quality
              << " max_packet=" << opt.max_packet
              << (opt.blocking ? " blocking=1" : "")
              << (opt.swap_rb ? " swap_rb=1" : "")
              << "\n";

    cv::Rect roi = center_rect(opt.src_width, opt.src_height, opt.width, opt.height);
    cv::Mat crop;
    cv::Mat encoded_source;
    std::vector<uchar> jpeg;
    using clock = std::chrono::steady_clock;
    auto stat_time = clock::now();
    int captured = 0;
    int sent = 0;
    int dropped_big = 0;
    int last_quality = opt.quality;
    size_t last_size = 0;

    while (true) {
        if (!opt.blocking) {
            pollfd pfd{};
            pfd.fd = fd;
            pfd.events = POLLIN;
            int pr = poll(&pfd, 1, 1000);
            if (pr < 0) {
                if (errno == EINTR) continue;
                std::cerr << "poll failed: " << std::strerror(errno) << "\n";
                break;
            }
            if (pr == 0) {
                std::cerr << "poll timeout waiting for HDMI RX frame\n";
                continue;
            }
        }

        v4l2_plane planes[VIDEO_MAX_PLANES]{};
        v4l2_buffer buf{};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.length = num_planes;
        buf.m.planes = planes;
        if (xioctl(fd, VIDIOC_DQBUF, &buf) < 0) {
            if (errno == EAGAIN) continue;
            std::cerr << "VIDIOC_DQBUF failed: " << std::strerror(errno) << "\n";
            break;
        }

        ++captured;
        cv::Mat frame(opt.src_height, opt.src_width, CV_8UC3, buffers[buf.index].start, stride);
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
        } else {
            ssize_t n = ::sendto(sock, jpeg.data(), jpeg.size(), 0, reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
            if (n < 0) {
                std::cerr << "sendto failed: " << std::strerror(errno) << "\n";
                break;
            }
            ++sent;
        }

        if (xioctl(fd, VIDIOC_QBUF, &buf) < 0) {
            std::cerr << "VIDIOC_QBUF requeue failed: " << std::strerror(errno) << "\n";
            break;
        }

        auto now = clock::now();
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

    xioctl(fd, VIDIOC_STREAMOFF, &type);
    for (auto& b : buffers) {
        if (b.start && b.start != MAP_FAILED) {
            munmap(b.start, b.length);
        }
    }
    ::close(fd);
    ::close(sock);
    return 0;
}
