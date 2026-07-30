#pragma once

#include <opencv2/core/core.hpp>
#include <memory>
#include <set>
#include <vector>

struct ByteTrackDetection {
    cv::Rect_<float> rect;
    float score;
    size_t source_index;
};

struct ByteTrackResult {
    unsigned long long local_id;
    size_t source_index;
    cv::Rect_<float> rect;
    float score;
    bool confirmed;
    const char* track_state;
    float velocity_x;
    float velocity_y;
};

struct ByteTrackStats {
    size_t active_tracks;
    size_t lost_tracks;
};

class ByteTracker {
public:
    ByteTracker(float high_threshold = 0.50f, float new_threshold = 0.60f,
                float first_match_threshold = 0.80f,
                float second_match_threshold = 0.50f,
                unsigned int max_lost = 18);
    ~ByteTracker();

    std::vector<ByteTrackResult> update(const std::vector<ByteTrackDetection>& detections,
                                        double monotonic_seconds);
    ByteTrackStats stats() const;
    std::set<unsigned long long> live_ids() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
