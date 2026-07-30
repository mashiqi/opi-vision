#include "pipe_mode.h"
#include "detection_event_logger.h"
#include "yolo26_postprocess.h"
#include <opencv2/core/core.hpp>
#include <opencv2/imgproc.hpp>
#include <condition_variable>
#include <cstring>
#include <cstdio>
#include <mutex>
#include <thread>
#include <unistd.h>
#include <vector>

extern int yolo26_preprocess_mat(const cv::Mat&, void*, unsigned int);

namespace {
bool read_exact(FILE* f, unsigned char* p, size_t n) {
    size_t done = 0;
    while (done < n) {
        size_t got = fread(p + done, 1, n - done, f);
        if (!got) return false;
        done += got;
    }
    return true;
}

bool write_exact(FILE* f, const unsigned char* p, size_t n) {
    size_t done = 0;
    while (done < n) {
        size_t put = fwrite(p + done, 1, n - done, f);
        if (!put) return false;
        done += put;
    }
    return true;
}

void rotate_frame(const cv::Mat& source, cv::Mat& destination,
                  int rotation_degrees) {
    switch (rotation_degrees) {
    case 90:
        cv::rotate(source, destination, cv::ROTATE_90_CLOCKWISE);
        break;
    case 180:
        cv::rotate(source, destination, cv::ROTATE_180);
        break;
    case 270:
        cv::rotate(source, destination, cv::ROTATE_90_COUNTERCLOCKWISE);
        break;
    default:
        destination = source;
        break;
    }
}

bool bgr_to_nv12(const cv::Mat& bgr, std::vector<unsigned char>& nv12) {
    if (bgr.empty() || (bgr.cols & 1) != 0 || (bgr.rows & 1) != 0)
        return false;

    cv::Mat i420;
    cv::cvtColor(bgr, i420, cv::COLOR_BGR2YUV_I420);
    if (!i420.isContinuous())
        i420 = i420.clone();

    const size_t y_size =
        static_cast<size_t>(bgr.cols) * static_cast<size_t>(bgr.rows);
    const size_t chroma_size = y_size / 4;
    nv12.resize(y_size + chroma_size * 2);

    const unsigned char* source = i420.ptr<unsigned char>(0);
    std::memcpy(nv12.data(), source, y_size);
    const unsigned char* source_u = source + y_size;
    const unsigned char* source_v = source_u + chroma_size;
    unsigned char* destination_uv = nv12.data() + y_size;
    for (size_t index = 0; index < chroma_size; ++index) {
        destination_uv[index * 2] = source_u[index];
        destination_uv[index * 2 + 1] = source_v[index];
    }
    return true;
}
}

int run_pipe_mode(NetworkItem& network, int width, int height,
                  int video_output_fd, PipePixelFormat pixel_format,
                  int rotation_degrees) {
    FILE* video_output = fdopen(video_output_fd, "wb");
    if (video_output == NULL) {
        fprintf(stderr, "cannot open dedicated video output fd\n");
        close(video_output_fd);
        return -1;
    }
    setvbuf(video_output, NULL, _IONBF, 0);
    void* input = NULL;
    unsigned int input_size = 0;
    network.get_network_input_buff_info(0, &input, &input_size);

    std::mutex mutex;
    std::condition_variable ready;
    cv::Mat pending;
    bool has_pending = false;
    bool stopping = false;
    std::vector<YoloDetection> latest;
    bool latest_camera_moving = false;
    const bool swaps_dimensions =
        rotation_degrees == 90 || rotation_degrees == 270;
    const int output_width = swaps_dimensions ? height : width;
    const int output_height = swaps_dimensions ? width : height;
    DetectionEventLogger event_logger(output_width, output_height);

    std::thread npu([&] {
        const int count = network.get_output_cnt();
        std::vector<float*> output(count, NULL);
        std::vector<output_info_s> info(count);
        for (;;) {
            cv::Mat inference_frame;
            {
                std::unique_lock<std::mutex> lock(mutex);
                ready.wait(lock, [&] { return stopping || has_pending; });
                if (stopping && !has_pending) break;
                inference_frame = pending;
                pending.release();
                has_pending = false;
            }

            if (yolo26_preprocess_mat(inference_frame, input, input_size) != 0 ||
                network.network_input_output_set() != 0 ||
                network.network_run() != 0) {
                fprintf(stderr, "NPU inference failed\n");
                continue;
            }
            network.get_output_fp_nocopy(info.data());
            for (int i = 0; i < count; ++i) output[i] = info[i].ptr;

            std::vector<YoloDetection> detections;
            if (yolo26_decode_detections(inference_frame, output.data(), detections) == 0) {
                const bool camera_moving = event_logger.submit(detections, inference_frame);
                std::lock_guard<std::mutex> lock(mutex);
                latest.swap(detections);
                latest_camera_moving = camera_moving;
            }
        }
    });

    cv::Mat bgr_input(height, width, CV_8UC3);
    std::vector<unsigned char> nv12_input(
        static_cast<size_t>(width) * static_cast<size_t>(height) * 3 / 2);
    std::vector<unsigned char> nv12_output;
    const size_t input_bytes =
        pixel_format == PIPE_PIXEL_NV12 ? nv12_input.size() :
                                          bgr_input.total() *
                                              bgr_input.elemSize();
    int result = 0;
    for (;;) {
        unsigned char* input_data =
            pixel_format == PIPE_PIXEL_NV12 ? nv12_input.data() :
                                              bgr_input.data;
        if (!read_exact(stdin, input_data, input_bytes))
            break;

        if (pixel_format == PIPE_PIXEL_NV12) {
            cv::Mat nv12(height + height / 2, width, CV_8UC1,
                         nv12_input.data());
            cv::cvtColor(nv12, bgr_input, cv::COLOR_YUV2BGR_NV12);
        }

        cv::Mat frame;
        rotate_frame(bgr_input, frame, rotation_degrees);
        {
            std::lock_guard<std::mutex> lock(mutex);
            pending = frame.clone();
            has_pending = true;
        }
        ready.notify_one();

        std::vector<YoloDetection> detections;
        bool camera_moving = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            detections = latest;
            camera_moving = latest_camera_moving;
        }
        yolo26_draw_detections(frame, detections, camera_moving);
        const unsigned char* output_data = frame.data;
        size_t output_bytes = frame.total() * frame.elemSize();
        if (pixel_format == PIPE_PIXEL_NV12) {
            if (!bgr_to_nv12(frame, nv12_output)) {
                fprintf(stderr, "cannot convert annotated frame to NV12\n");
                result = -1;
                break;
            }
            output_data = nv12_output.data();
            output_bytes = nv12_output.size();
        }
        if (!write_exact(video_output, output_data, output_bytes)) {
            fprintf(stderr, "video output stopped accepting frames\n");
            result = -1;
            break;
        }
        event_logger.note_output_frame();
    }

    {
        std::lock_guard<std::mutex> lock(mutex);
        stopping = true;
        has_pending = false;
        pending.release();
    }
    ready.notify_one();
    npu.join();
    fflush(video_output);
    fclose(video_output);
    return result;
}
