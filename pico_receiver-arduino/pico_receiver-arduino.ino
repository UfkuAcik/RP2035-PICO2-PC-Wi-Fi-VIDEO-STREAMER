#if F_CPU != 250000000
#error "CRITICAL: Please set 'CPU Speed' to '250 MHz (Overclock)' in the Arduino Tools menu!"
#endif

#ifdef __OPTIMIZE_SIZE__
#error "CRITICAL: Please set 'Optimize' to 'Optimize Even More (-O3)' in the Arduino Tools menu! The default (-Os) is too slow for DVI video encoding."
#endif

#pragma GCC optimize ("O3")
#include <Arduino.h>
#include <WiFi.h>
#include <lwip/udp.h>
#include "hardware/vreg.h"
#include "hardware/clocks.h"
#include "pico/multicore.h"

extern "C" {
#include "src/libdvi/dvi.h"
#include "src/libdvi/dvi_serialiser.h"
#include "src/libdvi/common_dvi_pin_configs.h"
#include "src/libdvi/tmds_encode.h"
#include "src/libdvi/audio_ring.h"
}
#include "src/JPEGDEC/JPEGDEC.h"
#include "src/font8x8_basic.h"

// Wi-Fi Configuration
const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
#define SERVER_PORT 8080

// Audio Configuration
#define AUDIO_BUFFER_SIZE 2048
#define AUDIO_PORT 8081
audio_sample_t audio_buffer[AUDIO_BUFFER_SIZE];

// DVI configuration for Olimex RP2040-PICO-PC
struct dvi_inst dvi0;

static const struct dvi_serialiser_cfg olimex_dvi_cfg = {
    .pio = pio1,
    .sm_tmds = {0, 1, 2},
    .pins_tmds = {14, 18, 16},
    .pins_clk = 12,
    .invert_diffpairs = true
};

#define FRAME_WIDTH 320
#define FRAME_HEIGHT 240
uint16_t __attribute__((aligned(4))) frame_buffer_0[FRAME_WIDTH * FRAME_HEIGHT];
uint16_t __attribute__((aligned(4))) frame_buffer_1[FRAME_WIDTH * FRAME_HEIGHT];
uint16_t* volatile display_buffer = frame_buffer_0;
uint16_t* volatile decode_buffer = frame_buffer_1;
volatile bool vblank_flag = false;

// FPS tracking
uint32_t frame_count = 0;
uint32_t current_fps = 0;
uint32_t last_fps_time = 0;

void draw_char(int x, int y, char c, uint16_t color) {
    if (c < 0 || c > 127) return;
    for (int row = 0; row < 8; row++) {
        uint8_t bits = font8x8_basic[(int)c][row];
        for (int col = 0; col < 8; col++) {
            if (bits & (1 << col)) {
                int px = x + col;
                int py = y + row;
                if (px >= 0 && px < FRAME_WIDTH && py >= 0 && py < FRAME_HEIGHT) {
                    decode_buffer[py * FRAME_WIDTH + px] = color;
                }
            }
        }
    }
}

void draw_string(int x, int y, const char *str, uint16_t color) {
    int cx = x;
    while (*str) {
        draw_char(cx, y, *str, color);
        cx += 8;
        str++;
    }
}

// JPEG Decoder
JPEGDEC jpeg;
#define JPEG_BUF_SIZE (20 * 1024)
uint8_t rx_buffer[2][JPEG_BUF_SIZE];
volatile int active_rx_idx = 0;
volatile int decode_rx_idx = -1;
volatile uint32_t current_frame_id = 0xFFFFFFFF;
volatile uint32_t chunks_received = 0;
volatile uint32_t chunk_mask = 0;
volatile bool is_decoding = false;
volatile uint32_t decode_frame_len = 0;

int __attribute__((section(".time_critical.JPEGDraw"))) JPEGDraw(JPEGDRAW *pDraw) {
    int x = pDraw->x;
    int y = pDraw->y;
    int w = pDraw->iWidth;
    int h = pDraw->iHeight;
    uint16_t *pixels = pDraw->pPixels;
    
    int max_w = (x + w <= FRAME_WIDTH) ? w : (FRAME_WIDTH - x);
    int max_h = (y + h <= FRAME_HEIGHT) ? h : (FRAME_HEIGHT - y);
    if (max_w <= 0 || max_h <= 0) return 1;

    for (int iy = 0; iy < max_h; iy++) {
        memcpy(&decode_buffer[(y + iy) * FRAME_WIDTH + x], &pixels[iy * w], max_w * 2);
    }
    return 1;
}

volatile bool dvi_initialized = false;

// SETUP1 and LOOP1 run automatically on Core 1 in Arduino-Pico
void setup1() {
    while (!dvi_initialized) {
        delay(1);
    }
    dvi_register_irqs_this_core(&dvi0, DMA_IRQ_1);
    irq_set_priority(DMA_IRQ_1, 0x00); // Absolute highest hardware priority!
    dvi_start(&dvi0);
}

void loop1() {
    while (true) {
        static uint y = 0;
        uint32_t *tmdsbuf;
        while (!queue_try_remove_u32(&dvi0.q_tmds_free, &tmdsbuf)) {
            __wfe(); // Wait for DMA event to prevent Bus/SRAM starvation
        }
        
        uint32_t *scanbuf = (uint32_t*)&display_buffer[y * FRAME_WIDTH];
        uint pixwidth = dvi0.timing->h_active_pixels;
        uint words_per_channel = pixwidth / DVI_SYMBOLS_PER_WORD;
        
        tmds_encode_data_channel_16bpp(scanbuf, tmdsbuf + 0 * words_per_channel, pixwidth / 2, DVI_16BPP_BLUE_MSB,  DVI_16BPP_BLUE_LSB );
        tmds_encode_data_channel_16bpp(scanbuf, tmdsbuf + 1 * words_per_channel, pixwidth / 2, DVI_16BPP_GREEN_MSB, DVI_16BPP_GREEN_LSB);
        tmds_encode_data_channel_16bpp(scanbuf, tmdsbuf + 2 * words_per_channel, pixwidth / 2, DVI_16BPP_RED_MSB,   DVI_16BPP_RED_LSB  );
        
        while (!queue_try_add_u32(&dvi0.q_tmds_valid, &tmdsbuf)) {
            __wfe(); // Wait for DMA event to prevent Bus/SRAM starvation
        }

        y++;
        if (y >= FRAME_HEIGHT) {
            y = 0;
            vblank_flag = true;
        }
    }
}

// RAW lwIP UDP callbacks for maximum performance (zero-copy)
static void __attribute__((section(".time_critical.udp_recv"))) udp_recv_callback(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port) {
    if (p == NULL) return;
    
    if (p->tot_len >= 8) {
        uint32_t frame_id;
        uint16_t chunk_idx;
        uint16_t total_chunks;
        
        pbuf_copy_partial(p, &frame_id, 4, 0);
        pbuf_copy_partial(p, &chunk_idx, 2, 4);
        pbuf_copy_partial(p, &total_chunks, 2, 6);
        
        uint16_t payload_len = p->tot_len - 8;
        
        if (frame_id != current_frame_id) {
            current_frame_id = frame_id;
            chunks_received = 0;
            chunk_mask = 0;
        }
        
        uint32_t offset = chunk_idx * 1400; // Safe MTU
        if (offset + payload_len <= JPEG_BUF_SIZE && chunk_idx < 32) {
            if (!(chunk_mask & (1 << chunk_idx))) { // Prevent UDP packet duplication crashes
                pbuf_copy_partial(p, &rx_buffer[active_rx_idx][offset], payload_len, 8);
                chunk_mask |= (1 << chunk_idx);
                chunks_received++;

                static uint32_t current_full_len = 0;
                if (chunk_idx == total_chunks - 1) {
                    current_full_len = offset + payload_len;
                }

                if (chunks_received == total_chunks) {
                    if (!is_decoding) {
                        decode_frame_len = current_full_len;
                        decode_rx_idx = active_rx_idx;
                        active_rx_idx = 1 - active_rx_idx; // Ping-Pong
                    }
                    chunks_received = 0;
                    chunk_mask = 0;
                }
            }
        }
    }
    pbuf_free(p);
}

static void __attribute__((section(".time_critical.udp_audio"))) udp_audio_recv_callback(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port) {
    if (p == NULL) return;
    
    int size = get_write_size(&dvi0.audio_ring, false);
    if (size > 0) {
        int samples_in_packet = p->tot_len / sizeof(audio_sample_t);
        int samples_to_write = (samples_in_packet < size) ? samples_in_packet : size;
        
        audio_sample_t *audio_ptr = get_write_pointer(&dvi0.audio_ring);
        pbuf_copy_partial(p, audio_ptr, samples_to_write * sizeof(audio_sample_t), 0);
        increase_write_pointer(&dvi0.audio_ring, samples_to_write);
    }
    
    pbuf_free(p);
}

void setup() {
    // 252.0 MHz is a clean PLL multiple close to the 25.175 MHz pixel clock required by DVI 640x480
    // Voltage bump is required for stability under heavy Wi-Fi + DVI load!
    vreg_set_voltage(VREG_VOLTAGE_1_25);
    delay(10);
    set_sys_clock_khz(252000, true); // Overclock to exactly 252 MHz
    // However, we need to configure our specific DVI parameters.
    
    // Fill buffers with black
    for (int i = 0; i < FRAME_WIDTH * FRAME_HEIGHT; i++) {
        frame_buffer_0[i] = 0x0000;
        frame_buffer_1[i] = 0x0000;
    }
    
    // I2S Audio Pin Configuration is not needed for DVI Audio stream!
    
    dvi0.timing = &dvi_timing_640x480p_60hz;
    dvi0.ser_cfg = olimex_dvi_cfg;
    dvi_init(&dvi0, next_striped_spin_lock_num(), next_striped_spin_lock_num());
    dvi_set_audio_freq(&dvi0, 44100, 28000, 6272);
    dvi_audio_sample_buffer_set(&dvi0, audio_buffer, AUDIO_BUFFER_SIZE);
    dvi_initialized = true;
    
    // Initialize Arduino WiFi
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    
    // Wait for connection and update UI
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        for (int i = 0; i < FRAME_WIDTH * FRAME_HEIGHT; i++) decode_buffer[i] = 0x0000;
        draw_string(10, 10, "Pico-Cast (Arduino Port)", 0xFFFF);
        draw_string(10, 30, "Connecting to Wi-Fi...", 0x07E0);
        
        // Swap buffers manually for UI
        uint16_t* temp = display_buffer;
        display_buffer = decode_buffer;
        decode_buffer = temp;
    }
    
    // Connected!
    for (int i = 0; i < FRAME_WIDTH * FRAME_HEIGHT; i++) decode_buffer[i] = 0x0000;
    draw_string(10, 10, "Wi-Fi Connected!", 0x07E0);
    draw_string(10, 30, "IP:", 0xFFFF);
    draw_string(40, 30, WiFi.localIP().toString().c_str(), 0xFFFF);
    draw_string(10, 50, "Waiting for stream...", 0xF800);
    
    uint16_t* temp = display_buffer;
    display_buffer = decode_buffer;
    decode_buffer = temp;
    
    // Setup Raw lwIP UDP bindings for extreme performance
    struct udp_pcb *video_pcb = udp_new();
    udp_bind(video_pcb, IP_ANY_TYPE, SERVER_PORT);
    udp_recv(video_pcb, udp_recv_callback, NULL);
    
    struct udp_pcb *audio_pcb = udp_new();
    udp_bind(audio_pcb, IP_ANY_TYPE, AUDIO_PORT);
    udp_recv(audio_pcb, udp_audio_recv_callback, NULL);
}

void loop() {
    if (decode_rx_idx != -1) {
        is_decoding = true;
        int target_idx = decode_rx_idx;
        uint32_t len = decode_frame_len;
        decode_rx_idx = -1; // Consume it immediately
        
        if (jpeg.openRAM(rx_buffer[target_idx], len, JPEGDraw)) {
            jpeg.setPixelType(RGB565_LITTLE_ENDIAN);
            jpeg.decode(0, 0, 0);
            jpeg.close();
            
            // Draw FPS Counter
            char fps_str[32];
            sprintf(fps_str, "FPS: %lu", current_fps);
            draw_string(FRAME_WIDTH - 80, 10, fps_str, 0x07E0); // Draw in green
            
            // Tear-Free Adaptive Beam Racing VSYNC
            if (!vblank_flag) {
                uint32_t wait_start = micros();
                // Wait max 2ms for VBLANK to prevent FPS stuttering!
                while (!vblank_flag && (micros() - wait_start < 2000)) {
                    tight_loop_contents(); // Keep it raw and fast, do not yield!
                }
            }
            vblank_flag = false;
            
            // Swap buffers ONLY after a successful decode and VSYNC synchronization!
            uint16_t* temp = display_buffer;
            display_buffer = decode_buffer;
            decode_buffer = temp;
            
            frame_count++;
        }
        is_decoding = false;
    }
    
    // FPS Calculation
    uint32_t now = micros();
    if (now - last_fps_time >= 1000000) {
        current_fps = frame_count;
        frame_count = 0;
        last_fps_time = now;
    }
}
