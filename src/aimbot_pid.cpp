#include "aimbot_pid.h"

#include <algorithm>
#include <cmath>
#include <mutex>

#include "aimbot_config.h"

void PIDController::compute(float err_x, float err_y, int& move_x, int& move_y) {
    std::lock_guard<std::mutex> lock(cfg.mtx);
    float deadzone = std::clamp(cfg.deadzone, 0.0f, 30.0f);
    if (std::abs(err_x) <= deadzone && std::abs(err_y) <= deadzone) {
        reset();
        move_x = 0;
        move_y = 0;
        return;
    }

    float current_kp_y = cfg.Kp_y;
    if (err_y > 0) {
        current_kp_y = cfg.Kp_y * cfg.recoil_y_mult;
    }

    float p_x = cfg.Kp_x * err_x;
    float p_y = current_kp_y * err_y;
    float d_x = cfg.Kd_x * (err_x - prev_err_x);
    float d_y = cfg.Kd_y * (err_y - prev_err_y);

    prev_err_x = err_x;
    prev_err_y = err_y;

    float slow_scale = 1.0f;
    float slow_radius = std::clamp(cfg.aim_slow_radius, 0.0f, 160.0f);
    float slow_min_scale = std::clamp(cfg.aim_slow_min_scale, 0.05f, 1.0f);
    float dist = std::hypot(err_x, err_y);
    if (slow_radius > deadzone && dist < slow_radius) {
        float t = std::clamp((dist - deadzone) / (slow_radius - deadzone), 0.0f, 1.0f);
        float smooth = t * t * (3.0f - 2.0f * t);
        slow_scale = slow_min_scale + (1.0f - slow_min_scale) * smooth;
    }

    float unclamped_x = (p_x + d_x) * slow_scale + rem_x;
    float unclamped_y = (p_y + d_y) * slow_scale + rem_y;

    float out_x = std::clamp(unclamped_x, -cfg.max_step, cfg.max_step);
    float out_y = std::clamp(unclamped_y, -cfg.max_step, cfg.max_step);

    move_x = static_cast<int>(out_x);
    move_y = static_cast<int>(out_y);
    rem_x = out_x - move_x;
    rem_y = out_y - move_y;
}

void PIDController::reset() {
    prev_err_x = 0.0f;
    prev_err_y = 0.0f;
    rem_x = 0.0f;
    rem_y = 0.0f;
}
