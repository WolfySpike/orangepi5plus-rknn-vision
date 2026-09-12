#pragma once

#include <opencv2/opencv.hpp>

cv::Mat build_crosshair_mask(const cv::Mat& bgr_roi);
void find_dynamic_crosshair(const cv::Mat& bgr_crop, float& cx, float& cy);
