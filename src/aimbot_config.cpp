#include "aimbot_config.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>

ConfigManager cfg;

static std::string url_decode(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (size_t i = 0; i < value.size(); i++) {
        if (value[i] == '%' && i + 2 < value.size()
            && std::isxdigit(static_cast<unsigned char>(value[i + 1]))
            && std::isxdigit(static_cast<unsigned char>(value[i + 2]))) {
            char hex[3] = {value[i + 1], value[i + 2], '\0'};
            out.push_back(static_cast<char>(std::strtol(hex, nullptr, 16)));
            i += 2;
        } else if (value[i] == '+') {
            out.push_back(' ');
        } else {
            out.push_back(value[i]);
        }
    }
    return out;
}

ConfigManager::ConfigManager() {
    ensure_class_defaults(num_classes);
}

static float default_height_ratio(int cls) {
    return (cls == 1 || cls == 7) ? 0.70f : 0.30f;
}

static int default_priority(int cls) {
    return (cls == 1 || cls == 7) ? 0 : 10;
}

void ConfigManager::ensure_class_defaults(int count) {
    for (int i = 0; i < count; i++) {
        if (!cls_cfg.count(i)) {
            cls_cfg[i] = {true, default_height_ratio(i), 0, default_priority(i)};
        }
    }
}

void ConfigManager::parse_payload(const std::string& payload) {
    std::lock_guard<std::mutex> lock(mtx);
    std::stringstream ss(payload);
    std::string item;
    while (std::getline(ss, item, ';')) {
        size_t pos = item.find('=');
        if (pos == std::string::npos) {
            continue;
        }
        std::string k = item.substr(0, pos);
        std::string v = item.substr(pos + 1);
        try {
            if (k.rfind("label", 0) == 0 && k.size() > 5) {
                int cid = std::stoi(k.substr(5));
                std::string name = url_decode(v);
                if (name.empty()) {
                    class_names.erase(cid);
                } else {
                    class_names[cid] = name;
                }
                continue;
            }

            float val = std::stof(v);
            if (k == "kp_x") Kp_x = val;
            else if (k == "kd_x") Kd_x = val;
            else if (k == "kp_y") Kp_y = val;
            else if (k == "kd_y") Kd_y = val;
            else if (k == "recoil") recoil_y_mult = val;
            else if (k == "max_step") max_step = val;
            else if (k == "deadzone") deadzone = std::clamp(val, 0.0f, 30.0f);
            else if (k == "aim_slow_radius") aim_slow_radius = std::clamp(val, 0.0f, 160.0f);
            else if (k == "aim_slow_min_scale") aim_slow_min_scale = std::clamp(val, 0.05f, 1.0f);
            else if (k == "pred_en") pred_en = (val > 0.5f);
            else if (k == "pred_lead_ms") pred_lead_ms = std::max(0.0f, val);
            else if (k == "pred_vel_smooth") pred_vel_smooth = std::clamp(val, 0.0f, 1.0f);
            else if (k == "pred_pos_smooth") pred_pos_smooth = std::clamp(val, 0.0f, 1.0f);
            else if (k == "pred_max_speed") pred_max_speed = std::max(1.0f, val);
            else if (k == "conf_thres") conf_thres = std::clamp(val, 0.01f, 0.95f);
            else if (k == "nms_iou") nms_iou = std::clamp(val, 0.01f, 0.95f);
            else if (k == "num_classes") {
                num_classes = std::max(1, static_cast<int>(val));
                ensure_class_defaults(num_classes);
            }
            else if (k == "model_family") model_family = static_cast<int>(val);
            else if (k == "aim_mask") aim_mask = std::clamp(static_cast<int>(val), 1, 31);
            else if (k == "trigger_mask") trigger_mask = std::clamp(static_cast<int>(val), 0, 31);
            else if (k == "aim_fov") aim_fov = std::clamp(val, 40.0f, 230.0f);
            else if (k == "trigger_radius") trigger_radius = std::clamp(val, 1.0f, 60.0f);
            else if (k == "trigger_delay_ms") trigger_delay_ms = std::clamp(val, 0.0f, 500.0f);
            else if (k == "preview_en") preview_en = (val > 0.5f);
            else if (k == "preview_fps") preview_fps = std::clamp(val, 5.0f, 30.0f);
            else if (k == "xhair_en") xhair_en = (val > 0.5f);
            else if (k == "xhair_c") xhair_color = static_cast<int>(val);
            else if (k == "xhair_h_min") xhair_h_min = std::clamp(static_cast<int>(val), 0, 180);
            else if (k == "xhair_h_max") xhair_h_max = std::clamp(static_cast<int>(val), 0, 180);
            else if (k == "xhair_s_min") xhair_s_min = std::clamp(static_cast<int>(val), 0, 255);
            else if (k == "xhair_s_max") xhair_s_max = std::clamp(static_cast<int>(val), 0, 255);
            else if (k == "xhair_v_min") xhair_v_min = std::clamp(static_cast<int>(val), 0, 255);
            else if (k == "xhair_v_max") xhair_v_max = std::clamp(static_cast<int>(val), 0, 255);
            else if (k.rfind("c", 0) == 0) {
                size_t split = k.find('_');
                if (split != std::string::npos && split > 1) {
                    int cid = std::stoi(k.substr(1, split - 1));
                    std::string prop = k.substr(split + 1);
                    if (!cls_cfg.count(cid)) {
                        cls_cfg[cid] = {true, default_height_ratio(cid), 0, default_priority(cid)};
                    }
                    if (prop == "en") cls_cfg[cid].enabled = (val > 0.5f);
                    else if (prop == "hr") cls_cfg[cid].height_ratio = val;
                    else if (prop == "py") cls_cfg[cid].pixel_offset = static_cast<int>(val);
                    else if (prop == "pri") cls_cfg[cid].priority = std::clamp(static_cast<int>(val), 0, 99);
                }
            }
        } catch (...) {
        }
    }
}
