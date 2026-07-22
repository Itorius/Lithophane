#pragma once

#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"

typedef struct
{
	gpio_num_t ctrl_gpio;
	bool disabled;
} tps61165_handle_t;

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t tps61165_init(tps61165_handle_t* handle, gpio_num_t gpio);
void tps61165_set_brightness(tps61165_handle_t* handle, uint8_t brightness);
void tps61165_enable(tps61165_handle_t* handle);
void tps61165_disable(tps61165_handle_t* handle);

#ifdef __cplusplus
}
#endif
