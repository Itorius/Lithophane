#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "stusb4500.h"
#include "esp_timer.h"
#include "tps61165.h"

#define TMP1075_I2C_ADDR 0x48
#define TMP1075_TEMP_REG 0x00

#define H_STUSB_I2C_ADDR 0x28
#define V_STUSB_I2C_ADDR 0x29

#define I2C_MASTER_PORT I2C_NUM_0
#define I2C_MASTER_SDA_IO GPIO_NUM_0
#define I2C_MASTER_SCL_IO GPIO_NUM_1
#define I2C_MASTER_FREQ_HZ 100000
#define I2C_TIMEOUT_MS 100

#define POWER_OK GPIO_NUM_2
#define LED_CTRL_A GPIO_NUM_6
#define LED_CTRL_B GPIO_NUM_7

// brightness+
#define GPIO_PB1 GPIO_NUM_5
// brightness-
#define GPIO_PB2 GPIO_NUM_4
// power
#define GPIO_PB3 GPIO_NUM_10

#define TPS61165_BRIGHTNESS_STEP 1

static const char* TAG = "tmp1075";

static i2c_master_bus_handle_t i2c_bus;
static i2c_master_dev_handle_t tmp1075;
static tps61165_handle_t led_driver_a;
static tps61165_handle_t led_driver_b;

static void set_led_brightness(const uint8_t brightness)
{
	tps61165_set_brightness(&led_driver_a, brightness);
	tps61165_set_brightness(&led_driver_b, brightness);
}

static void IRAM_ATTR button_isr(void* arg)
{
	static int brightness = 1;
	static uint64_t last_isr_time = 0;

	const uint64_t now = esp_timer_get_time();

	if (now - last_isr_time > 200000)
	{
		last_isr_time = now;

		if (++brightness > 31) brightness = 1;

		set_led_brightness(brightness);
	}
}

static esp_err_t gpio_init(void)
{
	const gpio_config_t powerok_config = {
		.pin_bit_mask = 1 << POWER_OK,
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
		.pin_bit_mask = 1 << GPIO_PB1 | 1 << GPIO_PB2 | 1 << GPIO_PB3,
		.mode = GPIO_MODE_INPUT,
		.pull_up_en = GPIO_PULLUP_DISABLE,
		.pull_down_en = GPIO_PULLDOWN_DISABLE,
		.intr_type = GPIO_INTR_NEGEDGE
	};

	err = gpio_config(&brightness_config);

	gpio_install_isr_service(0);
	gpio_isr_handler_add(GPIO_PB1, button_isr, NULL);

	return err;
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

// TODO: program both
bool flash_stusb(void)
{
	const i2c_device_config_t dev_config = {
		.dev_addr_length = I2C_ADDR_BIT_LEN_7,
		.device_address = V_STUSB_I2C_ADDR,
		.scl_speed_hz = I2C_MASTER_FREQ_HZ,
	};

	static i2c_master_dev_handle_t stusb;

	i2c_master_bus_add_device(i2c_bus, &dev_config, &stusb);

	const stusb4500_t device = {
		.addr = V_STUSB_I2C_ADDR,
		.write = i2c_write,
		.read = i2c_read,
		.context = stusb
	};

	// negotiate 12V/2A or 15V/1.5A
	const stusb4500_nvm_config_t config = {
		.num_valid_pdos = 3,
		.pdo2_voltage_mv = 12000,
		.pdo2_current_ma = 2000,
		.pdo3_voltage_mv = 15000,
		.pdo3_current_ma = 1500
	};

	return stusb4500_nvm_flash(&device, &config);
}

void app_main(void)
{
	ESP_ERROR_CHECK(gpio_init());
	ESP_ERROR_CHECK(tps61165_init(&led_driver_a, LED_CTRL_A));
	ESP_ERROR_CHECK(tps61165_init(&led_driver_b, LED_CTRL_B));
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

	set_led_brightness(TPS61165_BRIGHTNESS_STEP);

	while (true)
	{
		float temperature_c = 0.0f;
		const esp_err_t err = tmp1075_read_celsius(&temperature_c);
		if (err == ESP_OK)
		{
			ESP_LOGI(TAG, "TMP1075 temperature: %.2f C", temperature_c);
		}
		else
		{
			ESP_LOGE(TAG, "TMP1075 read failed: %s", esp_err_to_name(err));
		}

		int powerok = gpio_get_level(POWER_OK); // 0 when sufficient voltage is present

		ESP_LOGI(TAG, "power ok: %d", powerok);

		vTaskDelay(pdMS_TO_TICKS(1000));
	}
}
