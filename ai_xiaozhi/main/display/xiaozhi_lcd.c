#include "xiaozhi_lcd.h"

#include "esp_heap_caps.h"

#define LCD_HOST SPI2_HOST
#define PARALLEL_LINES 16
#define EXAMPLE_LCD_PIXEL_CLOCK_HZ (20 * 1000 * 1000)
#define EXAMPLE_LCD_BK_LIGHT_ON_LEVEL 1
#define EXAMPLE_LCD_BK_LIGHT_OFF_LEVEL !EXAMPLE_LCD_BK_LIGHT_ON_LEVEL
#define EXAMPLE_PIN_NUM_DATA0 48 /*!< for 1-line SPI, this also refereed as MOSI */
#define EXAMPLE_PIN_NUM_PCLK 47
#define EXAMPLE_PIN_NUM_CS 21
#define EXAMPLE_PIN_NUM_DC 45
#define EXAMPLE_PIN_NUM_RST 16
#define EXAMPLE_PIN_NUM_BK_LIGHT 40

// The pixel number in horizontal and vertical
#define EXAMPLE_LCD_H_RES 320
#define EXAMPLE_LCD_V_RES 240
// Bit number used to represent command and parameter
#define EXAMPLE_LCD_CMD_BITS 8
#define EXAMPLE_LCD_PARAM_BITS 8

// 渲染的双缓冲区
static uint16_t *s_lines[2];

esp_lcd_panel_handle_t panel_handle = NULL;
esp_lcd_panel_io_handle_t io_handle = NULL;

void xiaozhi_lcd_init(void)
{
  // 背光的配置
  gpio_config_t bk_gpio_config = {
      .mode = GPIO_MODE_OUTPUT,
      .pin_bit_mask = 1ULL << EXAMPLE_PIN_NUM_BK_LIGHT};
  ESP_ERROR_CHECK(gpio_config(&bk_gpio_config));
  // SPI BUS 配置
  spi_bus_config_t buscfg = {
      .sclk_io_num = EXAMPLE_PIN_NUM_PCLK,
      .mosi_io_num = EXAMPLE_PIN_NUM_DATA0,
      .miso_io_num = -1,
      .quadwp_io_num = -1,
      .quadhd_io_num = -1,
      .max_transfer_sz = PARALLEL_LINES * EXAMPLE_LCD_H_RES * 2 + 8};

  // Initialize the SPI bus
  ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

  // io引脚相关配置
  esp_lcd_panel_io_spi_config_t io_config = {
      .dc_gpio_num = EXAMPLE_PIN_NUM_DC,
      .cs_gpio_num = EXAMPLE_PIN_NUM_CS,
      .pclk_hz = EXAMPLE_LCD_PIXEL_CLOCK_HZ,
      .lcd_cmd_bits = EXAMPLE_LCD_CMD_BITS,
      .lcd_param_bits = EXAMPLE_LCD_PARAM_BITS,
      .spi_mode = 0,
      .trans_queue_depth = 10,
  };

  // Attach the LCD to the SPI bus
  ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &io_handle));
  // 面板的配置

  esp_lcd_panel_dev_config_t panel_config = {
      .reset_gpio_num = EXAMPLE_PIN_NUM_RST,
      .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
      .bits_per_pixel = 16,
  };
  // Initialize the LCD configuration
  ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_config, &panel_handle));

  // Turn off backlight to avoid unpredictable display on the LCD screen while initializing
  // the LCD panel driver. (Different LCD screens may need different levels)
  ESP_ERROR_CHECK(gpio_set_level(EXAMPLE_PIN_NUM_BK_LIGHT, EXAMPLE_LCD_BK_LIGHT_OFF_LEVEL));

  // Reset the display
  ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));

  // Initialize LCD panel
  ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));

  // Turn on the screen
  ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

  // ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, true));

  // Swap x and y axis (Different LCD screens may need different options)
  ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel_handle, true));

  // Turn on backlight (Different LCD screens may need different levels)
  ESP_ERROR_CHECK(gpio_set_level(EXAMPLE_PIN_NUM_BK_LIGHT, EXAMPLE_LCD_BK_LIGHT_ON_LEVEL));

  // Initialize the effect displayed
  // ESP_ERROR_CHECK(pretty_effect_init());

  // Allocate memory for the pixel buffers
  // for (int i = 0; i < 2; i++)
  // {
  //   s_lines[i] = heap_caps_malloc(EXAMPLE_LCD_H_RES * PARALLEL_LINES * sizeof(uint16_t), MALLOC_CAP_DMA);
  //   assert(s_lines[i] != NULL);
  // }

  esp_lcd_panel_mirror(panel_handle, true, false);
}

/* 封装用于显示内容的方法 */
// pixels: 230 * 240 * 2
void xiaozhi_lcd_show(uint8_t *pixels)
{
  int sending_line = 0;
  int calc_line = 0;
  size_t cpy_size = EXAMPLE_LCD_H_RES * PARALLEL_LINES * sizeof(uint16_t);
  for (int y = 0; y < EXAMPLE_LCD_V_RES; y += PARALLEL_LINES)
  {
    // Calculate a line
    // pretty_effect_calc_lines(s_lines[calc_line], y, frame, PARALLEL_LINES);
    // 一次计算16行的字节数据存入至缓冲区
    memcpy(s_lines[sending_line], pixels, cpy_size);
    pixels += cpy_size;

    sending_line = calc_line;
    calc_line = !calc_line;
    // Send the calculated data
    esp_lcd_panel_draw_bitmap(panel_handle, 0, y, 0 + EXAMPLE_LCD_H_RES, y + PARALLEL_LINES, s_lines[sending_line]);
  }
}

/* 说明（2026-11-14 OTA 瘦身）：原 xiaozhi_lcd_show_image() 及 EMBED_FILES image.jpg
 * 全项目无调用点，已删除以节省约 43.7 KB flash（OTA 双槽空间紧张）。
 * 如后续需要"上电先显示一张开机图"，重新加回函数与 EMBED_FILES 即可。 */