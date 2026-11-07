/**
 * @file ui_preview.c
 * @brief PC 端界面预览：把仪表页面渲染成 PPM，方便无硬件时检查排版
 *
 * 用法见 tools/preview/preview.sh
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui_draw.h"
#include "ui_pages.h"

// 与板级 BMP 逻辑分辨率一致：横屏 640x172
#define W 640
#define H 172

static void save_ppm(const char *path, const uint16_t *buf, int w, int h)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        perror("fopen");
        return;
    }
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; i++) {
        uint16_t c = buf[i];
        uint8_t r = (uint8_t)((c >> 11) & 0x1F);
        uint8_t g = (uint8_t)((c >> 5) & 0x3F);
        uint8_t b = (uint8_t)(c & 0x1F);
        uint8_t px[3] = {
            (uint8_t)((r << 3) | (r >> 2)),
            (uint8_t)((g << 2) | (g >> 4)),
            (uint8_t)((b << 3) | (b >> 2)),
        };
        fwrite(px, 1, 3, f);
    }
    fclose(f);
    printf("write %s\n", path);
}

int main(int argc, char **argv)
{
    uint16_t *buf = calloc((size_t)W * H, sizeof(uint16_t));
    if (!buf) {
        return 1;
    }
    ui_canvas_t cv = {.buf = buf, .w = W, .h = H};

    ui_state_t st;
    memset(&st, 0, sizeof(st));
    st.link = LCD_LINK_AUTHENTICATED;
    st.paired = true;
    strcpy(st.vin, "5YJ3E1EA7KF000000");
    st.page = 0;
    st.drive.valid = true;
    st.drive.power_kw = 123;
    st.drive.speed_kmh = 88;
    st.drive.gear = 'D';
    st.drive.odometer_km = 12345;
    st.drive.consumption_10 = 152;
    st.drive.eta_min = 18;
    strcpy(st.drive.destination, "上海虹桥站");
    st.drive.body_valid = true;
    st.drive.doors_closed = true;
    st.drive.frunk_closed = true;
    st.drive.trunk_closed = true;
    st.drive.charge_port_closed = false;   // 演示一个"未关闭"
    st.drive.tires_valid = true;
    st.drive.tire_fl = 2.9f;
    st.drive.tire_fr = 2.8f;
    st.drive.tire_rl = 2.7f;
    st.drive.tire_rr = 2.6f;
    st.drive.alert_valid = true;
    st.drive.alert = true;

    ui_render(&cv, &st);
    save_ppm("preview_drive.ppm", buf, W, H);

    // 信息页
    st.page = 1;
    ui_render(&cv, &st);
    save_ppm("preview_info.ppm", buf, W, H);

    // 配网页
    st.setup_mode = true;
    st.link = LCD_LINK_DISCONNECTED;
    st.page = 0;
    strcpy(st.ap_ssid, "TeslaDash-1A2B");
    strcpy(st.hint, "等待手机设置 VIN");
    ui_render(&cv, &st);
    save_ppm("preview_setup.ppm", buf, W, H);

    // 未连接：扫描等待页
    memset(&st, 0, sizeof(st));
    st.link = LCD_LINK_SCANNING;
    st.paired = true;
    strcpy(st.vin, "5YJ3E1EA7KF000000");
    st.spin = 40;
    ui_render(&cv, &st);
    save_ppm("preview_wait.ppm", buf, W, H);

    // 配对阶段：提示去车机刷卡授权
    st.link = LCD_LINK_PAIRING;
    st.paired = false;
    strcpy(st.hint, "请在车机上刷卡授权");
    ui_render(&cv, &st);
    save_ppm("preview_wait_pair.ppm", buf, W, H);

    free(buf);
    return 0;
}
