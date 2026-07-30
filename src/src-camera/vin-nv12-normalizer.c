#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(const char *program) {
    fprintf(stderr,
            "Usage: %s --width WIDTH --height HEIGHT "
            "--input-frame-bytes BYTES\n",
            program);
}

static int read_full(FILE *stream, uint8_t *buffer, size_t length) {
    size_t offset = 0;
    while (offset < length) {
        size_t received = fread(buffer + offset, 1, length - offset, stream);
        if (received > 0) {
            offset += received;
            continue;
        }
        if (ferror(stream)) {
            if (errno == EINTR) {
                clearerr(stream);
                continue;
            }
            return -1;
        }
        return offset == 0 ? 0 : -1;
    }
    return 1;
}

static int write_full(FILE *stream, const uint8_t *buffer, size_t length) {
    size_t offset = 0;
    while (offset < length) {
        size_t sent = fwrite(buffer + offset, 1, length - offset, stream);
        if (sent == 0) return -1;
        offset += sent;
    }
    return 0;
}

int main(int argc, char **argv) {
    unsigned long width = 0;
    unsigned long height = 0;
    unsigned long input_frame_bytes = 0;
    for (int index = 1; index < argc; ++index) {
        if (strcmp(argv[index], "--width") == 0 && index + 1 < argc) {
            width = strtoul(argv[++index], NULL, 10);
        } else if (strcmp(argv[index], "--height") == 0 && index + 1 < argc) {
            height = strtoul(argv[++index], NULL, 10);
        } else if (strcmp(argv[index], "--input-frame-bytes") == 0 && index + 1 < argc) {
            input_frame_bytes = strtoul(argv[++index], NULL, 10);
        } else {
            usage(argv[0]);
            return 2;
        }
    }
    if (width == 0 || height == 0 || input_frame_bytes == 0 ||
        (width & 1) || (height & 1)) {
        usage(argv[0]);
        return 2;
    }

    size_t standard_frame_bytes = (size_t)width * height * 3 / 2;
    size_t driver_frame_bytes = (size_t)input_frame_bytes;
    if (driver_frame_bytes < standard_frame_bytes) {
        fprintf(stderr,
                "[nv12-normalizer] input frame (%zu bytes) is smaller than "
                "standard NV12 output (%zu bytes)\n",
                driver_frame_bytes, standard_frame_bytes);
        return 2;
    }
    uint8_t *frame = malloc(driver_frame_bytes);
    if (frame == NULL) {
        fprintf(stderr, "[nv12-normalizer] allocation failed\n");
        return 1;
    }

    fprintf(stderr, "[nv12-normalizer] %lux%lu: input=%zu output=%zu discard=%zu\n",
            width, height, driver_frame_bytes, standard_frame_bytes,
            driver_frame_bytes - standard_frame_bytes);
    for (;;) {
        int result = read_full(stdin, frame, driver_frame_bytes);
        if (result == 0) break;
        if (result < 0 || write_full(stdout, frame, standard_frame_bytes) != 0 || fflush(stdout) != 0) {
            fprintf(stderr, "[nv12-normalizer] stream I/O failed\n");
            free(frame);
            return 1;
        }
    }
    free(frame);
    return 0;
}
