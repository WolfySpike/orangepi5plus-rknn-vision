#pragma once

#include <chrono>

#include <opencv2/opencv.hpp>

class PredictionTracker {
private:
    bool initialized = false;
    cv::Point2f last_raw_pt = cv::Point2f(0.0f, 0.0f);
    cv::Point2f filtered_vel = cv::Point2f(0.0f, 0.0f);
    cv::Point2f smoothed_pred_pt = cv::Point2f(0.0f, 0.0f);
    std::chrono::steady_clock::time_point last_ts;

public:
    cv::Point2f predict(const cv::Point2f& raw_pt);
    void reset();
};
