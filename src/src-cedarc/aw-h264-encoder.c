#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "memoryAdapter.h"
#include "sc_interface.h"
#include "vencoder.h"

#define PROGRAM_NAME "aw-h264-encoder"
#define PROGRAM_VERSION "0.1.11"

typedef struct EncoderOptions {
    unsigned int width;
    unsigned int height;
    unsigned int fps;
    unsigned int bitrate;
    unsigned int gop;
    unsigned int level;
    unsigned int rotate;
    uint64_t frame_limit;
} EncoderOptions;

typedef struct EncoderContext {
    struct ScMemOpsS *memops;
    VideoEncoder *encoder;
    bool memory_open;
    bool input_buffers_allocated;
} EncoderContext;

static volatile sig_atomic_t g_stop_requested;

static unsigned int align_to_16(unsigned int value)
{
    return (value + 15U) & ~15U;
}

static void handle_signal(int signo)
{
    (void)signo;
    g_stop_requested = 1;
}

static void print_usage(FILE *stream)
{
    fprintf(stream,
            "Usage: " PROGRAM_NAME " --width N --height N [options]\n"
            "\n"
            "Read tightly packed NV12 frames from stdin and write Annex-B H.264\n"
            "to stdout. Diagnostic messages are written to stderr.\n"
            "\n"
            "Required:\n"
            "  -W, --width N       Input width; must be even\n"
            "  -H, --height N      Input height; must be even\n"
            "\n"
            "Options:\n"
            "  -r, --fps N         Frame rate (default: 20)\n"
            "  -b, --bitrate N     Bit rate in bit/s (default: 2500000)\n"
            "  -g, --gop N         Maximum key-frame interval (default: 30)\n"
            "  -L, --level N       H.264 level: 10..52 from the supported list\n"
            "                       (default: 31; use 40 for 1080p24)\n"
            "  -R, --rotate N      Hardware rotation: 0, 1, 2, or 3\n"
            "                       (0/90/180/270 degrees; default: 0)\n"
            "                       1920x1080 does not support 1 or 3\n"
            "  -n, --frames N      Stop after N frames; 0 means until EOF\n"
            "  -h, --help          Show this help\n"
            "  -V, --version       Show program version\n");
}

static int parse_u32(const char *text, unsigned int min_value,
                     unsigned int max_value, const char *name,
                     unsigned int *result)
{
    char *end = NULL;
    unsigned long value;

    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' ||
        value < min_value || value > max_value) {
        fprintf(stderr, PROGRAM_NAME ": invalid %s: %s\n", name, text);
        return -1;
    }

    *result = (unsigned int)value;
    return 0;
}

static bool is_supported_h264_level(unsigned int level)
{
    switch (level) {
    case VENC_H264Level1:
    case VENC_H264Level11:
    case VENC_H264Level12:
    case VENC_H264Level13:
    case VENC_H264Level2:
    case VENC_H264Level21:
    case VENC_H264Level22:
    case VENC_H264Level3:
    case VENC_H264Level31:
    case VENC_H264Level32:
    case VENC_H264Level4:
    case VENC_H264Level41:
    case VENC_H264Level42:
    case VENC_H264Level5:
    case VENC_H264Level51:
    case VENC_H264Level52:
        return true;
    default:
        return false;
    }
}

static int parse_u64(const char *text, uint64_t *result)
{
    char *end = NULL;
    unsigned long long value;

    errno = 0;
    value = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0') {
        fprintf(stderr, PROGRAM_NAME ": invalid frame count: %s\n", text);
        return -1;
    }

    *result = (uint64_t)value;
    return 0;
}

static int parse_options(int argc, char **argv, EncoderOptions *options)
{
    static const struct option long_options[] = {
        {"width", required_argument, NULL, 'W'},
        {"height", required_argument, NULL, 'H'},
        {"fps", required_argument, NULL, 'r'},
        {"bitrate", required_argument, NULL, 'b'},
        {"gop", required_argument, NULL, 'g'},
        {"level", required_argument, NULL, 'L'},
        {"rotate", required_argument, NULL, 'R'},
        {"frames", required_argument, NULL, 'n'},
        {"help", no_argument, NULL, 'h'},
        {"version", no_argument, NULL, 'V'},
        {NULL, 0, NULL, 0},
    };
    int option;

    memset(options, 0, sizeof(*options));
    options->fps = 20;
    options->bitrate = 2500000;
    options->gop = 30;
    options->level = VENC_H264Level31;

    while ((option = getopt_long(argc, argv, "W:H:r:b:g:L:R:n:hV",
                                 long_options, NULL)) != -1) {
        switch (option) {
        case 'W':
            if (parse_u32(optarg, 16, 8192, "width", &options->width) != 0)
                return -1;
            break;
        case 'H':
            if (parse_u32(optarg, 2, 8192, "height", &options->height) != 0)
                return -1;
            break;
        case 'r':
            if (parse_u32(optarg, 1, 240, "fps", &options->fps) != 0)
                return -1;
            break;
        case 'b':
            if (parse_u32(optarg, 1000, INT_MAX, "bitrate",
                          &options->bitrate) != 0)
                return -1;
            break;
        case 'g':
            if (parse_u32(optarg, 1, 10000, "gop", &options->gop) != 0)
                return -1;
            break;
        case 'L':
            if (parse_u32(optarg, VENC_H264Level1, VENC_H264Level52,
                          "H.264 level", &options->level) != 0)
                return -1;
            break;
        case 'R':
            if (parse_u32(optarg, 0, 3, "rotation", &options->rotate) != 0)
                return -1;
            break;
        case 'n':
            if (parse_u64(optarg, &options->frame_limit) != 0)
                return -1;
            break;
        case 'h':
            print_usage(stdout);
            exit(EXIT_SUCCESS);
        case 'V':
            printf(PROGRAM_NAME " " PROGRAM_VERSION "\n");
            exit(EXIT_SUCCESS);
        default:
            print_usage(stderr);
            return -1;
        }
    }

    if (optind != argc) {
        fprintf(stderr, PROGRAM_NAME ": unexpected argument: %s\n",
                argv[optind]);
        return -1;
    }
    if (options->width == 0 || options->height == 0) {
        fprintf(stderr, PROGRAM_NAME ": --width and --height are required\n");
        return -1;
    }
    if ((options->width % 2U) != 0) {
        fprintf(stderr, PROGRAM_NAME ": width must be even\n");
        return -1;
    }
    if ((options->height % 2U) != 0) {
        fprintf(stderr, PROGRAM_NAME ": height must be even\n");
        return -1;
    }
    if (!is_supported_h264_level(options->level)) {
        fprintf(stderr, PROGRAM_NAME ": unsupported H.264 level: %u\n",
                options->level);
        return -1;
    }
    if (options->width == 1920U && options->height == 1080U &&
        (options->rotate == 1U || options->rotate == 3U)) {
        fprintf(stderr,
                PROGRAM_NAME
                ": 1920x1080 rotation %u is disabled: rotation 1 timed "
                "out on the tested A733 Cedarc/VE2 stack, so rotations "
                "1 and 3 are conservatively blocked; use rotation 0 or 2, or a "
                "tested four-rotation size such as 1280x720\n",
                options->rotate);
        return -1;
    }

    return 0;
}

/*
 * Return 1 when the buffer is full, 0 for clean EOF before a new frame,
 * -2 for EOF in the middle of a frame, and -1 for a read error.
 */
static int read_exact(int fd, unsigned char *buffer, size_t size,
                      bool frame_started)
{
    size_t offset = 0;

    while (offset < size) {
        ssize_t count = read(fd, buffer + offset, size - offset);

        if (count > 0) {
            offset += (size_t)count;
            frame_started = true;
            continue;
        }
        if (count == 0)
            return frame_started ? -2 : 0;
        if (errno == EINTR) {
            if (g_stop_requested)
                return frame_started ? -2 : 0;
            continue;
        }

        fprintf(stderr, PROGRAM_NAME ": stdin read failed: %s\n",
                strerror(errno));
        return -1;
    }

    return 1;
}

static int write_all(int fd, const unsigned char *buffer, size_t size)
{
    size_t offset = 0;

    while (offset < size) {
        ssize_t count = write(fd, buffer + offset, size - offset);

        if (count > 0) {
            offset += (size_t)count;
            continue;
        }
        if (count < 0 && errno == EINTR) {
            if (g_stop_requested)
                return -1;
            continue;
        }

        fprintf(stderr, PROGRAM_NAME ": stdout write failed: %s\n",
                count < 0 ? strerror(errno) : "zero-length write");
        return -1;
    }

    return 0;
}

static void cleanup_encoder(EncoderContext *context)
{
    if (context->input_buffers_allocated) {
        ReleaseAllocInputBuffer(context->encoder);
        context->input_buffers_allocated = false;
    }
    if (context->encoder != NULL) {
        VideoEncDestroy(context->encoder);
        context->encoder = NULL;
    }
    if (context->memory_open) {
        CdcMemClose(context->memops);
        context->memory_open = false;
    }
}

static int initialize_encoder(const EncoderOptions *options,
                              EncoderContext *context)
{
    const unsigned int storage_width = align_to_16(options->width);
    const unsigned int storage_height = align_to_16(options->height);
    VencBaseConfig base_config;
    VencH264Param h264_param;
    VencAllocateBufferParam buffer_param;
    int filter = 0;
    int pskip = 1;
    int rotation = (int)options->rotate;

    memset(context, 0, sizeof(*context));
    memset(&base_config, 0, sizeof(base_config));
    memset(&h264_param, 0, sizeof(h264_param));
    memset(&buffer_param, 0, sizeof(buffer_param));

    context->memops = MemAdapterGetOpsS();
    if (context->memops == NULL) {
        fprintf(stderr, PROGRAM_NAME ": MemAdapterGetOpsS failed\n");
        return -1;
    }
    if (CdcMemOpen(context->memops) != 0) {
        fprintf(stderr, PROGRAM_NAME ": CdcMemOpen failed\n");
        return -1;
    }
    context->memory_open = true;

    context->encoder = VideoEncCreate(VENC_CODEC_H264);
    if (context->encoder == NULL) {
        fprintf(stderr, PROGRAM_NAME ": VideoEncCreate failed\n");
        return -1;
    }

    h264_param.sProfileLevel.nProfile = VENC_H264ProfileBaseline;
    h264_param.sProfileLevel.nLevel =
        (VENC_H264LEVELTYPE)options->level;
    h264_param.bEntropyCodingCABAC = 0;
    h264_param.sQPRange.nMinqp = 10;
    h264_param.sQPRange.nMaxqp = 40;
    h264_param.sQPRange.nMinPqp = 10;
    h264_param.sQPRange.nMaxPqp = 50;
    h264_param.sQPRange.nQpInit = 30;
    h264_param.nFramerate = (int)options->fps;
    h264_param.nSrcFramerate = (int)options->fps;
    h264_param.nBitrate = (int)options->bitrate;
    h264_param.nMaxKeyInterval = (int)options->gop;
    h264_param.nCodingMode = VENC_FRAME_CODING;
    h264_param.sRcParam.eRcMode = AW_CBR;

    if (VideoEncSetParameter(context->encoder, VENC_IndexParamH264Param,
                              &h264_param) != 0 ||
        VideoEncSetParameter(context->encoder, VENC_IndexParamIfilter,
                              &filter) != 0 ||
        VideoEncSetParameter(context->encoder, VENC_IndexParamRotation,
                             &rotation) != 0 ||
        VideoEncSetParameter(context->encoder, VENC_IndexParamSetPSkip,
                             &pskip) != 0) {
        fprintf(stderr, PROGRAM_NAME ": setting encoder parameters failed\n");
        return -1;
    }

    base_config.nInputWidth = options->width;
    base_config.nInputHeight = options->height;
    base_config.nDstWidth = options->width;
    base_config.nDstHeight = options->height;
    base_config.nStride = storage_width;
    base_config.eInputFormat = VENC_PIXEL_YUV420SP;
    base_config.memops = context->memops;

    if (VideoEncInit(context->encoder, &base_config) != 0) {
        fprintf(stderr, PROGRAM_NAME ": VideoEncInit failed\n");
        return -1;
    }
    buffer_param.nBufferNum = 1;
    buffer_param.nSizeY = storage_width * storage_height;
    buffer_param.nSizeC = buffer_param.nSizeY / 2U;
    if (AllocInputBuffer(context->encoder, &buffer_param) != 0) {
        fprintf(stderr, PROGRAM_NAME ": AllocInputBuffer failed\n");
        return -1;
    }
    context->input_buffers_allocated = true;

    return 0;
}

static int read_padded_plane(int fd, unsigned char *destination,
                             unsigned int row_bytes, unsigned int rows,
                             unsigned int stride, unsigned char padding,
                             bool frame_started)
{
    unsigned int row;

    for (row = 0; row < rows; ++row) {
        int result = read_exact(fd, destination + (size_t)row * stride,
                                row_bytes, frame_started || row > 0U);
        if (result != 1)
            return result;
        if (stride > row_bytes) {
            memset(destination + (size_t)row * stride + row_bytes,
                   padding, stride - row_bytes);
        }
    }
    return 1;
}

static int write_h264_header(VideoEncoder *encoder, int output_fd)
{
    VencHeaderData header;

    memset(&header, 0, sizeof(header));
    if (VideoEncGetParameter(encoder, VENC_IndexParamH264SPSPPS,
                             &header) != 0 ||
        header.pBuffer == NULL || header.nLength == 0) {
        fprintf(stderr, PROGRAM_NAME ": obtaining H.264 SPS/PPS failed\n");
        return -1;
    }

    return write_all(output_fd, header.pBuffer, header.nLength);
}

static int write_output_frame(VideoEncoder *encoder, int output_fd)
{
    VencOutputBuffer output;

    memset(&output, 0, sizeof(output));
    if (GetOneBitstreamFrame(encoder, &output) != 0) {
        fprintf(stderr, PROGRAM_NAME ": GetOneBitstreamFrame failed\n");
        return -1;
    }

    if ((output.nSize0 > 0 &&
         write_all(output_fd, output.pData0, output.nSize0) != 0) ||
        (output.nSize1 > 0 &&
         write_all(output_fd, output.pData1, output.nSize1) != 0) ||
        (output.nSize2 > 0 &&
         write_all(output_fd, output.pData2, output.nSize2) != 0)) {
        FreeOneBitStreamFrame(encoder, &output);
        return -1;
    }

    if (FreeOneBitStreamFrame(encoder, &output) != 0) {
        fprintf(stderr, PROGRAM_NAME ": FreeOneBitStreamFrame failed\n");
        return -1;
    }

    return 0;
}

static int encode_stream(const EncoderOptions *options,
                         EncoderContext *context, int output_fd,
                         uint64_t *encoded_frames)
{
    const unsigned int storage_width = align_to_16(options->width);
    const unsigned int storage_height = align_to_16(options->height);
    const size_t storage_y_size =
        (size_t)storage_width * storage_height;
    const size_t storage_uv_size = storage_y_size / 2U;
    uint64_t frame_index = 0;

    while (!g_stop_requested &&
           (options->frame_limit == 0 ||
            frame_index < options->frame_limit)) {
        VencInputBuffer input;
        VencInputBuffer used;
        bool input_acquired = false;
        int read_result;
        int result = -1;

        memset(&input, 0, sizeof(input));
        memset(&used, 0, sizeof(used));

        if (GetOneAllocInputBuffer(context->encoder, &input) != 0) {
            fprintf(stderr, PROGRAM_NAME
                    ": GetOneAllocInputBuffer failed at frame %llu\n",
                    (unsigned long long)frame_index);
            return -1;
        }
        input_acquired = true;

        read_result = read_padded_plane(
            STDIN_FILENO, input.pAddrVirY, options->width, options->height,
            storage_width, 16, false);
        if (read_result == 0) {
            ReturnOneAllocInputBuffer(context->encoder, &input);
            break;
        }
        if (read_result != 1) {
            if (read_result == -2)
                fprintf(stderr, PROGRAM_NAME
                        ": incomplete NV12 frame at end of stdin\n");
            ReturnOneAllocInputBuffer(context->encoder, &input);
            return -1;
        }
        if (storage_height > options->height) {
            memset(input.pAddrVirY +
                       (size_t)storage_width * options->height,
                   16,
                   (size_t)storage_width *
                       (storage_height - options->height));
        }

        read_result = read_padded_plane(
            STDIN_FILENO, input.pAddrVirC, options->width,
            options->height / 2U, storage_width, 128, true);
        if (read_result != 1) {
            if (read_result == -2)
                fprintf(stderr, PROGRAM_NAME
                        ": incomplete NV12 frame at end of stdin\n");
            ReturnOneAllocInputBuffer(context->encoder, &input);
            return -1;
        }
        if (storage_height > options->height) {
            const size_t visible_uv_storage =
                (size_t)storage_width * (options->height / 2U);
            memset(input.pAddrVirC + visible_uv_storage, 128,
                   storage_uv_size - visible_uv_storage);
        }

        input.nPts = (long long)((frame_index * 1000000ULL) / options->fps);
        input.nFlag = 0;
        input.nWidth = (int)options->width;
        input.nHeight = (int)options->height;
        input.nAlign = 16;
        input.bEnableCorp = 0;

        if (FlushCacheAllocInputBuffer(context->encoder, &input) != 0) {
            fprintf(stderr, PROGRAM_NAME
                    ": FlushCacheAllocInputBuffer failed at frame %llu\n",
                    (unsigned long long)frame_index);
            goto return_input;
        }
        if (AddOneInputBuffer(context->encoder, &input) != 0) {
            fprintf(stderr, PROGRAM_NAME
                    ": AddOneInputBuffer failed at frame %llu\n",
                    (unsigned long long)frame_index);
            goto return_input;
        }
        input_acquired = false;

        result = VideoEncodeOneFrame(context->encoder);
        if (result != VENC_RESULT_OK) {
            fprintf(stderr, PROGRAM_NAME
                    ": VideoEncodeOneFrame returned %d at frame %llu\n",
                    result, (unsigned long long)frame_index);
            return -1;
        }

        if (AlreadyUsedInputBuffer(context->encoder, &used) != 0) {
            fprintf(stderr, PROGRAM_NAME
                    ": AlreadyUsedInputBuffer failed at frame %llu\n",
                    (unsigned long long)frame_index);
            return -1;
        }
        if (ReturnOneAllocInputBuffer(context->encoder, &used) != 0) {
            fprintf(stderr, PROGRAM_NAME
                    ": ReturnOneAllocInputBuffer failed at frame %llu\n",
                    (unsigned long long)frame_index);
            return -1;
        }

        if (ValidBitstreamFrameNum(context->encoder) <= 0) {
            fprintf(stderr, PROGRAM_NAME
                    ": encoder produced no output for frame %llu\n",
                    (unsigned long long)frame_index);
            return -1;
        }
        if (write_output_frame(context->encoder, output_fd) != 0)
            return -1;

        frame_index++;
        *encoded_frames = frame_index;
        continue;

return_input:
        if (input_acquired)
            ReturnOneAllocInputBuffer(context->encoder, &input);
        return -1;
    }

    *encoded_frames = frame_index;
    return 0;
}

int main(int argc, char **argv)
{
    EncoderOptions options;
    EncoderContext context;
    struct sigaction action;
    uint64_t encoded_frames = 0;
    int output_fd = -1;
    int status = EXIT_FAILURE;

    if (parse_options(argc, argv, &options) != 0) {
        print_usage(stderr);
        return EXIT_FAILURE;
    }

    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    signal(SIGPIPE, SIG_IGN);

    /*
     * Cedarc writes diagnostics to process stdout. Preserve the caller's
     * original stdout for the bitstream, then send all later writes to file
     * descriptor 1 (including closed-source library logs) to stderr.
     */
    output_fd = dup(STDOUT_FILENO);
    if (output_fd < 0) {
        fprintf(stderr, PROGRAM_NAME ": cannot preserve stdout: %s\n",
                strerror(errno));
        return EXIT_FAILURE;
    }
    if (dup2(STDERR_FILENO, STDOUT_FILENO) < 0) {
        fprintf(stderr, PROGRAM_NAME ": cannot redirect vendor stdout: %s\n",
                strerror(errno));
        close(output_fd);
        return EXIT_FAILURE;
    }
    setvbuf(stdout, NULL, _IONBF, 0);

    fprintf(stderr,
            PROGRAM_NAME ": NV12 %ux%u, %u fps, %u bit/s, GOP %u, "
            "level %u, rotation %u\n",
            options.width, options.height, options.fps, options.bitrate,
            options.gop, options.level, options.rotate);
    if (align_to_16(options.height) != options.height) {
        fprintf(stderr,
                PROGRAM_NAME ": input storage height aligned from %u to %u\n",
                options.height, align_to_16(options.height));
    }
    if (align_to_16(options.width) != options.width) {
        fprintf(stderr,
                PROGRAM_NAME ": input storage width aligned from %u to %u\n",
                options.width, align_to_16(options.width));
    }

    if (initialize_encoder(&options, &context) != 0)
        goto cleanup;
    if (write_h264_header(context.encoder, output_fd) != 0)
        goto cleanup;
    if (encode_stream(&options, &context, output_fd, &encoded_frames) != 0)
        goto cleanup;

    status = EXIT_SUCCESS;

cleanup:
    cleanup_encoder(&context);
    if (close(output_fd) != 0 && status == EXIT_SUCCESS) {
        fprintf(stderr, PROGRAM_NAME ": closing stdout stream failed: %s\n",
                strerror(errno));
        status = EXIT_FAILURE;
    }
    fprintf(stderr, PROGRAM_NAME ": encoded %llu frame(s), status=%s\n",
            (unsigned long long)encoded_frames,
            status == EXIT_SUCCESS ? "ok" : "error");
    return status;
}
