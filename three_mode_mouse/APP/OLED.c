#include "OLED.h"
#include "OLED_Font.h"
#include <string.h>

/* ============================================================
 *  帧缓冲：64 列 × 6 页 = 384 字节
 *  脏页位掩码：每个 page 一个 bit；只有改动过的 page 才被刷新
 * ============================================================ */
static uint8_t s_fb[OLED_PAGES][OLED_WIDTH];
#define OLED_DIRTY_ALL   ((uint8_t)((1u << OLED_PAGES) - 1u))

static uint8_t s_dirty = OLED_DIRTY_ALL;     /* 上电时认为全部脏，强制全刷一次 */

#define OLED_SPI_TIMEOUT  60000u

static inline void mark_dirty_page(int page)
{
    if(page >= 0 && page < OLED_PAGES) s_dirty |= (uint8_t)(1u << page);
}

static void oled_spi_send_byte(uint8_t d)
{
    uint32_t guard = OLED_SPI_TIMEOUT;

    R8_SPI1_CTRL_MOD &= (uint8_t)~RB_SPI_FIFO_DIR;
    R16_SPI1_TOTAL_CNT = 1;
    R8_SPI1_FIFO = d;
    while(!(R8_SPI1_INT_FLAG & RB_SPI_FREE) && --guard);
}

static void oled_spi_send_block(const uint8_t *buf, uint16_t len)
{
    uint16_t sendlen = len;
    uint32_t guard = OLED_SPI_TIMEOUT * 8u;

    R8_SPI1_CTRL_MOD &= (uint8_t)~RB_SPI_FIFO_DIR;
    R16_SPI1_TOTAL_CNT = sendlen;
    R8_SPI1_INT_FLAG = RB_SPI_IF_CNT_END;

    while(sendlen && --guard) {
        if(R8_SPI1_FIFO_COUNT < SPI_FIFO_SIZE) {
            R8_SPI1_FIFO = *buf++;
            sendlen--;
        }
    }

    guard = OLED_SPI_TIMEOUT;
    while(R8_SPI1_FIFO_COUNT != 0 && --guard);
}

/* ============================================================
 *  低层访问：写命令 / 写数据
 * ============================================================ */
static void oled_cmd(uint8_t c)
{
    OLED_DC_Clr();
    oled_spi_send_byte(c);
}

static void oled_data_block(const uint8_t *buf, uint16_t len)
{
    OLED_DC_Set();
    oled_spi_send_block(buf, len);
}

/* ============================================================
 *  初始化
 * ============================================================ */
static void oled_set_window(uint8_t page)
{
    /* 列起始地址（含 0.66 屏的 X 偏移） */
    uint8_t col = OLED_X_OFFSET;
    oled_cmd(0xB0 + page);               /* page address */
    oled_cmd(0x10 | ((col >> 4) & 0x0F)); /* high col */
    oled_cmd(0x00 | (col & 0x0F));        /* low col */
}

void OLED_SetContrast(uint8_t val)
{
    oled_cmd(0x81);
    oled_cmd(val);
}

void OLED_Init(void)
{
    /* GPIO + SPI */
    GPIOA_ModeCfg(GPIO_Pin_0 | GPIO_Pin_1 | GPIO_Pin_2 | GPIO_Pin_3, GPIO_ModeOut_PP_5mA);
    SPI1_MasterDefInit();

    /* 硬件复位 */
    OLED_RES_Set(); mDelaymS(10);
    OLED_RES_Clr(); mDelaymS(20);
    OLED_RES_Set(); mDelaymS(20);

    /* SSD1306 初始化（适配 0.66" 64x48 居中可视区）*/
    oled_cmd(0xAE);             /* display off */
    oled_cmd(0xD5); oled_cmd(0x80);   /* set display clock */
    oled_cmd(0xA8); oled_cmd(0x2F);   /* multiplex ratio = 47 (48 行) */
    oled_cmd(0xD3); oled_cmd(0x00);   /* display offset 0 */
    oled_cmd(0x40);                   /* start line 0 */
    oled_cmd(0x8D); oled_cmd(0x14);   /* charge pump on */
    oled_cmd(0x20); oled_cmd(0x02);   /* page addressing mode */
    oled_cmd(0xA1);                   /* segment remap */
    oled_cmd(0xC8);                   /* COM scan dir */
    oled_cmd(0xDA); oled_cmd(0x12);   /* com pins */
    oled_cmd(0x81); oled_cmd(0xCF);   /* contrast */
    oled_cmd(0xD9); oled_cmd(0xF1);   /* pre-charge */
    oled_cmd(0xDB); oled_cmd(0x40);   /* vcom detect */
    oled_cmd(0xA4);                   /* normal display from RAM */
    oled_cmd(0xA6);                   /* not inverted */
    oled_cmd(0xAF);                   /* display on */

    /* 清屏 + 全刷一次 */
    OLED_FB_Clear();
    s_dirty = OLED_DIRTY_ALL;
    OLED_Refresh();
}

/* ============================================================
 *  帧缓冲操作
 * ============================================================ */
void OLED_FB_Clear(void)
{
    memset(s_fb, 0, sizeof(s_fb));
    s_dirty = OLED_DIRTY_ALL;
}

void OLED_DrawPixel(int x, int y, uint8_t on)
{
    if(x < 0 || x >= OLED_WIDTH || y < 0 || y >= OLED_HEIGHT) return;
    int page = y >> 3;
    uint8_t bit = (uint8_t)(1u << (y & 7));
    if(on) s_fb[page][x] |=  bit;
    else   s_fb[page][x] &= (uint8_t)~bit;
    mark_dirty_page(page);
}

void OLED_DrawHLine(int x, int y, int w, uint8_t on)
{
    for(int i = 0; i < w; i++) OLED_DrawPixel(x + i, y, on);
}

void OLED_DrawVLine(int x, int y, int h, uint8_t on)
{
    for(int i = 0; i < h; i++) OLED_DrawPixel(x, y + i, on);
}

void OLED_DrawRect(int x, int y, int w, int h, uint8_t on)
{
    if(w <= 0 || h <= 0) return;
    OLED_DrawHLine(x, y,           w, on);
    OLED_DrawHLine(x, y + h - 1,   w, on);
    OLED_DrawVLine(x,         y,   h, on);
    OLED_DrawVLine(x + w - 1, y,   h, on);
}

void OLED_FillRect(int x, int y, int w, int h, uint8_t on)
{
    for(int j = 0; j < h; j++)
        for(int i = 0; i < w; i++)
            OLED_DrawPixel(x + i, y + j, on);
}

/* ============================================================
 *  文字
 * ============================================================ */
void OLED_DrawChar6x8(int x, int y, char ch)
{
    if(ch < ' ' || ch > '~') ch = ' ';
    int idx = ch - ' ';
    for(int i = 0; i < 6; i++) {
        uint8_t col = OLED_F6x8[idx][i];
        for(int b = 0; b < 8; b++) {
            if(col & (1 << b)) OLED_DrawPixel(x + i, y + b, 1);
        }
    }
}

void OLED_DrawString6x8(int x, int y, const char *s)
{
    while(*s) {
        OLED_DrawChar6x8(x, y, *s);
        x += 6;
        if(x + 6 > OLED_WIDTH) { x = 0; y += 8; if(y >= OLED_HEIGHT) return; }
        s++;
    }
}

/* ============================================================
 *  12x16 大字体（仅数字 0-9 / "." / ":" / "%" / 空格）
 *  用于显示心率 / 血氧的大数字。
 *  存储格式：每个字符 12 列 × 16 行；列优先，每列 16 位（uint16_t）
 *            低字节 = 上半 page (y 0..7)，高字节 = 下半 page (y 8..15)
 *            位 0 = 顶端像素
 * ============================================================ */
static const uint16_t FONT_12x16[] = {
    /* '0' */
    0x07E0,0x0FF0,0x1818,0x300C,0x300C,0x600C,0x6006,0x6006,
    0x300C,0x300C,0x1818,0x0FF0,
    /* '1' */
    0x0000,0x0000,0x0030,0x0018,0x000C,0x7FFE,0x7FFE,0x0000,
    0x0000,0x0000,0x0000,0x0000,
    /* '2' */
    0x0008,0x300C,0x600C,0x6006,0x6006,0x6006,0x600E,0x301C,
    0x3838,0x1C70,0x0FE0,0x07C0,
    /* '3' */
    0x0008,0x180C,0x300C,0x6006,0x6306,0x6306,0x6306,0x6306,
    0x7706,0x7F8E,0x39FC,0x10F8,
    /* '4' */
    0x00C0,0x01C0,0x03C0,0x06C0,0x0CC0,0x18C0,0x30C0,0x60C0,
    0x7FFE,0x7FFE,0x00C0,0x00C0,
    /* '5' */
    0x000E,0x7E0E,0x7E0E,0x6306,0x6306,0x6306,0x6306,0x6306,
    0x6306,0x7386,0x7186,0x60FE,
    /* '6' */
    0x07E0,0x1FF8,0x383C,0x301E,0x6307,0x6307,0x6307,0x6307,
    0x6307,0x7386,0x3386,0x0100,
    /* '7' */
    0x0006,0x0006,0x0006,0x0006,0x7806,0x7E06,0x0786,0x00E6,
    0x0036,0x000E,0x0006,0x0006,
    /* '8' */
    0x1CF8,0x3FFC,0x738E,0x6306,0x6306,0x6306,0x6306,0x6306,
    0x6306,0x738E,0x3FFC,0x1CF8,
    /* '9' */
    0x00F0,0x61F8,0x631C,0x630E,0x6306,0x6306,0x6306,0x6306,
    0x630E,0x631C,0x73F8,0x3FF0,
    /* '.' */
    0x0000,0x0000,0x0000,0x0000,0x6000,0xF000,0xF000,0x6000,
    0x0000,0x0000,0x0000,0x0000,
    /* ':' */
    0x0000,0x0000,0x0000,0x0606,0x0F0F,0x0F0F,0x0606,0x0000,
    0x0000,0x0000,0x0000,0x0000,
    /* '%' */
    0x000E,0x381F,0x4431,0x4421,0x4431,0x381F,0x06E0,0x01F0,
    0x7C18,0x4408,0x6418,0x381E,
    /* ' ' */
    0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    0x0000,0x0000,0x0000,0x0000,
};

static int big_font_index(char ch)
{
    if(ch >= '0' && ch <= '9') return ch - '0';
    if(ch == '.') return 10;
    if(ch == ':') return 11;
    if(ch == '%') return 12;
    return 13;     /* 空白 */
}

void OLED_DrawChar12x16(int x, int y, char ch)
{
    int idx = big_font_index(ch);
    const uint16_t *gly = &FONT_12x16[idx * 12];
    for(int i = 0; i < 12; i++) {
        uint16_t col = gly[i];
        for(int b = 0; b < 16; b++) {
            if(col & (1u << b)) OLED_DrawPixel(x + i, y + b, 1);
        }
    }
}

void OLED_DrawString12x16(int x, int y, const char *s)
{
    while(*s) {
        OLED_DrawChar12x16(x, y, *s);
        x += 12;
        if(x + 12 > OLED_WIDTH) return;
        s++;
    }
}

/* ============================================================
 *  进度条
 * ============================================================ */
void OLED_DrawProgress(int x, int y, int w, int h, uint8_t pct)
{
    if(pct > 100) pct = 100;
    OLED_DrawRect(x, y, w, h, 1);
    int inner_w = (w - 4) * (int)pct / 100;
    if(inner_w > 0) OLED_FillRect(x + 2, y + 2, inner_w, h - 4, 1);
}

/* ============================================================
 *  波形
 * ============================================================ */
void OLED_DrawWave(int x, int y, int w, int h,
                   const int16_t *buf, int n,
                   int16_t v_min, int16_t v_max)
{
    if(w <= 1 || h <= 1 || n < 2 || !buf) return;
    if(v_max <= v_min) v_max = v_min + 1;

    int prev_x = x;
    int prev_y = y + h - 1;
    int first  = 1;
    for(int i = 0; i < n; i++) {
        int xi = x + (i * (w - 1)) / (n - 1);
        int v  = buf[i];
        if(v < v_min) v = v_min;
        if(v > v_max) v = v_max;
        int yi = y + h - 1 - ((v - v_min) * (h - 1) / (v_max - v_min));
        if(first) {
            prev_x = xi; prev_y = yi; first = 0; continue;
        }
        /* 简易 Bresenham */
        int dx = xi - prev_x;
        int dy = yi - prev_y;
        int adx = dx < 0 ? -dx : dx;
        int ady = dy < 0 ? -dy : dy;
        int sx = dx < 0 ? -1 : 1;
        int sy = dy < 0 ? -1 : 1;
        int err = (adx > ady ? adx : -ady) >> 1;
        int cx = prev_x, cy = prev_y;
        for(;;) {
            OLED_DrawPixel(cx, cy, 1);
            if(cx == xi && cy == yi) break;
            int e2 = err;
            if(e2 > -adx) { err -= ady; cx += sx; }
            if(e2 <  ady) { err += adx; cy += sy; }
        }
        prev_x = xi; prev_y = yi;
    }
}

/* ============================================================
 *  整屏 / 脏页推送
 * ============================================================ */
void OLED_Refresh(void)
{
    for(int p = 0; p < OLED_PAGES; p++) {
        oled_set_window(p);
        oled_data_block(s_fb[p], OLED_WIDTH);
    }
    s_dirty = 0;
}

void OLED_RefreshDirty(void)
{
    if(!s_dirty) return;
    for(int p = 0; p < OLED_PAGES; p++) {
        if(s_dirty & (1u << p)) {
            oled_set_window(p);
            oled_data_block(s_fb[p], OLED_WIDTH);
        }
    }
    s_dirty = 0;
}

uint8_t OLED_HasDirty(void)
{
    return (s_dirty & OLED_DIRTY_ALL) ? 1u : 0u;
}

void OLED_RefreshDirtyStep(uint8_t max_pages)
{
    s_dirty &= OLED_DIRTY_ALL;
    if(!s_dirty) return;
    if(max_pages == 0) max_pages = 1;

    uint8_t sent = 0;
    for(int p = 0; p < OLED_PAGES && sent < max_pages; p++) {
        uint8_t bit = (uint8_t)(1u << p);
        if(s_dirty & bit) {
            oled_set_window((uint8_t)p);
            oled_data_block(s_fb[p], OLED_WIDTH);
            s_dirty &= (uint8_t)~bit;
            sent++;
        }
    }
}
