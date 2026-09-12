#include <arpa/inet.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>

#include "aimbot_core.h"
#include "mist_kmbox.h"

static std::string env_or_default(const char* name, const char* fallback) {
    const char* value = std::getenv(name);
    return value && *value ? std::string(value) : std::string(fallback);
}

int main(int argc, char** argv) {
    std::string model_path = "../sjzdawan.rknn";
    int initial_num_classes = 9;
    int initial_model_family = 8;
    int npu_core_count = 3;
    bool kmbox_test_buttons = false;
    bool kmbox_test_monitor = false;
    bool kmbox_test_raw_buttons = false;
    const std::string kmbox_host = env_or_default("KMBOX_HOST", "127.0.0.1");
    const int kmbox_port = std::max(1, std::atoi(env_or_default("KMBOX_PORT", "26947").c_str()));
    const std::string kmbox_key = env_or_default("KMBOX_KEY", "CHANGE_ME");
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--model" && i + 1 < argc) model_path = argv[++i];
        else if (arg == "--classes" && i + 1 < argc) initial_num_classes = std::max(1, std::atoi(argv[++i]));
        else if (arg == "--family" && i + 1 < argc) initial_model_family = std::max(1, std::atoi(argv[++i]));
        else if (arg == "--npu-cores" && i + 1 < argc) npu_core_count = std::clamp(std::atoi(argv[++i]), 1, 3);
        else if (arg == "--kmbox-test-buttons") kmbox_test_buttons = true;
        else if (arg == "--kmbox-test-monitor") kmbox_test_monitor = true;
        else if (arg == "--kmbox-test-raw-buttons") kmbox_test_raw_buttons = true;
    }
    {
        std::lock_guard<std::mutex> lock(cfg.mtx);
        cfg.num_classes = initial_num_classes;
        cfg.model_family = initial_model_family;
        cfg.ensure_class_defaults(cfg.num_classes);
    }

    std::cout << "========== Mist AI Turbo C++ (Universal Config Engine) ==========" << std::endl;
    if (kmbox_test_buttons) {
        MistKmbox test_kmbox(kmbox_host, kmbox_port, kmbox_key);
        std::cout << "[KMBTEST] right down 900ms" << std::endl;
        test_kmbox.set_button_mask(0x02);
        std::this_thread::sleep_for(std::chrono::milliseconds(900));
        std::cout << "[KMBTEST] left+right down 180ms" << std::endl;
        test_kmbox.set_button_mask(0x03);
        std::this_thread::sleep_for(std::chrono::milliseconds(180));
        std::cout << "[KMBTEST] release left, keep right 500ms" << std::endl;
        test_kmbox.set_button_mask(0x02);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        std::cout << "[KMBTEST] release all" << std::endl;
        test_kmbox.set_button_mask(0x00);
        return 0;
    }
    if (kmbox_test_monitor) {
        MistKmbox test_kmbox(kmbox_host, kmbox_port, kmbox_key);
        test_kmbox.start_monitor(12345);
        std::cout << "[KMBTEST] press mouse buttons for 10 seconds" << std::endl;
        for (int i = 0; i < 100; ++i) {
            uint8_t mask = test_kmbox.get_buttons_mask();
            std::cout << "\r[KMBTEST] hw=0x" << std::hex << static_cast<int>(mask) << std::dec << "   " << std::flush;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        std::cout << std::endl;
        return 0;
    }
    if (kmbox_test_raw_buttons) {
        MistKmbox test_kmbox(kmbox_host, kmbox_port, kmbox_key);
        std::cout << "[KMBTEST] raw cmd A 0x9823AE8D down 700ms" << std::endl;
        test_kmbox.raw_button_cmd(0x9823AE8D, true);
        std::this_thread::sleep_for(std::chrono::milliseconds(700));
        test_kmbox.raw_button_cmd(0x9823AE8D, false);
        std::this_thread::sleep_for(std::chrono::milliseconds(700));
        std::cout << "[KMBTEST] raw cmd B 0x238d8212 down 700ms" << std::endl;
        test_kmbox.raw_button_cmd(0x238d8212, true);
        std::this_thread::sleep_for(std::chrono::milliseconds(700));
        test_kmbox.raw_button_cmd(0x238d8212, false);
        std::this_thread::sleep_for(std::chrono::milliseconds(700));
        std::cout << "[KMBTEST] raw legacy mouse button=1 down 700ms" << std::endl;
        test_kmbox.raw_mouse_button_mask(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(700));
        test_kmbox.raw_mouse_button_mask(0);
        std::this_thread::sleep_for(std::chrono::milliseconds(700));
        std::cout << "[KMBTEST] raw legacy mouse button=2 down 700ms" << std::endl;
        test_kmbox.raw_mouse_button_mask(2);
        std::this_thread::sleep_for(std::chrono::milliseconds(700));
        test_kmbox.raw_mouse_button_mask(0);
        std::cout << "[KMBTEST] raw test done" << std::endl;
        return 0;
    }

    std::thread config_listener([]() {
        int sock = socket(AF_INET, SOCK_DGRAM, 0); struct sockaddr_in addr; addr.sin_family = AF_INET; addr.sin_port = htons(9999); addr.sin_addr.s_addr = INADDR_ANY; bind(sock, (struct sockaddr*)&addr, sizeof(addr));
        char buf[8192]; while (true) { int n = recvfrom(sock, buf, sizeof(buf)-1, 0, nullptr, nullptr); if (n > 0) { buf[n] = '\0'; cfg.parse_payload(std::string(buf)); } }
    }); config_listener.detach();

    CameraThread cam; if (!cam.isOpened()) return -1;
    RKNNMultiCoreEngine engine(model_path, npu_core_count);
    MistKmbox kmbox(kmbox_host, kmbox_port, kmbox_key); kmbox.start_monitor(12345);
    PIDController pid;
    PredictionTracker predictor;
    int tele_sock = socket(AF_INET, SOCK_DGRAM, 0); struct sockaddr_in tele_addr; tele_addr.sin_family = AF_INET; tele_addr.sin_port = htons(9998); inet_pton(AF_INET, "127.0.0.1", &tele_addr.sin_addr);

    float max_fov = 180.0f; float sticky_tolerance = 70.0f; 
    bool was_aiming_last_frame = false; cv::Point2f last_locked_pos(0, 0); cv::Rect2f last_locked_box;
    int last_locked_cls = -1; int last_locked_priority = 1000000; int lost_target_frames = 0;
    uint64_t last_pushed_id = 0, last_processed_id = 0; auto start_time = std::chrono::steady_clock::now();
    auto last_preview_time = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    bool trigger_fire_held = false;
    int trigger_virtual_mask = 0;
    auto last_trigger_button_send = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    bool last_trigger_down_log = false;
    int last_trigger_hw_log = -1;
    int last_trigger_cfg_log = -1;
    int trigger_lost_frames = 0;
    auto trigger_condition_since = std::chrono::steady_clock::time_point{};
    auto last_trigger_safety_release = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    float display_conf = 0.0f; int display_cls = 0; double display_infer_time = 0.0; int display_core = 0;
    auto configured_label = [](int cls) {
        std::lock_guard<std::mutex> lock(cfg.mtx);
        auto it = cfg.class_names.find(cls);
        if (it != cfg.class_names.end() && !it->second.empty()) {
            return it->second;
        }
        return std::string("ID:") + std::to_string(cls);
    };
    auto box_iou = [](const cv::Rect2f& a, const cv::Rect2f& b) {
        cv::Rect2f inter = a & b;
        float inter_area = std::max(0.0f, inter.width) * std::max(0.0f, inter.height);
        float union_area = a.area() + b.area() - inter_area;
        return union_area > 0.0f ? inter_area / union_area : 0.0f;
    };
    auto expanded_contains = [](cv::Rect2f box, const cv::Point2f& pt, float pad) {
        box.x -= pad;
        box.y -= pad;
        box.width += pad * 2.0f;
        box.height += pad * 2.0f;
        return box.contains(pt);
    };
    auto apply_trigger_button_mask = [&](int mask) {
        mask &= 0x03; // left fire + right scope only
        auto now = std::chrono::steady_clock::now();
        bool refresh = mask != 0
            && std::chrono::duration_cast<std::chrono::milliseconds>(now - last_trigger_button_send).count() >= 40;
        if (trigger_virtual_mask != mask || refresh) {
            kmbox.set_button_mask(mask);
            if (trigger_virtual_mask != mask) {
                std::cout << "\n[TRIGGER] virtual_buttons=0x" << std::hex << mask << std::dec << std::endl;
            }
            trigger_virtual_mask = mask;
            last_trigger_button_send = now;
        }
    };

    std::cout << "[INFO] Engine Started. Universal target tracking active.\n" << std::endl;

    while (true) {
        cv::Mat crop_img; uint64_t current_id = 0;
        if (cam.read(crop_img, current_id) && current_id != last_pushed_id) { last_pushed_id = current_id; cv::Mat rgb_img; cv::cvtColor(crop_img, rgb_img, cv::COLOR_BGR2RGB); engine.put_task(rgb_img, current_id); }

        std::vector<DetectResult> results; uint64_t res_id = 0; double infer_time = 0; int core_id = 0;
        if (engine.get_latest_result(results, res_id, infer_time, core_id) && res_id != last_processed_id) {
            last_processed_id = res_id;

            float center_x = 160.0f, center_y = 160.0f;
            find_dynamic_crosshair(crop_img, center_x, center_y);

            std::vector<TargetPoint> all_targets;
            cfg.mtx.lock(); 
            for (const auto& res : results) {
                int cid = res.class_id;
                if (cfg.cls_cfg.count(cid) && cfg.cls_cfg[cid].enabled) {
                    const ClassConfig& class_cfg = cfg.cls_cfg[cid];
                    float target_x = res.box.x + res.box.width / 2.0f;
                    float target_y = res.box.y + (res.box.height * class_cfg.height_ratio) + class_cfg.pixel_offset;
                    TargetPoint target{cv::Point2f(target_x, target_y), res.box, cid, class_cfg.priority, res.conf};
                    all_targets.push_back(target);
                }
            }
            cfg.mtx.unlock();

            bool target_found = false; TargetPoint best_target;
            int aim_mask = 0x03, trigger_mask = 0x04;
            float trigger_radius = 8.0f;
            float trigger_delay_ms = 0.0f;
            float preview_fps = 20.0f;
            bool preview_en = false;
            {
                std::lock_guard<std::mutex> lock(cfg.mtx);
                aim_mask = cfg.aim_mask;
                trigger_mask = cfg.trigger_mask;
                max_fov = cfg.aim_fov;
                trigger_radius = cfg.trigger_radius;
                trigger_delay_ms = cfg.trigger_delay_ms;
                preview_fps = cfg.preview_fps;
                preview_en = cfg.preview_en;
            }
            uint8_t btn_mask = kmbox.get_buttons_mask();
            bool is_aim_button_down = (btn_mask & (uint8_t)aim_mask) != 0;
            bool is_trigger_button_down = trigger_mask > 0 && ((btn_mask & (uint8_t)trigger_mask) != 0);
            if (is_trigger_button_down != last_trigger_down_log
                || static_cast<int>(btn_mask) != last_trigger_hw_log
                || trigger_mask != last_trigger_cfg_log) {
                std::cout << "\n[TRIGGER] hw=0x" << std::hex << static_cast<int>(btn_mask)
                          << " trigger_mask=0x" << trigger_mask << std::dec
                          << " down=" << (is_trigger_button_down ? 1 : 0) << std::endl;
                last_trigger_down_log = is_trigger_button_down;
                last_trigger_hw_log = static_cast<int>(btn_mask);
                last_trigger_cfg_log = trigger_mask;
            }
            bool is_control_down = is_aim_button_down || is_trigger_button_down;
            if (!is_control_down) {
                was_aiming_last_frame = false;
                lost_target_frames = 0;
                trigger_fire_held = false;
                trigger_lost_frames = 0;
                trigger_condition_since = std::chrono::steady_clock::time_point{};
                apply_trigger_button_mask(0);
                last_locked_cls = -1;
                last_locked_priority = 1000000;
                predictor.reset();
            }
            // Do not auto-hold right click here until the user's Kmbox firmware
            // button mapping is verified. A wrong right command behaves as
            // left-fire on some boxes.
            bool crosshair_in_any_target = false;
            for (const auto& target : all_targets) {
                cv::Rect2f fire_box = target.box;
                fire_box.x -= trigger_radius;
                fire_box.y -= trigger_radius;
                fire_box.width += trigger_radius * 2.0f;
                fire_box.height += trigger_radius * 2.0f;
                if (fire_box.contains(cv::Point2f(center_x, center_y))) {
                    crosshair_in_any_target = true;
                    break;
                }
            }

            if (is_control_down) {
                if (was_aiming_last_frame) {
                    float best_score = 1e9f;
                    for (const auto& target : all_targets) {
                        float d = std::hypot(target.pt.x - last_locked_pos.x, target.pt.y - last_locked_pos.y);
                        float iou = box_iou(target.box, last_locked_box);
                        bool same_track = expanded_contains(target.box, last_locked_pos, 18.0f)
                            || expanded_contains(target.box, cv::Point2f(center_x, center_y), 10.0f)
                            || expanded_contains(last_locked_box, target.pt, 18.0f)
                            || iou > 0.02f;
                        if (same_track || d < sticky_tolerance) {
                            // After a target is locked, continuity beats raw class priority.
                            // This lets an unstable head label fall back to the same body's box
                            // instead of jumping to another high-priority head.
                            float score = d + target.priority * 4.0f - iou * 120.0f;
                            if (same_track) {
                                score -= 100.0f;
                            }
                            if (last_locked_cls >= 0 && target.cls != last_locked_cls) {
                                score += 15.0f;
                            }
                            if (last_locked_priority < 1000000 && target.priority > last_locked_priority) {
                                score += 20.0f;
                            }
                            if (score < best_score) {
                                best_score = score;
                                best_target = target;
                                target_found = true;
                            }
                        }
                    }
                    if (target_found) {
                        lost_target_frames = 0;
                        last_locked_pos = best_target.pt;
                        last_locked_box = best_target.box;
                        last_locked_cls = best_target.cls;
                        last_locked_priority = best_target.priority;
                    } else {
                        lost_target_frames++;
                        if (lost_target_frames > 10) {
                            was_aiming_last_frame = false;
                            last_locked_cls = -1;
                            last_locked_priority = 1000000;
                        }
                    }
                }
                if (!was_aiming_last_frame) {
                    float best_score = 1e9f;
                    for (const auto& target : all_targets) {
                        float d = std::hypot(target.pt.x - center_x, target.pt.y - center_y);
                        if (d < max_fov) {
                            float score = target.priority * 1000.0f + d;
                            if (score < best_score) {
                                best_score = score;
                                best_target = target;
                                target_found = true;
                            }
                        }
                    }
                    if (target_found) {
                        was_aiming_last_frame = true;
                        last_locked_pos = best_target.pt;
                        last_locked_box = best_target.box;
                        last_locked_cls = best_target.cls;
                        last_locked_priority = best_target.priority;
                        lost_target_frames = 0;
                    }
                }
            }

            bool is_aiming = false;
            bool fire_start_condition = false;
            if (is_control_down && target_found) {
                is_aiming = true; display_cls = best_target.cls; display_conf = best_target.conf; display_infer_time = infer_time; display_core = core_id;
                cv::Point2f aim_pt = predictor.predict(best_target.pt);
                float error_x = aim_pt.x - center_x; float error_y = aim_pt.y - center_y;
                int move_x = 0, move_y = 0; pid.compute(error_x, error_y, move_x, move_y); kmbox.move(move_x, move_y);
                bool point_in_radius = std::hypot(best_target.pt.x - center_x, best_target.pt.y - center_y) <= trigger_radius;
                bool pred_in_radius = std::hypot(error_x, error_y) <= trigger_radius;
                fire_start_condition = crosshair_in_any_target || point_in_radius || pred_in_radius;
            } else {
                pid.reset();
                predictor.reset();
            }
            bool trigger_delay_ready = false;
            if (!trigger_fire_held && is_trigger_button_down && target_found && fire_start_condition) {
                auto trigger_now = std::chrono::steady_clock::now();
                if (trigger_condition_since == std::chrono::steady_clock::time_point{}) {
                    trigger_condition_since = trigger_now;
                }
                float elapsed_ms = static_cast<float>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(trigger_now - trigger_condition_since).count()
                );
                trigger_delay_ready = elapsed_ms >= trigger_delay_ms;
            } else if (!trigger_fire_held) {
                trigger_condition_since = std::chrono::steady_clock::time_point{};
            }
            bool should_hold_fire = false;
            if (is_trigger_button_down) {
                if (trigger_fire_held) {
                    should_hold_fire = target_found && fire_start_condition;
                    if (!should_hold_fire) {
                        trigger_lost_frames = 0;
                        trigger_condition_since = std::chrono::steady_clock::time_point{};
                    }
                } else if (target_found && fire_start_condition && trigger_delay_ready) {
                    trigger_lost_frames = 0;
                    should_hold_fire = true;
                }
            } else {
                trigger_lost_frames = 0;
                trigger_condition_since = std::chrono::steady_clock::time_point{};
            }
            int desired_trigger_mask = 0x00;
            if (should_hold_fire) {
                desired_trigger_mask |= 0x01;
            }
            apply_trigger_button_mask(desired_trigger_mask);
            if (is_trigger_button_down && !should_hold_fire) {
                auto safety_now = std::chrono::steady_clock::now();
                if (std::chrono::duration_cast<std::chrono::milliseconds>(safety_now - last_trigger_safety_release).count() >= 80) {
                    // If no target/range condition is valid, left must be up.
                    // Repeat the up packet because some KmboxNet firmwares can
                    // miss one transition when both side buttons are held.
                    kmbox.force_button_mask(0x00);
                    trigger_virtual_mask = 0x00;
                    last_trigger_button_send = safety_now;
                    last_trigger_safety_release = safety_now;
                }
            }

            if (should_hold_fire && !trigger_fire_held) {
                trigger_fire_held = true;
                trigger_condition_since = std::chrono::steady_clock::time_point{};
            } else if (!should_hold_fire && trigger_fire_held) {
                trigger_fire_held = false;
                trigger_lost_frames = 0;
                trigger_condition_since = std::chrono::steady_clock::time_point{};
            }

            if (preview_en) {
                auto preview_now = std::chrono::steady_clock::now();
                auto preview_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(preview_now - last_preview_time).count();
                int preview_interval_ms = std::max(1, static_cast<int>(std::round(1000.0f / std::max(1.0f, preview_fps))));
                if (preview_elapsed >= preview_interval_ms) {
                    cv::Mat preview = crop_img.clone();
                    cv::Rect xhair_roi(120, 120, 80, 80);
                    cv::Mat xhair_mask = build_crosshair_mask(crop_img(xhair_roi));
                    if (!xhair_mask.empty()) {
                        cv::Mat roi_view = preview(xhair_roi);
                        cv::Mat highlight = roi_view.clone();
                        highlight.setTo(cv::Scalar(0, 255, 255), xhair_mask);
                        cv::addWeighted(roi_view, 0.62, highlight, 0.38, 0.0, roi_view);
                        cv::rectangle(preview, xhair_roi, cv::Scalar(0, 255, 255), 1);
                        int hit_count = cv::countNonZero(xhair_mask);
                        cv::putText(preview, "HSV " + std::to_string(hit_count), cv::Point(122, 116),
                                    cv::FONT_HERSHEY_SIMPLEX, 0.38, cv::Scalar(0, 255, 255), 1);
                    }
                    cv::line(preview, cv::Point(154, 160), cv::Point(166, 160), cv::Scalar(255, 255, 255), 1);
                    cv::line(preview, cv::Point(160, 154), cv::Point(160, 166), cv::Scalar(255, 255, 255), 1);
                    cv::circle(preview, cv::Point(static_cast<int>(std::round(center_x)), static_cast<int>(std::round(center_y))),
                               4, cv::Scalar(0, 255, 255), 2);
                    for (const auto& res : results) {
                        cv::Rect2f clipped = res.box & cv::Rect2f(0, 0, 320, 320);
                        if (clipped.width <= 0 || clipped.height <= 0) continue;
                        cv::Rect box(
                            static_cast<int>(std::round(clipped.x)),
                            static_cast<int>(std::round(clipped.y)),
                            static_cast<int>(std::round(clipped.width)),
                            static_cast<int>(std::round(clipped.height))
                        );
                        cv::Scalar color = (target_found && res.class_id == display_cls) ? cv::Scalar(0, 0, 255) : cv::Scalar(0, 190, 80);
                        cv::rectangle(preview, box, color, 2);
                        char conf_text[16];
                        std::snprintf(conf_text, sizeof(conf_text), "%.1f%%", res.conf * 100.0f);
                        std::string label = configured_label(res.class_id) + " " + conf_text;
                        cv::putText(preview, label, cv::Point(box.x, std::max(12, box.y - 4)), cv::FONT_HERSHEY_SIMPLEX, 0.42, color, 1);
                    }
                    for (const auto& target : all_targets) {
                        cv::Point p(static_cast<int>(std::round(target.pt.x)), static_cast<int>(std::round(target.pt.y)));
                        if (p.x < 0 || p.x >= 320 || p.y < 0 || p.y >= 320) continue;
                        cv::Scalar aim_color = cv::Scalar(255, 0, 255);
                        bool is_best = target_found && target.cls == best_target.cls
                            && std::hypot(target.pt.x - best_target.pt.x, target.pt.y - best_target.pt.y) < 1.0f;
                        if (is_best) {
                            aim_color = cv::Scalar(0, 0, 255);
                            cv::circle(preview, p, 6, aim_color, 2);
                        }
                        cv::line(preview, cv::Point(p.x - 5, p.y), cv::Point(p.x + 5, p.y), aim_color, 1);
                        cv::line(preview, cv::Point(p.x, p.y - 5), cv::Point(p.x, p.y + 5), aim_color, 1);
                    }
                    cv::imwrite("../static/preview_tmp.jpg", preview);
                    std::rename("../static/preview_tmp.jpg", "../static/preview.jpg");
                    last_preview_time = preview_now;
                }
            }

            auto now = std::chrono::steady_clock::now(); std::chrono::duration<double> elapsed = now - start_time;
            if (elapsed.count() >= 0.5) { 
                int real_npu_fps = engine.true_npu_frames.exchange(0) * 2;
                char tele_buf[256]; int target_cls_send = target_found ? display_cls : -1;
                snprintf(tele_buf, sizeof(tele_buf), "%d,%.1f,%d,%d,%d,%.2f", real_npu_fps, display_infer_time, display_core, is_aiming?1:0, target_cls_send, display_conf);
                sendto(tele_sock, tele_buf, strlen(tele_buf), 0, (struct sockaddr*)&tele_addr, sizeof(tele_addr));
                std::string aim_state = is_aiming ? "[LOCK]" : "[IDLE]";
                std::cout << "\r[FPS:" << std::setw(3) << real_npu_fps << "|C" << display_core << ":" << std::fixed << std::setprecision(1) << display_infer_time << "ms] " << aim_state << " ";
                if (target_found) std::cout << get_label_name(display_cls) << "(" << std::fixed << std::setprecision(2) << display_conf << ")      "; 
                else std::cout << "Safe              "; 
                std::cout << std::flush; start_time = now;
            }
        } else {
            if (trigger_fire_held || trigger_virtual_mask != 0) {
                int trigger_mask = 0x04;
                { std::lock_guard<std::mutex> lock(cfg.mtx); trigger_mask = cfg.trigger_mask; }
                uint8_t btn_mask = kmbox.get_buttons_mask();
                bool is_trigger_button_down = trigger_mask > 0 && ((btn_mask & (uint8_t)trigger_mask) != 0);
                if (!is_trigger_button_down) {
                    apply_trigger_button_mask(0);
                    trigger_fire_held = false;
                    trigger_lost_frames = 0;
                    trigger_condition_since = std::chrono::steady_clock::time_point{};
                }
            }
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    }
    return 0;
}
