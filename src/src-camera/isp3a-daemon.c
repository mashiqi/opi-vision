#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <AWIspApi.h>

static volatile sig_atomic_t keep_running = 1;

static void stop_handler(int signal_number) {
    (void)signal_number;
    keep_running = 0;
}

static void usage(const char *program) {
    fprintf(stderr, "Usage: %s [--video-id=N] [--log-interval=N]\n", program);
}

int main(int argc, char **argv) {
    int video_id = 0;
    int log_interval = 5;
    int isp_id;
    int result;
    int elapsed = 0;
    AWIspApi *api;

    for (int i = 1; i < argc; ++i) {
        if (sscanf(argv[i], "--video-id=%d", &video_id) == 1) {
            continue;
        }
        if (sscanf(argv[i], "--log-interval=%d", &log_interval) == 1 && log_interval > 0) {
            continue;
        }
        usage(argv[0]);
        return 2;
    }

    signal(SIGINT, stop_handler);
    signal(SIGTERM, stop_handler);
    api = CreateAWIspApi();
    if (api == NULL || api->ispApiInit == NULL || api->ispGetIspId == NULL ||
        api->ispStart == NULL || api->ispStop == NULL || api->ispApiUnInit == NULL) {
        fprintf(stderr, "[isp3a] incomplete AWIspApi interface\n");
        if (api != NULL) DestroyAWIspApi(api);
        return 1;
    }
    result = api->ispApiInit();
    if (result != 0) {
        fprintf(stderr, "[isp3a] ispApiInit failed: %d\n", result);
        DestroyAWIspApi(api);
        return 1;
    }
    isp_id = api->ispGetIspId(video_id);
    if (isp_id < 0) {
        fprintf(stderr, "[isp3a] ispGetIspId(video%d) failed: %d\n", video_id, isp_id);
        api->ispApiUnInit();
        DestroyAWIspApi(api);
        return 1;
    }
    result = api->ispStart(isp_id);
    if (result != 0) {
        fprintf(stderr, "[isp3a] ispStart(isp%d) failed: %d\n", isp_id, result);
        api->ispApiUnInit();
        DestroyAWIspApi(api);
        return 1;
    }
    fprintf(stderr, "[isp3a] started: video%d -> isp%d\n", video_id, isp_id);
    while (keep_running) {
        sleep(1);
        if (++elapsed < log_interval) continue;
        elapsed = 0;
        if (api->ispGetIspExp != NULL && api->ispGetIspGain != NULL) {
            unsigned int numerator = 0, denominator = 0;
            int exposure_result = api->ispGetIspExp(isp_id, &numerator, &denominator);
            int gain = api->ispGetIspGain(isp_id);
            fprintf(stderr, "[isp3a] exposure=%u/%u result=%d gain=%d\n", numerator, denominator, exposure_result, gain);
        }
    }
    result = api->ispStop(isp_id);
    fprintf(stderr, "[isp3a] stopped isp%d: %d\n", isp_id, result);
    api->ispApiUnInit();
    DestroyAWIspApi(api);
    return result == 0 ? 0 : 1;
}
