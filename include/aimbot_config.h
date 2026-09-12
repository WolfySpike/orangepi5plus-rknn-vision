#pragma once

#include <mutex>
#include <string>
#include <unordered_map>

#include "aimbot_types.h"

class ConfigManager {
public:
    std::mutex mtx;
    float Kp_x = 0.25f;
    float Kd_x = 0.10f;
    float Kp_y = 0.25f;
    float Kd_y = 0.10f;
    float recoil_y_mult = 2.0f;
    float max_step = 10.0f;
    float deadzone = 3.0f;
    float aim_slow_radius = 45.0f;
    float aim_slow_min_scale = 0.35f;
    bool pred_en = true;
    float pred_lead_ms = 28.0f;
    float pred_vel_smooth = 0.35f;
    float pred_pos_smooth = 0.55f;
    float pred_max_speed = 550.0f;
    float conf_thres = 0.20f;
    float nms_iou = 0.30f;
    int num_classes = 9;
    int model_family = 8;  // 8 / 11 / 26
    int aim_mask = 0x03;   // bit0:left bit1:right bit2:middle bit3:side_up bit4:side_down
    int trigger_mask = 0x04;
    float aim_fov = 180.0f;
    float trigger_radius = 8.0f;
    float trigger_delay_ms = 0.0f;
    bool preview_en = false;
    float preview_fps = 20.0f;
    bool xhair_en = false;
    int xhair_color = 0;
    int xhair_h_min = 170;
    int xhair_h_max = 10;
    int xhair_s_min = 120;
    int xhair_s_max = 255;
    int xhair_v_min = 120;
    int xhair_v_max = 255;
    std::unordered_map<int, ClassConfig> cls_cfg;
    std::unordered_map<int, std::string> class_names;

    ConfigManager();
    void ensure_class_defaults(int count);
    void parse_payload(const std::string& payload);
};

extern ConfigManager cfg;
