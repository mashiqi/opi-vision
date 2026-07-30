#include "detection_event_logger.h"
#include "bytetrack.h"
#include "model_config.h"
#include "movement_classifier.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <opencv2/imgproc/imgproc.hpp>

namespace {

unsigned long long env_unsigned(const char* name, unsigned long long fallback,
                                unsigned long long minimum, unsigned long long maximum) {
    const char* raw = std::getenv(name);
    if (!raw || !*raw) return fallback;
    char* end = NULL;
    const unsigned long long value = std::strtoull(raw, &end, 10);
    if (end == raw || *end || value < minimum || value > maximum) {
        std::fprintf(stderr, "invalid %s=%s; using %llu\n", name, raw, fallback);
        return fallback;
    }
    return value;
}

double steady_seconds(std::chrono::steady_clock::time_point value) {
    return std::chrono::duration_cast<std::chrono::duration<double> >(value.time_since_epoch()).count();
}

std::string utc_timestamp(std::chrono::system_clock::time_point value) {
    const std::time_t seconds = std::chrono::system_clock::to_time_t(value);
    std::tm utc; gmtime_r(&seconds, &utc);
    const long long millis = std::chrono::duration_cast<std::chrono::milliseconds>(value.time_since_epoch()).count() % 1000;
    std::ostringstream out;
    out << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S") << '.' << std::setfill('0') << std::setw(3) << millis << 'Z';
    return out.str();
}

std::string process_instance_id() {
    const std::time_t now = std::time(NULL); std::tm utc; gmtime_r(&now, &utc);
    std::ostringstream out; out << std::put_time(&utc, "%Y%m%dT%H%M%SZ") << '-' << getpid(); return out.str();
}

std::string json_escape(const std::string& value) {
    std::ostringstream out;
    for (size_t i = 0; i < value.size(); ++i) {
        const unsigned char ch = static_cast<unsigned char>(value[i]);
        switch (ch) {
        case '"': out << "\\\""; break; case '\\': out << "\\\\"; break;
        case '\n': out << "\\n"; break; case '\r': out << "\\r"; break; case '\t': out << "\\t"; break;
        default: if (ch < 0x20) out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(ch) << std::dec;
                 else out << static_cast<char>(ch);
        }
    }
    return out.str();
}

std::string class_name(int label) {
    if (label >= 0 && static_cast<size_t>(label) < g_classes_name.size()) return g_classes_name[label];
    std::ostringstream out; out << "class_" << label; return out.str();
}

bool repair_partial_jsonl_tail(const std::string& path) {
    const int fd = open(path.c_str(), O_RDWR);
    if (fd < 0) return errno == ENOENT;
    const off_t end = lseek(fd, 0, SEEK_END);
    if (end <= 0) { close(fd); return end == 0; }
    char ch = 0;
    if (pread(fd, &ch, 1, end - 1) != 1) { close(fd); return false; }
    if (ch == '\n') { close(fd); return true; }
    off_t cursor = end;
    while (cursor > 0) {
        --cursor;
        if (pread(fd, &ch, 1, cursor) != 1) { close(fd); return false; }
        if (ch == '\n') { ++cursor; break; }
    }
    const bool ok = ftruncate(fd, cursor) == 0; close(fd); return ok;
}

} // namespace

class DetectionEventLogger::Impl {
public:
    struct Observation {
        unsigned long long track_id;
        std::string tracking_state;
        std::string movement_state;
        float speed_diagonals_per_second;
    };
    struct Snapshot {
        unsigned long long inference_sequence;
        std::chrono::system_clock::time_point captured_at;
        std::vector<YoloDetection> detections;
        std::vector<Observation> observations;
        bool zones_stale;
        bool camera_moving;
    };
    Impl(int width, int height)
        : width_(width), height_(height), max_bytes_(5ULL * 1024 * 1024), backups_(3), interval_ms_(1000),
          instance_id_(process_instance_id()), inference_sequence_(0), next_global_id_(1), truncated_total_(0),
          output_frames_(0),
          tracking_samples_(0), tracking_sum_ms_(0), tracking_max_ms_(0), tracking_histogram_(201, 0),
          has_pending_(false), stopping_(false), failed_(false), have_last_snapshot_(false), have_scene_(false),
          camera_moving_until_(0), scene_shift_hits_(0), zones_stale_(false), last_metrics_write_(0) {
        const char* path = std::getenv("YOLO_DETECTION_LOG");
        if (path && *path) path_ = path;
        const char* metrics = std::getenv("YOLO_TRACKER_METRICS");
        if (metrics && *metrics) metrics_path_ = metrics;
        max_bytes_ = env_unsigned("YOLO_DETECTION_LOG_MAX_BYTES", max_bytes_, 1024, 1024ULL * 1024 * 1024);
        backups_ = static_cast<unsigned int>(env_unsigned("YOLO_DETECTION_LOG_BACKUPS", backups_, 1, 20));
        interval_ms_ = env_unsigned("YOLO_DETECTION_INTERVAL_MS", interval_ms_, 100, 60000);
        if (!path_.empty() && !repair_partial_jsonl_tail(path_)) {
            std::fprintf(stderr, "YOLO detection logging disabled: cannot repair %s\n", path_.c_str()); path_.clear();
        }
        if (!path_.empty()) {
            try { worker_ = std::thread(&Impl::run, this); }
            catch (...) { std::fprintf(stderr, "YOLO detection logging disabled: writer thread failed\n"); path_.clear(); }
        }
    }

    ~Impl() {
        if (!worker_.joinable()) return;
        { std::lock_guard<std::mutex> lock(mutex_); stopping_ = true; }
        ready_.notify_one(); worker_.join();
    }

    bool submit(std::vector<YoloDetection>& detections, const cv::Mat& frame) {
        const std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();
        const double now = steady_seconds(begin);
        ++inference_sequence_;
        update_scene(frame, now);

        std::vector<Observation> observations(detections.size());
        for (size_t i = 0; i < observations.size(); ++i) {
            observations[i].track_id = 0;
            observations[i].tracking_state = yolo26_is_tracked_label(detections[i].label) ? "unmatched" : "not_tracked";
            observations[i].movement_state = "not_applicable";
            observations[i].speed_diagonals_per_second = 0;
        }
        std::map<int, std::vector<ByteTrackDetection> > by_class;
        for (size_t i = 0; i < detections.size(); ++i) if (yolo26_is_tracked_label(detections[i].label)) {
            ByteTrackDetection item = {detections[i].rect, detections[i].prob, i};
            by_class[detections[i].label].push_back(item);
            if (!trackers_.count(detections[i].label)) trackers_[detections[i].label].reset(new ByteTracker());
        }
        size_t active = 0, lost = 0;
        size_t tracked_candidates = 0;
        for (std::map<int, std::vector<ByteTrackDetection> >::const_iterator it = by_class.begin(); it != by_class.end(); ++it)
            tracked_candidates += it->second.size();
        for (std::map<int, std::unique_ptr<ByteTracker> >::iterator it = trackers_.begin(); it != trackers_.end(); ++it) {
            const std::vector<ByteTrackDetection> empty;
            const std::vector<ByteTrackDetection>& candidates = by_class.count(it->first) ? by_class[it->first] : empty;
            const std::vector<ByteTrackResult> results = it->second->update(candidates, now);
            for (size_t r = 0; r < results.size(); ++r) {
                const std::pair<int, unsigned long long> key(it->first, results[r].local_id);
                if (!global_ids_.count(key)) global_ids_[key] = next_global_id_++;
                const unsigned long long global = global_ids_[key];
                Observation& observation = observations[results[r].source_index];
                observation.track_id = global;
                observation.tracking_state = results[r].track_state;
                const float diagonal = std::sqrt(static_cast<float>(width_ * width_ + height_ * height_));
                const float speed = diagonal > 0 ? std::sqrt(results[r].velocity_x * results[r].velocity_x + results[r].velocity_y * results[r].velocity_y) / diagonal : 0;
                observation.speed_diagonals_per_second = speed;
                observation.movement_state = results[r].confirmed ? motions_[global].update(speed, now) : "unknown";
                detections[results[r].source_index].rect = results[r].rect;
            }
            const ByteTrackStats stats = it->second->stats(); active += stats.active_tracks; lost += stats.lost_tracks;
            const std::set<unsigned long long> live = it->second->live_ids();
            for (std::map<std::pair<int, unsigned long long>, unsigned long long>::iterator mapping = global_ids_.begin(); mapping != global_ids_.end();) {
                if (mapping->first.first == it->first && !live.count(mapping->first.second)) {
                    motions_.erase(mapping->second);
                    global_ids_.erase(mapping++);
                } else ++mapping;
            }
        }
        truncated_total_ += yolo26_last_truncated_candidates();
        for (size_t i = 0; i < detections.size(); ++i) {
            detections[i].track_id = observations[i].track_id;
            detections[i].tracking_state = observations[i].tracking_state;
            detections[i].movement_state = observations[i].movement_state;
        }
        const bool camera_moving = now < camera_moving_until_;
        const double elapsed_ms = std::chrono::duration_cast<std::chrono::duration<double, std::milli> >(
            std::chrono::steady_clock::now() - begin).count();
        ++tracking_samples_;
        tracking_sum_ms_ += elapsed_ms;
        tracking_max_ms_ = std::max(tracking_max_ms_, elapsed_ms);
        const size_t histogram_bin = std::min<size_t>(tracking_histogram_.size() - 1,
                                                       static_cast<size_t>(elapsed_ms * 10.0));
        ++tracking_histogram_[histogram_bin];
        if (!metrics_path_.empty() && now - last_metrics_write_ >= 1.0) {
            write_metrics(tracked_candidates, active, lost); last_metrics_write_ = now;
        }

        if (path_.empty()) return camera_moving;
        if (have_last_snapshot_ && std::chrono::duration_cast<std::chrono::milliseconds>(begin - last_snapshot_).count() < static_cast<long long>(interval_ms_)) return camera_moving;
        last_snapshot_ = begin; have_last_snapshot_ = true;
        std::lock_guard<std::mutex> lock(mutex_);
        if (failed_ || stopping_) return camera_moving;
        pending_.inference_sequence = inference_sequence_;
        pending_.captured_at = std::chrono::system_clock::now();
        pending_.detections = detections; pending_.observations.swap(observations);
        pending_.zones_stale = zones_stale_; pending_.camera_moving = camera_moving;
        has_pending_ = true; ready_.notify_one();
        return camera_moving;
    }

    void note_output_frame() { ++output_frames_; }

private:
    void update_scene(const cv::Mat& frame, double now) {
        if (have_scene_ && now - last_scene_check_ < 0.5) return;
        cv::Mat gray, small, sample;
        cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY); cv::resize(gray, small, cv::Size(160, 90)); small.convertTo(sample, CV_32F);
        if (!scene_previous_.empty()) {
            double response = 0; const cv::Point2d shift = cv::phaseCorrelate(scene_previous_, sample, cv::noArray(), &response);
            const double normalized = std::sqrt(shift.x * shift.x + shift.y * shift.y) / std::sqrt(160.0 * 160.0 + 90.0 * 90.0);
            if (normalized > 0.015 && response > 0.10) camera_moving_until_ = now + 2.0;
            scene_shift_hits_ = normalized > 0.05 && response > 0.15 ? scene_shift_hits_ + 1 : 0;
            if (scene_shift_hits_ >= 3) zones_stale_ = true;
        }
        scene_previous_ = sample.clone(); last_scene_check_ = now; have_scene_ = true;
    }

    void write_metrics(size_t candidates, size_t active, size_t lost) const {
        if (!tracking_samples_) return;
        const unsigned long long target = static_cast<unsigned long long>(std::ceil(tracking_samples_ * 0.95));
        unsigned long long cumulative = 0; size_t p95_bin = 0;
        for (; p95_bin < tracking_histogram_.size(); ++p95_bin) {
            cumulative += tracking_histogram_[p95_bin];
            if (cumulative >= target) break;
        }
        const double p95_ms = p95_bin / 10.0;
        const std::string temporary = metrics_path_ + ".tmp";
        std::ofstream out(temporary.c_str(), std::ios::binary | std::ios::trunc);
        out << std::fixed << std::setprecision(6)
            << "{\"samples\":" << tracking_samples_ << ",\"average_ms\":" << tracking_sum_ms_ / tracking_samples_
            << ",\"p95_ms\":" << p95_ms << ",\"max_ms\":" << tracking_max_ms_
            << ",\"candidates\":" << candidates << ",\"active_tracks\":" << active
            << ",\"lost_tracks\":" << lost << ",\"truncated_total\":" << truncated_total_
            << ",\"inference_sequence\":" << inference_sequence_ << ",\"output_frames\":" << output_frames_.load() << "}\n";
        out.close(); if (out.good()) std::rename(temporary.c_str(), metrics_path_.c_str());
    }

    std::string encode(const Snapshot& value) const {
        std::ostringstream out; out << std::fixed << std::setprecision(6)
            << "{\"schema_version\":3,\"event_id\":\"vision-" << json_escape(instance_id_) << '-' << value.inference_sequence
            << "\",\"source_instance\":\"" << json_escape(instance_id_) << "\",\"timestamp\":\"" << utc_timestamp(value.captured_at)
            << "\",\"inference_sequence\":" << value.inference_sequence << ",\"image\":{\"width\":" << width_ << ",\"height\":" << height_
            << "},\"camera_moving\":" << (value.camera_moving ? "true" : "false") << ",\"zones_stale\":" << (value.zones_stale ? "true" : "false") << ",\"objects\":[";
        for (size_t i = 0; i < value.detections.size(); ++i) {
            if (i) out << ',';
            const YoloDetection& detection = value.detections[i]; const Observation& observation = value.observations[i];
            out << "{\"class\":\"" << json_escape(class_name(detection.label)) << "\",\"class_id\":" << detection.label
                << ",\"track_id\":" << observation.track_id << ",\"tracking_state\":\"" << observation.tracking_state
                << "\",\"movement_state\":\"" << observation.movement_state << "\",\"speed_diagonals_per_second\":" << observation.speed_diagonals_per_second
                << ",\"confidence\":" << detection.prob << ",\"bbox\":{\"x\":" << static_cast<int>(std::lround(detection.rect.x))
                << ",\"y\":" << static_cast<int>(std::lround(detection.rect.y)) << ",\"width\":" << static_cast<int>(std::lround(detection.rect.width))
                << ",\"height\":" << static_cast<int>(std::lround(detection.rect.height)) << "}}";
        }
        out << "]}\n"; return out.str();
    }

    bool rotate(size_t incoming) const {
        struct stat info; if (stat(path_.c_str(), &info) || static_cast<unsigned long long>(info.st_size) + incoming <= max_bytes_) return true;
        for (unsigned int i = backups_; i > 0; --i) {
            const std::string destination = path_ + '.' + std::to_string(i);
            if (i == backups_) std::remove(destination.c_str());
            const std::string source = i == 1 ? path_ : path_ + '.' + std::to_string(i - 1);
            if (std::rename(source.c_str(), destination.c_str()) && errno != ENOENT) return false;
        }
        return true;
    }

    bool append(const Snapshot& value) const {
        const std::string line = encode(value); if (!rotate(line.size())) return false;
        std::ofstream out(path_.c_str(), std::ios::binary | std::ios::app); out.write(line.data(), line.size()); out.flush(); return out.good();
    }

    void run() {
        for (;;) {
            Snapshot value;
            { std::unique_lock<std::mutex> lock(mutex_); ready_.wait(lock, [this] { return stopping_ || has_pending_; });
              if (stopping_ && !has_pending_) break; value = pending_; has_pending_ = false; }
            if (!append(value)) { std::lock_guard<std::mutex> lock(mutex_); failed_ = true; has_pending_ = false; }
        }
    }

    int width_, height_;
    std::string path_, metrics_path_;
    unsigned long long max_bytes_; unsigned int backups_; unsigned long long interval_ms_;
    std::string instance_id_; unsigned long long inference_sequence_, next_global_id_, truncated_total_;
    std::atomic<unsigned long long> output_frames_;
    std::map<int, std::unique_ptr<ByteTracker> > trackers_;
    std::map<std::pair<int, unsigned long long>, unsigned long long> global_ids_;
    std::map<unsigned long long, MovementClassifier> motions_;
    unsigned long long tracking_samples_;
    double tracking_sum_ms_, tracking_max_ms_;
    std::vector<unsigned long long> tracking_histogram_;
    std::mutex mutex_; std::condition_variable ready_; Snapshot pending_;
    bool has_pending_, stopping_, failed_, have_last_snapshot_;
    std::chrono::steady_clock::time_point last_snapshot_;
    std::thread worker_;
    cv::Mat scene_previous_; bool have_scene_; double last_scene_check_, camera_moving_until_;
    unsigned int scene_shift_hits_; bool zones_stale_; double last_metrics_write_;
};

DetectionEventLogger::DetectionEventLogger(int width, int height) : impl_(new Impl(width, height)) {}
DetectionEventLogger::~DetectionEventLogger() {}
bool DetectionEventLogger::submit(std::vector<YoloDetection>& detections, const cv::Mat& frame) { return impl_->submit(detections, frame); }
void DetectionEventLogger::note_output_frame() { impl_->note_output_frame(); }
