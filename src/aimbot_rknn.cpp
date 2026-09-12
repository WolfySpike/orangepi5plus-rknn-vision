#include "aimbot_rknn.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <mutex>
#include <stdexcept>

#include "aimbot_config.h"

RKNNModel::RKNNModel(const std::string& model_path, rknn_core_mask core_mask) {
    std::ifstream file(model_path, std::ios::in | std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error("open model failed: " + model_path);
    }

    file.seekg(0, std::ios::end);
    int model_size = static_cast<int>(file.tellg());
    file.seekg(0, std::ios::beg);
    if (model_size <= 0) {
        throw std::runtime_error("invalid model size: " + model_path);
    }

    model_data = new unsigned char[model_size];
    file.read(reinterpret_cast<char*>(model_data), model_size);
    file.close();

    int ret = rknn_init(&ctx, model_data, model_size, 0, nullptr);
    if (ret != RKNN_SUCC) {
        delete[] model_data;
        model_data = nullptr;
        throw std::runtime_error("rknn_init failed, ret=" + std::to_string(ret));
    }

    rknn_set_core_mask(ctx, core_mask);

    rknn_input_output_num io_num;
    ret = rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num));
    if (ret != RKNN_SUCC) {
        rknn_destroy(ctx);
        delete[] model_data;
        model_data = nullptr;
        throw std::runtime_error("rknn_query IO_NUM failed, ret=" + std::to_string(ret));
    }

    num_outputs = io_num.n_output;
    is_optimized = (num_outputs >= 9);
    std::cout << "[RKNN] load model=" << model_path << " outputs=" << num_outputs << std::endl;

    for (int i = 0; i < io_num.n_output; i++) {
        rknn_tensor_attr out_attr;
        std::memset(&out_attr, 0, sizeof(out_attr));
        out_attr.index = i;
        if (rknn_query(ctx, RKNN_QUERY_OUTPUT_ATTR, &out_attr, sizeof(out_attr)) == RKNN_SUCC) {
            std::cout << "[RKNN] output[" << i << "] dims=";
            for (uint32_t d = 0; d < out_attr.n_dims; d++) {
                std::cout << out_attr.dims[d];
                if (d + 1 < out_attr.n_dims) {
                    std::cout << "x";
                }
            }
            std::cout << " fmt=" << out_attr.fmt
                      << " type=" << out_attr.type
                      << " qnt=" << out_attr.qnt_type
                      << " zp=" << out_attr.zp
                      << " scale=" << out_attr.scale
                      << std::endl;
        }
    }
}

RKNNModel::~RKNNModel() {
    rknn_destroy(ctx);
    delete[] model_data;
}

float RKNNModel::sigmoid(float x) {
    return 1.0f / (1.0f + std::exp(-x));
}

float RKNNModel::compute_dfl(const float* tensor, int index, int spatial_size) {
    float sum = 0.0f;
    float max_val = -1000.0f;
    for (int i = 0; i < 16; i++) {
        float val = tensor[(index * 16 + i) * spatial_size];
        if (val > max_val) {
            max_val = val;
        }
    }

    float exp_vals[16];
    float res = 0.0f;
    for (int i = 0; i < 16; i++) {
        exp_vals[i] = std::exp(tensor[(index * 16 + i) * spatial_size] - max_val);
        sum += exp_vals[i];
    }
    for (int i = 0; i < 16; i++) {
        res += (exp_vals[i] / sum) * i;
    }
    return res;
}

std::vector<DetectResult> RKNNModel::inference(const cv::Mat& input_img) {
    int runtime_num_classes = 9;
    int runtime_model_family = 8;
    float runtime_conf_thres = 0.20f;
    float runtime_nms_iou = 0.30f;
    {
        std::lock_guard<std::mutex> lock(cfg.mtx);
        runtime_num_classes = std::max(1, cfg.num_classes);
        runtime_model_family = cfg.model_family;
        runtime_conf_thres = std::clamp(cfg.conf_thres, 0.01f, 0.95f);
        runtime_nms_iou = std::clamp(cfg.nms_iou, 0.01f, 0.95f);
    }

    rknn_input inputs[1];
    std::memset(inputs, 0, sizeof(inputs));
    inputs[0].index = 0;
    inputs[0].type = RKNN_TENSOR_UINT8;
    inputs[0].size = input_img.cols * input_img.rows * 3;
    inputs[0].fmt = RKNN_TENSOR_NHWC;
    inputs[0].buf = input_img.data;

    rknn_inputs_set(ctx, 1, inputs);
    rknn_run(ctx, nullptr);

    if (num_outputs < 1) {
        return {};
    }

    std::vector<rknn_output> outputs(num_outputs);
    std::memset(outputs.data(), 0, sizeof(rknn_output) * outputs.size());
    for (int i = 0; i < num_outputs; i++) {
        outputs[i].want_float = 1;
    }

    if (rknn_outputs_get(ctx, num_outputs, outputs.data(), nullptr) != RKNN_SUCC) {
        return {};
    }

    int strides[3] = {8, 16, 32};
    std::vector<cv::Rect> boxes;
    std::vector<float> confidences;
    std::vector<int> class_ids;

    auto score_from_raw = [&](float v) -> float {
        if (!std::isfinite(v)) {
            return 0.0f;
        }
        if (v < 0.0f || v > 1.0f) {
            v = sigmoid(v);
        }
        return v;
    };

    auto append_candidate = [&](float x1, float y1, float x2, float y2, float conf, int cls) {
        if (cls < 0 || cls >= runtime_num_classes) {
            return;
        }
        if (!std::isfinite(conf) || conf < runtime_conf_thres) {
            return;
        }

        // If coordinates are xywh, convert to xyxy.
        if (x2 <= x1 || y2 <= y1) {
            float cx = x1;
            float cy = y1;
            float w = std::max(0.0f, x2);
            float h = std::max(0.0f, y2);
            x1 = cx - w * 0.5f;
            y1 = cy - h * 0.5f;
            x2 = cx + w * 0.5f;
            y2 = cy + h * 0.5f;
        }

        float max_abs = std::max({std::fabs(x1), std::fabs(y1), std::fabs(x2), std::fabs(y2)});
        // Many exporters output normalized boxes in [0, 1].
        if (max_abs <= 2.5f) {
            x1 *= 320.0f;
            y1 *= 320.0f;
            x2 *= 320.0f;
            y2 *= 320.0f;
        }

        x1 = std::clamp(x1, 0.0f, 319.0f);
        y1 = std::clamp(y1, 0.0f, 319.0f);
        x2 = std::clamp(x2, 0.0f, 319.0f);
        y2 = std::clamp(y2, 0.0f, 319.0f);
        if (x2 <= x1 || y2 <= y1) {
            return;
        }

        boxes.push_back(cv::Rect(x1, y1, x2 - x1, y2 - y1));
        confidences.push_back(conf);
        class_ids.push_back(cls);
    };

    bool parsed_split_outputs = false;
    if (num_outputs == 2 && outputs[0].buf && outputs[1].buf) {
        struct HeadInfo {
            int C = 0;
            int N = 0;
            bool channels_first = true;
            bool valid = false;
        };

        auto infer_head_info = [&](int out_idx, int total_vals) -> HeadInfo {
            HeadInfo info;
            rknn_tensor_attr attr;
            std::memset(&attr, 0, sizeof(attr));
            attr.index = out_idx;
            if (rknn_query(ctx, RKNN_QUERY_OUTPUT_ATTR, &attr, sizeof(attr)) != RKNN_SUCC || attr.n_dims < 2) {
                return info;
            }

            int da = static_cast<int>(attr.dims[attr.n_dims - 2]);
            int db = static_cast<int>(attr.dims[attr.n_dims - 1]);
            if (da <= 512 && db > 512) {
                info.C = da;
                info.N = db;
                info.channels_first = true;
            } else if (db <= 512 && da > 512) {
                info.C = db;
                info.N = da;
                info.channels_first = false;
            } else if (da <= db) {
                info.C = da;
                info.N = db;
                info.channels_first = true;
            } else {
                info.C = db;
                info.N = da;
                info.channels_first = false;
            }

            info.valid = (info.C > 0 && info.N > 0 && info.C * info.N <= total_vals);
            return info;
        };

        int total0 = static_cast<int>(outputs[0].size / sizeof(float));
        int total1 = static_cast<int>(outputs[1].size / sizeof(float));
        HeadInfo h0 = infer_head_info(0, total0);
        HeadInfo h1 = infer_head_info(1, total1);

        int box_out = -1;
        int score_out = -1;
        HeadInfo box_info;
        HeadInfo score_info;
        if (h0.valid && h1.valid && h0.N == h1.N) {
            if (h0.C == 4 && h1.C >= 1) {
                box_out = 0;
                score_out = 1;
                box_info = h0;
                score_info = h1;
            } else if (h1.C == 4 && h0.C >= 1) {
                box_out = 1;
                score_out = 0;
                box_info = h1;
                score_info = h0;
            }
        }

        if (box_out >= 0 && score_out >= 0) {
            float* box_ptr = static_cast<float*>(outputs[box_out].buf);
            float* score_ptr = static_cast<float*>(outputs[score_out].buf);
            auto box_val = [&](int c, int i) -> float {
                if (box_info.channels_first) {
                    return box_ptr[c * box_info.N + i];
                }
                return box_ptr[i * box_info.C + c];
            };
            auto score_val = [&](int c, int i) -> float {
                if (score_info.channels_first) {
                    return score_ptr[c * score_info.N + i];
                }
                return score_ptr[i * score_info.C + c];
            };

            int class_loop = std::max(1, std::min(runtime_num_classes, score_info.C));
            for (int i = 0; i < box_info.N; i++) {
                float best_cls_score = -std::numeric_limits<float>::infinity();
                int best_cls = -1;
                for (int c = 0; c < class_loop; c++) {
                    float cls_score = score_from_raw(score_val(c, i));
                    if (cls_score > best_cls_score) {
                        best_cls_score = cls_score;
                        best_cls = c;
                    }
                }
                if (best_cls < 0) {
                    continue;
                }
                append_candidate(box_val(0, i), box_val(1, i), box_val(2, i), box_val(3, i),
                                 std::max(0.0f, best_cls_score), best_cls);
            }

            static int last_split_mode_key = -1;
            int split_mode_key = box_out * 10000 + score_out * 1000
                + (box_info.channels_first ? 100 : 0)
                + (score_info.channels_first ? 10 : 0)
                + score_info.C;
            if (split_mode_key != last_split_mode_key) {
                last_split_mode_key = split_mode_key;
                std::cout << "[RKNN] split-output decode mode: box_out=" << box_out
                          << " boxes=" << (box_info.channels_first ? "CxN" : "NxC")
                          << " score_out=" << score_out
                          << " scores=" << (score_info.channels_first ? "CxN" : "NxC")
                          << " classes=" << score_info.C << std::endl;
            }

            if (runtime_num_classes != score_info.C) {
                static bool warned_split_class_mismatch_once = false;
                if (!warned_split_class_mismatch_once) {
                    warned_split_class_mismatch_once = true;
                    std::cout << "[RKNN] class count mismatch: ui/classes=" << runtime_num_classes
                              << " split_score_classes=" << score_info.C
                              << " (model_family=" << runtime_model_family << ")" << std::endl;
                }
            }
            parsed_split_outputs = true;
        }
    }

    if (!parsed_split_outputs && num_outputs == 1 && outputs[0].buf) {
        int total_vals = static_cast<int>(outputs[0].size / sizeof(float));
        float* det_ptr = static_cast<float*>(outputs[0].buf);

        rknn_tensor_attr out0_attr;
        std::memset(&out0_attr, 0, sizeof(out0_attr));
        out0_attr.index = 0;
        bool has_attr = (rknn_query(ctx, RKNN_QUERY_OUTPUT_ATTR, &out0_attr, sizeof(out0_attr)) == RKNN_SUCC);

        bool parsed = false;

        // Branch A: raw single-output head, e.g. 1xCx2100 / 1x2100xC / 1xCx8400 / 1x8400xC.
        if (has_attr && out0_attr.n_dims >= 2) {
            int da = static_cast<int>(out0_attr.dims[out0_attr.n_dims - 2]);
            int db = static_cast<int>(out0_attr.dims[out0_attr.n_dims - 1]);
            bool maybe_raw = ((da >= 8 && db >= 500) || (db >= 8 && da >= 500));
            if (maybe_raw) {
                int C = 0;
                int N = 0;
                bool channels_first = true;  // CxN

                if (da <= 512 && db > 512) {
                    C = da;
                    N = db;
                    channels_first = true;
                } else if (db <= 512 && da > 512) {
                    C = db;
                    N = da;
                    channels_first = false;  // NxC
                } else if (da <= db) {
                    C = da;
                    N = db;
                    channels_first = true;
                } else {
                    C = db;
                    N = da;
                    channels_first = false;
                }

                if (C >= 6 && N > 0 && (C * N) <= total_vals) {
                    // Safety guard: flattened DFL-style heads (e.g. C = 64 + nc) need a different decoder.
                    // If we can't decode that format reliably, skip instead of producing high-confidence garbage boxes.
                    if (C >= runtime_num_classes + 32) {
                        static bool warned_once = false;
                        if (!warned_once) {
                            warned_once = true;
                            std::cout << "[RKNN] skip unsupported single-output layout C=" << C
                                      << " N=" << N
                                      << " (looks like DFL/raw-dist; need dedicated decoder)" << std::endl;
                        }
                        parsed = true;
                    } else {
                        int noobj_nc = C - 4;
                        int obj_nc = C - 5;
                        struct RawDecodeResult {
                            std::vector<cv::Rect> boxes;
                            std::vector<float> scores;
                            std::vector<int> classes;
                            bool channels_first = true;
                            bool has_obj = false;
                            int inferred_nc = 0;
                            float score_sum = 0.0f;
                            bool logged_raw_scores = false;
                        };

                        auto decode_raw_with_layout = [&](bool cf, bool mode_has_obj, int inferred_nc) -> RawDecodeResult {
                            RawDecodeResult r;
                            r.channels_first = cf;
                            r.has_obj = mode_has_obj;
                            r.inferred_nc = inferred_nc;
                            if (inferred_nc <= 0) {
                                return r;
                            }

                            int class_loop = std::max(1, std::min(runtime_num_classes, inferred_nc));
                            int cls_base = mode_has_obj ? 5 : 4;
                            auto val_at = [&](int c, int i) -> float {
                                if (cf) {
                                    return det_ptr[c * N + i];
                                }
                                return det_ptr[i * C + c];
                            };

                            auto append_local = [&](float x1, float y1, float x2, float y2, float conf, int cls) {
                                if (cls < 0 || cls >= runtime_num_classes) return;
                                if (!std::isfinite(conf) || conf < runtime_conf_thres) return;

                                if (x2 <= x1 || y2 <= y1) {
                                    float cx = x1;
                                    float cy = y1;
                                    float w = std::max(0.0f, x2);
                                    float h = std::max(0.0f, y2);
                                    x1 = cx - w * 0.5f;
                                    y1 = cy - h * 0.5f;
                                    x2 = cx + w * 0.5f;
                                    y2 = cy + h * 0.5f;
                                }

                                float max_abs = std::max({std::fabs(x1), std::fabs(y1), std::fabs(x2), std::fabs(y2)});
                                if (max_abs <= 2.5f) {
                                    x1 *= 320.0f;
                                    y1 *= 320.0f;
                                    x2 *= 320.0f;
                                    y2 *= 320.0f;
                                }

                                x1 = std::clamp(x1, 0.0f, 319.0f);
                                y1 = std::clamp(y1, 0.0f, 319.0f);
                                x2 = std::clamp(x2, 0.0f, 319.0f);
                                y2 = std::clamp(y2, 0.0f, 319.0f);
                                if (x2 <= x1 || y2 <= y1) return;

                                r.boxes.push_back(cv::Rect(x1, y1, x2 - x1, y2 - y1));
                                r.scores.push_back(conf);
                                r.classes.push_back(cls);
                                r.score_sum += conf;
                            };

                            for (int i = 0; i < N; i++) {
                                float x1 = val_at(0, i);
                                float y1 = val_at(1, i);
                                float x2 = val_at(2, i);
                                float y2 = val_at(3, i);

                                float obj = 1.0f;
                                if (mode_has_obj) {
                                    obj = score_from_raw(val_at(4, i));
                                }

                                float best_cls_score = -std::numeric_limits<float>::infinity();
                                int best_cls = -1;
                                for (int c = 0; c < class_loop; c++) {
                                    float raw_cls = val_at(cls_base + c, i);
                                    float cls_score = score_from_raw(raw_cls);
                                    if (cls_score > best_cls_score) {
                                        best_cls_score = cls_score;
                                        best_cls = c;
                                    }
                                }
                                if (best_cls < 0) continue;

                                float conf = obj * std::max(0.0f, best_cls_score);
                                static bool logged_v11_raw_scores_once = false;
                                if (!logged_v11_raw_scores_once && runtime_model_family == 11 && !mode_has_obj &&
                                    C == inferred_nc + 4 && conf >= runtime_conf_thres) {
                                    logged_v11_raw_scores_once = true;
                                    std::cout << "[RKNN] v11 raw class vector:";
                                    for (int c = 0; c < class_loop; c++) {
                                        std::cout << " c" << c << "=" << val_at(cls_base + c, i);
                                    }
                                    std::cout << " best_cls=" << best_cls
                                              << " best_conf=" << best_cls_score
                                              << std::endl;
                                }
                                append_local(x1, y1, x2, y2, conf, best_cls);
                            }
                            return r;
                        };

                        // Prefer the layout that exactly matches the configured class count.
                        // Some YOLOv11 exports are cls-only (4 + nc), while others include
                        // an objectness channel (5 + nc). Decoding a 5+nc model as 4+nc makes
                        // the objectness channel look like a class score, often producing
                        // suspiciously uniform confidences in preview.
                        bool prefer_obj = (runtime_model_family >= 26);
                        if (obj_nc == runtime_num_classes && noobj_nc != runtime_num_classes) {
                            prefer_obj = true;
                        } else if (noobj_nc == runtime_num_classes && obj_nc != runtime_num_classes) {
                            prefer_obj = false;
                        }
                        int inferred_nc = prefer_obj ? obj_nc : noobj_nc;
                        if (inferred_nc <= 0) {
                            prefer_obj = !prefer_obj;
                            inferred_nc = prefer_obj ? obj_nc : noobj_nc;
                        }

                        if (inferred_nc > 0) {
                            RawDecodeResult best = decode_raw_with_layout(channels_first, prefer_obj, inferred_nc);
                            // Keep v8/v11 behavior stable: do not auto-switch layout unless v26+.
                            bool allow_layout_fallback = (runtime_model_family >= 26);
                            if (!allow_layout_fallback) {
                                static bool warned_layout_locked_once = false;
                                if (!warned_layout_locked_once) {
                                    warned_layout_locked_once = true;
                                    std::cout << "[RKNN] single-output layout locked to primary for model_family="
                                              << runtime_model_family << std::endl;
                                }
                            }
                            if (allow_layout_fallback && best.boxes.empty()) {
                                RawDecodeResult alt = decode_raw_with_layout(!channels_first, prefer_obj, inferred_nc);
                                if (!alt.boxes.empty()) {
                                    best = std::move(alt);
                                    static bool warned_layout_switch_once = false;
                                    if (!warned_layout_switch_once) {
                                        warned_layout_switch_once = true;
                                        std::cout << "[RKNN] single-output layout auto-switch: using NxC decode fallback" << std::endl;
                                    }
                                }
                            }

                            if (best.inferred_nc > 0 && runtime_num_classes != best.inferred_nc) {
                                static bool warned_class_mismatch_once = false;
                                if (!warned_class_mismatch_once) {
                                    warned_class_mismatch_once = true;
                                    std::cout << "[RKNN] class count mismatch: ui/classes=" << runtime_num_classes
                                              << " inferred_from_output=" << best.inferred_nc
                                              << " (model_family=" << runtime_model_family << ")" << std::endl;
                                }
                            }

                            static int last_logged_mode_key = -1;
                            int mode_key = (best.has_obj ? 1 : 0) * 10000 + (best.channels_first ? 1 : 0) * 1000 + best.inferred_nc;
                            if (mode_key != last_logged_mode_key) {
                                last_logged_mode_key = mode_key;
                                std::cout << "[RKNN] single-output decode mode: " << (best.has_obj ? "obj+cls" : "cls-only")
                                          << " layout=" << (best.channels_first ? "CxN" : "NxC")
                                          << " inferred_nc=" << best.inferred_nc << std::endl;
                            }

                            for (size_t i = 0; i < best.boxes.size(); i++) {
                                boxes.push_back(best.boxes[i]);
                                confidences.push_back(best.scores[i]);
                                class_ids.push_back(best.classes[i]);
                            }
                        }

                        parsed = true;
                    }
                }
            }
        }

        // Branch B: row detections (NMS-style), e.g. Nx6 / Nx7 or their transposed variant.
        if (!parsed) {
            int cols = 0;
            int rows = 0;
            bool col_major = false;

            if (has_attr && out0_attr.n_dims >= 2) {
                int da = static_cast<int>(out0_attr.dims[out0_attr.n_dims - 2]);
                int db = static_cast<int>(out0_attr.dims[out0_attr.n_dims - 1]);

                if (db == 6 || db == 7) {
                    cols = db;
                    rows = total_vals / cols;
                    col_major = false;
                } else if (da == 6 || da == 7) {
                    cols = da;
                    rows = db;
                    col_major = true;
                }
            }

            if (cols == 0) {
                if (total_vals % 6 == 0) cols = 6;
                else if (total_vals % 7 == 0) cols = 7;
                else if (total_vals >= 6) cols = 6;
                rows = (cols > 0) ? (total_vals / cols) : 0;
                col_major = false;
            }

            if (cols > 0 && rows > 0) {
                auto row_val = [&](int r, int c) -> float {
                    if (col_major) {
                        return det_ptr[c * rows + r];
                    }
                    return det_ptr[r * cols + c];
                };

                for (int i = 0; i < rows; i++) {
                    float x1 = 0.0f;
                    float y1 = 0.0f;
                    float x2 = 0.0f;
                    float y2 = 0.0f;
                    float conf = 0.0f;
                    int cls = -1;

                    if (cols == 6) {
                        x1 = row_val(i, 0);
                        y1 = row_val(i, 1);
                        x2 = row_val(i, 2);
                        y2 = row_val(i, 3);
                        conf = score_from_raw(row_val(i, 4));
                        cls = static_cast<int>(std::round(row_val(i, 5)));
                    } else {
                        // [batch_id, x1, y1, x2, y2, score, cls]
                        x1 = row_val(i, 1);
                        y1 = row_val(i, 2);
                        x2 = row_val(i, 3);
                        y2 = row_val(i, 4);
                        conf = score_from_raw(row_val(i, 5));
                        cls = static_cast<int>(std::round(row_val(i, 6)));
                    }

                    append_candidate(x1, y1, x2, y2, conf, cls);
                }
            }
        }
    } else if (!parsed_split_outputs) {
        bool use_optimized_layout = (num_outputs >= 9);
        for (int i = 0; i < 3; i++) {
            int stride = strides[i];
            int spatial_size = (320 / stride) * (320 / stride);
            int box_idx = use_optimized_layout ? (i * 3) : (i * 2);
            int cls_idx = use_optimized_layout ? (i * 3 + 1) : (i * 2 + 1);

            if (box_idx >= num_outputs || cls_idx >= num_outputs) continue;
            if (!outputs[box_idx].buf || !outputs[cls_idx].buf) continue;

            int box_float_count = static_cast<int>(outputs[box_idx].size / sizeof(float));
            int cls_float_count = static_cast<int>(outputs[cls_idx].size / sizeof(float));
            if (box_float_count < 64 * spatial_size) continue;
            if (cls_float_count < spatial_size) continue;

            float* box_ptr = static_cast<float*>(outputs[box_idx].buf);
            float* cls_ptr = static_cast<float*>(outputs[cls_idx].buf);
            int tensor_num_classes = cls_float_count / spatial_size;
            if (tensor_num_classes <= 0) continue;

            int class_loop = std::max(1, std::min(runtime_num_classes, tensor_num_classes));
            for (int idx = 0; idx < spatial_size; idx++) {
                float max_conf = -1000.0f;
                int best_cls = -1;
                for (int c = 0; c < class_loop; c++) {
                    float raw_conf = cls_ptr[c * spatial_size + idx];
                    if (raw_conf > max_conf) {
                        max_conf = raw_conf;
                        best_cls = c;
                    }
                }

                float conf = sigmoid(max_conf);
                if (conf >= runtime_conf_thres) {
                    float d[4];
                    for (int k = 0; k < 4; k++) {
                        d[k] = compute_dfl(box_ptr + idx, k, spatial_size);
                    }
                    int x = idx % (320 / stride);
                    int y = idx / (320 / stride);
                    float x1 = (x + 0.5f - d[0]) * stride;
                    float y1 = (y + 0.5f - d[1]) * stride;
                    float x2 = (x + 0.5f + d[2]) * stride;
                    float y2 = (y + 0.5f + d[3]) * stride;
                    append_candidate(x1, y1, x2, y2, conf, best_cls);
                }
            }
        }
    }

    rknn_outputs_release(ctx, num_outputs, outputs.data());

    std::vector<int> indices;
    cv::dnn::NMSBoxes(boxes, confidences, runtime_conf_thres, runtime_nms_iou, indices);

    std::vector<DetectResult> final_results;
    for (int idx : indices) {
        final_results.push_back({class_ids[idx], confidences[idx], boxes[idx]});
    }
    return final_results;
}

void RKNNMultiCoreEngine::worker_loop(int core_idx) {
    while (running) {
        cv::Mat frame;
        uint64_t task_id = 0;
        {
            std::unique_lock<std::mutex> lock(task_mtx);
            task_cv.wait(lock, [this] { return !running || current_task_id > 0; });
            if (!running) {
                break;
            }
            frame = current_task_frame;
            task_id = current_task_id;
            current_task_id = 0;
        }

        if (!frame.empty()) {
            auto start = std::chrono::high_resolution_clock::now();
            auto res = models[core_idx]->inference(frame);
            auto end = std::chrono::high_resolution_clock::now();

            true_npu_frames++;
            double elapsed = std::chrono::duration<double, std::milli>(end - start).count();
            std::lock_guard<std::mutex> lock(result_mtx);
            if (task_id > latest_result_id) {
                latest_results = res;
                latest_result_id = task_id;
                latest_infer_time = elapsed;
                latest_core_id = core_idx;
            }
        }
    }
}

RKNNMultiCoreEngine::RKNNMultiCoreEngine(const std::string& model_path, int core_count) {
    core_count = std::clamp(core_count, 1, 3);
    std::cout << "[RKNN] engine mode=" << (core_count == 1 ? "low-latency" : "high-throughput")
              << " npu_cores=" << core_count << std::endl;

    const rknn_core_mask core_masks[3] = {
        RKNN_NPU_CORE_0,
        RKNN_NPU_CORE_1,
        RKNN_NPU_CORE_2,
    };

    for (int i = 0; i < core_count; i++) {
        models.push_back(new RKNNModel(model_path, core_masks[i]));
    }
    for (int i = 0; i < core_count; i++) {
        workers.emplace_back(&RKNNMultiCoreEngine::worker_loop, this, i);
    }
}

RKNNMultiCoreEngine::~RKNNMultiCoreEngine() {
    running = false;
    task_cv.notify_all();
    for (auto& w : workers) {
        if (w.joinable()) {
            w.join();
        }
    }
    for (auto* m : models) {
        delete m;
    }
}

void RKNNMultiCoreEngine::put_task(const cv::Mat& frame, uint64_t frame_id) {
    std::lock_guard<std::mutex> lock(task_mtx);
    current_task_frame = frame;
    current_task_id = frame_id;
    task_cv.notify_one();
}

bool RKNNMultiCoreEngine::get_latest_result(
    std::vector<DetectResult>& out_res,
    uint64_t& out_id,
    double& out_time,
    int& out_core
) {
    std::lock_guard<std::mutex> lock(result_mtx);
    if (latest_result_id > 0) {
        out_res = latest_results;
        out_id = latest_result_id;
        out_time = latest_infer_time;
        out_core = latest_core_id;
        return true;
    }
    return false;
}
