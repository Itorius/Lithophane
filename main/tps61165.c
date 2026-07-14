#include "tps61165.h"

#include "esp_rom_sys.h"

#define TPS61165_SHUTDOWN_US 3000
#define TPS61165_START_US 10
#define TPS61165_EOS_US 10
#define TPS61165_SHORT_US 20
#define TPS61165_LONG_US 60

#define TPS61165_ES_ADDRESS 0x72
#define TPS61165_ES_DETECT_DELAY_US 120
#define TPS61165_ES_DETECT_TIME_US 300
#define TPS61165_ES_DETECT_WINDOW_US 1000

static void tps61165_set_ctrl(const tps61165_handle_t* handle, const uint32_t level)
{
	gpio_set_level(handle->ctrl_gpio, level);
}

static void tps61165_write_bit(const tps61165_handle_t* handle, const bool bit)
{
	tps61165_set_ctrl(handle, 0);
	esp_rom_delay_us(bit ? TPS61165_SHORT_US : TPS61165_LONG_US);
	tps61165_set_ctrl(handle, 1);
	esp_rom_delay_us(bit ? TPS61165_LONG_US : TPS61165_SHORT_US);
}

static void tps61165_write_byte(const tps61165_handle_t* handle, const uint8_t data)
{
	tps61165_set_ctrl(handle, 1);
	esp_rom_delay_us(TPS61165_START_US);

	for (int bit = 7; bit >= 0; bit--)
	{
		tps61165_write_bit(handle, (data & 1U << bit) != 0);
	}

	tps61165_set_ctrl(handle, 0);
	esp_rom_delay_us(TPS61165_EOS_US);
	tps61165_set_ctrl(handle, 1);
}

esp_err_t tps61165_init(tps61165_handle_t* handle, const gpio_num_t ctrl_gpio)
{
	if (handle == nullptr)
	{
		return ESP_ERR_INVALID_ARG;
	}

	handle->ctrl_gpio = ctrl_gpio;

	const gpio_config_t ctrl_config = {
		.pin_bit_mask = 1 << handle->ctrl_gpio,
		.mode = GPIO_MODE_OUTPUT,
		.pull_up_en = GPIO_PULLUP_DISABLE,
		.pull_down_en = GPIO_PULLDOWN_DISABLE,
		.intr_type = GPIO_INTR_DISABLE
	};

	const esp_err_t err = gpio_config(&ctrl_config);
	if (err != ESP_OK)
	{
		return err;
	}

	tps61165_reset(handle);

	return ESP_OK;
}

void tps61165_set_brightness(const tps61165_handle_t* handle, const uint8_t step)
{
	if (handle == nullptr)
	{
		return;
	}

	const uint8_t data = step & 0x1F;

	tps61165_write_byte(handle, TPS61165_ES_ADDRESS);
	tps61165_write_byte(handle, data);
}

void tps61165_reset(const tps61165_handle_t* handle)
{
	if (handle == nullptr)
	{
		return;
	}

	tps61165_set_ctrl(handle, 0);
	esp_rom_delay_us(TPS61165_SHUTDOWN_US);

	tps61165_set_ctrl(handle, 1);
	esp_rom_delay_us(TPS61165_ES_DETECT_DELAY_US);
	tps61165_set_ctrl(handle, 0);
	esp_rom_delay_us(TPS61165_ES_DETECT_TIME_US);
	tps61165_set_ctrl(handle, 1);
	esp_rom_delay_us(TPS61165_ES_DETECT_WINDOW_US);
}
