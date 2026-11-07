// 仅用于 PC 端界面预览的最小 esp_err.h 替身
#ifndef ESP_ERR_STUB_H
#define ESP_ERR_STUB_H

typedef int esp_err_t;

#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NOT_FOUND 0x105
#define ESP_ERR_INVALID_ARG 0x102

#endif
