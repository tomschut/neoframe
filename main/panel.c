/* Pin map, register values, selection order, and row split transcribed from
 * Good Display 2026420: pindefine.h, comm.c, GDEP133C02.c (ESP-IDF 4.4.4).
 * The reference uses ordinary SPI, not quad transactions. */
#include "panel.h"
#include "core.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
enum { CS_M=18, CS_S=17, CLK=9, DATA0=41, DATA1=40, EPD_BUSY=7, RST=6, SW=45 };
static spi_device_handle_t spi;
static void select_mask(unsigned mask) {
    gpio_set_level(CS_M, !(mask & 1)); gpio_set_level(CS_S, !(mask & 2));
}
static esp_err_t ready(void) {
    int64_t deadline = esp_timer_get_time() + 120000000;
    /* Let BUSY propagate after commands before sampling it. */
    vTaskDelay(pdMS_TO_TICKS(10));
    while (!gpio_get_level(EPD_BUSY)) {
        if (esp_timer_get_time() >= deadline) return ESP_ERR_TIMEOUT;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return ESP_OK;
}
static esp_err_t command(uint8_t cmd, const uint8_t *data, size_t n) {
    spi_transaction_t t = {.cmd=cmd, .length=n*8, .tx_buffer=data};
    return spi_device_transmit(spi, &t);
}
static esp_err_t reg(unsigned mask, uint8_t cmd, const uint8_t *data, size_t n) {
    select_mask(mask); esp_err_t e=command(cmd, data, n); select_mask(0); return e;
}
esp_err_t nf_panel_init(void) {
    gpio_config_t g = {.pin_bit_mask=(1ULL<<CS_M)|(1ULL<<CS_S)|(1ULL<<RST)|(1ULL<<SW),
        .mode=GPIO_MODE_OUTPUT};
    esp_err_t e=gpio_config(&g); if (e != ESP_OK) return e;
    select_mask(0); gpio_set_level(RST, 1); gpio_set_level(SW, 1);
    g.pin_bit_mask=1ULL<<EPD_BUSY; g.mode=GPIO_MODE_INPUT;
    e=gpio_config(&g); if (e != ESP_OK) return e;
    spi_bus_config_t bus = {.mosi_io_num=DATA0, .miso_io_num=DATA1, .sclk_io_num=CLK,
        .quadwp_io_num=-1, .quadhd_io_num=-1, .max_transfer_sz=32768,
        .flags=SPICOMMON_BUSFLAG_MASTER};
    e=spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO); if (e != ESP_OK) return e;
    spi_device_interface_config_t dev = {.command_bits=8, .clock_speed_hz=10000000,
        .duty_cycle_pos=128, .queue_size=7, .cs_ena_posttrans=3, .spics_io_num=-1};
    e=spi_bus_add_device(SPI3_HOST, &dev, &spi);
    if (e!=ESP_OK) spi_bus_free(SPI3_HOST);
    return e;
}
esp_err_t nf_panel_render(const uint8_t *frame) {
    if (!spi || !nf_pixels_valid(frame, NF_FRAME_SIZE)) return ESP_ERR_INVALID_ARG;
    esp_err_t e;
#define TRY(x) do { e=(x); if (e != ESP_OK) goto done; } while(0)
    gpio_set_level(SW, 1); vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(RST, 0); vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(RST, 1); vTaskDelay(pdMS_TO_TICKS(20)); TRY(ready());
    static const struct { uint8_t mask, cmd, n, data[9]; } init[] = {
        {1,0x74,9,{0xC0,0x1E,0x1E,0xCE,0xCE,0xCE,0x15,0x15,0x55}},
        {3,0xF0,6,{0x49,0x55,0x13,0x5D,0x05,0x10}}, {3,0x00,2,{0xDF,0x69}},
        {3,0x50,1,{0xF7}}, {3,0x60,2,{0x03,0x03}}, {3,0x86,1,{0x10}},
        {3,0xE3,1,{0x22}}, {3,0xE0,1,{0x01}}, {3,0x61,4,{0x04,0xB0,0x03,0x20}},
        {1,0x01,6,{0x0F,0x00,0x28,0x2C,0x28,0x38}}, {1,0xB6,1,{0x07}},
        {1,0x06,2,{0xE8,0x28}}, {1,0xB7,1,{0x01}}, {1,0x05,2,{0xE8,0x28}},
        {1,0xB0,1,{0x01}}, {1,0xB1,1,{0x02}}
    };
    for (size_t i=0; i<sizeof(init)/sizeof(init[0]); ++i)
        TRY(reg(init[i].mask, init[i].cmd, init[i].data, init[i].n));
    for (unsigned cs=0; cs<2; ++cs) {
        select_mask(1U<<cs); TRY(command(0x10, NULL, 0));
        for (unsigned row=0; row<NF_ROWS; ++row) {
            /* Internal, aligned DMA bounce buffer; frame itself lives in PSRAM. */
            uint32_t data[75]; memcpy(data, frame+nf_row_offset(cs,row), sizeof(data));
            spi_transaction_ext_t t = {.command_bits=0, .base={.length=2400,
                .tx_buffer=data, .flags=SPI_TRANS_VARIABLE_CMD}};
            TRY(spi_device_transmit(spi, &t.base)); vTaskDelay(pdMS_TO_TICKS(1));
        }
        select_mask(0);
    }
    select_mask(3); TRY(command(0x04, NULL, 0)); TRY(ready()); select_mask(0);
    select_mask(3); vTaskDelay(pdMS_TO_TICKS(30));
    const uint8_t refresh=1, off=0;
    TRY(command(0x12, &refresh, 1)); TRY(ready()); select_mask(0);
    select_mask(3); TRY(command(0x02, &off, 1)); TRY(ready());
done:
    select_mask(0);
    /* A timeout must not leave the analog boosts energized indefinitely. */
    if (e!=ESP_OK) gpio_set_level(SW, 0);
    return e;
#undef TRY
}
