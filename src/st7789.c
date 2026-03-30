#include "st7789.h"
#include "pindefs.h"
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <lvgl.h>
#include <hardware/gpio.h>
#include <hardware/pio.h>
#include <hardware/dma.h>
#include <pico/time.h>
#include "st7789_lcd.pio.h"




// Format: cmd length (including cmd byte), post delay in units of 5 ms, then cmd payload
// Note the delays have been shortened a little
static const uint8_t st7789_init_seq[] = {
        1, 20, 0x01,                        // Software reset
        1, 10, 0x11,                        // Exit sleep mode
        2, 2, 0x3a, 0x55,                   // Set colour mode to 16 bit
        //2, 0, 0x36, 0xc0,                // Set MADCTL: Portrate Mode.
        //2, 0, 0x36, 0x00,                // Set MADCTL: Portrate Mode.
        2, 0, 0x36, 0xa0,                   // Set MADCTL: Landscape Mode (BGR)
        //2, 0, 0x36, 0x60,                 // Set MADCTL: Landscape Mode
        3, 0, 0xb0, 0x00, 0xf8,            // Set RAMCTL: endian bit
        5, 0, 0x2a, 0x00, 0x00, SCREEN_WIDTH >> 8, SCREEN_WIDTH & 0xff,   // CASET: column addresses
        5, 0, 0x2b, 0x00, 0x00, SCREEN_HEIGHT >> 8, SCREEN_HEIGHT & 0xff, // RASET: row addresses
        1, 2, 0x21,                         // Inversion on, then 10 ms delay (supposedly a hack?)
        1, 2, 0x13,                         // Normal display on, then 10 ms delay
        1, 2, 0x29,                         // Main screen turn on, then wait 500 ms
        0                                   // Terminate list
};

static inline void lcd_set_dc_cs(bool dc, bool cs) {
    sleep_us(1);
    gpio_put_masked((1u << PIN_DC) | (1u << PIN_CS), !!dc << PIN_DC | !!cs << PIN_CS);
    sleep_us(1);
}

static inline void lcd_write_cmd(PIO pio, uint sm, const uint8_t *cmd, size_t count) {
    st7789_lcd_wait_idle(pio, sm);
    lcd_set_dc_cs(0, 0);
    st7789_lcd_put(pio, sm, *cmd++);
    if (count >= 2) {
        st7789_lcd_wait_idle(pio, sm);
        lcd_set_dc_cs(1, 0);
        for (size_t i = 0; i < count - 1; ++i)
            st7789_lcd_put(pio, sm, *cmd++);
    }
    st7789_lcd_wait_idle(pio, sm);
    lcd_set_dc_cs(1, 1);
}

static inline void lcd_init(PIO pio, uint sm, const uint8_t *init_seq) {
    const uint8_t *cmd = init_seq;
    while (*cmd) {
        lcd_write_cmd(pio, sm, cmd + 2, *cmd);
        sleep_ms(*(cmd + 1) * 5);
        cmd += *cmd + 2;
    }
}

static inline void st7789_start_pixels(PIO pio, uint sm) {
    uint8_t cmd = 0x2c; // RAMWR
    lcd_write_cmd(pio, sm, &cmd, 1);
    lcd_set_dc_cs(1, 0);
}


static PIO pio = (void*) pio0;
static uint sm = 0;
static int dma_chan = -1;



void start_pixels() {
  st7789_start_pixels ( pio, sm );
}


void lcd_write_pixels ( const uint16_t *data, size_t count ) {
    st7789_lcd_wait_idle(pio, sm);
    lcd_set_dc_cs(1, 0);
    for (size_t i = 0; i < count; ++i){
        uint16_t v = data[i];
        st7789_lcd_put(pio,sm,v&0xff);
        st7789_lcd_put(pio,sm,v>>8);
    }   
    st7789_lcd_wait_idle(pio, sm);
    lcd_set_dc_cs(1, 1);
}

void lcd_write_pixels_dma(const uint16_t *data, size_t count) {
    if (dma_chan < 0) {
        // Fallback to non-DMA if DMA channel not initialized
        lcd_write_pixels(data, count);
        return;
    }
    
    st7789_lcd_wait_idle(pio, sm);
    lcd_set_dc_cs(1, 0);
    
    // DMA directly from the data pointer without intermediate buffer
    dma_channel_abort(dma_chan);
    
    dma_channel_config c = dma_channel_get_default_config(dma_chan);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_dreq(&c, pio_get_dreq(pio, sm, true));
    
    dma_channel_set_config(dma_chan, &c, false);
    dma_channel_set_read_addr(dma_chan, (uint8_t *)data, false);
    dma_channel_set_write_addr(dma_chan, (uint8_t *)&pio->txf[sm], false);
    dma_channel_set_trans_count(dma_chan, count * 2, true);
    
    dma_channel_wait_for_finish_blocking(dma_chan);
    
    st7789_lcd_wait_idle(pio, sm);
    lcd_set_dc_cs(1, 1);
}

void setDrawArea(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
    uint16_t x1p = x0+x1-1, y1p = y0+y1-1;

    uint8_t cmd_buf[5];
    cmd_buf[0] = 0x2a; // CASET
    cmd_buf[1] = x0 >> 8;
    cmd_buf[2] = x0 & 0xff;
    cmd_buf[3] = x1p >> 8;
    cmd_buf[4] = x1p & 0xff;
    lcd_write_cmd(pio, sm, cmd_buf, sizeof(cmd_buf));
    cmd_buf[0] = 0x2b; // RASET
    cmd_buf[1] = y0 >> 8;
    cmd_buf[2] = y0 & 0xff;
    cmd_buf[3] = y1p >> 8;
    cmd_buf[4] = y1p & 0xff;
    lcd_write_cmd(pio, sm, cmd_buf, sizeof(cmd_buf));
}


#define SERIAL_CLK_DIV 1.f
int st7789_lcd_init( void  ) {
    // Claim DMA channel for pixel transfers
    dma_chan = dma_claim_unused_channel(false);
    if (dma_chan < 0) {
        // No DMA available, will fall back to non-DMA mode
    }
   
    uint offset = pio_add_program(pio, &st7789_lcd_program);
    st7789_lcd_program_init(pio, sm, offset, PIN_DIN, PIN_CLK, SERIAL_CLK_DIV);

    gpio_init(PIN_CS);
    gpio_init(PIN_DC);
    gpio_init(PIN_RESET);
    gpio_init(PIN_BL);
    gpio_set_dir(PIN_CS, GPIO_OUT);
    gpio_set_dir(PIN_DC, GPIO_OUT);
    gpio_set_dir(PIN_RESET, GPIO_OUT);
    gpio_set_dir(PIN_BL, GPIO_OUT);

    gpio_put(PIN_CS, 1);
    gpio_put(PIN_BL, 0);  // Keep backlight off during init
    
    // Perform proper reset sequence: pull low, wait, release high
    gpio_put(PIN_RESET, 0);
    sleep_ms(100);
    gpio_put(PIN_RESET, 1);
    sleep_ms(100);
    
    lcd_init(pio, sm, st7789_init_seq);
    sleep_ms(150);
    gpio_put(PIN_BL, 1);  // Turn on backlight after full init

    return 0;
}


static void disp_flush(lv_display_t * disp, const lv_area_t * area, uint8_t * px_map) {
    // With partial rendering, use the area dimensions from LVGL
    uint16_t width = lv_area_get_width(area);
    uint16_t height = lv_area_get_height(area);
    uint16_t x1 = area->x1, y1 = area->y1;
    
    setDrawArea(x1, y1, width, height);
    start_pixels();
    lcd_write_pixels_dma((const uint16_t *)px_map, width * height);
    lv_display_flush_ready(disp);
}

static uint32_t my_tick(void) {
    return to_ms_since_boot(get_absolute_time());
}

// Partial buffer: 10 rows at a time instead of full screen
// Full screen = 320×240×2 = 150 KB
// This buffer = 320×10×2 = 6.4 KB
// Saves 143.6 KB RAM!
#define BUFFER_HEIGHT 10
static uint16_t g_displayBuffer0[SCREEN_WIDTH * BUFFER_HEIGHT];

lv_display_t * initialize_lvgl() {
    st7789_lcd_init();
    lv_init();
    lv_display_t *disp = lv_display_create(SCREEN_WIDTH, SCREEN_HEIGHT);
    lv_display_set_flush_cb(disp, disp_flush);
    
    // Use partial render mode with a 10-row buffer
    // LVGL will render the display in horizontal strips
    lv_display_set_buffers(
        disp,
        g_displayBuffer0,          // Buffer 1
        NULL,                      // No second buffer (single buffered)
        SCREEN_WIDTH * BUFFER_HEIGHT * sizeof(uint16_t),  // Buffer size in bytes
        LV_DISPLAY_RENDER_MODE_PARTIAL                     // Partial mode
    );
    
    // Set this display as the default display for LVGL
    lv_disp_set_default(disp);
    
    // CRITICAL: Create and load a default screen
    // lv_timer_handler() returns -1 if there's no active screen
    // lv_obj_create(NULL) creates a screen object
    lv_obj_t *screen = lv_obj_create(NULL);
    if (screen == NULL) {
        printf("ERROR: Failed to create screen\n");
        return disp;
    }
    lv_screen_load(screen);
    
    // lv_tick_set_cb(my_tick);
    return disp;
}