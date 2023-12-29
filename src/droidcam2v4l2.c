#include <string.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>
#include "v4l2loopback.h"
#include <droidmedia/droidmedia.h>
#include <droidmedia/droidmediacamera.h>
#include <droidmedia/droidmediaconstants.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>

#define SUPPORT_ROTATION 1

#ifdef SUPPORT_ROTATION
#include <libyuv.h>
#include <libyuv/rotate.h>
#endif

// Why is this not in v4l2loopback.h??? Come on...
#define V4L2LOOPBACK_EVENT_BASE (V4L2_EVENT_PRIVATE_START)
#define V4L2LOOPBACK_EVENT_OFFSET 0x08E00000
#define V4L2_EVENT_PRI_CLIENT_USAGE (V4L2LOOPBACK_EVENT_BASE + V4L2LOOPBACK_EVENT_OFFSET + 1)

struct v4l2_event_client_usage {
    __u32 count;
};


// These variables are not necessarily the exact size we'll end up getting.
// We'll do our best to get as close as possible.
// TODO: make these configurable.
#define CAMERA_WIDTH     1280
#define CAMERA_HEIGHT    720
#define MAX_CAMERAS      8

typedef struct camera_config {
    DroidMediaCamera *camera;
    int camera_idx;

    int v4l2_fd;
    int v4l2_idx;

    int width;
    int height;
#ifdef SUPPORT_ROTATION
    int rotation; // must be 0, 90, 180, or 270
#endif

    char *parameters;

    bool asleep;
    pthread_t thread;
} camera_config;

camera_config cameras[MAX_CAMERAS];
int camera_last = -1;

DroidMediaCameraConstants CAMERA_CONSTANTS;
DroidMediaPixelFormatConstants PIXEL_FORMAT_CONSTANTS;
DroidMediaColourFormatConstants COLOR_FORMAT_CONSTANTS;

camera_config *v4l2_setup(const char *name, int width, int height) {
    if (camera_last >= MAX_CAMERAS) {
        printf("Too many cameras. What are you even running this on?\n");
        return NULL;
    }

    camera_config *config = &cameras[++camera_last];
    config->asleep = true;
    struct v4l2_loopback_config cfg;
    struct v4l2_format fmt;
    char DEVICE_PATH[32];
    struct v4l2_event_subscription sub;
    uint8_t *blank;
    ssize_t blank_size, written;

    int control_fd = open("/dev/v4l2loopback", 0);
    if (control_fd < 0) {
        printf("Unable to open control device: %s\n", strerror(errno));
        --camera_last;
        return NULL;
    }

    memset(&cfg, 0, sizeof(cfg));

    strncpy(cfg.card_label, name, sizeof(cfg.card_label));
    cfg.output_nr = -1;
    cfg.announce_all_caps = 0;
    cfg.max_width = width;
    cfg.max_height = height;
    cfg.max_buffers = 1;
    cfg.max_openers = 32;

    if ((config->v4l2_idx = ioctl(control_fd, V4L2LOOPBACK_CTL_ADD, &cfg)) < 0) {
        printf("Unable to create device: %s\n", strerror(errno));
        close(control_fd);
        --camera_last;
        return NULL;
    }

    close(control_fd);

    sprintf(DEVICE_PATH, "/dev/video%d", config->v4l2_idx);
    printf("Created device: %s\n", DEVICE_PATH);

    config->v4l2_fd = open(DEVICE_PATH, O_WRONLY);
    if (config->v4l2_fd < 0) {
        printf("Unable to open device: %s\n", strerror(errno));
        goto fail;
    }

    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    fmt.fmt.pix.width = width;
    fmt.fmt.pix.height = height;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUV420;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;

    if (ioctl(config->v4l2_fd, VIDIOC_S_FMT, &fmt) < 0) {
        printf("Failed to set pixel format: %s\n", strerror(errno));
        goto fail; // I've always wanted a "goto fail" of my own. h/t Apple :)
    }

    memset(&sub, 0, sizeof(sub));
    sub.type = V4L2_EVENT_PRI_CLIENT_USAGE;
    if (ioctl(config->v4l2_fd, VIDIOC_SUBSCRIBE_EVENT, &sub) < 0) {
        printf("Failed to subscribe to events: %s\n", strerror(errno));
        goto fail;
    }

    // Emit one blank frame to configure the device.
    blank_size = width * height * 3 / 2;
    blank = malloc(blank_size);
    memset(blank, 47, blank_size);
    written = write(config->v4l2_fd, blank, blank_size);
    free(blank);

    if (written < blank_size) {
        printf("Error writing to v4l2 device: %s\n", strerror(errno));
        goto fail;
    }

    config->width = width;
    config->height = height;

    return config;

fail:
    close(config->v4l2_fd);
    --camera_last;
    return NULL;
}

void destroy(camera_config *config) {
    close(config->v4l2_fd);  // Forces the thread to exit.

    if (config->thread) {
        pthread_kill(config->thread, SIGINT);
        pthread_join(config->thread, NULL);
    }

    if (config->camera) {
        droid_media_camera_stop_preview(config->camera);
        droid_media_camera_unlock(config->camera);
        droid_media_camera_disconnect(config->camera);
    }

    int control_fd = open("/dev/v4l2loopback", 0);
    if (control_fd < 0) {
        printf("Unable to open control device: %s\n", strerror(errno));
        return;
    }

    if (ioctl(control_fd, V4L2LOOPBACK_CTL_REMOVE, config->v4l2_idx) < 0) {
        printf("Unable to remove device %d: %s\n", config->v4l2_idx, strerror(errno));
    }

    close(control_fd);
    memset(config, 0, sizeof(camera_config));
}

void cleanup() {
    static int cleaning_up = 0;
    if (cleaning_up++) {
        return;
    }

    printf("Cleaning up... (please don't mash Ctrl+C unless you want stuck V4L2 devices)\n");
    for (; camera_last >= 0; --camera_last) {
        destroy(&cameras[camera_last]);
    }
    exit(0);
}

size_t droid_media_camera_get_parameter_value(DroidMediaCamera *camera, const char *parameter_name, char *value_out, size_t value_len, const char *params) {
    char *original_params, *handle;

    if (params) {
        original_params = strdup(params);
    } else {
        original_params = strdup(droid_media_camera_get_parameters(camera));
    }
    handle = original_params;

    while (original_params && *original_params) {
        const char *key = original_params;
        char *value = strchr(original_params, '=');
        *value = '\0';
        value++;
        char *next = strchr(value, ';');
        if (next) {
            *next = '\0';
            next++;
        }
        original_params = next;

        if (strcmp(key, parameter_name) == 0) {
            if (value_out && value_len)
            {
                strncpy(value_out, value, value_len);
            }
            free(handle);
            return strlen(value);
        }
    }

    free(handle);
    return 0;
}

bool droid_media_camera_add_parameters(DroidMediaCamera *camera, const char *params) {
    char *original_params = strdup(droid_media_camera_get_parameters(camera));
    char *handle = original_params;

    size_t final_buffer_size = strlen(original_params) + strlen(params) + 1;
    char *final_params = malloc(final_buffer_size);
    *final_params = '\0';

    while (original_params && *original_params) {
        const char *key = original_params;
        char *value = strchr(original_params, '=');
        *value = '\0';
        value++;

        char *next = strchr(value, ';');
        if (next) {
            *next = '\0';
            next++;
        }
        original_params = next;

        strcat(final_params, key);
        strcat(final_params, "=");

        if (droid_media_camera_get_parameter_value(camera, key, final_params + strlen(final_params), final_buffer_size - strlen(final_params), params) > 0) {
            strcat(final_params, ";");
            continue;
        }

        strcat(final_params, value);
        strcat(final_params, ";");
    }

    bool ret = droid_media_camera_set_parameters(camera, final_params);
    free(handle);
    free(final_params);

    return ret;
}

#ifdef SUPPORT_ROTATION

void rotate_yuv420p(const void *src_y, const void *src_u, const void *src_v, void *dst_y, void *dst_u, void *dst_v, int src_width, int src_height, int rotation) {
    enum RotationMode mode;
    int dst_width = src_width;
    int dst_height = src_height;
    switch (rotation) {
        case 90:
            dst_width = src_height;
            dst_height = src_width;
            mode = kRotate90;
            break;
        case 180:
            mode = kRotate180;
            break;
        case 270:
            dst_width = src_height;
            dst_height = src_width;
            mode = kRotate270;
            break;
        default:
            mode = kRotate0;
            break;
    }

    I420Rotate((const uint8_t *)src_y, src_width,
            (const uint8_t *)src_u, src_width / 2,
            (const uint8_t *)src_v, src_width / 2,
            (uint8_t *)dst_y, dst_width,
            (uint8_t *)dst_u, dst_width / 2,
            (uint8_t *)dst_v, dst_width / 2,
            dst_height, dst_width, mode);
}
#endif

void preview_frame_callback(void *userdata, DroidMediaData *data) {
    ssize_t written = 0;
    struct camera_config *cfg = (struct camera_config *) userdata;
    ssize_t y_size = cfg->width * cfg->height;
    ssize_t uv_size = cfg->width * cfg->height / 4;

    // Sadly can't write() three times; v4l2loopback doesn't support it.
    // Create a crappy intermediate buffer. :(
    // Caveat emptor: if you decide to allow multiple cameras, this will
    // need to be per-camera. It is not thread-safe.
    static size_t last_intermediate_size = 0;
    static uint8_t *intermediate = NULL;

    if (y_size + uv_size * 2 > last_intermediate_size) {
        if (intermediate) {
            free(intermediate);
        }
        intermediate = malloc(y_size + uv_size * 2);
        last_intermediate_size = y_size + uv_size * 2;
    }

#ifdef SUPPORT_ROTATION
    // Intermediate buffer ends up giving us a nice chance to rotate the image.
    rotate_yuv420p(data->data,                      // Source Y
                   data->data + y_size,             // Source U
                   data->data + y_size + uv_size,   // Source V
                   intermediate,                    // Destination Y
                   intermediate + y_size + uv_size, // Destination U (swapped!)
                   intermediate + y_size,           // Destination V (swapped!)
                   cfg->width, cfg->height, cfg->rotation);
#else
    memcpy(intermediate, data->data, y_size);
    memcpy(intermediate + y_size, data->data + y_size + uv_size, uv_size);
    memcpy(intermediate + y_size + uv_size, data->data + y_size, uv_size);
#endif

    written = write(cfg->v4l2_fd, intermediate, y_size + uv_size * 2);
    if (written < 0) {
        printf("Error writing to v4l2 device: %s\n", strerror(errno));
    }
}

camera_config *init_camera(int index, int desired_width, int desired_height) {
    DroidMediaCamera *camera;
    DroidMediaCameraInfo info;
    camera_config *conf;
    int width = -1;
    int height = -1;
    int size_diff = 0x7FFFFFFF;
    char parameter_buffer[1024];
    char name_buffer[32];

    camera = droid_media_camera_connect(index);
    if (!camera) {
        printf("Failed to connect to camera %d.\n", index);
        return NULL;
    }

    droid_media_camera_get_info(&info, index);

    // Give it a name. It would probably be better to either allow the user
    // to set their own names, or try to figure out which is telephoto, wide, etc...
    // Right now, you get to deal with the numbers.
    if (info.facing == DROID_MEDIA_CAMERA_FACING_FRONT) {
        static int front_camera_count = 0;
        if (front_camera_count++ == 0) {
            sprintf(name_buffer, "Front Camera");
        } else {
            sprintf(name_buffer, "Front Camera %d", front_camera_count);
        }
    } else {
        static int back_camera_count = 0;
        if (back_camera_count++ == 0) {
            sprintf(name_buffer, "Back Camera");
        } else {
            sprintf(name_buffer, "Back Camera %d", back_camera_count);
        }
    }

    // Figure out a preview size that's somewhat close to what we want.
    if (!droid_media_camera_get_parameter_value(camera, "preview-size-values", parameter_buffer, sizeof(parameter_buffer), NULL)) {
        printf("[CAMERA %d] Failed to get preview size values.\n", index);
        return NULL;
    }

    char *needle = parameter_buffer;
    while (needle && *needle) {
        char *x = strchr(needle, 'x');
        if (!x) {
            break;
        }
        *x = '\0';
        x++;
        int w = atoi(needle);
        int h = atoi(x);
        if (w == desired_width && h == desired_height) {
            width = w;
            height = h;
            break;
        }
        int diff = abs(w - desired_width) + abs(h - desired_height);
        if (diff < size_diff) {
            size_diff = diff;
            width = w;
            height = h;
        }
        needle = strchr(x, ',');
        if (needle) {
            needle++;
        }
    }

    if (width <= 0 || height <= 0) {
        printf("[CAMERA %d] Failed to find a suitable preview size.\n", index);
        return NULL;
    }

    // TODO: not all devices necessarily support yuv420p or 30 FPS. Be smarter about this.
    sprintf(parameter_buffer, "preview-size=%dx%d;preview-frame-rate=30;preview-format=yuv420p;", width, height);

    // OK, close the camera to give direct consumers a chance (and to let the camera HAL sleep)
    // It will be reopened on demand.
    droid_media_camera_disconnect(camera);

#ifdef SUPPORT_ROTATION
    if (info.orientation == 90 || info.orientation == 270) {
        conf = v4l2_setup(name_buffer, height, width);

        if (!conf) {
            printf("[CAMERA %d] Failed to set up v4l2 device.\n", index);
            return NULL;
        }

        // Ensure width and height are the original values.
        conf->width = width;
        conf->height = height;
    } else {
        conf = v4l2_setup(name_buffer, width, height);
    }
    conf->rotation = info.orientation;
#else
    conf = v4l2_setup(name_buffer, width, height);
#endif

    if (!conf) {
        printf("[CAMERA %d] Failed to set up v4l2 device.\n", index);
        return NULL;
    }

    conf->camera = NULL;
    conf->camera_idx = index;
    conf->parameters = strdup(parameter_buffer);

    return conf;
}

// Only one camera can be active at a time on most devices. Ensure we don't try
// waking up a camera before the previous one has gone to sleep. Otherwise we
// risk freezing the camera HAL - because they're super well written.
pthread_mutex_t camera_lock = PTHREAD_MUTEX_INITIALIZER;

bool start_preview(camera_config *conf) {
    DroidMediaCameraCallbacks callbacks;

    printf("[CAMERA %d] Wakey wakey, hands off snakey.\n", conf->v4l2_idx);
    pthread_mutex_lock(&camera_lock);
    printf("[CAMERA %d] Got lock.\n", conf->v4l2_idx);

    conf->camera = droid_media_camera_connect(conf->camera_idx);
    if (!conf->camera) {
        printf("[CAMERA %d] Failed to reconnect to camera.\n", conf->v4l2_idx);
        pthread_mutex_unlock(&camera_lock);
        return false;
    }

    if (!droid_media_camera_lock(conf->camera)) {
        printf("[CAMERA %d] Failed to lock camera.\n", conf->v4l2_idx);
        pthread_mutex_unlock(&camera_lock);
        return false;
    }

    if (!droid_media_camera_add_parameters(conf->camera, conf->parameters)) {
        printf("[CAMERA %d] Failed to add parameters.\n", conf->v4l2_idx);
        pthread_mutex_unlock(&camera_lock);
        return false;
    }

    droid_media_camera_set_preview_callback_flags(conf->camera, CAMERA_CONSTANTS.CAMERA_FRAME_CALLBACK_FLAG_ENABLE_MASK);

    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.preview_frame_cb = preview_frame_callback;
    droid_media_camera_set_callbacks(conf->camera, &callbacks, conf);

    // TODO: make configurable. I just really want it to focus.
    droid_media_camera_start_auto_focus(conf->camera);

    if (!droid_media_camera_start_preview(conf->camera)) {
        printf("[CAMERA %d] Failed to start preview.\n", conf->v4l2_idx);
        pthread_mutex_unlock(&camera_lock);
        return false;
    }

    conf->asleep = false;
    return true;
}

void stop_preview(camera_config *conf) {
    printf("[CAMERA %d] Sleep time!\n", conf->v4l2_idx);
    droid_media_camera_stop_preview(conf->camera);
    droid_media_camera_unlock(conf->camera);
    droid_media_camera_disconnect(conf->camera);
    conf->camera = NULL;
    conf->asleep = true;
    pthread_mutex_unlock(&camera_lock);
}

void *camera_event_loop(void *userdata) {
    camera_config *conf = (camera_config *) userdata;
    while (1) {
        struct v4l2_event event;
        printf("[CAMERA %d] Waiting for events...\n", conf->v4l2_idx);
        int ret = ioctl(conf->v4l2_fd, VIDIOC_DQEVENT, &event);
        if (ret < 0) {
            if (errno == EBADF) {
                // Probably shutting down. Don't complain.
                return NULL;
            }

            printf("[CAMERA %d] Failed to get event: %s\n", conf->v4l2_idx, strerror(errno));
            return NULL;
        }

        if (event.type == V4L2_EVENT_PRI_CLIENT_USAGE) {
            struct v4l2_event_client_usage *usage = (struct v4l2_event_client_usage *) event.u.data;
            printf("[CAMERA %d] Consumers: %d\n", conf->v4l2_idx, usage->count);
            if (usage->count > 0 && conf->asleep) {
                start_preview(conf);
            } else if (usage->count == 0 && !conf->asleep) {
                stop_preview(conf);
            }
        }
    }

    return NULL;
}

int main(int argc, char *argv[]) {
    camera_config *conf;
    int camera_count = 0;
    int desired_camera;

    droid_media_init();
    droid_media_camera_constants_init(&CAMERA_CONSTANTS);
    droid_media_pixel_format_constants_init(&PIXEL_FORMAT_CONSTANTS);
    droid_media_colour_format_constants_init(&COLOR_FORMAT_CONSTANTS);

    camera_count = droid_media_camera_get_number_of_cameras();
    printf("Camera count: %d\n", camera_count);

    if (camera_count < 1) {
        printf("No cameras found.\n");
        return -1;
    }

    for (desired_camera = 0; desired_camera < camera_count; ++desired_camera) {
        printf("Trying camera %d...\n", desired_camera);
        conf = init_camera(desired_camera, CAMERA_WIDTH, CAMERA_HEIGHT);
        if (!conf) {
            printf("Failed to init camera.\n");
        }
    }

    if (camera_last < 0) {
        printf("No cameras initialized.\n");
        return -1;
    }

    // Start the event loop for each camera.
    for (desired_camera = 0; desired_camera <= camera_last; ++desired_camera) {
        pthread_create(&cameras[desired_camera].thread, NULL, camera_event_loop, &cameras[desired_camera]);
    }

    atexit(cleanup);
    signal(SIGINT, cleanup);
    signal(SIGTERM, cleanup);

    while (1) {
        usleep(100000);
    }
}
