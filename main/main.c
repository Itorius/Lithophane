#include <inttypes.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "stusb4500.h"
#include "tps61165.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TMP1075_I2C_ADDR 0x48
#define TMP1075_TEMP_REG 0x00

#define H_STUSB_I2C_ADDR 0x28
#define V_STUSB_I2C_ADDR 0x29

#define I2C_MASTER_PORT I2C_NUM_0
#define I2C_MASTER_SDA_IO GPIO_NUM_0
#define I2C_MASTER_SCL_IO GPIO_NUM_1
#define I2C_MASTER_FREQ_HZ 100000
#define I2C_TIMEOUT_MS 100

#define GPIO_POWER_OK GPIO_NUM_2
#define GPIO_LEDCTRL_A GPIO_NUM_6
#define GPIO_LEDCTRL_B GPIO_NUM_7
#define GPIO_BRIGHTNESS_M GPIO_NUM_4
#define GPIO_BRIGHTNESS_P GPIO_NUM_5
#define GPIO_POWER GPIO_NUM_10

#define MIN_BRIGHTNESS 1
#define MAX_BRIGHTNESS 8
#define OVERTEMP_SHUTDOWN_C 50.0f
#define OVERTEMP_RECOVERY_C 45.0f
#define BUTTON_PRESSED_LEVEL 0
#define NVM_NAMESPACE "settings"
#define NVM_TARGET_BRIGHTNESS_KEY "brightness"

static const char* TAG = "lithophane";

static i2c_master_bus_handle_t i2c_bus;
static i2c_master_dev_handle_t tmp1075;
static tps61165_handle_t led_driver_a;
static tps61165_handle_t led_driver_b;

static volatile uint32_t target_brightness = MAX_BRIGHTNESS;
static volatile bool led_disabled = false;
static volatile bool brightness_decrease_armed = true;
static volatile bool brightness_increase_armed = true;
static volatile bool power_armed = true;
static volatile bool target_brightness_save_pending = false;

static void led_set_brightness(const uint8_t brightness)
{
	tps61165_set_brightness(&led_driver_a, brightness);
	tps61165_set_brightness(&led_driver_b, brightness);
}

static bool IRAM_ATTR button_is_pressed(const gpio_num_t gpio)
{
	return gpio_get_level(gpio) == BUTTON_PRESSED_LEVEL;
}

static void rearm_button_if_released(volatile bool* armed, const gpio_num_t gpio)
{
	if (!*armed && !button_is_pressed(gpio))
	{
		*armed = true;
	}
}

static void rearm_released_buttons(void)
{
	rearm_button_if_released(&brightness_decrease_armed, GPIO_BRIGHTNESS_M);
	rearm_button_if_released(&brightness_increase_armed, GPIO_BRIGHTNESS_P);
	rearm_button_if_released(&power_armed, GPIO_POWER);
}

static void IRAM_ATTR brightness_decrease(void* arg)
{
	if (!brightness_decrease_armed || !button_is_pressed(GPIO_BRIGHTNESS_M))
	{
		return;
	}

	brightness_decrease_armed = false;

	if (target_brightness > MIN_BRIGHTNESS)
	{
		target_brightness--;
		target_brightness_save_pending = true;
	}
}

static void IRAM_ATTR brightness_increase(void* arg)
{
	if (!brightness_increase_armed || !button_is_pressed(GPIO_BRIGHTNESS_P))
	{
		return;
	}

	brightness_increase_armed = false;

	if (target_brightness < MAX_BRIGHTNESS)
	{
		target_brightness++;
		target_brightness_save_pending = true;
	}
}

static void IRAM_ATTR toggle_power(void* arg)
{
	if (!power_armed || !button_is_pressed(GPIO_POWER))
	{
		return;
	}

	power_armed = false;
	led_disabled = !led_disabled;
}

static esp_err_t gpio_init(void)
{
	const gpio_config_t powerok_config = {
		.pin_bit_mask = 1ULL << GPIO_POWER_OK,
		.mode = GPIO_MODE_INPUT,
		.pull_up_en = GPIO_PULLUP_DISABLE,
		.pull_down_en = GPIO_PULLDOWN_DISABLE,
		.intr_type = GPIO_INTR_DISABLE
	};

	esp_err_t err = gpio_config(&powerok_config);
	if (err != ESP_OK)
	{
		return err;
	}

	const gpio_config_t brightness_config = {
		.pin_bit_mask = 1ULL << GPIO_BRIGHTNESS_M | 1ULL << GPIO_BRIGHTNESS_P | 1ULL << GPIO_POWER,
		.mode = GPIO_MODE_INPUT,
		.pull_up_en = GPIO_PULLUP_ENABLE,
		.pull_down_en = GPIO_PULLDOWN_DISABLE,
		.intr_type = GPIO_INTR_NEGEDGE
	};

	err = gpio_config(&brightness_config);

	gpio_install_isr_service(0);
	gpio_isr_handler_add(GPIO_BRIGHTNESS_M, brightness_decrease, NULL);
	gpio_isr_handler_add(GPIO_BRIGHTNESS_P, brightness_increase, NULL);
	gpio_isr_handler_add(GPIO_POWER, toggle_power, NULL);

	return err;
}

static uint32_t clamp_brightness(const uint32_t brightness)
{
	if (brightness < MIN_BRIGHTNESS)
	{
		return MIN_BRIGHTNESS;
	}

	if (brightness > MAX_BRIGHTNESS)
	{
		return MAX_BRIGHTNESS;
	}

	return brightness;
}

static esp_err_t nvm_init(void)
{
	esp_err_t err = nvs_flash_init();
	if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
	{
		ESP_ERROR_CHECK(nvs_flash_erase());
		err = nvs_flash_init();
	}

	return err;
}

static esp_err_t target_brightness_load(void)
{
	nvs_handle_t handle;
	esp_err_t err = nvs_open(NVM_NAMESPACE, NVS_READONLY, &handle);
	if (err == ESP_ERR_NVS_NOT_FOUND)
	{
		return ESP_OK;
	}

	if (err != ESP_OK)
	{
		return err;
	}

	uint8_t brightness = 0;
	err = nvs_get_u8(handle, NVM_TARGET_BRIGHTNESS_KEY, &brightness);
	nvs_close(handle);

	if (err == ESP_ERR_NVS_NOT_FOUND)
	{
		return ESP_OK;
	}

	if (err != ESP_OK)
	{
		return err;
	}

	target_brightness = clamp_brightness(brightness);
	target_brightness_save_pending = target_brightness != brightness;

	return ESP_OK;
}

static esp_err_t target_brightness_save(const uint32_t brightness)
{
	nvs_handle_t handle;
	esp_err_t err = nvs_open(NVM_NAMESPACE, NVS_READWRITE, &handle);
	if (err != ESP_OK)
	{
		return err;
	}

	const uint8_t clamped_brightness = (uint8_t)clamp_brightness(brightness);

	err = nvs_set_u8(handle, NVM_TARGET_BRIGHTNESS_KEY, clamped_brightness);
	if (err == ESP_OK)
	{
		err = nvs_commit(handle);
	}

	nvs_close(handle);
	return err;
}

static void target_brightness_save_if_pending(void)
{
	if (!target_brightness_save_pending)
	{
		return;
	}

	target_brightness_save_pending = false;
	const uint32_t brightness = target_brightness;

	const esp_err_t err = target_brightness_save(brightness);
	if (err != ESP_OK)
	{
		target_brightness_save_pending = true;
		ESP_LOGW(TAG, "Failed to save brightness: %s", esp_err_to_name(err));
	}
}

static esp_err_t i2c_init(void)
{
	const i2c_master_bus_config_t bus_config = {
		.i2c_port = I2C_MASTER_PORT,
		.sda_io_num = I2C_MASTER_SDA_IO,
		.scl_io_num = I2C_MASTER_SCL_IO,
		.clk_source = I2C_CLK_SRC_DEFAULT,
		.glitch_ignore_cnt = 7
	};

	const esp_err_t err = i2c_new_master_bus(&bus_config, &i2c_bus);
	if (err != ESP_OK)
	{
		return err;
	}

	const i2c_device_config_t dev_config = {
		.dev_addr_length = I2C_ADDR_BIT_LEN_7,
		.device_address = TMP1075_I2C_ADDR,
		.scl_speed_hz = I2C_MASTER_FREQ_HZ,
	};

	return i2c_master_bus_add_device(i2c_bus, &dev_config, &tmp1075);
}

static esp_err_t tmp1075_read_celsius(float* temperature_c)
{
	const uint8_t reg = TMP1075_TEMP_REG;
	uint8_t data[2] = {0};

	const esp_err_t err = i2c_master_transmit_receive(tmp1075, &reg, sizeof(reg), data, sizeof(data), I2C_TIMEOUT_MS);

	if (err != ESP_OK)
	{
		return err;
	}

	uint16_t raw = ((uint16_t)data[0] << 8 | data[1]) >> 4;
	if ((raw & 0x0800) != 0)
	{
		raw |= 0xF000;
	}

	*temperature_c = (float)(int16_t)raw * 0.0625f;
	return ESP_OK;
}

bool i2c_write(const uint16_t address, const uint8_t reg, const void* buffer, const size_t length, void* context)
{
	(void)address; // Address is already configured in the device handle

	if (context == NULL)
		return false;

	if (length > 0 && buffer == NULL)
		return false;

	const i2c_master_dev_handle_t device = context;

	uint8_t data[length + 1];

	data[0] = reg;

	if (length > 0)
	{
		memcpy(&data[1], buffer, length);
	}

	const esp_err_t err = i2c_master_transmit(device, data, length + 1, I2C_TIMEOUT_MS);

	return err == ESP_OK;
}

bool i2c_read(const uint16_t address, const uint8_t reg, void* buffer, const size_t length, void* context)
{
	(void)address; // Address is already configured in the device handle

	if (context == NULL)
		return false;

	if (length > 0 && buffer == NULL)
		return false;

	const i2c_master_dev_handle_t device = context;

	const esp_err_t err = i2c_master_transmit_receive(device, &reg, 1, buffer, length, I2C_TIMEOUT_MS);

	return err == ESP_OK;
}

bool flash_stusb(const uint32_t address)
{
	const i2c_device_config_t dev_config = {
		.dev_addr_length = I2C_ADDR_BIT_LEN_7,
		.device_address = address,
		.scl_speed_hz = I2C_MASTER_FREQ_HZ,
	};

	static i2c_master_dev_handle_t stusb;

	i2c_master_bus_add_device(i2c_bus, &dev_config, &stusb);

	const stusb4500_t device = {
		.addr = address,
		.write = i2c_write,
		.read = i2c_read,
		.context = stusb
	};

	// negotiate 12V/2A or 15V/1.5A
	const stusb4500_nvm_config_t config = {
		.num_valid_pdos = 3,
		.pdo2_voltage_mv = 12000,
		.pdo2_current_ma = 1000,
		.pdo3_voltage_mv = 15000,
		.pdo3_current_ma = 800
	};

	return stusb4500_nvm_flash(&device, &config);
}

void app_main(void)
{
	ESP_ERROR_CHECK(nvm_init());
	ESP_ERROR_CHECK(target_brightness_load());
	ESP_ERROR_CHECK(gpio_init());
	ESP_ERROR_CHECK(tps61165_init(&led_driver_a, GPIO_LEDCTRL_A));
	ESP_ERROR_CHECK(tps61165_init(&led_driver_b, GPIO_LEDCTRL_B));
	ESP_ERROR_CHECK(i2c_init());

	esp_err_t probe_err = i2c_master_probe(i2c_bus, TMP1075_I2C_ADDR, I2C_TIMEOUT_MS);
	if (probe_err != ESP_OK)
	{
		ESP_LOGW(TAG, "TMP1075 did not ACK at 0x%02x: %s", TMP1075_I2C_ADDR, esp_err_to_name(probe_err));
	}

	probe_err = i2c_master_probe(i2c_bus, H_STUSB_I2C_ADDR, I2C_TIMEOUT_MS);
	if (probe_err != ESP_OK)
	{
		ESP_LOGW(TAG, "STUSB4500 (horizonal) did not ACK at 0x%02x: %s", H_STUSB_I2C_ADDR, esp_err_to_name(probe_err));
	}

	probe_err = i2c_master_probe(i2c_bus, V_STUSB_I2C_ADDR, I2C_TIMEOUT_MS);
	if (probe_err != ESP_OK)
	{
		ESP_LOGW(TAG, "STUSB4500 (vertical) did not ACK at 0x%02x: %s", V_STUSB_I2C_ADDR, esp_err_to_name(probe_err));
	}

#if 0
	if (!flash_stusb(H_STUSB_I2C_ADDR))
	{
		ESP_LOGW(TAG, "Failed to flash STUSB (horizontal)");
	}

	if (!flash_stusb(V_STUSB_I2C_ADDR))
	{
		ESP_LOGW(TAG, "Failed to flash STUSB (vertical)");
	}
#endif

	uint32_t timer = 0;
	float temperature = 0.0f;
	bool power_ok = false;
	bool overtemp_active = false;

	while (true)
	{
		rearm_released_buttons();
		target_brightness_save_if_pending();

		uint32_t brightness = target_brightness;
		static uint32_t last_brightness = UINT32_MAX;

		if (timer++ % 10 == 0)
		{
			power_ok = !gpio_get_level(GPIO_POWER_OK);

			if (tmp1075_read_celsius(&temperature) == ESP_OK)
			{
				if (!overtemp_active && temperature >= OVERTEMP_SHUTDOWN_C)
				{
					overtemp_active = true;
					ESP_LOGW(TAG, "Overtemp protection active: %.2f C", temperature);
				}
				else if (overtemp_active && temperature <= OVERTEMP_RECOVERY_C)
				{
					overtemp_active = false;
					ESP_LOGI(TAG, "Overtemp protection cleared: %.2f C", temperature);
				}
			}

			ESP_LOGI(TAG, "Disabled: %d", led_disabled);
			ESP_LOGI(TAG, "Overtemp: %d", overtemp_active);
			ESP_LOGI(TAG, "Power OK: %d", power_ok);
			ESP_LOGI(TAG, "Target brightness: %" PRIu32 ", actual brightness: %" PRIu32, (uint32_t)target_brightness, brightness);
			ESP_LOGI(TAG, "Temperature: %.2f C", temperature);
		}

		if (overtemp_active || led_disabled || !power_ok)
		{
			brightness = 0;
		}

		if (brightness != last_brightness)
		{
			led_set_brightness(brightness);
			last_brightness = brightness;
		}

		vTaskDelay(pdMS_TO_TICKS(100));
	}
}
