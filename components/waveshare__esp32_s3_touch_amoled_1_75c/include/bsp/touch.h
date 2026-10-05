#pragma once
#include "esp_lcd_touch.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief BSP touch configuration structure
 *
 */
typedef struct {
    void *dummy;    /*!< Prepared for future use. */
} bsp_touch_config_t;

/**
 * @brief Create new touchscreen
 *
 * If you want to free resources allocated by this function, you can use esp_lcd_touch API, ie.:
 *
 * \code{.c}
 * esp_lcd_touch_del(tp);
 * \endcode
 *
 * @param[in]  config    touch configuration
 * @param[out] ret_touch esp_lcd_touch touchscreen handle
 * @return
 *      - ESP_OK         On success
 *      - Else           esp_lcd_touch failure
 */
esp_err_t bsp_touch_new(const bsp_display_cfg_t *cfg, esp_lcd_touch_handle_t *ret_touch);

/**
 * @brief The touch handle owned by the BSP's LVGL indev, for extra app-side
 *        reads (e.g. multi-point gestures via esp_lcd_touch_get_coordinates).
 *
 * @note get_coordinates only copies the last IRQ-refreshed snapshot; do NOT
 *       call esp_lcd_touch_read_data yourself, and stay on the LVGL task so
 *       reads serialize with the adapter's own polling.
 *
 * @return Touch handle, or NULL before bsp_display_start().
 */
esp_lcd_touch_handle_t bsp_touch_get_handle(void);

#ifdef __cplusplus
}
#endif
