#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>

#include <opencv2/opencv.hpp>

class CameraThread {
private:
    cv::VideoCapture cap;
    cv::Mat current_crop;
    std::mutex mtx;
    std::atomic<bool> running{false};
    std::thread worker_thread;
    uint64_t frame_id = 0;

    void update();

public:
    CameraThread();
    ~CameraThread();

    bool read(cv::Mat& out_crop, uint64_t& out_id);
    bool isOpened();
};
