#pragma once

#include "npulib.h"

enum PipePixelFormat {
    PIPE_PIXEL_BGR,
    PIPE_PIXEL_NV12,
};

int run_pipe_mode(NetworkItem& network, int width, int height,
                  int video_output_fd, PipePixelFormat pixel_format,
                  int rotation_degrees);
