#pragma once

#include "yolo26_postprocess.h"

#include <memory>
#include <vector>

// 将检测快照异步写入容量受控的 JSONL；未配置路径时自动退化为空操作。
class DetectionEventLogger {
public:
    DetectionEventLogger(int width, int height);
    ~DetectionEventLogger();

    bool submit(std::vector<YoloDetection>& detections, const cv::Mat& frame);
    void note_output_frame();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
