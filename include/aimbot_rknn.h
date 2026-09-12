#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <opencv2/opencv.hpp>

#include "aimbot_types.h"
#include "rknn_api.h"

class RKNNModel {
public:
    rknn_context ctx = 0;
    unsigned char* model_data = nullptr;
    int num_outputs = 0;
    bool is_optimized = false;

    RKNNModel(const std::string& model_path, rknn_core_mask core_mask);
    ~RKNNModel();

    std::vector<DetectResult> inference(const cv::Mat& input_img);

private:
    static float sigmoid(float x);
    static float compute_dfl(const float* tensor, int index, int spatial_size);
};

class RKNNMultiCoreEngine {
private:
    std::vector<RKNNModel*> models;
    std::vector<std::thread> workers;
    std::atomic<bool> running{true};
    std::mutex task_mtx;
    std::condition_variable task_cv;
    cv::Mat current_task_frame;
    uint64_t current_task_id = 0;

    std::mutex result_mtx;
    std::vector<DetectResult> latest_results;
    uint64_t latest_result_id = 0;
    double latest_infer_time = 0.0;
    int latest_core_id = 0;

    void worker_loop(int core_idx);

public:
    std::atomic<int> true_npu_frames{0};

    RKNNMultiCoreEngine(const std::string& model_path, int core_count = 3);
    ~RKNNMultiCoreEngine();

    void put_task(const cv::Mat& frame, uint64_t frame_id);
    bool get_latest_result(std::vector<DetectResult>& out_res, uint64_t& out_id, double& out_time, int& out_core);
};
