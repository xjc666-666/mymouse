#include "mks142.h"
#include "CH58x_common.h"
#include <string.h>

#define MKS142_STALE_CLEAR_TICKS   180000000u

/* ===== 引脚 =====
 * RST 引脚（原 PB16）已弃用——硬件上未连接，仅靠 UART 通信。
 * 复位改为软件方式：发送 0x88(停止) -> 等待 -> 0x8A(启动)。
 */

/* ===== 接收环形缓冲（ISR 写、主循环读）===== */
static volatile uint8_t  s_rx_ring[MKS142_RX_RING_SIZE];
static volatile uint16_t s_rx_head = 0;   /* ISR 写指针 */
static volatile uint16_t s_rx_tail = 0;   /* 主循环读指针 */

/* ===== 帧组装缓冲 =====
 *  一旦发现 0xFF 候选头，就把后续 (MKS142_PKT_SIZE-1) 字节收齐做一次性校验。
 */
static uint8_t  s_pkt[MKS142_PKT_SIZE];
static uint16_t s_pkt_fill = 0;       /* 已积累的字节数，含头 0xFF */

/* ===== ECG 历史环（一帧带来 64 个采样，主循环若刷新慢需要批量取走） ===== */
static volatile int8_t   s_ecg_ring[MKS142_ECG_RING_SIZE];
static volatile uint32_t s_ecg_write_seq = 0;     /* 自增写序号；写指针 = seq & (SIZE-1) */

/* ===== 最新数据快照 ===== */
static volatile MKS142_Data_t s_data_latest = {0};

/* ===== 启动看门狗：若长期收不到一帧，就重发 0x8A ===== */
static uint32_t s_last_frame_tick = 0;
static uint32_t s_start_retry_tick = 0;

/* ===== 调试计数：用于上位机判断 UART 链路是否真正有数据进来 =====
 *  - s_rx_byte_total : ISR 累计收到的 UART 字节数（不论是否被丢弃）
 *  - s_rx_overflow   : 因 ring 满被丢弃的字节数
 *  - s_frame_drop    : 校验失败的候选帧数
 *  - s_process_calls : MKS142_Process 累计调用次数（无条件递增，
 *                      用于排查"固件根本没在跑"这种最坏情况）
 *  - s_isr_calls     : UART3_IRQHandler 累计进入次数（如果 ISR 一次都没跑，
 *                      但 loop 在涨，说明 PFIC/中断使能那一侧被关掉了）
 *  - s_lsr_snapshot  : 主循环里采样的 LSR（线路状态）
 */
static volatile uint32_t s_rx_byte_total = 0;
static volatile uint32_t s_rx_overflow   = 0;
static volatile uint32_t s_frame_drop    = 0;
static volatile uint32_t s_process_calls = 0;
static volatile uint32_t s_isr_calls     = 0;
static volatile uint8_t  s_lsr_snapshot  = 0;

/* ============================================================
 *  环形缓冲辅助
 * ============================================================ */
static inline uint16_t ring_count(void)
{
    return (uint16_t)((s_rx_head - s_rx_tail) & (MKS142_RX_RING_SIZE - 1));
}

static inline uint8_t ring_pop(void)
{
    uint8_t b = s_rx_ring[s_rx_tail];
    s_rx_tail = (s_rx_tail + 1) & (MKS142_RX_RING_SIZE - 1);
    return b;
}

static inline void ring_push_byte(uint8_t b)
{
    uint16_t next = (s_rx_head + 1) & (MKS142_RX_RING_SIZE - 1);
    if(next != s_rx_tail) {
        s_rx_ring[s_rx_head] = b;
        s_rx_head = next;
    } else {
        s_rx_overflow++;
    }
}

static void poll_uart_fifo(void)
{
    PFIC_DisableIRQ(UART3_IRQn);
    while(R8_UART3_RFC) {
        uint8_t b = UART3_RecvByte();
        s_rx_byte_total++;
        ring_push_byte(b);
    }
    PFIC_EnableIRQ(UART3_IRQn);
}

/* ============================================================
 *  软件复位 —— 之前通过 PB16 拉低做硬复位，硬件未连接 RST 后改为
 *  发送 0x88 停止 -> 等待模块进入待命 -> 由调用方再发 0x8A 启动。
 *  Init() 调用 Reset() 时与上电几乎同步，所以先给模块 ~120ms 启动余量，
 *  再发送 0x88，再次给 ~80ms 让其稳定进入待命。
 * ============================================================ */
void MKS142_Reset(void)
{
    /* 等模块自身上电完成（手册：上电 100ms 内进入待命） */
    mDelaymS(120);
    UART3_SendString((uint8_t*)"\x88", 1);
    mDelaymS(80);
}

/* ============================================================
 *  发送单字节命令（0x8A / 0x88 / 0x98）
 * ============================================================ */
static void mks_send_cmd(uint8_t c)
{
    UART3_SendString(&c, 1);
}

void MKS142_StartStream(void) { mks_send_cmd(MKS142_CMD_START); }
void MKS142_StopStream(void)  { mks_send_cmd(MKS142_CMD_STOP); }

/* ============================================================
 *  初始化：UART3 重映射到 PB20/PB21（RST 引脚不再使用）
 * ============================================================ */
void MKS142_Init(void)
{
    /* UART3 重映射到 PB20(RXD3) / PB21(TXD3) */
    GPIOPinRemap(ENABLE, RB_PIN_UART3);
    GPIOB_SetBits(GPIO_Pin_21);                                /* TXD 默认高 */
    GPIOB_ModeCfg(GPIO_Pin_20, GPIO_ModeIN_PU);                /* RXD 输入上拉 */
    GPIOB_ModeCfg(GPIO_Pin_21, GPIO_ModeOut_PP_5mA);           /* TXD 推挽输出 */

    UART3_DefInit();
    UART3_BaudRateCfg(MKS142_BAUDRATE);                         /* 38400 8N1 */
    UART3_ByteTrigCfg(UART_1BYTE_TRIG);
    UART3_INTCfg(ENABLE, RB_IER_RECV_RDY | RB_IER_LINE_STAT);
    PFIC_EnableIRQ(UART3_IRQn);

    /* 软件复位模块并启动测量（不再依赖 RST 引脚） */
    MKS142_Reset();
    MKS142_StartStream();

    /* 内部状态清零 */
    s_pkt_fill = 0;
    s_rx_head = s_rx_tail = 0;
    s_ecg_write_seq = 0;
    memset((void*)&s_data_latest, 0, sizeof(s_data_latest));
    memset((void*)s_ecg_ring, 0, sizeof(s_ecg_ring));

    s_last_frame_tick  = SYS_GetSysTickCnt();
    s_start_retry_tick = s_last_frame_tick;
}

/* ============================================================
 *  UART3 接收中断
 * ============================================================ */
__INTERRUPT
__HIGH_CODE
void UART3_IRQHandler(void)
{
    volatile uint8_t b;
    s_isr_calls++;
    switch(UART3_GetITFlag())
    {
        case UART_II_LINE_STAT:
            UART3_GetLinSTA();
            break;

        case UART_II_RECV_RDY:
        case UART_II_RECV_TOUT:
            while(R8_UART3_RFC) {
                b = UART3_RecvByte();
                s_rx_byte_total++;
                ring_push_byte(b);
            }
            break;

        default: break;
    }
}

/* ============================================================
 *  RT_PACK 合法性检查 —— 帧无 CRC，靠字段量程粗校验
 * ============================================================ */
static int pkt_looks_sane(const uint8_t *p)
{
    if(p[0] != MKS142_PKT_HEADER) return 0;
    uint8_t hr  = p[65];
    int8_t  sp  = (int8_t)p[66];
    /* HR: 0=无效，1..220 是合理范围 */
    if(hr > 220) return 0;
    /* SpO2: 手册写 int8，正常 70..100；模块用负值（-128）表示无效 */
    if(sp > 100) return 0;
    /* state 高位预留位最好为 0；但手册未严格定义，这里不强求 */
    return 1;
}

static void ecg_push(int8_t v)
{
    s_ecg_ring[s_ecg_write_seq & (MKS142_ECG_RING_SIZE - 1)] = v;
    s_ecg_write_seq++;
}

static void clear_measurement(uint8_t status)
{
    s_data_latest.ecg = 0;
    s_data_latest.ecg_updated = 0;
    s_data_latest.heart_rate = 0;
    s_data_latest.spo2 = 0;
    s_data_latest.status = status;
}

/* 把一整包 RT_PACK 应用到全局快照 */
static void apply_packet(const uint8_t *p)
{
    uint8_t st = p[87];
    uint8_t bk = p[67];

    if(st & MKS142_STATUS_FINGER_OFF) {
        clear_measurement(st);
        s_data_latest.bk = bk;
        s_data_latest.frames_total++;
        s_last_frame_tick = SYS_GetSysTickCnt();
        return;
    }

    /* 64 个 ECG 采样按顺序入环 */
    for(int i = 0; i < 64; i++) {
        ecg_push((int8_t)p[1 + i]);
    }

    /* 把最近 1 个采样投到 latest.ecg，便于旧 PeekEcg 接口工作 */
    s_data_latest.ecg         = (int16_t)(int8_t)p[64];
    s_data_latest.ecg_updated = 1;
    s_data_latest.ecg_seq     = s_ecg_write_seq;     /* 与 ring 序号对齐 */

    s_data_latest.heart_rate  = p[65];
    int8_t sp = (int8_t)p[66];
    s_data_latest.spo2        = (sp > 0 && sp <= 100) ? (uint8_t)sp : 0;
    s_data_latest.bk          = bk;
    s_data_latest.status      = st;

    s_data_latest.frames_total++;
    s_last_frame_tick = SYS_GetSysTickCnt();
}

/* ============================================================
 *  主循环帧解析
 *
 *  RT_PACK 无 CRC，无尾字节，无长度域。只能以 0xFF 为帧头并依靠
 *  字段量程做兜底校验。组装策略：
 *    - 寻找 0xFF 作为候选头
 *    - 凑齐 88 字节后调用 pkt_looks_sane() 校验
 *    - 校验失败则把 s_pkt 整体左移 1 字节（即把当前候选头丢弃，
 *      用 s_pkt[1] 作为新的候选头），继续吃后续字节直到再次凑满
 * ============================================================ */
void MKS142_Process(void)
{
    s_process_calls++;       /* 进入即 +1，证明该函数确实在跑 */
    s_lsr_snapshot = R8_UART3_LSR;   /* 采样线路状态（RX 数据可用、错误等）*/

    poll_uart_fifo();

    while(ring_count())
    {
        uint8_t b = ring_pop();

        if(s_pkt_fill == 0) {
            /* 等待帧头 */
            if(b == MKS142_PKT_HEADER) {
                s_pkt[0] = b;
                s_pkt_fill = 1;
            }
            continue;
        }

        s_pkt[s_pkt_fill++] = b;

        if(s_pkt_fill < MKS142_PKT_SIZE) continue;

        /* 已凑齐 88 字节 */
        if(pkt_looks_sane(s_pkt)) {
            apply_packet(s_pkt);
            s_pkt_fill = 0;
        } else {
            /* 重同步：丢弃当前帧头，左移寻找下一个 0xFF */
            s_frame_drop++;
            int new_fill = 0;
            for(int i = 1; i < MKS142_PKT_SIZE; i++) {
                if(s_pkt[i] == MKS142_PKT_HEADER) {
                    /* 把 [i..end] 拷到 [0..] 作为新候选 */
                    new_fill = MKS142_PKT_SIZE - i;
                    memmove(s_pkt, s_pkt + i, new_fill);
                    break;
                }
            }
            s_pkt_fill = (uint16_t)new_fill;
        }
    }

    /* 启动看门狗：超过 1.5 秒没收到一帧合法 RT_PACK，就重发 0x8A，
     * 兜底以下两类问题：
     *   1) 上电瞬间发出的 0x8A 被模块丢掉（模块还没准备好接收）。
     *   2) 模块异常掉电再上电，但 MCU 不知道而没重发启动命令。
     * 重试间隔本身限到 ~0.5 秒一次，避免 UART TX 上过度刷屏。
     */
    uint32_t now = SYS_GetSysTickCnt();
    /* 60 MHz 系统时钟 → 90M ticks ≈ 1.5s, 30M ticks ≈ 0.5s */
    if((uint32_t)(now - s_last_frame_tick)  > 90000000u &&
       (uint32_t)(now - s_start_retry_tick) > 30000000u)
    {
        MKS142_StartStream();
        s_start_retry_tick = now;
    }

    if((uint32_t)(now - s_last_frame_tick) > MKS142_STALE_CLEAR_TICKS) {
        clear_measurement(MKS142_STATUS_FINGER_OFF);
    }
}

/* ============================================================
 *  访问接口
 * ============================================================ */
void MKS142_GetData(MKS142_Data_t *out)
{
    if(!out) return;
    *out = (MKS142_Data_t)s_data_latest;
}

int16_t MKS142_GetEcg(void)
{
    int16_t v = s_data_latest.ecg;
    s_data_latest.ecg_updated = 0;
    return v;
}

int16_t  MKS142_PeekEcg(void)    { return s_data_latest.ecg; }
uint32_t MKS142_GetEcgSeq(void)  { return s_data_latest.ecg_seq; }
uint8_t  MKS142_GetHR(void)      { return s_data_latest.heart_rate; }
uint8_t  MKS142_GetSpO2(void)    { return s_data_latest.spo2; }
uint8_t  MKS142_GetStatus(void)  { return s_data_latest.status; }
uint8_t  MKS142_HasNewEcg(void)  { return s_data_latest.ecg_updated; }

/* === 诊断计数（用于上位机判断 UART 是否真的有数据进来）=== */
uint32_t MKS142_GetRxBytes(void)      { return s_rx_byte_total; }
uint32_t MKS142_GetRxOverflow(void)   { return s_rx_overflow; }
uint32_t MKS142_GetFrameTotal(void)   { return s_data_latest.frames_total; }
uint32_t MKS142_GetFrameDrop(void)    { return s_isr_calls; }       /* 临时复用：ISR 调用次数 */
uint32_t MKS142_GetStartRetry(void)   { return s_process_calls; }   /* 临时复用：Process 调用次数 */
uint8_t  MKS142_GetLSR(void)          { return s_lsr_snapshot; }

/* 直接采样关键硬件寄存器，用于排查重映射是否真的生效 */
uint16_t MKS142_GetPinAlternate(void) { return R16_PIN_ALTERNATE; }
/* PB20/PB21 的当前电平（bit20=PB20, bit21=PB21）—— 注意若 UART3 已重映射
 * 到 PB20/PB21，硬件会接管这两个引脚，R32_PB_PIN 此时反映的就是真实的
 * 信号线电平。可以借此判断模块 UTX 在物理上有没有送出信号。
 */
uint8_t  MKS142_GetPb20Pb21(void)
{
    uint32_t v = R32_PB_PIN;
    uint8_t r = 0;
    if(v & (1u << 20)) r |= 0x01;
    if(v & (1u << 21)) r |= 0x02;
    return r;
}

int MKS142_PullEcgSince(uint32_t *cursor, int8_t *out, int max)
{
    if(!cursor || !out || max <= 0) return 0;

    uint32_t w = s_ecg_write_seq;          /* 写者位置 */
    uint32_t c = *cursor;

    /* 首次调用 (c == 0) 时，跳到当前最新一段，不补历史 */
    if(c == 0 || (uint32_t)(w - c) > MKS142_ECG_RING_SIZE) {
        c = (w > MKS142_ECG_RING_SIZE) ? (w - MKS142_ECG_RING_SIZE) : 0;
    }

    int n = (int)(w - c);
    if(n > max) {
        c = w - (uint32_t)max;
        n = max;
    }

    for(int i = 0; i < n; i++) {
        out[i] = s_ecg_ring[(c + i) & (MKS142_ECG_RING_SIZE - 1)];
    }
    *cursor = w;
    return n;
}
