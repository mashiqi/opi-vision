/*
 * Company: AW
 * Extended for image input and GStreamer-piped video on Allwinner A733.
 */

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>
#include <sstream>
#include <string>
#include <vector>

#include <opencv2/core/core.hpp>
#include <opencv2/imgcodecs.hpp>

#include "npulib.h"
#include "pipe_mode.h"

#define USE_FP_NO_COPY 1

extern int yolo26_preprocess_mat(const cv::Mat& image, void* buff_ptr, unsigned int buff_size);
extern int yolo26_postprocess_mat(cv::Mat& image, float **output);

const char *usage =
    "YOLO26 A733 demo\n"
    "Image: yolo26_a733 -nb model.nb -i image.jpg [-l count]\n"
    "Video: yolo26_a733 -nb model.nb -v input -W width -H height -f fps [-o output.mp4]\n"
    "Pipe:  yolo26_a733 -nb model.nb --pipe -W width -H height"
    " [--pixel-format bgr|nv12] [--rotate 0|90|180|270]\n"
    "-nb path: NBG model path\n"
    "-i path:  input image path\n"
    "-v path:  input video decoded by GStreamer\n"
    "-W value: decoded/output frame width\n"
    "-H value: decoded/output frame height\n"
    "-f value: decoded/output integer frame rate\n"
    "--pixel-format value: pipe pixel format (default: bgr)\n"
    "--rotate value: rotate pipe frames clockwise before inference\n"
    "-o path:  H.264 MP4 output path (default: out_yolo26.mp4)\n"
    "-l count: repeat count for image mode (default: 1)\n"
    "-m MiB:   compatibility parameter retained from the original demo\n"
    "-h:       show help\n";

static uint64_t get_time_us()
{
    struct timeval time;
    gettimeofday(&time, NULL);
    return (uint64_t)(time.tv_usec + time.tv_sec * 1000000);
}

static std::string shell_quote(const char* value)
{
    std::string result("'");
    for (const char* p = value; *p != '\0'; ++p) {
        if (*p == '\'') {
            result += "'\\''";
        } else {
            result += *p;
        }
    }
    result += "'";
    return result;
}

static bool read_exact(FILE* stream, unsigned char* data, size_t size)
{
    size_t offset = 0;
    while (offset < size) {
        const size_t count = fread(data + offset, 1, size - offset, stream);
        if (count == 0) {
            return false;
        }
        offset += count;
    }
    return true;
}

static bool write_exact(FILE* stream, const unsigned char* data, size_t size)
{
    size_t offset = 0;
    while (offset < size) {
        const size_t count = fwrite(data + offset, 1, size - offset, stream);
        if (count == 0) {
            return false;
        }
        offset += count;
    }
    return true;
}

static std::string make_input_pipeline(const char* input, int width, int height, int fps)
{
    std::ostringstream command;
    command << "gst-launch-1.0 -q "
            << "filesrc location=" << shell_quote(input)
            << " ! decodebin ! videoconvert ! videoscale ! videorate"
            << " ! video/x-raw,format=BGR,width=" << width
            << ",height=" << height << ",framerate=" << fps << "/1"
            << " ! fdsink fd=1 sync=false 2>gst-input.log";
    return command.str();
}

static std::string make_output_pipeline(const char* output, int width, int height, int fps)
{
    std::ostringstream command;
    command << "gst-launch-1.0 -q fdsrc fd=0"
            << " ! rawvideoparse format=bgr width=" << width
            << " height=" << height << " framerate=" << fps << "/1"
            << " ! videoconvert"
            << " ! x264enc tune=zerolatency speed-preset=ultrafast bitrate=2000 key-int-max=" << fps
            << " ! h264parse ! mp4mux faststart=true"
            << " ! filesink location=" << shell_quote(output)
            << " 2>gst-output.log";
    return command.str();
}

static int run_one_frame(NetworkItem& network, cv::Mat& frame,
                         void* input_buffer_ptr, unsigned int input_buffer_size,
                         float** output_data, unsigned int network_id,
                         uint64_t* inference_us)
{
    if (yolo26_preprocess_mat(frame, input_buffer_ptr, input_buffer_size) != 0) {
        return -1;
    }

    int status = network.network_input_output_set();
    if (status != 0) {
        fprintf(stderr, "set network input/output %u failed, status=%d\n", network_id, status);
        return -1;
    }

    const uint64_t begin = get_time_us();
    status = network.network_run();
    const uint64_t elapsed = get_time_us() - begin;
    if (status != 0) {
        fprintf(stderr, "run network %u failed, status=%d\n", network_id, status);
        return -1;
    }
    if (inference_us != NULL) {
        *inference_us = elapsed;
    }

#if !USE_FP_NO_COPY
    network.get_output(output_data);
#else
    const int output_count = network.get_output_cnt();
    std::vector<output_info_s> output_info(output_count);
    network.get_output_fp_nocopy(output_info.data());
    for (int i = 0; i < output_count; ++i) {
        output_data[i] = output_info[i].ptr;
    }
#endif

    return yolo26_postprocess_mat(frame, output_data);
}

int main(int argc, char** argv)
{
    char* model_file = NULL;
    char* image_file = NULL;
    char* video_file = NULL;
    char* output_file = NULL;
    unsigned int loop_count = 1;
    unsigned int malloc_mbyte = 10;
    int video_width = 0;
    int video_height = 0;
    int video_fps = 0;
    bool pipe_mode = false;
    PipePixelFormat pipe_pixel_format = PIPE_PIXEL_BGR;
    int pipe_rotation = 0;

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "-nb") && i + 1 < argc) {
            model_file = argv[++i];
        } else if (!strcmp(argv[i], "-i") && i + 1 < argc) {
            image_file = argv[++i];
        } else if (!strcmp(argv[i], "-v") && i + 1 < argc) {
            video_file = argv[++i];
        } else if (!strcmp(argv[i], "-o") && i + 1 < argc) {
            output_file = argv[++i];
        } else if (!strcmp(argv[i], "--pipe")) {
            pipe_mode = true;
        } else if (!strcmp(argv[i], "--pixel-format") && i + 1 < argc) {
            const char* value = argv[++i];
            if (!strcmp(value, "bgr")) {
                pipe_pixel_format = PIPE_PIXEL_BGR;
            } else if (!strcmp(value, "nv12")) {
                pipe_pixel_format = PIPE_PIXEL_NV12;
            } else {
                fprintf(stderr, "unsupported pipe pixel format: %s\n%s",
                        value, usage);
                return -1;
            }
        } else if (!strcmp(argv[i], "--rotate") && i + 1 < argc) {
            pipe_rotation = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "-W") && i + 1 < argc) {
            video_width = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "-H") && i + 1 < argc) {
            video_height = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "-f") && i + 1 < argc) {
            video_fps = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "-l") && i + 1 < argc) {
            loop_count = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "-m") && i + 1 < argc) {
            malloc_mbyte = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "-h")) {
            printf("%s", usage);
            return 0;
        } else {
            fprintf(stderr, "unknown or incomplete option: %s\n%s", argv[i], usage);
            return -1;
        }
    }

    const int selected_modes = (image_file != NULL) + (video_file != NULL) + (pipe_mode ? 1 : 0);
    if (model_file == NULL || selected_modes != 1) {
        fprintf(stderr, "%s", usage);
        return -1;
    }
    if (video_file != NULL && (video_width <= 0 || video_height <= 0 || video_fps <= 0)) {
        fprintf(stderr, "video mode requires positive -W, -H, and -f values\n%s", usage);
        return -1;
    }
    if (pipe_mode && (video_width <= 0 || video_height <= 0)) {
        fprintf(stderr, "pipe mode requires positive -W and -H values\n%s", usage);
        return -1;
    }
    if (pipe_mode && (video_width % 2 != 0 || video_height % 2 != 0)) {
        fprintf(stderr, "pipe mode requires even frame dimensions\n%s", usage);
        return -1;
    }
    if (pipe_rotation != 0 && pipe_rotation != 90 &&
        pipe_rotation != 180 && pipe_rotation != 270) {
        fprintf(stderr, "--rotate must be 0, 90, 180, or 270\n%s", usage);
        return -1;
    }

    // Preserve the raw-video channel before closed-source NPU code can log to
    // stdout. From this point onward stdout is redirected to the log channel.
    int video_output_fd = -1;
    if (pipe_mode) {
        fflush(stdout);
        video_output_fd = dup(STDOUT_FILENO);
        if (video_output_fd < 0 || dup2(STDERR_FILENO, STDOUT_FILENO) < 0) {
            fprintf(stderr, "cannot isolate raw video output from stdout\n");
            if (video_output_fd >= 0) close(video_output_fd);
            return -1;
        }
    }

    fprintf(stderr, "model=%s, mode=%s, input=%s, loop=%u, malloc_mbyte=%u\n",
            model_file, pipe_mode ?
                (pipe_pixel_format == PIPE_PIXEL_NV12 ? "raw-nv12-pipe" :
                                                        "raw-bgr-pipe") :
                (video_file != NULL ? "video-gstreamer" : "image"),
            pipe_mode ? "stdin" : (video_file != NULL ? video_file : image_file), loop_count, malloc_mbyte);

    NpuUint npu_unit;
    int status = npu_unit.npu_init();
    if (status != 0) {
        fprintf(stderr, "NPU init failed, status=%d\n", status);
        return -1;
    }

    NetworkItem network;
    const unsigned int network_id = 0;
    status = network.network_create(model_file, network_id);
    if (status != 0) {
        fprintf(stderr, "network create failed, status=%d\n", status);
        return -1;
    }
    status = network.network_prepare();
    if (status != 0) {
        fprintf(stderr, "network prepare failed, status=%d\n", status);
        return -1;
    }
    if (pipe_mode) {
        signal(SIGPIPE, SIG_IGN);
        return run_pipe_mode(network, video_width, video_height,
                             video_output_fd, pipe_pixel_format,
                             pipe_rotation);
    }

    void* input_buffer_ptr = NULL;
    unsigned int input_buffer_size = 0;
    network.get_network_input_buff_info(0, &input_buffer_ptr, &input_buffer_size);
    fprintf(stderr, "input buffer=%p, size=%u\n", input_buffer_ptr, input_buffer_size);

    const int output_count = network.get_output_cnt();
    float** output_data = new float*[output_count]();
#if !USE_FP_NO_COPY
    for (int i = 0; i < output_count; ++i) {
        output_data[i] = new float[network.m_output_data_len[i]];
    }
#endif

    int result = 0;
    uint64_t total_inference_us = 0;
    unsigned int processed_frames = 0;

    if (image_file != NULL) {
        cv::Mat image = cv::imread(image_file, cv::IMREAD_COLOR);
        if (image.empty()) {
            fprintf(stderr, "cannot read image: %s\n", image_file);
            result = -1;
        } else {
            for (unsigned int i = 0; i < loop_count; ++i) {
                cv::Mat annotated = image.clone();
                uint64_t inference_us = 0;
                result = run_one_frame(network, annotated, input_buffer_ptr, input_buffer_size,
                                       output_data, network_id, &inference_us);
                if (result != 0) {
                    break;
                }
                total_inference_us += inference_us;
                ++processed_frames;
                if (i + 1 == loop_count) {
                    cv::imwrite("out_yolo26.png", annotated);
                }
            }
        }
    } else {
        const char* encoded_output = output_file != NULL ? output_file : "out_yolo26.mp4";
        const std::string input_command = make_input_pipeline(video_file, video_width, video_height, video_fps);
        const std::string output_command = make_output_pipeline(encoded_output, video_width, video_height, video_fps);
        fprintf(stderr, "video: %dx%d %d FPS, output=%s\n", video_width, video_height, video_fps, encoded_output);

        signal(SIGPIPE, SIG_IGN);
        FILE* input_pipe = popen(input_command.c_str(), "r");
        FILE* output_pipe = popen(output_command.c_str(), "w");
        if (input_pipe == NULL || output_pipe == NULL) {
            fprintf(stderr, "cannot start GStreamer input/output pipelines\n");
            if (input_pipe != NULL) pclose(input_pipe);
            if (output_pipe != NULL) pclose(output_pipe);
            result = -1;
        } else {
            cv::Mat frame(video_height, video_width, CV_8UC3);
            const size_t frame_bytes = frame.total() * frame.elemSize();
            const uint64_t video_begin = get_time_us();

            while (read_exact(input_pipe, frame.data, frame_bytes)) {
                uint64_t inference_us = 0;
                result = run_one_frame(network, frame, input_buffer_ptr, input_buffer_size,
                                       output_data, network_id, &inference_us);
                if (result != 0) {
                    break;
                }
                if (!write_exact(output_pipe, frame.data, frame_bytes)) {
                    fprintf(stderr, "GStreamer output pipeline stopped accepting frames\n");
                    result = -1;
                    break;
                }
                total_inference_us += inference_us;
                ++processed_frames;
                if (processed_frames % 30 == 0) {
                    fprintf(stderr, "processed %u frames, last NPU inference %.2f ms\n",
                           processed_frames, inference_us / 1000.0);
                }
            }

            fflush(output_pipe);
            const int output_status = pclose(output_pipe);
            const int input_status = pclose(input_pipe);
            if (result == 0 && (input_status != 0 || output_status != 0)) {
                fprintf(stderr, "GStreamer failed: input status=%d, output status=%d; see gst-*.log\n",
                        input_status, output_status);
                result = -1;
            }

            const uint64_t video_elapsed = get_time_us() - video_begin;
            if (processed_frames > 0) {
                fprintf(stderr, "video complete: %u frames, wall %.2f FPS\n", processed_frames,
                       processed_frames * 1000000.0 / video_elapsed);
            }
        }
    }

    if (processed_frames > 0 && total_inference_us > 0) {
        fprintf(stderr, "average NPU inference: %.3f ms (%.2f FPS, inference only)\n",
               total_inference_us / processed_frames / 1000.0,
               processed_frames * 1000000.0 / total_inference_us);
    }

    for (int i = 0; i < output_count; ++i) {
#if !USE_FP_NO_COPY
        delete[] output_data[i];
#endif
        output_data[i] = NULL;
    }
    delete[] output_data;

    return result;
}
