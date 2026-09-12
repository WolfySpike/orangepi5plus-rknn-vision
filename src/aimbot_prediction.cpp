#include "aimbot_prediction.h"

#include <algorithm>
#include <cmath>
#include <mutex>

#include "aimbot_config.h"

cv::Point2f PredictionTracker::predict(const cv::Point2f& raw_pt) {
    bool pred_en = false;
    float pred_lead_ms = 0.0f;
    float pred_vel_smooth = 0.0f;
    float pred_pos_smooth = 0.0f;
    float pred_max_speed = 1.0f;
    {
        std::lock_guard<std::mutex> lock(cfg.mtx);
        pred_en = cfg.pred_en;
        pred_lead_ms = cfg.pred_lead_ms;
        pred_vel_smooth = cfg.pred_vel_smooth;
        pred_pos_smooth = cfg.pred_pos_smooth;
        pred_max_speed = cfg.pred_max_speed;
    }

    if (!pred_en) {
        reset();
        return raw_pt;
    }

    auto now = std::chrono::steady_clock::now();
    if (!initialized) {
        initialized = true;
        last_raw_pt = raw_pt;
        filtered_vel = cv::Point2f(0.0f, 0.0f);
        smoothed_pred_pt = raw_pt;
        last_ts = now;
        return raw_pt;
    }

    float dt = std::chrono::duration<float>(now - last_ts).count();
    if (dt <= 0.0001f || dt > 0.2f) {
        last_raw_pt = raw_pt;
        smoothed_pred_pt = raw_pt;
        last_ts = now;
        return raw_pt;
    }

    cv::Point2f inst_vel((raw_pt.x - last_raw_pt.x) / dt, (raw_pt.y - last_raw_pt.y) / dt);
    float speed = std::hypot(inst_vel.x, inst_vel.y);
    if (speed > pred_max_speed && speed > 0.0001f) {
        float scale = pred_max_speed / speed;
        inst_vel.x *= scale;
        inst_vel.y *= scale;
    }

    float vel_alpha = std::clamp(pred_vel_smooth, 0.0f, 1.0f);
    filtered_vel = filtered_vel * (1.0f - vel_alpha) + inst_vel * vel_alpha;

    float lead_s = std::max(0.0f, pred_lead_ms) * 0.001f;
    cv::Point2f predicted(raw_pt.x + filtered_vel.x * lead_s, raw_pt.y + filtered_vel.y * lead_s);

    float pos_alpha = std::clamp(pred_pos_smooth, 0.0f, 1.0f);
    smoothed_pred_pt = smoothed_pred_pt * (1.0f - pos_alpha) + predicted * pos_alpha;

    last_raw_pt = raw_pt;
    last_ts = now;
    return smoothed_pred_pt;
}

void PredictionTracker::reset() {
    initialized = false;
    last_raw_pt = cv::Point2f(0.0f, 0.0f);
    filtered_vel = cv::Point2f(0.0f, 0.0f);
    smoothed_pred_pt = cv::Point2f(0.0f, 0.0f);
}
