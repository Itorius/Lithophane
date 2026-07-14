#pragma once

#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"

typedef struct
{
	gpio_num_t ctrl_gpio;
} tps61165_handle_t;

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t tps61165_init(tps61165_handle_t* handle, gpio_num_t ctrl_gpio);
void tps61165_set_brightness(const tps61165_handle_t* handle, uint8_t step);
void tps61165_reset(const tps61165_handle_t* handle);

#ifdef __cplusplus
}
#endif
