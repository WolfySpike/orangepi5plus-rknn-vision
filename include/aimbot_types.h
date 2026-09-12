#pragma once

#include <opencv2/opencv.hpp>

struct ClassConfig {
    bool enabled;
    float height_ratio;
    int pixel_offset;
    int priority;
};

struct DetectResult {
    int class_id;
    float conf;
    cv::Rect_<float> box;
};

struct TargetPoint {
    cv::Point2f pt;
    cv::Rect_<float> box;
    int cls;
    int priority;
    float conf;
};
