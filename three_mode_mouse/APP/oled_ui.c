#include "oled_ui.h"
#include "OLED.h"
#include "mks142.h"
#include "adc.h"
#include "switch.h"
#include "usbd_mouse.h"
#include <stdio.h>

extern Mouse_Mode_t sys_current_mode;
extern uint16_t dpi_levels[];
extern uint8_t  dpi_index;

#define OLED_RENDER_CALLS      250u
#define OLED_RENDER_CALLS_BLE  20u
#define OLED_PAGE_FRAMES       24u

static uint8_t s_page = 0;
static uint8_t s_force = 1;
static uint16_t s_render_calls = 0;
static uint8_t s_page_frames = 0;

static uint8_t s_last_pct = 0xFF;
static uint8_t s_last_hr = 0xFF;
static uint8_t s_last_spo = 0xFF;
static uint8_t s_last_page = 0xFF;
static uint8_t s_last_stage = 0xFF;
static uint16_t s_last_dpi = 0xFFFF;
static Mouse_Mode_t s_last_mode = MODE_UNKNOWN;

static const char *mode_label(Mouse_Mode_t m)
{
    switch(m) {
        case MODE_WIRED: return "USB";
        case MODE_BLE:   return "BLE";
        case MODE_24G:   return "2.4G";
        default:         return "---";
    }
}

static uint16_t render_call_threshold(void)
{
    /*
     * Wired/2.4G call OLED_UI_Tick from the main polling loop, while BLE calls
     * it from the 12 ms HID event. Use call-count timing so page switching does
     * not depend on SYS_GetSysTickCnt(), which is not reliable in every mode.
     */
    return (sys_current_mode == MODE_BLE) ? OLED_RENDER_CALLS_BLE : OLED_RENDER_CALLS;
}

static void draw_batt_icon(int x, int y, uint8_t pct)
{
    if(pct > 100) pct = 100;
    OLED_DrawRect(x, y, 9, 5, 1);
    OLED_FillRect(x + 9, y + 1, 1, 3, 1);
    int inner_w = (7 * (int)pct) / 100;
    if(inner_w > 0) OLED_FillRect(x + 1, y + 1, inner_w, 3, 1);
}

static void draw_page_dots(uint8_t page)
{
    int x0 = 29;
    for(int i = 0; i < 2; i++) {
        int x = x0 + i * 5;
        if((uint8_t)i == page) OLED_FillRect(x, 2, 3, 3, 1);
        else                   OLED_DrawRect(x, 2, 3, 3, 1);
    }
}

static void draw_header(uint8_t pct, Mouse_Mode_t mode, uint8_t page)
{
    OLED_DrawString6x8(0, 0, mode_label(mode));
    draw_page_dots(page);
    draw_batt_icon(53, 1, pct);
    OLED_DrawHLine(0, 8, OLED_WIDTH, 1);
}

static const uint8_t DIGIT_5X7[10][5] = {
    {0x3E,0x51,0x49,0x45,0x3E},
    {0x00,0x42,0x7F,0x40,0x00},
    {0x42,0x61,0x51,0x49,0x46},
    {0x21,0x41,0x45,0x4B,0x31},
    {0x18,0x14,0x12,0x7F,0x10},
    {0x27,0x45,0x45,0x45,0x39},
    {0x3C,0x4A,0x49,0x49,0x30},
    {0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36},
    {0x06,0x49,0x49,0x29,0x1E},
};

static int scaled_char_width(char ch, uint8_t scale)
{
    (void)ch;
    return 5 * (int)scale;
}

static int scaled_text_width(const char *s, uint8_t scale)
{
    int w = 0;
    while(*s) {
        w += scaled_char_width(*s, scale);
        s++;
        if(*s) w += scale;
    }
    return w;
}

static void draw_scaled_digit(int x, int y, uint8_t d, uint8_t scale)
{
    if(d > 9) return;
    for(int col = 0; col < 5; col++) {
        uint8_t bits = DIGIT_5X7[d][col];
        for(int row = 0; row < 7; row++) {
            if(bits & (1u << row)) {
                OLED_FillRect(x + col * scale, y + row * scale, scale, scale, 1);
            }
        }
    }
}

static void draw_scaled_dash(int x, int y, uint8_t scale)
{
    OLED_FillRect(x, y + 3 * scale, 5 * scale, scale, 1);
}

static void draw_scaled_text(int x, int y, const char *s, uint8_t scale)
{
    while(*s) {
        if(*s >= '0' && *s <= '9') {
            draw_scaled_digit(x, y, (uint8_t)(*s - '0'), scale);
        } else if(*s == '-') {
            draw_scaled_dash(x, y, scale);
        }
        x += scaled_char_width(*s, scale);
        s++;
        if(*s) x += scale;
    }
}

static void draw_scaled_text_center(int y, const char *s, uint8_t scale)
{
    int w = scaled_text_width(s, scale);
    int x = (OLED_WIDTH - w) / 2;
    if(x < 0) x = 0;
    draw_scaled_text(x, y, s, scale);
}

static void draw_scaled_text_center_region(int x0, int x1, int y, const char *s, uint8_t scale)
{
    int w = scaled_text_width(s, scale);
    int x = x0 + (x1 - x0 - w) / 2;
    if(x < x0) x = x0;
    draw_scaled_text(x, y, s, scale);
}

static void draw_stage_bar(uint8_t stage)
{
    int x = 0;
    for(uint8_t i = 0; i < DPI_STAGE_NUM; i++) {
        if(i == stage) OLED_FillRect(x, 34, 13, 4, 1);
        else           OLED_DrawRect(x, 34, 13, 4, 1);
        x += 16;
    }
}

static void draw_dpi_page(uint8_t pct, Mouse_Mode_t mode, uint8_t stage, uint16_t cur_dpi)
{
    char buf[12];

    draw_header(pct, mode, 0);
    OLED_DrawString6x8(0, 10, "DPI");

    if(cur_dpi > 9999) cur_dpi = 9999;
    snprintf(buf, sizeof(buf), "%u", cur_dpi);
    draw_scaled_text_center(17, buf, 2);

    draw_stage_bar(stage);

    snprintf(buf, sizeof(buf), "BAT %u%%", pct);
    OLED_DrawString6x8(0, 40, buf);
}

static void draw_bio_page(uint8_t hr, uint8_t spo)
{
    char buf[16];
    uint8_t hr_ok = (hr > 0 && hr < 250) ? 1u : 0u;
    uint8_t spo_ok = (spo > 0 && spo <= 100) ? 1u : 0u;

    OLED_DrawString6x8(23, 0, "BIO");
    OLED_DrawHLine(0, 8, OLED_WIDTH, 1);

    OLED_DrawString6x8(0, 13, "HR");
    if(hr_ok) snprintf(buf, sizeof(buf), "%u", hr);
    else      snprintf(buf, sizeof(buf), "--");
    draw_scaled_text_center_region(18, 64, 10, buf, 2);

    OLED_DrawHLine(0, 25, OLED_WIDTH, 1);

    OLED_DrawString6x8(0, 33, "O2");
    if(spo_ok) snprintf(buf, sizeof(buf), "%u", spo);
    else       snprintf(buf, sizeof(buf), "--");
    draw_scaled_text_center_region(18, spo_ok ? 58 : 64, 28, buf, 2);

    if(spo_ok) OLED_DrawString6x8(58, 34, "%");
}

void OLED_UI_Init(void)
{
    OLED_Init();
    s_page = 0;
    s_force = 1;
    s_render_calls = 0;
    s_page_frames = 0;
    s_last_page = 0xFF;
    s_last_pct = 0xFF;
    s_last_hr = 0xFF;
    s_last_spo = 0xFF;
    s_last_stage = 0xFF;
    s_last_dpi = 0xFFFF;
    s_last_mode = MODE_UNKNOWN;
}

void OLED_UI_ForceRedraw(void)
{
    s_page = 0;
    s_page_frames = 0;
    s_force = 1;
    s_render_calls = 0;
}

void OLED_UI_Tick(void)
{
    if(OLED_HasDirty()) {
        OLED_RefreshDirtyStep(1);
        return;
    }

    if(!s_force && ++s_render_calls < render_call_threshold()) {
        return;
    }
    s_render_calls = 0;

    if(!s_force && ++s_page_frames >= OLED_PAGE_FRAMES) {
        s_page_frames = 0;
        s_page ^= 1u;
        s_force = 1;
    }

    uint8_t pct = Battery_GetCachedPercent();
    uint8_t hr = MKS142_GetHR();
    uint8_t spo = MKS142_GetSpO2();
    Mouse_Mode_t mode = sys_current_mode;
    uint8_t stage = (dpi_index < DPI_STAGE_NUM) ? dpi_index : 0;
    uint16_t cur_dpi = dpi_levels[stage];

    uint8_t need_redraw = s_force;
    if(pct != s_last_pct || mode != s_last_mode || s_page != s_last_page) need_redraw = 1;
    if(stage != s_last_stage || cur_dpi != s_last_dpi) need_redraw = 1;
    if(s_page == 1 && (hr != s_last_hr || spo != s_last_spo)) need_redraw = 1;

    if(!need_redraw) return;

    OLED_FB_Clear();
    if(s_page == 0) {
        draw_dpi_page(pct, mode, stage, cur_dpi);
    } else {
        draw_bio_page(hr, spo);
    }

    s_last_pct = pct;
    s_last_hr = hr;
    s_last_spo = spo;
    s_last_mode = mode;
    s_last_page = s_page;
    s_last_stage = stage;
    s_last_dpi = cur_dpi;
    s_force = 0;
}
