// OV02C10 camera (MIPI-CSI, 1 lane) through esp_video's V4L2-style API, plus QR decoding with quirc.
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_video_init.h"
#include "esp_video_device.h"
#include "linux/videodev2.h"
#include "quirc.h"
#include "board.h"
#include "camera.h"

static const char *TAG = "camera";

#define NUM_BUFS 2

static bool s_inited;
static SemaphoreHandle_t s_mtx;  // start/stop/grab from the motion and QR tasks
static int s_fd = -1;
static uint8_t *s_buf[NUM_BUFS];
static size_t s_buf_len[NUM_BUFS];
static int s_w, s_h;
static struct quirc *s_qr;
static int s_qr_w, s_qr_h;

esp_err_t camera_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }
    if (!s_mtx) {
        s_mtx = xSemaphoreCreateRecursiveMutex();
    }
    esp_video_init_csi_config_t csi = {
        .sccb_config = {
            .init_sccb = false,
            .i2c_handle = board_i2c_bus(),
            .freq = 100000,
        },
        .reset_pin = -1,
        .pwdn_pin = -1,
        .dont_init_ldo = true,  // LDO ch3 (2.5 V, MIPI PHY) is already on for the display
    };
    esp_video_init_config_t cfg = {.csi = &csi};
    esp_err_t err = esp_video_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_video_init: %s", esp_err_to_name(err));
        return err;
    }
    s_inited = true;
    return ESP_OK;
}

static bool camera_grab_locked(camera_frame_cb_t cb, void *user);

static esp_err_t camera_start_locked(void)
{
    if (s_fd >= 0) {
        return ESP_OK;
    }
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    int fd = open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, O_RDONLY);
    if (fd < 0) {
        ESP_LOGE(TAG, "open %s failed", ESP_VIDEO_MIPI_CSI_DEVICE_NAME);
        return ESP_FAIL;
    }
    struct v4l2_format fmt = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE};
    if (ioctl(fd, VIDIOC_G_FMT, &fmt) != 0) {
        goto fail;
    }
    s_w = fmt.fmt.pix.width;
    s_h = fmt.fmt.pix.height;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB565;
    if (ioctl(fd, VIDIOC_S_FMT, &fmt) != 0) {
        ESP_LOGE(TAG, "S_FMT RGB565 failed");
        goto fail;
    }
    struct v4l2_requestbuffers req = {.count = NUM_BUFS, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP};
    if (ioctl(fd, VIDIOC_REQBUFS, &req) != 0) {
        goto fail;
    }
    for (int i = 0; i < NUM_BUFS; i++) {
        struct v4l2_buffer b = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP, .index = i};
        if (ioctl(fd, VIDIOC_QUERYBUF, &b) != 0) {
            goto fail;
        }
        s_buf[i] = mmap(NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, b.m.offset);
        s_buf_len[i] = b.length;
        if (!s_buf[i] || ioctl(fd, VIDIOC_QBUF, &b) != 0) {
            goto fail;
        }
    }
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd, VIDIOC_STREAMON, &type) != 0) {
        goto fail;
    }
    s_fd = fd;
    ESP_LOGI(TAG, "streaming %dx%d RGB565", s_w, s_h);
    return ESP_OK;
fail:
    ESP_LOGE(TAG, "camera start failed");
    close(fd);
    return ESP_FAIL;
}

static void camera_stop_locked(void)
{
    if (s_fd < 0) {
        return;
    }
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(s_fd, VIDIOC_STREAMOFF, &type);
    close(s_fd);
    s_fd = -1;
}

esp_err_t camera_start(void)
{
    if (!s_mtx) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTakeRecursive(s_mtx, portMAX_DELAY);
    esp_err_t err = camera_start_locked();
    xSemaphoreGiveRecursive(s_mtx);
    return err;
}

void camera_stop(void)
{
    if (!s_mtx) {
        return;
    }
    xSemaphoreTakeRecursive(s_mtx, portMAX_DELAY);
    camera_stop_locked();
    xSemaphoreGiveRecursive(s_mtx);
}

bool camera_streaming(void)
{
    return s_fd >= 0;
}

bool camera_grab(camera_frame_cb_t cb, void *user)
{
    if (!s_mtx) {
        return false;
    }
    xSemaphoreTakeRecursive(s_mtx, portMAX_DELAY);
    bool ok = camera_grab_locked(cb, user);
    xSemaphoreGiveRecursive(s_mtx);
    return ok;
}

int camera_width(void)
{
    return s_w;
}

int camera_height(void)
{
    return s_h;
}

static bool camera_grab_locked(camera_frame_cb_t cb, void *user)
{
    if (s_fd < 0) {
        return false;
    }
    struct v4l2_buffer b = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP};
    if (ioctl(s_fd, VIDIOC_DQBUF, &b) != 0) {
        return false;
    }
    cb((const uint16_t *)s_buf[b.index], s_w, s_h, user);
    ioctl(s_fd, VIDIOC_QBUF, &b);
    return true;
}

static inline uint8_t luma565(uint16_t p)
{
    unsigned r = (p >> 11) << 3, g = ((p >> 5) & 0x3f) << 2, b = (p & 0x1f) << 3;
    return (uint8_t)((r * 77 + g * 150 + b * 29) >> 8);
}

int camera_qr_decode(const uint16_t *px, int w, int h, char *out, size_t out_size)
{
    // Decode on a 2x-downscaled grey image: enough resolution for a phone/monitor QR, and 4x faster.
    int qw = w / 2, qh = h / 2;
    if (!s_qr) {
        s_qr = quirc_new();
        if (!s_qr) {
            return -1;
        }
    }
    if (qw != s_qr_w || qh != s_qr_h) {
        if (quirc_resize(s_qr, qw, qh) < 0) {
            return -1;
        }
        s_qr_w = qw;
        s_qr_h = qh;
    }
    uint8_t *g = quirc_begin(s_qr, NULL, NULL);
    for (int y = 0; y < qh; y++) {
        const uint16_t *row = px + (size_t)(y * 2) * w;
        for (int x = 0; x < qw; x++) {
            g[y * qw + x] = luma565(row[x * 2]);
        }
    }
    quirc_end(s_qr);
    int n = quirc_count(s_qr);
    // quirc_code + quirc_data are ~13 KB: keep them off the caller's stack (decode is single-threaded).
    static struct quirc_code code;
    static struct quirc_data data;
    for (int i = 0; i < n; i++) {
        quirc_extract(s_qr, i, &code);
        quirc_decode_error_t e = quirc_decode(&code, &data);
        if (e == QUIRC_ERROR_DATA_ECC) {
            quirc_flip(&code);  // mirrored image (camera facing the user)
            e = quirc_decode(&code, &data);
        }
        if (e == QUIRC_SUCCESS) {
            size_t len = data.payload_len < (int)out_size - 1 ? data.payload_len : out_size - 1;
            memcpy(out, data.payload, len);
            out[len] = 0;
            return 1;
        }
    }
    return n > 0 ? 0 : -2;  // 0: QR seen but not decodable, -2: none seen
}

static void mean_cb(const uint16_t *px, int w, int h, void *user)
{
    uint64_t sum = 0;
    int n = 0;
    for (int y = h / 4; y < 3 * h / 4; y += 8) {
        for (int x = w / 4; x < 3 * w / 4; x += 8) {
            sum += luma565(px[y * w + x]);
            n++;
        }
    }
    *(int *)user = n ? (int)(sum / n) : 0;
}

esp_err_t camera_selftest(int *mean_luma)
{
    esp_err_t err = camera_init();
    if (err == ESP_OK) {
        err = camera_start();
    }
    if (err != ESP_OK) {
        return err;
    }
    int frames = 0, mean = -1;
    for (int i = 0; i < 5; i++) {
        frames += camera_grab(mean_cb, &mean);
    }
    camera_stop();
    ESP_LOGI(TAG, "self-test: %d frames, mean luma %d", frames, mean);
    if (mean_luma) {
        *mean_luma = mean;
    }
    return frames > 0 ? ESP_OK : ESP_FAIL;
}
