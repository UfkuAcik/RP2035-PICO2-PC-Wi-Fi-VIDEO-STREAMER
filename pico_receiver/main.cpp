#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/vreg.h"
#include "hardware/clocks.h"
#include "pico/cyw43_arch.h"
#include "lwip/pbuf.h"
#include "lwip/udp.h"
extern "C" {
#include "dvi.h"
#include "dvi_serialiser.h"
#include "common_dvi_pin_configs.h"
#include "tmds_encode.h"
#include "audio_ring.h"
}
#include "JPEGDEC.h"
#include "font8x8_basic.h"

// Audio Configuration
#define AUDIO_BUFFER_SIZE 2048
#define AUDIO_PORT 8081
audio_sample_t audio_buffer[AUDIO_BUFFER_SIZE];

// Wi-Fi Configuration
#define WIFI_SSID "YOUR_WIFI_SSID"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"
#define SERVER_PORT 8080

// DVI configuration for Olimex RP2040-PICO-PC
struct dvi_inst dvi0;

static const struct dvi_serialiser_cfg olimex_dvi_cfg = {
    .pio = pio0,
    .sm_tmds = {0, 1, 2},
    .pins_tmds = {14, 18, 16},
    .pins_clk = 12,
    .invert_diffpairs = true
};

#define FRAME_WIDTH 320
#define FRAME_HEIGHT 240
uint16_t frame_buffer_0[FRAME_WIDTH * FRAME_HEIGHT];
uint16_t frame_buffer_1[FRAME_WIDTH * FRAME_HEIGHT];
uint16_t* volatile display_buffer = frame_buffer_0;
uint16_t* volatile decode_buffer = frame_buffer_1;
volatile bool vblank_flag = false;

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

int __time_critical_func(JPEGDraw)(JPEGDRAW *pDraw) {
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

void core1_main() {
    dvi_register_irqs_this_core(&dvi0, DMA_IRQ_0);
    dvi_start(&dvi0);

    uint y = 0;
    while (1) {
        uint32_t *tmdsbuf;
        queue_remove_blocking_u32(&dvi0.q_tmds_free, &tmdsbuf);
        
        uint32_t *scanbuf = (uint32_t*)&display_buffer[y * FRAME_WIDTH];
        uint pixwidth = dvi0.timing->h_active_pixels;
        uint words_per_channel = pixwidth / DVI_SYMBOLS_PER_WORD;
        
        tmds_encode_data_channel_16bpp(scanbuf, tmdsbuf + 0 * words_per_channel, pixwidth / 2, DVI_16BPP_BLUE_MSB,  DVI_16BPP_BLUE_LSB );
        tmds_encode_data_channel_16bpp(scanbuf, tmdsbuf + 1 * words_per_channel, pixwidth / 2, DVI_16BPP_GREEN_MSB, DVI_16BPP_GREEN_LSB);
        tmds_encode_data_channel_16bpp(scanbuf, tmdsbuf + 2 * words_per_channel, pixwidth / 2, DVI_16BPP_RED_MSB,   DVI_16BPP_RED_LSB  );
        
        queue_add_blocking_u32(&dvi0.q_tmds_valid, &tmdsbuf);

        y++;
        if (y >= FRAME_HEIGHT) {
            y = 0;
            vblank_flag = true;
        }
    }
}

static void __time_critical_func(udp_recv_callback)(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port) {
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

static void __time_critical_func(udp_audio_recv_callback)(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port) {
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

int main() {
    vreg_set_voltage(VREG_VOLTAGE_1_25); // Increase voltage for stable 252MHz overclock
    sleep_ms(10);
    set_sys_clock_khz(252000, true); // 252.0 MHz is a clean PLL multiple close to the 25.175 MHz pixel clock required by DVI 640x480
    stdio_init_all();
    printf("Starting UDP Mode...\n");

    dvi0.timing = &dvi_timing_640x480p_60hz;
    dvi0.ser_cfg = olimex_dvi_cfg;
    dvi_init(&dvi0, next_striped_spin_lock_num(), next_striped_spin_lock_num());

    if (cyw43_arch_init()) return -1;
    cyw43_arch_enable_sta_mode();

    // HDMI Audio Setup
    dvi_get_blank_settings(&dvi0)->top    = 4 * 0;
    dvi_get_blank_settings(&dvi0)->bottom = 4 * 0;
    dvi_audio_sample_buffer_set(&dvi0, audio_buffer, AUDIO_BUFFER_SIZE);
    dvi_set_audio_freq(&dvi0, 44100, 28000, 6272);

    multicore_launch_core1(core1_main);

    draw_string(10, 10, "Pico Cast - Booting...", 0xFFFF);
    draw_string(10, 30, "Connecting to Wi-Fi...", 0xFFFF);

    if (cyw43_arch_wifi_connect_timeout_ms(WIFI_SSID, WIFI_PASSWORD, CYW43_AUTH_WPA2_AES_PSK, 15000)) {
        printf("Wi-Fi connection failed!\n");
        draw_string(10, 50, "ERROR: Wi-Fi Failed!", 0xF800);
        draw_string(10, 65, "Check if AP is 2.4GHz", 0xFFFF);
        while(1) tight_loop_contents();
    }
    
    const char *ip_str = ip4addr_ntoa(netif_ip4_addr(netif_list));
    
    for(int i=0; i<FRAME_WIDTH*FRAME_HEIGHT; i++) {
        frame_buffer_0[i] = 0;
        frame_buffer_1[i] = 0;
    }
    
    draw_string(10, 10, "Pico Cast - Wi-Fi Connected!", 0xFFFF);
    draw_string(10, 25, "IP Address:", 0xFFFF);
    draw_string(10, 35, ip_str, 0x07E0);
    draw_string(10, 55, "Waiting for UDP stream...", 0xFFFF);
    
    // Swap buffer to make text visible
    uint16_t* temp = display_buffer;
    display_buffer = decode_buffer;
    decode_buffer = temp;

    struct udp_pcb *audio_upcb = udp_new();
    udp_bind(audio_upcb, IP_ADDR_ANY, AUDIO_PORT);
    udp_recv(audio_upcb, udp_audio_recv_callback, NULL);

    struct udp_pcb *upcb = udp_new();
    udp_bind(upcb, IP_ADDR_ANY, SERVER_PORT);
    udp_recv(upcb, udp_recv_callback, NULL);

    uint32_t frame_count = 0;
    uint32_t current_fps = 0;
    uint32_t last_fps_time = time_us_32();

    while (true) {
        cyw43_arch_poll();
        
        if (decode_rx_idx != -1) {
            is_decoding = true;
            int target_idx = decode_rx_idx;
            uint32_t len = decode_frame_len;
            decode_rx_idx = -1; // Consume it immediately so network can write to it next time
            
            if (jpeg.openRAM(rx_buffer[target_idx], len, JPEGDraw)) {
                jpeg.setPixelType(RGB565_LITTLE_ENDIAN);
                
                jpeg.decode(0, 0, 0);
                jpeg.close();
                
                char fps_str[32];
                sprintf(fps_str, "FPS: %lu", current_fps);
                draw_string(FRAME_WIDTH - 64, 10, fps_str, 0xF800);
                
                // Swap buffers at VBLANK for Tear-Free output
                if (!vblank_flag) {
                    uint32_t wait_start = time_us_32();
                    while (!vblank_flag && (time_us_32() - wait_start < 2000)) tight_loop_contents();
                }
                vblank_flag = false;
                
                uint16_t* temp_buf = display_buffer;
                display_buffer = decode_buffer;
                decode_buffer = temp_buf;
                
                frame_count++;
            }
            is_decoding = false;
        }

        uint32_t now = time_us_32();
        if (now - last_fps_time >= 1000000) {
            current_fps = frame_count;
            frame_count = 0;
            last_fps_time = now;
        }

        cyw43_arch_poll();
    }
    return 0;
}
