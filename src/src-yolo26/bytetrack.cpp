/*
 * Mobile C++ ByteTrack core adapted for this project from FoundationVision/ByteTrack.
 * Upstream: https://github.com/FoundationVision/ByteTrack
 * Commit: d1bf0191adff59bc8fcfeaa0b33d3d1642552a99
 * Algorithm: Kalman-filtered STrack lifecycle and two-stage association using
 * a LAPJV-style shortest augmenting path assignment. No ncnn code is required.
 * SPDX-License-Identifier: MIT
 */
#include "bytetrack.h"

#include <opencv2/video/tracking.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <string>

namespace {

enum TrackState { NEW_TRACK, TRACKED, LOST, REMOVED };

float iou(const cv::Rect_<float>& a, const cv::Rect_<float>& b) {
    const float intersection = (a & b).area();
    const float united = a.area() + b.area() - intersection;
    return united > 0.0f ? intersection / united : 0.0f;
}

// Rectangular shortest-augmenting-path assignment. This is the dense LAPJV
// formulation used by ByteTrack: each row is assigned globally, with dummy
// columns representing an unmatched row.
std::vector<int> lapjv(const std::vector<std::vector<float> >& input, float limit) {
    const size_t rows = input.size();
    const size_t cols = rows ? input[0].size() : 0;
    if (!rows) return std::vector<int>();
    const size_t n = rows;
    const size_t m = cols + rows;
    std::vector<std::vector<double> > cost(n + 1, std::vector<double>(m + 1, limit));
    for (size_t i = 0; i < rows; ++i) {
        for (size_t j = 0; j < cols; ++j) cost[i + 1][j + 1] = input[i][j];
        for (size_t j = 0; j < rows; ++j) cost[i + 1][cols + j + 1] = (i == j ? limit : limit + 1.0);
    }
    std::vector<double> u(n + 1), v(m + 1);
    std::vector<int> p(m + 1), way(m + 1);
    for (size_t i = 1; i <= n; ++i) {
        p[0] = static_cast<int>(i);
        int j0 = 0;
        std::vector<double> minv(m + 1, std::numeric_limits<double>::infinity());
        std::vector<char> used(m + 1, false);
        do {
            used[j0] = true;
            const int i0 = p[j0];
            double delta = std::numeric_limits<double>::infinity();
            int j1 = 0;
            for (size_t j = 1; j <= m; ++j) if (!used[j]) {
                const double cur = cost[i0][j] - u[i0] - v[j];
                if (cur < minv[j]) { minv[j] = cur; way[j] = j0; }
                if (minv[j] < delta) { delta = minv[j]; j1 = static_cast<int>(j); }
            }
            for (size_t j = 0; j <= m; ++j) {
                if (used[j]) { u[p[j]] += delta; v[j] -= delta; }
                else minv[j] -= delta;
            }
            j0 = j1;
        } while (p[j0] != 0);
        do {
            const int j1 = way[j0];
            p[j0] = p[j1];
            j0 = j1;
        } while (j0);
    }
    std::vector<int> result(rows, -1);
    for (size_t j = 1; j <= cols; ++j) {
        if (p[j] > 0 && input[p[j] - 1][j - 1] <= limit)
            result[p[j] - 1] = static_cast<int>(j - 1);
    }
    return result;
}

struct STrack {
    unsigned long long id;
    TrackState state;
    bool confirmed;
    unsigned int hits;
    unsigned int lost_frames;
    float score;
    size_t source_index;
    cv::KalmanFilter kalman;
    cv::Rect_<float> box;
    double last_update_seconds;
    float velocity_x;
    float velocity_y;

    STrack(unsigned long long value, const ByteTrackDetection& detection, double now)
        : id(value), state(NEW_TRACK), confirmed(false), hits(1), lost_frames(0),
          score(detection.score), source_index(detection.source_index), kalman(8, 4, 0, CV_32F),
          box(detection.rect), last_update_seconds(now), velocity_x(0), velocity_y(0) {
        cv::setIdentity(kalman.transitionMatrix);
        for (int i = 0; i < 4; ++i) kalman.transitionMatrix.at<float>(i, i + 4) = 1.0f;
        kalman.measurementMatrix = cv::Mat::zeros(4, 8, CV_32F);
        for (int i = 0; i < 4; ++i) kalman.measurementMatrix.at<float>(i, i) = 1.0f;
        cv::setIdentity(kalman.processNoiseCov, cv::Scalar::all(1e-2));
        cv::setIdentity(kalman.measurementNoiseCov, cv::Scalar::all(1e-1));
        cv::setIdentity(kalman.errorCovPost, cv::Scalar::all(10));
        const float h = std::max(1.0f, box.height);
        kalman.statePost.at<float>(0) = box.x + box.width * 0.5f;
        kalman.statePost.at<float>(1) = box.y + box.height * 0.5f;
        kalman.statePost.at<float>(2) = box.width / h;
        kalman.statePost.at<float>(3) = h;
        for (int i = 4; i < 8; ++i) kalman.statePost.at<float>(i) = 0.0f;
    }

    void predict() {
        const cv::Mat state_value = kalman.predict();
        set_box(state_value);
        if (state == LOST) ++lost_frames;
    }

    void update(const ByteTrackDetection& detection, double now) {
        const double elapsed = std::max(1e-3, now - last_update_seconds);
        cv::Mat measurement(4, 1, CV_32F);
        const float h = std::max(1.0f, detection.rect.height);
        measurement.at<float>(0) = detection.rect.x + detection.rect.width * 0.5f;
        measurement.at<float>(1) = detection.rect.y + detection.rect.height * 0.5f;
        measurement.at<float>(2) = detection.rect.width / h;
        measurement.at<float>(3) = h;
        const cv::Mat corrected = kalman.correct(measurement);
        set_box(corrected);
        velocity_x = corrected.at<float>(4) / static_cast<float>(elapsed);
        velocity_y = corrected.at<float>(5) / static_cast<float>(elapsed);
        last_update_seconds = now;
        score = detection.score;
        source_index = detection.source_index;
        state = TRACKED;
        lost_frames = 0;
        ++hits;
        confirmed = hits >= 2;
    }

    void set_box(const cv::Mat& state_value) {
        const float h = std::max(1.0f, state_value.at<float>(3));
        const float w = std::max(1.0f, state_value.at<float>(2) * h);
        box = cv::Rect_<float>(state_value.at<float>(0) - w * 0.5f,
                               state_value.at<float>(1) - h * 0.5f, w, h);
    }
};

typedef std::shared_ptr<STrack> TrackPtr;

std::vector<std::vector<float> > iou_cost(const std::vector<TrackPtr>& tracks,
                                          const std::vector<ByteTrackDetection>& detections) {
    std::vector<std::vector<float> > result(tracks.size(), std::vector<float>(detections.size(), 1.0f));
    for (size_t i = 0; i < tracks.size(); ++i)
        for (size_t j = 0; j < detections.size(); ++j)
            result[i][j] = 1.0f - iou(tracks[i]->box, detections[j].rect);
    return result;
}

} // namespace

class ByteTracker::Impl {
public:
    Impl(float high, float fresh, float first, float second, unsigned int lost)
        : high_(high), new_(fresh), first_(first), second_(second), max_lost_(lost), next_id_(1) {}

    std::vector<ByteTrackResult> update(const std::vector<ByteTrackDetection>& detections, double now) {
        std::vector<ByteTrackDetection> high, low;
        for (size_t i = 0; i < detections.size(); ++i) {
            if (detections[i].score >= high_) high.push_back(detections[i]);
            else low.push_back(detections[i]);
        }
        for (size_t i = 0; i < tracks_.size(); ++i) {
            if (tracks_[i]->state != REMOVED) tracks_[i]->predict();
            if (tracks_[i]->state == LOST && tracks_[i]->lost_frames > max_lost_)
                tracks_[i]->state = REMOVED;
        }

        std::vector<TrackPtr> pool, unconfirmed;
        for (size_t i = 0; i < tracks_.size(); ++i) {
            if (tracks_[i]->state == REMOVED) continue;
            if (tracks_[i]->confirmed && (tracks_[i]->state == TRACKED || tracks_[i]->state == LOST)) pool.push_back(tracks_[i]);
            else if (!tracks_[i]->confirmed && tracks_[i]->state != LOST) unconfirmed.push_back(tracks_[i]);
        }

        std::set<size_t> used_high;
        std::set<unsigned long long> updated;
        const std::vector<int> first_assignment = lapjv(iou_cost(pool, high), first_);
        std::vector<TrackPtr> unmatched_active;
        for (size_t i = 0; i < pool.size(); ++i) {
            const int match = first_assignment[i];
            if (match >= 0) {
                pool[i]->update(high[match], now); used_high.insert(static_cast<size_t>(match)); updated.insert(pool[i]->id);
            } else if (pool[i]->state == TRACKED) unmatched_active.push_back(pool[i]);
        }

        const std::vector<int> second_assignment = lapjv(iou_cost(unmatched_active, low), second_);
        for (size_t i = 0; i < unmatched_active.size(); ++i) {
            const int match = second_assignment[i];
            if (match >= 0) {
                unmatched_active[i]->update(low[match], now); updated.insert(unmatched_active[i]->id);
            } else {
                unmatched_active[i]->state = LOST;
                unmatched_active[i]->lost_frames = 1;
            }
        }

        std::vector<ByteTrackDetection> remaining_high;
        std::vector<size_t> remaining_indices;
        for (size_t i = 0; i < high.size(); ++i) if (!used_high.count(i)) {
            remaining_high.push_back(high[i]); remaining_indices.push_back(i);
        }
        const std::vector<int> tentative_assignment = lapjv(iou_cost(unconfirmed, remaining_high), 0.70f);
        std::set<size_t> used_remaining;
        for (size_t i = 0; i < unconfirmed.size(); ++i) {
            const int match = tentative_assignment[i];
            if (match >= 0) {
                unconfirmed[i]->update(remaining_high[match], now);
                updated.insert(unconfirmed[i]->id); used_remaining.insert(static_cast<size_t>(match));
            } else unconfirmed[i]->state = REMOVED;
        }
        for (size_t i = 0; i < remaining_high.size(); ++i) {
            if (!used_remaining.count(i) && remaining_high[i].score >= new_) {
                TrackPtr track(new STrack(next_id_++, remaining_high[i], now));
                track->state = TRACKED;
                tracks_.push_back(track);
                updated.insert(track->id);
            }
        }
        for (size_t i = 0; i < tracks_.size(); ++i)
            if (tracks_[i]->state == LOST && tracks_[i]->lost_frames > max_lost_) tracks_[i]->state = REMOVED;

        // ByteTrack duplicate suppression between currently tracked and lost
        // lists: retain the longer-lived STrack when boxes collapse together.
        for (size_t i = 0; i < tracks_.size(); ++i) if (tracks_[i]->state == TRACKED) {
            for (size_t j = 0; j < tracks_.size(); ++j) if (tracks_[j]->state == LOST) {
                if (iou(tracks_[i]->box, tracks_[j]->box) > 0.85f) {
                    if (tracks_[i]->hits >= tracks_[j]->hits) tracks_[j]->state = REMOVED;
                    else tracks_[i]->state = REMOVED;
                }
            }
        }

        std::vector<ByteTrackResult> result;
        for (size_t i = 0; i < tracks_.size(); ++i) if (tracks_[i]->state != REMOVED && updated.count(tracks_[i]->id)) {
            ByteTrackResult item = {tracks_[i]->id, tracks_[i]->source_index, tracks_[i]->box,
                tracks_[i]->score, tracks_[i]->confirmed,
                tracks_[i]->confirmed ? "tracked" : "tentative",
                tracks_[i]->velocity_x, tracks_[i]->velocity_y};
            result.push_back(item);
        }
        tracks_.erase(std::remove_if(tracks_.begin(), tracks_.end(),
            [](const TrackPtr& track) { return track->state == REMOVED; }), tracks_.end());
        return result;
    }

    ByteTrackStats stats() const {
        ByteTrackStats value = {0, 0};
        for (size_t i = 0; i < tracks_.size(); ++i) {
            if (tracks_[i]->state == TRACKED && tracks_[i]->confirmed) ++value.active_tracks;
            else if (tracks_[i]->state == LOST) ++value.lost_tracks;
        }
        return value;
    }

    std::set<unsigned long long> live_ids() const {
        std::set<unsigned long long> ids;
        for (size_t i = 0; i < tracks_.size(); ++i) ids.insert(tracks_[i]->id);
        return ids;
    }

private:
    float high_, new_, first_, second_;
    unsigned int max_lost_;
    unsigned long long next_id_;
    std::vector<TrackPtr> tracks_;
};

ByteTracker::ByteTracker(float high, float fresh, float first, float second, unsigned int lost)
    : impl_(new Impl(high, fresh, first, second, lost)) {}
ByteTracker::~ByteTracker() {}
std::vector<ByteTrackResult> ByteTracker::update(const std::vector<ByteTrackDetection>& detections, double now) { return impl_->update(detections, now); }
ByteTrackStats ByteTracker::stats() const { return impl_->stats(); }
std::set<unsigned long long> ByteTracker::live_ids() const { return impl_->live_ids(); }
