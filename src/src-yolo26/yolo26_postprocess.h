#pragma once

#include <opencv2/core/core.hpp>
#include <string>
#include <vector>

struct YoloDetection {
    cv::Rect_<float> rect;
    int label;
    float prob;
    unsigned long long track_id;
    std::string tracking_state;
    std::string movement_state;
};

int yolo26_decode_detections(const cv::Mat& image, float** output,
                             std::vector<YoloDetection>& detections);
void yolo26_draw_detections(cv::Mat& image,
                            const std::vector<YoloDetection>& detections,
                            bool camera_moving = false);
bool yolo26_is_tracked_label(int label);
unsigned int yolo26_last_truncated_candidates();
