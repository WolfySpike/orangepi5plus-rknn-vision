#include "aimbot_camera.h"

#include <algorithm>
#include <string>

void CameraThread::update() {
    cv::Mat frame;
    while (running) {
        if (cap.read(frame) && !frame.empty()) {
            int cx = frame.cols / 2;
            int cy = frame.rows / 2;
            int x1 = std::max(0, std::min(cx - 160, frame.cols - 320));
            int y1 = std::max(0, std::min(cy - 160, frame.rows - 320));
            cv::Mat small_crop = frame(cv::Rect(x1, y1, 320, 320)).clone();
            std::lock_guard<std::mutex> lock(mtx);
            current_crop = small_crop;
            frame_id++;
        }
    }
}

CameraThread::CameraThread() {
    std::string pipeline = "v4l2src device=/dev/video0 ! video/x-raw,format=BGR,width=1920,height=1080,framerate=240/1 ! appsink drop=true max-buffers=1 sync=false";
    cap.open(pipeline, cv::CAP_GSTREAMER);
    if (cap.isOpened()) {
        running = true;
        worker_thread = std::thread(&CameraThread::update, this);
    }
}

CameraThread::~CameraThread() {
    running = false;
    if (worker_thread.joinable()) {
        worker_thread.join();
    }
    if (cap.isOpened()) {
        cap.release();
    }
}

bool CameraThread::read(cv::Mat& out_crop, uint64_t& out_id) {
    std::lock_guard<std::mutex> lock(mtx);
    if (frame_id > 0 && !current_crop.empty()) {
        out_crop = current_crop.clone();
        out_id = frame_id;
        return true;
    }
    return false;
}

bool CameraThread::isOpened() {
    return cap.isOpened();
}
