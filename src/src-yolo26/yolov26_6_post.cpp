/*
 * Company:    AW
 * Author:     Penng
 * Date:    2026/01/15
 */

#include <opencv2/core/core.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/imgproc/imgproc.hpp>
#include <opencv2/dnn.hpp>
#include <iostream>
#include <stdio.h>
#include <vector>
#include <cmath>
#include <algorithm>
#include <cstdlib>
#include <set>
#include <sstream>

#include "model_config.h"
#include "yolo26_postprocess.h"


using namespace std;


typedef YoloDetection Object;

struct ChineseGlyph { const char* text; unsigned short rows[16]; };
static const ChineseGlyph chinese_glyphs[] = {
    {"中", {0x0100,0x0100,0x0100,0x0100,0x3FF8,0x2108,0x2108,0x2108,0x2108,0x2108,0x3FF8,0x2108,0x0100,0x0100,0x0100,0x0100}},
    {"像", {0x0900,0x09F0,0x0A10,0x17FC,0x1A24,0x3244,0x33FC,0x5080,0x9144,0x16A8,0x1130,0x1668,0x10A8,0x1124,0x16A2,0x1040}},
    {"动", {0x0040,0x0040,0x7C40,0x0040,0x01FC,0x0044,0xFE44,0x2044,0x2044,0x2044,0x4884,0x4484,0xFD04,0x4504,0x2204,0x1004}},
    {"匹", {0x0000,0x7FFC,0x4440,0x4440,0x4440,0x4440,0x4440,0x4440,0x4444,0x4448,0x4448,0x3C50,0x0040,0x0040,0x7FFE,0x0000}},
    {"头", {0x0080,0x0080,0x0880,0x0480,0x2480,0x1080,0x1080,0x0080,0xFFFE,0x0100,0x0140,0x0220,0x0410,0x0808,0x3004,0xC004}},
    {"已", {0x0000,0x3FF0,0x0010,0x0010,0x1020,0x1020,0x3FF0,0x2000,0x2000,0x2000,0x2000,0x4200,0x4200,0x41FC,0x0000,0x0000}},
    {"待", {0x0840,0x0840,0x1040,0x23FC,0x4840,0x0840,0x17FE,0x3010,0x5010,0x97FE,0x1010,0x1210,0x1110,0x1110,0x1050,0x1020}},
    {"摄", {0x1000,0x13FC,0x1108,0x11F8,0xFD08,0x11F8,0x110E,0x17F8,0x1808,0x37BC,0xD0A4,0x12A4,0x1128,0x1290,0x54A8,0x2846}},
    {"未", {0x0100,0x0100,0x0100,0x3FF8,0x0100,0x0100,0x0100,0xFFFE,0x0380,0x0540,0x0920,0x1110,0x2108,0xC106,0x0100,0x0100}},
    {"止", {0x0100,0x0100,0x0100,0x0100,0x1100,0x1100,0x11F8,0x1100,0x1100,0x1100,0x1100,0x1100,0x1100,0x1100,0xFFFE,0x0000}},
    {"确", {0x0040,0x0040,0xFC7C,0x1084,0x1108,0x22FE,0x3C92,0x6492,0x64FE,0xA492,0x2492,0x24FE,0x3C92,0x2512,0x210A,0x0204}},
    {"移", {0x0820,0x1C20,0xF07C,0x1084,0x1148,0xFC30,0x1020,0x3048,0x3990,0x543E,0x5442,0x91A4,0x1018,0x1010,0x1060,0x1180}},
    {"认", {0x0040,0x2040,0x1040,0x1040,0x0040,0x0040,0xF040,0x1040,0x10A0,0x10A0,0x10A0,0x1510,0x1910,0x1208,0x0404,0x0802}},
    {"跟", {0x0000,0x7DF8,0x4508,0x4508,0x45F8,0x7D08,0x1108,0x11F8,0x5D44,0x5148,0x5130,0x5120,0x5D10,0xE148,0x0186,0x0100}},
    {"踪", {0x0040,0x7820,0x4BFE,0x4A02,0x4C04,0x79F8,0x1000,0x1000,0x53FE,0x5C20,0x5128,0x5124,0x5A22,0xE422,0x00A0,0x0040}},
    {"配", {0x0000,0xFE00,0x28F8,0x2808,0xFE08,0xAA08,0xAA08,0xAAF8,0xAE88,0xC280,0x8280,0xFE80,0x8282,0x8282,0xFE7E,0x8200}},
    {"静", {0x1040,0x1040,0xFE78,0x1088,0x7C10,0x11FC,0xFE24,0x0024,0x7DFE,0x4424,0x7C24,0x45FC,0x7C24,0x4420,0x54A0,0x4840}}
};

static void draw_chinese(cv::Mat& image, const std::string& text, int x, int y,
                         const cv::Scalar& color, int scale = 1)
{
    size_t offset = 0;
    while (offset < text.size()) {
        const ChineseGlyph* glyph = NULL;
        for (size_t i = 0; i < sizeof(chinese_glyphs) / sizeof(chinese_glyphs[0]); ++i) {
            const std::string value(chinese_glyphs[i].text);
            if (text.compare(offset, value.size(), value) == 0) { glyph = &chinese_glyphs[i]; offset += value.size(); break; }
        }
        if (!glyph) { ++offset; x += 16 * scale; continue; }
        for (int row = 0; row < 16; ++row) for (int col = 0; col < 16; ++col) {
            if (glyph->rows[row] & (0x8000u >> col))
                cv::rectangle(image, cv::Rect(x + col * scale, y + row * scale, scale, scale), color, -1);
        }
        x += 16 * scale;
    }
}

static std::string chinese_state(const Object& object)
{
    if (object.tracking_state == "tentative") return "待确认";
    if (object.tracking_state == "unmatched") return "未匹配";
    if (object.movement_state == "moving") return "移动中";
    if (object.movement_state == "stationary") return "静止";
    if (object.tracking_state == "tracked") return "已跟踪";
    return std::string();
}

static unsigned int g_last_truncated_candidates = 0;

static const std::set<int>& tracked_labels()
{
    static std::set<int> labels;
    static bool initialized = false;
    if (initialized) return labels;
    initialized = true;
    const char* raw = std::getenv("YOLO_TRACK_CLASSES");
    std::stringstream input(raw && *raw ? raw : "person,cat,dog");
    std::string name;
    while (std::getline(input, name, ',')) {
        name.erase(0, name.find_first_not_of(" \t"));
        const size_t end = name.find_last_not_of(" \t");
        if (end != std::string::npos) name.erase(end + 1);
        for (size_t i = 0; i < g_classes_name.size(); ++i)
            if (g_classes_name[i] == name) labels.insert(static_cast<int>(i));
    }
    return labels;
}

bool yolo26_is_tracked_label(int label) { return tracked_labels().count(label) != 0; }
unsigned int yolo26_last_truncated_candidates() { return g_last_truncated_candidates; }


static inline float sigmoid(float x)
{
    return 1.0f / (1.0f + expf(-x));
}


static void generate_proposals_6(int stride, const float* feat_grid, const float* feat_score, float prob_threshold, std::vector<Object>& objects,
                                int letterbox_cols, int letterbox_rows)
{
    const int num_grid_x = letterbox_cols / stride;
    const int num_grid_y = letterbox_rows / stride;
    const int num_grid_size = num_grid_x * num_grid_y;

    const int num_class = CLASS_NUM; // number of classes. 80 for COCO

    cv::Mat out_grid  = cv::Mat(4,         num_grid_size, CV_32FC1, (float*)feat_grid);
    cv::Mat out_score = cv::Mat(num_class, num_grid_size, CV_32FC1, (float*)feat_score);

    cv::transpose(out_grid,  out_grid);     // num_grid_size x 4
    cv::transpose(out_score, out_score);    // num_grid_size x num_class

    for (int y = 0; y < num_grid_y; y++)
    {
        for (int x = 0; x < num_grid_x; x++)
        {
            int num_grid_idx = y * num_grid_x + x;

            // find label with max score
            int label = -1;
            float score = -FLT_MAX;
            {
                const float *pred_score = (float*)out_score.data + num_grid_idx * num_class;

                for (int k = 0; k < num_class; k++)
                {
                    float s = *(pred_score + k);
                    if (s > score)
                    {
                        label = k;
                        score = s;
                    }
                }

                score = sigmoid(score);
            }

            const float class_threshold = yolo26_is_tracked_label(label) ? 0.10f : prob_threshold;
            if (score >= class_threshold)
            {
                const float *pred_grid = (float*)out_grid.data + num_grid_idx * 4;

                float x0 = x + 0.5f - pred_grid[0];
                float y0 = y + 0.5f - pred_grid[1];
                float x1 = x + 0.5f + pred_grid[2];
                float y1 = y + 0.5f + pred_grid[3];

                x0 *= stride;
                y0 *= stride;
                x1 *= stride;
                y1 *= stride;

                Object obj;
                obj.rect.x = x0;
                obj.rect.y = y0;
                obj.rect.width = x1 - x0;
                obj.rect.height = y1 - y0;
                obj.label = label;
                obj.prob = score;
                obj.track_id = 0;
                obj.tracking_state = "unmatched";
                obj.movement_state = "not_applicable";

                objects.push_back(obj);
            }
        }
    }
}

static int detect_yolo26_6_post(const cv::Mat& bgr, std::vector<Object> &detections, float **output)
{
    std::chrono::steady_clock::time_point Tbegin, Tend;
    Tbegin = std::chrono::steady_clock::now();

    const float  *p8_data_0_ptr = output[0];
    const float *p16_data_0_ptr = output[1];
    const float *p32_data_0_ptr = output[2];
    const float  *p8_data_1_ptr = output[3];    // cls
    const float *p16_data_1_ptr = output[4];
    const float *p32_data_1_ptr = output[5];

    int img_w = bgr.cols;
    int img_h = bgr.rows;

    // set default letterbox size
    int letterbox_rows = LETTERBOX_ROWS;
    int letterbox_cols = LETTERBOX_COLS;

    /* postprocess */
    const float prob_threshold = SCORE_THRESHOLD;

    std::vector<Object> proposals;
    std::vector<Object> objects8;
    std::vector<Object> objects16;
    std::vector<Object> objects32;

    {
        generate_proposals_6(8, p8_data_0_ptr, p8_data_1_ptr, prob_threshold, objects8, letterbox_cols, letterbox_rows);
        proposals.insert(proposals.end(), objects8.begin(), objects8.end());
    }

    {
        generate_proposals_6(16, p16_data_0_ptr, p16_data_1_ptr, prob_threshold, objects16, letterbox_cols, letterbox_rows);
        proposals.insert(proposals.end(), objects16.begin(), objects16.end());
    }

    {
        generate_proposals_6(32, p32_data_0_ptr, p32_data_1_ptr, prob_threshold, objects32, letterbox_cols, letterbox_rows);
        proposals.insert(proposals.end(), objects32.begin(), objects32.end());
    }


    float scale_letterbox = 1.0f;
    if ((letterbox_rows * 1.0 / bgr.rows) < (letterbox_cols * 1.0 / bgr.cols))
    {
        scale_letterbox = letterbox_rows * 1.0 / bgr.rows;
    }
    else
    {
        scale_letterbox = letterbox_cols * 1.0 / bgr.cols;
    }
    int resize_cols = int(round(scale_letterbox * bgr.cols));
    int resize_rows = int(round(scale_letterbox * bgr.rows));

    int hpad = (letterbox_rows - resize_rows);
    int wpad = (letterbox_cols - resize_cols);

    float ratio_y = (float)bgr.rows / resize_rows;
    float ratio_x = (float)bgr.cols / resize_cols;

    int count = proposals.size();

    std::vector<size_t> tracked;
    for (size_t i = 0; i < proposals.size(); ++i)
        if (yolo26_is_tracked_label(proposals[i].label)) tracked.push_back(i);
    g_last_truncated_candidates = tracked.size() > 100 ? static_cast<unsigned int>(tracked.size() - 100) : 0;
    if (g_last_truncated_candidates) {
        std::sort(tracked.begin(), tracked.end(), [&](size_t a, size_t b) { return proposals[a].prob > proposals[b].prob; });
        std::set<size_t> keep(tracked.begin(), tracked.begin() + 100);
        std::vector<Object> limited;
        limited.reserve(proposals.size() - g_last_truncated_candidates);
        for (size_t i = 0; i < proposals.size(); ++i)
            if (!yolo26_is_tracked_label(proposals[i].label) || keep.count(i)) limited.push_back(proposals[i]);
        proposals.swap(limited);
        count = proposals.size();
    }

    detections.resize(count);
    for (int i = 0; i < count; ++i)
    {
        detections[i] = proposals[i];

        // adjust offset to original unpadded
        float x0 = (detections[i].rect.x - (wpad / 2)) * ratio_x;
        float y0 = (detections[i].rect.y - (hpad / 2)) * ratio_y;
        float x1 = (detections[i].rect.x + detections[i].rect.width  - (wpad / 2)) * ratio_x;
        float y1 = (detections[i].rect.y + detections[i].rect.height - (hpad / 2)) * ratio_y;

        // clip
        x0 = std::max(std::min(x0, (float)(img_w - 1)), 0.f);
        y0 = std::max(std::min(y0, (float)(img_h - 1)), 0.f);
        x1 = std::max(std::min(x1, (float)(img_w - 1)), 0.f);
        y1 = std::max(std::min(y1, (float)(img_h - 1)), 0.f);

        detections[i].rect.x = x0;
        detections[i].rect.y = y0;
        detections[i].rect.width = x1 - x0;
        detections[i].rect.height = y1 - y0;
    }

    Tend = std::chrono::steady_clock::now();
    float f = std::chrono::duration_cast <std::chrono::milliseconds> (Tend - Tbegin).count();

    (void)f;

    return 0;
}

static void draw_objects(cv::Mat& image, const std::vector<Object>& objects, bool camera_moving)
{
    for (size_t i = 0; i < objects.size(); i++)
    {
        const Object& obj = objects[i];

        if (obj.prob > 1.0) {
            continue;
        }
        if (obj.prob < SCORE_THRESHOLD && !obj.track_id) {
            continue;
        }

        cv::rectangle(image, obj.rect, cv::Scalar(255, 0, 0));

        char prefix[256], confidence[64];
        if (obj.track_id) sprintf(prefix, "%s #%llu", g_classes_name[obj.label].c_str(), obj.track_id);
        else sprintf(prefix, "%s", g_classes_name[obj.label].c_str());
        sprintf(confidence, " %.1f%%", obj.prob * 100);
        const std::string status = chinese_state(obj);

        int baseLine = 0;
        cv::Size prefix_size = cv::getTextSize(prefix, cv::FONT_HERSHEY_SIMPLEX, 0.5, 1, &baseLine);
        cv::Size confidence_size = cv::getTextSize(confidence, cv::FONT_HERSHEY_SIMPLEX, 0.5, 1, &baseLine);
        const int chinese_width = static_cast<int>(status.size() / 3) * 16;
        const int label_width = prefix_size.width + (status.empty() ? 0 : 4 + chinese_width) + confidence_size.width;
        cv::Size label_size(label_width, std::max(prefix_size.height, 16));

        int x = obj.rect.x;
        int y = obj.rect.y - label_size.height - baseLine;
        if (y < 0)
            y = 0;
        if (x + label_size.width > image.cols)
            x = image.cols - label_size.width;
        if (x < 0)
            x = 0;

        cv::rectangle(image, cv::Rect(cv::Point(x, y), cv::Size(label_size.width, label_size.height + baseLine)),
            cv::Scalar(255, 255, 255), -1);

        cv::putText(image, prefix, cv::Point(x, y + label_size.height),
            cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0, 0, 0));
        int cursor = x + prefix_size.width + 4;
        if (!status.empty()) { draw_chinese(image, status, cursor, y, cv::Scalar(0, 0, 0)); cursor += chinese_width; }
        cv::putText(image, confidence, cv::Point(cursor, y + label_size.height),
            cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0, 0, 0));
    }
    if (camera_moving) {
        cv::rectangle(image, cv::Rect(8, 8, 16 * 7 + 8, 24), cv::Scalar(255, 255, 255), -1);
        draw_chinese(image, "摄像头移动中", 12, 12, cv::Scalar(0, 0, 255));
    }
}

int yolo26_postprocess_mat(cv::Mat& image, float **output)
{
    if (image.empty()) {
        fprintf(stderr, "postprocess input image is empty\n");
        return -1;
    }

    std::vector<Object> objects;
    detect_yolo26_6_post(image, objects, output);
    draw_objects(image, objects, false);
    return 0;
}

int yolo26_decode_detections(const cv::Mat& image, float** output,
                             std::vector<YoloDetection>& detections)
{
    if (image.empty()) {
        fprintf(stderr, "postprocess input image is empty\n");
        return -1;
    }
    return detect_yolo26_6_post(image, detections, output);
}

void yolo26_draw_detections(cv::Mat& image,
                            const std::vector<YoloDetection>& detections,
                            bool camera_moving)
{
    draw_objects(image, detections, camera_moving);
}

int yolo26_postprocess(const char *imagepath, float **output)
{
    cv::Mat m = cv::imread(imagepath, 1);
    if (m.empty()) {
        fprintf(stderr, "cv::imread %s failed\n", imagepath);
        return -1;
    }

    yolo26_postprocess_mat(m, output);
    cv::imwrite("out_yolo26.png", m);

    return 0;
}
