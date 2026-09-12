#include "aimbot_crosshair.h"

#include <algorithm>
#include <mutex>
#include <vector>

#include "aimbot_config.h"

cv::Mat build_crosshair_mask(const cv::Mat& bgr_roi) {
    int h_min;
    int h_max;
    int s_min;
    int s_max;
    int v_min;
    int v_max;
    {
        std::lock_guard<std::mutex> lock(cfg.mtx);
        h_min = cfg.xhair_h_min;
        h_max = cfg.xhair_h_max;
        s_min = cfg.xhair_s_min;
        s_max = cfg.xhair_s_max;
        v_min = cfg.xhair_v_min;
        v_max = cfg.xhair_v_max;
    }

    cv::Mat hsv;
    cv::cvtColor(bgr_roi, hsv, cv::COLOR_BGR2HSV);
    cv::Mat mask;

    h_min = std::clamp(h_min, 0, 180);
    h_max = std::clamp(h_max, 0, 180);
    s_min = std::clamp(s_min, 0, 255);
    s_max = std::clamp(s_max, 0, 255);
    v_min = std::clamp(v_min, 0, 255);
    v_max = std::clamp(v_max, 0, 255);
    if (s_min > s_max) std::swap(s_min, s_max);
    if (v_min > v_max) std::swap(v_min, v_max);

    if (h_min <= h_max) {
        cv::inRange(hsv, cv::Scalar(h_min, s_min, v_min), cv::Scalar(h_max, s_max, v_max), mask);
    } else {
        // Hue wraps at red: e.g. 170..10 means [170,180] OR [0,10].
        cv::Mat m1, m2;
        cv::inRange(hsv, cv::Scalar(0, s_min, v_min), cv::Scalar(h_max, s_max, v_max), m1);
        cv::inRange(hsv, cv::Scalar(h_min, s_min, v_min), cv::Scalar(180, s_max, v_max), m2);
        mask = m1 | m2;
    }

    return mask;
}

void find_dynamic_crosshair(const cv::Mat& bgr_crop, float& cx, float& cy) {
    bool en;
    {
        std::lock_guard<std::mutex> lock(cfg.mtx);
        en = cfg.xhair_en;
    }

    if (!en) {
        cx = 160.0f;
        cy = 160.0f;
        return;
    }

    cv::Rect roi(120, 120, 80, 80);
    cv::Mat mask = build_crosshair_mask(bgr_crop(roi));

    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(mask, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
    if (!contours.empty()) {
        auto largest = std::max_element(
            contours.begin(),
            contours.end(),
            [](const std::vector<cv::Point>& a, const std::vector<cv::Point>& b) {
                return cv::contourArea(a) < cv::contourArea(b);
            }
        );
        if (cv::contourArea(*largest) >= 2.0) {
            auto m = cv::moments(*largest);
            if (m.m00 != 0) {
                cx = 120.0f + static_cast<float>(m.m10 / m.m00);
                cy = 120.0f + static_cast<float>(m.m01 / m.m00);
                return;
            }
        }
    }

    cx = 160.0f;
    cy = 160.0f;
}
