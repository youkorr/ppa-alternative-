/* SPDX-FileCopyrightText: 2026 youkorr
 * SPDX-License-Identifier: MIT
 * Part of ppa-alternative: https://github.com/youkorr/ppa-alternative-
 *
 * The camera for the benchmark's "camera" scene. Wired as the household's
 * own working ESPHome configurations wire it, board for board:
 *
 *   Tab5        SC2356 (Espressif's SC202CS driver: same chip, PID 0xeb52),
 *               1280x720 at 30 fps, one lane, on the system I2C bus
 *               (GPIO31/32) the display already opened; its reset is pin 6
 *               of the PI4IOE5V6408 at 0x43 (hw_camera_prepare).
 *   Waveshare   OV5647, 1280x960 at 45 fps (binned), two lanes, on I2C
 *               GPIO7/8.
 *
 * Both take a 24 MHz XCLK on GPIO36 from LEDC, which esp_video does not make
 * for a MIPI-CSI sensor. The MIPI PHY's LDO is the display's, already on.
 *
 * Buffers are the driver's (mmap). The display may still be reading the
 * frame it was handed last, so the two most recent frames returned stay
 * out of the driver's queue; a frame goes back once two newer ones exist.
 */
#include "ab_camera.h"
#include "ab_bench_config.h"
#include "hw_init.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_video_device.h"
#include "esp_video_init.h"
#include "esp_video_ioctl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "linux/videodev2.h"
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <unistd.h>

#define BUFFERS       4
#define XCLK_GPIO     36
#define XCLK_HZ       24000000
#define WAIT_MS       1000

static const char *TAG = "ab_camera";
static int s_fd = -1;
static uint8_t *s_buffer[BUFFERS];
static int s_held[2] = {-1, -1};    /* [0] the older of the two the caller has */
static ab_camera_counts_t s_counts;

static esp_err_t start_xclk(void)
{
    /* Timer 1 and channel 2: the Tab5's backlight has timer 0 and channel 1.
     * Every LEDC timer of a P4 shares one clock source, and 24 MHz can only
     * come from the 80 MHz PLL: the backlight is put on it too (hw_init), or
     * whichever starts second is refused ("timer clock conflict"). */
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_1_BIT,
        .timer_num = LEDC_TIMER_1,
        .freq_hz = XCLK_HZ,
        .clk_cfg = LEDC_USE_PLL_DIV_CLK,
    };
    const ledc_channel_config_t channel = {
        .gpio_num = XCLK_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_2,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_1,
        .duty = 1,
        .hpoint = 0,
    };
    esp_err_t err = ledc_timer_config(&timer);
    if (err == ESP_OK) err = ledc_channel_config(&channel);
    return err;
}

static void set_wait(uint32_t ms)
{
    struct timeval tv = {.tv_sec = ms / 1000, .tv_usec = (ms % 1000) * 1000};
    (void)ioctl(s_fd, VIDIOC_S_DQBUF_TIMEOUT, &tv);
}

static bool dequeue(struct v4l2_buffer *buf)
{
    memset(buf, 0, sizeof(*buf));
    buf->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf->memory = V4L2_MEMORY_MMAP;
    return ioctl(s_fd, VIDIOC_DQBUF, buf) == 0;
}

static void requeue(int index)
{
    if (index < 0) return;
    struct v4l2_buffer buf = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP,
        .index = (uint32_t)index,
    };
    if (ioctl(s_fd, VIDIOC_QBUF, &buf) != 0) ESP_LOGE(TAG, "could not give buffer %d back", index);
}

esp_err_t ab_camera_start(ab_camera_info_t *info)
{
    memset(info, 0, sizeof(*info));
    void *bus = hw_camera_prepare();
    esp_err_t err = start_xclk();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "XCLK on GPIO%d: %s", XCLK_GPIO, esp_err_to_name(err));
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(50));

    esp_video_init_csi_config_t csi = {
        .reset_pin = -1,
        .pwdn_pin = -1,
        .dont_init_ldo = true,
    };
    csi.sccb_config.freq = 400000;
    if (bus) {
        csi.sccb_config.init_sccb = false;
        csi.sccb_config.i2c_handle = (i2c_master_bus_handle_t)bus;
    } else {
        csi.sccb_config.init_sccb = true;
        csi.sccb_config.i2c_config.port = 0;
        csi.sccb_config.i2c_config.scl_pin = AB_CAMERA_SCL;
        csi.sccb_config.i2c_config.sda_pin = AB_CAMERA_SDA;
    }
    const esp_video_init_config_t config = {.csi = &csi};
    err = esp_video_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_video_init: %s -- no sensor answered on the bus", esp_err_to_name(err));
        return err;
    }

    s_fd = open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, O_RDONLY);
    if (s_fd < 0) {
        ESP_LOGE(TAG, "%s does not exist: no sensor was detected", ESP_VIDEO_MIPI_CSI_DEVICE_NAME);
        return ESP_ERR_NOT_FOUND;
    }
    struct v4l2_capability cap = {0};
    if (ioctl(s_fd, VIDIOC_QUERYCAP, &cap) == 0) {
        snprintf(info->sensor, sizeof(info->sensor), "%s", (const char *)cap.card);
    }
    struct v4l2_format format = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE};
    if (ioctl(s_fd, VIDIOC_G_FMT, &format) != 0) {
        ESP_LOGE(TAG, "VIDIOC_G_FMT failed");
        return ESP_FAIL;
    }
    const uint32_t width = format.fmt.pix.width, height = format.fmt.pix.height;
    format = (struct v4l2_format) {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .fmt.pix.width = width,
        .fmt.pix.height = height,
        .fmt.pix.pixelformat = V4L2_PIX_FMT_RGB565,
    };
    if (ioctl(s_fd, VIDIOC_S_FMT, &format) != 0) {
        ESP_LOGE(TAG, "the ISP refused RGB565 at %" PRIu32 "x%" PRIu32, width, height);
        return ESP_FAIL;
    }
    info->width = format.fmt.pix.width;
    info->height = format.fmt.pix.height;
    info->stride = format.fmt.pix.bytesperline ? format.fmt.pix.bytesperline : info->width * 2u;

    struct v4l2_requestbuffers req = {
        .count = BUFFERS,
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP,
    };
    if (ioctl(s_fd, VIDIOC_REQBUFS, &req) != 0 || req.count < BUFFERS) {
        ESP_LOGE(TAG, "could not have %d buffers", BUFFERS);
        return ESP_ERR_NO_MEM;
    }
    for (int i = 0; i < BUFFERS; i++) {
        struct v4l2_buffer buf = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_MMAP,
            .index = (uint32_t)i,
        };
        if (ioctl(s_fd, VIDIOC_QUERYBUF, &buf) != 0) return ESP_FAIL;
        s_buffer[i] = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, s_fd, buf.m.offset);
        if (!s_buffer[i] || buf.length < info->stride * info->height) {
            ESP_LOGE(TAG, "buffer %d: %" PRIu32 " bytes for a %ux%u picture", i, buf.length,
                     info->width, info->height);
            return ESP_FAIL;
        }
        if (ioctl(s_fd, VIDIOC_QBUF, &buf) != 0) return ESP_FAIL;
    }
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(s_fd, VIDIOC_STREAMON, &type) != 0) {
        ESP_LOGE(TAG, "VIDIOC_STREAMON failed");
        return ESP_FAIL;
    }
    set_wait(WAIT_MS);
    /* The first picture proves the whole chain before anything is measured. */
    if (!ab_camera_take()) {
        ESP_LOGE(TAG, "streaming, but no frame came within %d ms", WAIT_MS);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

const uint8_t *ab_camera_take(void)
{
    const int64_t started = esp_timer_get_time();
    struct v4l2_buffer buf;
    int newest = -1;
    /* Wait for a frame, then take whatever newer ones are already there, so
     * the display shows the latest picture rather than the oldest queued. */
    while (newest < 0) {
        if (!dequeue(&buf)) {
            s_counts.wait_us += (uint64_t)(esp_timer_get_time() - started);
            return NULL;
        }
        if (buf.flags & V4L2_BUF_FLAG_ERROR) {
            s_counts.errors++;
            requeue((int)buf.index);
            continue;
        }
        newest = (int)buf.index;
        s_counts.received++;
    }
    set_wait(0);
    while (dequeue(&buf)) {
        if (buf.flags & V4L2_BUF_FLAG_ERROR) {
            s_counts.errors++;
            requeue((int)buf.index);
            continue;
        }
        requeue(newest);
        newest = (int)buf.index;
        s_counts.received++;
        s_counts.skipped++;
    }
    set_wait(WAIT_MS);
    requeue(s_held[0]);
    s_held[0] = s_held[1];
    s_held[1] = newest;
    s_counts.shown++;
    s_counts.wait_us += (uint64_t)(esp_timer_get_time() - started);
    return s_buffer[newest];
}

void ab_camera_reset_counts(void)
{
    s_counts = (ab_camera_counts_t) {0};
}

ab_camera_counts_t ab_camera_counts(void)
{
    return s_counts;
}
