/*
 * nv12-timestamp — 在 NV12 原始视频帧上叠加系统实时时间戳
 *
 * 从 stdin 读取 NV12 帧，用标准 VGA 8×16 位图字体在 Y 平面上
 * 绘制当前系统时间，输出修改后的帧到 stdout。
 *
 * 零外部依赖（纯 libc）。旋转预补偿确保编码器旋转后时间戳在最终画面左上角正向。
 *
 * 用法:
 *   nv12-timestamp --width 1280 --height 720 --rotate 180 [--margin 8]
 *
 * 编译:
 *   cc -O2 -Wall -Wextra -o nv12-timestamp nv12-timestamp.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <io.h>
#define READ  _read
#define WRITE _write
#else
#include <unistd.h>
#define READ  read
#define WRITE write
#endif

/* ── ASCII → 字形索引映射表（未定义字符返回 0=空格） ─── */

static const unsigned char font_index[128] = {
    [0x20] =  0,                                        /*   */
    [0x2D] =  1,                                        /* - */
    [0x30] =  2, [0x31] =  3, [0x32] =  4, [0x33] =  5, /* 0-3 */
    [0x34] =  6, [0x35] =  7, [0x36] =  8, [0x37] =  9, /* 4-7 */
    [0x38] = 10, [0x39] = 11,                           /* 8-9 */
    [0x3A] = 12,                                        /* : */
};

/* ── 标准 VGA 8×16 位图字体（13 字符，按 font_index 编号） ─
 * 每字符 16 字节，每字节对应一行 8 像素（bit 7 = 最左像素）。*/

static const unsigned char font[13][16] = {
    [0] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /*   */
    [1] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFE,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* - */
    [2] = {0x00,0x00,0x38,0x6C,0xC6,0xC6,0xD6,0xD6,0xC6,0xC6,0x6C,0x38,0x00,0x00,0x00,0x00}, /* 0 */
    [3] = {0x00,0x00,0x18,0x38,0x78,0x18,0x18,0x18,0x18,0x18,0x18,0x7E,0x00,0x00,0x00,0x00}, /* 1 */
    [4] = {0x00,0x00,0x7C,0xC6,0x06,0x0C,0x18,0x30,0x60,0xC0,0xC6,0xFE,0x00,0x00,0x00,0x00}, /* 2 */
    [5] = {0x00,0x00,0x7C,0xC6,0x06,0x06,0x3C,0x06,0x06,0x06,0xC6,0x7C,0x00,0x00,0x00,0x00}, /* 3 */
    [6] = {0x00,0x00,0x0C,0x1C,0x3C,0x6C,0xCC,0xFE,0x0C,0x0C,0x0C,0x1E,0x00,0x00,0x00,0x00}, /* 4 */
    [7] = {0x00,0x00,0xFE,0xC0,0xC0,0xC0,0xFC,0x06,0x06,0x06,0xC6,0x7C,0x00,0x00,0x00,0x00}, /* 5 */
    [8] = {0x00,0x00,0x38,0x60,0xC0,0xC0,0xFC,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00}, /* 6 */
    [9] = {0x00,0x00,0xFE,0xC6,0x06,0x06,0x0C,0x18,0x30,0x30,0x30,0x30,0x00,0x00,0x00,0x00}, /* 7 */
   [10] = {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0x7C,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00}, /* 8 */
   [11] = {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0xC6,0x7E,0x06,0x06,0x0C,0x78,0x00,0x00,0x00,0x00}, /* 9 */
   [12] = {0x00,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x00,0x00}, /* : */
};

#define CHAR_W  8
#define CHAR_H 16


/*
 * 采样字符区域的背景亮度
 * 
 * 参数说明：
 *   @frame      : NV12 帧的 Y 平面数据（输入帧，旋转前）
 *   @W_in       : 输入帧宽度（旋转前）
 *   @H_in       : 输入帧高度（旋转前）
 *   @char_ox    : 字符左上角在输出空间（旋转后）的 X 坐标
 *   @char_oy    : 字符左上角在输出空间（旋转后）的 Y 坐标
 *   @rotate     : 编码器将应用的顺时针旋转角度（0/90/180/270）
 *
 * 返回值：
 *   返回字符区域的平均亮度值 (0-255)，如果采样失败则返回 128
 *
 * 注意：
 *   - 本函数在绘制字符之前调用，此时 frame 中尚未写入任何时间戳像素
 *   - 采样区域为字符本身的 8×16 区域 (即将绘制的位置)
 *   - 由于采样先于绘制，因此不会采到时间戳本身，无正反馈问题
 *   - 通过逆旋转映射，确保采样位置与绘制位置一致
 */
static int sample_background(
    unsigned char *frame,    /* 输入帧 Y 平面数据 */
    int W_in,                /* 输入帧宽度（旋转前） */
    int H_in,                /* 输入帧高度（旋转前） */
    int char_ox,             /* 输出空间字符左上角 X（旋转后） */
    int char_oy,             /* 输出空间字符左上角 Y（旋转后） */
    int rotate)              /* 编码器旋转角度：0/90/180/270 */
{
    int sum = 0;
    int count = 0;

    /* 采样字符区域 (此时 frame 还是原始帧，未绘制任何像素) */
    //int step_H = CHAR_H / 2;
    //int step_W = CHAR_W / 2;
    for (int dy = 0; dy < CHAR_H; dy++) {
        for (int dx = 0; dx < CHAR_W; dx++) {
            int ox = char_ox + dx;
            int oy = char_oy + dy;

            /* 逆映射到输入帧坐标 */
            int ix, iy;
            switch (rotate) {
            case 0:
                ix = ox; iy = oy; break;
            case 90:
                ix = oy; iy = H_in - 1 - ox; break;
            case 180:
                ix = W_in - 1 - ox; iy = H_in - 1 - oy; break;
            case 270:
                ix = W_in - 1 - oy; iy = ox; break;
            default:
                return 150;
            }

            if (ix < 0 || ix >= W_in || iy < 0 || iy >= H_in) continue;

            sum += frame[iy * W_in + ix];
            count++;
        }
    }

    return (count > 0) ? (sum / count) : 150;
}



/*
 * 在输出空间中绘制一个字符
 * 
 * 参数说明：
 *   @frame      : NV12 帧的 Y 平面数据（输入帧，旋转前）
 *   @W_in       : 输入帧宽度（旋转前）
 *   @H_in       : 输入帧高度（旋转前）
 *   @char_ox    : 字符左上角在输出空间（旋转后）的 X 坐标
 *   @char_oy    : 字符左上角在输出空间（旋转后）的 Y 坐标
 *   @ch         : 要绘制的字符（ASCII）
 *   @rotate     : 编码器将应用的顺时针旋转角度（0/90/180/270）
 *
 * 注意：
 *   - 本函数在输入帧上绘制，但坐标计算基于输出空间
 *   - 通过逆旋转映射，确保编码器旋转后文字在正确位置
 *   - (char_ox, char_oy) = 字符左上角在最终输出画面中的坐标
 *   - 函数内部会根据字符区域的背景亮度自动选择文字颜色
 */
static void draw_char_output_space(
    unsigned char *frame,    /* 输入帧 Y 平面数据 */
    int W_in,                /* 输入帧宽度（旋转前） */
    int H_in,                /* 输入帧高度（旋转前） */
    int char_ox,             /* 输出空间字符左上角 X（旋转后） */
    int char_oy,             /* 输出空间字符左上角 Y（旋转后） */
    unsigned char ch,        /* 要绘制的 ASCII 字符 */
    int rotate)              /* 编码器旋转角度：0/90/180/270 */
{
    const unsigned char *glyph = font[font_index[ch]];
    int brow, bcol;
    
    // 第一步：采样背景（在绘制之前，使用 char_ox, char_oy）
    int avg_bg = sample_background(frame, W_in, H_in, char_ox, char_oy, rotate);
    int color = (avg_bg < 150) ? 200 : 50;
    
    // 第二步：用计算好的颜色绘制
    for (brow = 0; brow < CHAR_H; brow++) {
        unsigned char row = glyph[brow];
        for (bcol = 0; bcol < CHAR_W; bcol++) {
            if (!(row & (1 << (7 - bcol))))
                continue;

            /* 该像素在输出画面中的x坐标 */
            int ox = char_ox + bcol;
            int oy = char_oy + brow;

            /* 逆映射到输入帧坐标 */
            int ix, iy;
            switch (rotate) {
            case 0:
                ix = ox; iy = oy; break;
            case 90:
                ix = oy; iy = H_in - 1 - ox; break;
            case 180:
                ix = W_in - 1 - ox; iy = H_in - 1 - oy; break;
            case 270:
                ix = W_in - 1 - oy; iy = ox; break;
            default:
                return;
            }

            if (ix < 0 || ix >= W_in || iy < 0 || iy >= H_in) continue;
            
            /* 写入 Y 平面 */
            frame[iy * W_in + ix] = color;
        }
    }
}

static void print_usage(const char *prog)
{
    fprintf(stderr,
        "用法: %s --width <W> --height <H> --rotate <0|90|180|270> [--margin <N>]\n"
        "\n"
        "在 NV12 原始视频帧上叠加系统实时时间戳 (\"YYYY-MM-DD HH:MM:SS\").\n"
        "从 stdin 读取帧, 输出到 stdout.\n"
        "\n"
        "  --width    输入帧宽度 (编码前)\n"
        "  --height   输入帧高度 (编码前)\n"
        "  --rotate   编码器将应用的顺时针旋转角度\n"
        "  --margin   文字距边缘的像素, 默认 8\n",
        prog);
}

int main(int argc, char *argv[])
{
    int W = 0, H = 0, rotate = -1, margin = 8;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--width") && i + 1 < argc)
            W = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--height") && i + 1 < argc)
            H = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--rotate") && i + 1 < argc)
            rotate = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--margin") && i + 1 < argc)
            margin = atoi(argv[++i]);
        else {
            print_usage(argv[0]);
            return 1;
        }
    }

    if (W <= 0 || H <= 0) {
        fprintf(stderr, "错误: 必须指定有效的 --width 和 --height\n");
        return 1;
    }
    if (rotate != 0 && rotate != 90 && rotate != 180 && rotate != 270) {
        fprintf(stderr, "错误: --rotate 必须是 0, 90, 180 或 270\n");
        return 1;
    }
    if (margin < 0) margin = 0;

    size_t frame_size = (size_t)W * H * 3 / 2;
    unsigned char *frame = (unsigned char *)malloc(frame_size);
    if (!frame) {
        perror("malloc");
        return 1;
    }

    char time_str[32];

    for (;;) {
        size_t total = 0;
        while (total < frame_size) {
            ssize_t n = (ssize_t)READ(STDIN_FILENO, frame + total, frame_size - total);
            if (n <= 0) {
                free(frame);
                return n == 0 ? 0 : 1;
            }
            total += (size_t)n;
        }

        time_t now_ts = time(NULL);
        struct tm tm_now;
        localtime_r(&now_ts, &tm_now);
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &tm_now);

        int text_len = (int)strlen(time_str);
        int k;
        for (k = 0; k < text_len; k++) {
            draw_char_output_space(frame, W, H, margin + k * CHAR_W, margin, (unsigned char)time_str[k], rotate);
        }

        total = 0;
        while (total < frame_size) {
            ssize_t n = (ssize_t)WRITE(STDOUT_FILENO, frame + total, frame_size - total);
            if (n <= 0) {
                free(frame);
                return n == 0 ? 0 : 1;
            }
            total += (size_t)n;
        }
    }
}
