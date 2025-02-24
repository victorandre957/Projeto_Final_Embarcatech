#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include "hardware/adc.h"
#include "hardware/dma.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"

// OLED Configuration
#define OLED_WIDTH 128
#define OLED_HEIGHT 64
#define OLED_ADDR 0x3C
#define I2C_INST i2c1

// PIO Buzzer Config
PIO pio = pio0;
const uint buzzer_sm_a = 0;
const uint buzzer_sm_b = 1;

// Hardware Pins
#define OLED_SCL 15
#define OLED_SDA 14
#define BUZZER_A 10
#define BUZZER_B 21
#define MIC_PIN 28
#define JOYSTICK_VRY 26
#define JOYSTICK_SW 22
#define BUTTON_A 5

// Global State
uint8_t volume = 50;
uint16_t sound_threshold = 0;
bool alarm_active = false;
absolute_time_t last_update;
absolute_time_t last_joystick_move;

// 5x7 Font Array (simplified)
static const uint8_t font[86][5] = {
    [0]  = {0x00, 0x00, 0x00, 0x00, 0x00}, // ' ' (ASCII 32)
    [5]  = {0x63, 0x13, 0x08, 0x64, 0x63}, // '%' (ASCII 37)
    [16] = {0x3E, 0x51, 0x49, 0x45, 0x3E}, // '0' (ASCII 48)
    [17] = {0x00, 0x42, 0x7F, 0x40, 0x00}, // '1' (ASCII 49)
    [18] = {0x42, 0x61, 0x51, 0x49, 0x46}, // '2' (ASCII 50)
    [19] = {0x21, 0x41, 0x45, 0x4B, 0x31}, // '3' (ASCII 51)
    [20] = {0x18, 0x14, 0x12, 0x7F, 0x10}, // '4' (ASCII 52)
    [21] = {0x27, 0x45, 0x45, 0x45, 0x39}, // '5' (ASCII 53)
    [22] = {0x3C, 0x4A, 0x49, 0x49, 0x30}, // '6' (ASCII 54)
    [23] = {0x01, 0x71, 0x09, 0x05, 0x03}, // '7' (ASCII 55)
    [24] = {0x36, 0x49, 0x49, 0x49, 0x36}, // '8' (ASCII 56)
    [25] = {0x06, 0x49, 0x49, 0x29, 0x1E}, // '9' (ASCII 57)
    [26] = {0x00, 0x36, 0x36, 0x00, 0x00}, // ':' (ASCII 58)
    [54] = {0x1F, 0x20, 0x40, 0x20, 0x1F}, // 'V' (ASCII 86)
    [69] = {0x38, 0x54, 0x54, 0x54, 0x18}, // 'e' (ASCII 101)
    [76] = {0x00, 0x41, 0x7F, 0x40, 0x00}, // 'l' (ASCII 108)
    [77] = {0x7C, 0x04, 0x18, 0x04, 0x78}, // 'm' (ASCII 109)
    [79] = {0x38, 0x44, 0x44, 0x44, 0x38}, // 'o' (ASCII 111)
    [85] = {0x3C, 0x40, 0x40, 0x20, 0x7C}  // 'u' (ASCII 117)
};
// PIO Program
static const uint16_t buzzer_instructions[] = {
    0x6008, // out pins, 8
    0x6008, // out pins, 8
    0x00c7  // mov isr, osr
};

static const pio_program_t buzzer_program = {
    .instructions = buzzer_instructions,
    .length = 3,
    .origin = -1
};

// OLED Functions
void i2c_write(uint8_t reg, uint8_t data) {
    uint8_t buf[2] = {reg, data};
    i2c_write_blocking(I2C_INST, OLED_ADDR, buf, 2, false);
}

void oled_init() {
    uint8_t init_seq[] = {
        0xAE, 0xD5, 0x80, 0xA8, 0x3F, 0xD3, 0x00, 0x40,
        0x8D, 0x14, 0x20, 0x00, 0xA1, 0xC8, 0xDA, 0x12,
        0x81, 0xCF, 0xD9, 0xF1, 0xDB, 0x30, 0xA4, 0xA6, 0x2E, 0xAF
    };
    
    for(int i=0; i<sizeof(init_seq); i++)
        i2c_write(0x00, init_seq[i]);
}

void oled_clear() {
    i2c_write(0x00, 0x21); // Column address
    i2c_write(0x00, 0);
    i2c_write(0x00, 127);
    i2c_write(0x00, 0x22); // Page address
    i2c_write(0x00, 0);
    i2c_write(0x00, 7);
    
    for(int i=0; i<1024; i++)
        i2c_write(0x40, 0x00);
}

void oled_draw_text(uint8_t x, uint8_t y, const char *text) {
    i2c_write(0x00, 0x21); // Column address
    i2c_write(0x00, x*6);
    i2c_write(0x00, 0x7F);
    i2c_write(0x00, 0x22); // Page address
    i2c_write(0x00, y);
    i2c_write(0x00, y);

    for(int i=0; text[i]; i++) {
        const uint8_t *chr = font[text[i] - ' '];
        for(int col=0; col<5; col++) {
            i2c_write(0x40, chr[col]);
        }
        i2c_write(0x40, 0x00); // Space between chars
    }
}

void update_display() {
    oled_clear();
    char buf[16];
    snprintf(buf, sizeof(buf), "Volume: %3d%%", volume);
    oled_draw_text(0, 0, buf);
}

// Buzzer Functions
void buzzer_init() {
    uint offset = pio_add_program(pio, &buzzer_program);
    pio_sm_config c = pio_get_default_sm_config();
    
    sm_config_set_wrap(&c, offset, offset + 2);
    sm_config_set_out_pins(&c, BUZZER_A, 1);
    pio_sm_init(pio, buzzer_sm_a, offset, &c);
    
    sm_config_set_out_pins(&c, BUZZER_B, 1);
    pio_sm_init(pio, buzzer_sm_b, offset, &c);
    
    pio_sm_set_enabled(pio, buzzer_sm_a, false);
    pio_sm_set_enabled(pio, buzzer_sm_b, false);
}

void buzzer_beep(uint duration_ms) {
    uint32_t level = (volume * 0xFFFF) / 100;
    pio_sm_put_blocking(pio, buzzer_sm_a, level);
    pio_sm_put_blocking(pio, buzzer_sm_b, ~level);
    pio_sm_set_enabled(pio, buzzer_sm_a, true);
    pio_sm_set_enabled(pio, buzzer_sm_b, true);
    sleep_ms(duration_ms);
    pio_sm_set_enabled(pio, buzzer_sm_a, false);
    pio_sm_set_enabled(pio, buzzer_sm_b, false);
}

// ADC/DMA Functions
void adc_init_dma() {
    adc_init();
    adc_gpio_init(MIC_PIN);
    adc_gpio_init(JOYSTICK_VRY);
    
    dma_channel_config c = dma_channel_get_default_config(0);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_16);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, true);
    channel_config_set_dreq(&c, DREQ_ADC);
    
    dma_channel_configure(0, &c,
        NULL,
        &adc_hw->fifo,
        0xFFFFFFFF,
        true
    );
}


uint16_t get_sound_level() {
    uint32_t sum = 0;
    for(int i=0; i<1000; i++)
        sum += adc_read();
    uint16_t level = sum / 1000;
    
    // Add debug output
    printf("Current sound level: %d\n", level);
    return level;
}

// Main Application
int main() {
    stdio_init_all();
    
    // Initialize I2C
    i2c_init(I2C_INST, 400000);
    gpio_set_function(OLED_SDA, GPIO_FUNC_I2C);
    gpio_set_function(OLED_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(OLED_SDA);
    gpio_pull_up(OLED_SCL);
    oled_init();
    oled_clear();
    
    // Initialize hardware
    buzzer_init();
    adc_init_dma();
    
    // Buttons
    gpio_init(BUTTON_A);
    gpio_set_dir(BUTTON_A, GPIO_IN);
    gpio_pull_up(BUTTON_A);
    
    gpio_init(JOYSTICK_SW);
    gpio_set_dir(JOYSTICK_SW, GPIO_IN);
    gpio_pull_up(JOYSTICK_SW);

    last_update = get_absolute_time();
    last_joystick_move = get_absolute_time();

    printf("System initialized. Start monitoring...\n");

    while(true) {
        // Calibration with audio feedback
        if(!gpio_get(BUTTON_A)) {
            sound_threshold = get_sound_level() * 0.7;  // Set threshold to 70% of current level
            printf("New threshold set: %d\n", sound_threshold);
            buzzer_beep(200); // Confirmation beep
            update_display();
            sleep_ms(1000);
        }
        
        // Volume control with debounce
        adc_select_input(1); // Read joystick Y
        uint16_t y = adc_read();
        
        if(absolute_time_diff_us(last_joystick_move, get_absolute_time()) > 250000) {
            if(y < 500 && volume < 100) {
                volume += 10;
                last_joystick_move = get_absolute_time();
            }
            else if(y > 3500 && volume > 0) {
                volume -= 10;
                last_joystick_move = get_absolute_time();
            }
        }
        
        // Volume confirmation
        if(!gpio_get(JOYSTICK_SW)) {
            update_display();
            buzzer_beep(100); // Acknowledge press
            while(!gpio_get(JOYSTICK_SW));
        }
        
        // Sound monitoring
        adc_select_input(0); // Read microphone
        if(sound_threshold > 0) {
            uint16_t current_level = get_sound_level();
            alarm_active = current_level < (sound_threshold * 2);
            
            printf("Current: %d, Threshold: %d, Alarm: %d\n", 
                  current_level, sound_threshold, alarm_active);
            
            if(alarm_active) {
                pio_sm_set_enabled(pio, buzzer_sm_a, true);
                pio_sm_set_enabled(pio, buzzer_sm_b, true);
            } else {
                pio_sm_set_enabled(pio, buzzer_sm_a, false);
                pio_sm_set_enabled(pio, buzzer_sm_b, false);
            }
        }
        
        // Update display
        if(absolute_time_diff_us(last_update, get_absolute_time()) > 500000) {
            update_display();
            last_update = get_absolute_time();
        }
        
        sleep_ms(50);
    }
}