#ifndef __MKS142_H
#define __MKS142_H

#include "CH58x_common.h"

/* ============================================================
 * 京凡 JFH142 / MKS-142 心电 + 血氧模块驱动
 * 数据手册：JFH142 V1.6 (2023-09-12)
 *
 * 硬件接线（CH585M）:
 *   MKS RXD <- PB21 (CH585 TXD3, UART3 重映射)
 *   MKS TXD -> PB20 (CH585 RXD3, UART3 重映射)
 *   MKS RST   未使用（原 PB16 已弃用，复位改为软件命令 0x88 → 0x8A）
 *
 * 通信参数: UART 38400 8N1（手册第 7 节）
 *
 * 控制命令（MCU -> 模块, 单字节）:
 *   0x8A   启动实时测量（上电后必须发送，否则模块不发数据）
 *   0x88   停止测量
 *   0x98   设备状态查询
 *
 * 实时数据帧（模块 -> MCU, 88 字节 RT_PACK）:
 *   每 1.28s 主动上传一帧（即每秒 0.78 帧），无校验，仅靠 0xFF 帧头
 *   + 字段量程做粗对齐。
 *
 *   字节  内容            说明
 *    [0]  0xFF            帧头
 *   [1]..[64]  acdata[64] 64 个 ECG 采样（int8, -128..+127, 50 Hz）
 *    [65] heartrate       心率（uint8 BPM, 0=无效）
 *    [66] spo2            血氧（int8 %, -128/0 = 无效）
 *    [67] bk              电极/手指脱落标记
 *    [68..75] rsv[8]      保留
 *    [76] sdnn            HRV - SDNN
 *    [77] rmssd           HRV - RMSSD
 *    [78] nn50            HRV - NN50
 *    [79] pnn50           HRV - pNN50
 *    [80..85] rra[6]      最近 6 个 RR 间期
 *    [86] rsv             保留
 *    [87] state           状态字
 *
 * state 字段（手册第 4 节）:
 *   bit0  指夹脱落标志    (1 = 脱落 / 未佩戴)
 *   bit1  ECG 电极接触    (1 = 接触不良)
 *   bit2  其它状态位
 * （手册中位定义部分被打印模糊，我们在上层用 hr/spo2 是否有效做兜底判断）
 *
 * ============================================================ */

/* 协议常量 */
#define MKS142_PKT_SIZE         88u
#define MKS142_PKT_HEADER       0xFFu
#define MKS142_BAUDRATE         38400u

#define MKS142_CMD_START        0x8Au
#define MKS142_CMD_STOP         0x88u
#define MKS142_CMD_QUERY        0x98u

/* RT_PACK[87] state bit2: no body/finger detected in the sensor area. */
#define MKS142_STATUS_FINGER_OFF   0x04u

/* 内部缓冲尺寸 */
#define MKS142_RX_RING_SIZE     256u    /* 必须为 2 的幂 */
#define MKS142_ECG_RING_SIZE    128u    /* 必须为 2 的幂；至少 ≥ 一帧 64 个采样的 2 倍 */

/* 公共数据快照（多消费者访问） */
typedef struct {
    int16_t  ecg;          /* 最近 1 个 ECG 采样（int8 sign-extended，方便兼容旧 int16 接口）*/
    uint8_t  heart_rate;   /* BPM；0=无效 */
    uint8_t  spo2;         /* %；0=无效 */
    uint8_t  status;       /* 来自帧 [87] state，原样透传 */
    uint8_t  bk;           /* 来自帧 [67]；脱落 / 触发标记 */
    uint8_t  ecg_updated;  /* 1=收到至少一个新 ECG 采样未读出 */
    uint32_t ecg_seq;      /* 每收到一个新采样自增；多消费者通过该序号差去重 */
    uint32_t frames_total; /* 累计成功解析的 RT_PACK 数（调试用）*/
} MKS142_Data_t;

/* === 生命周期 === */
void   MKS142_Init(void);              /* UART3 初始化（不再使用 RST 引脚），软复位 + 发 0x8A 启动测量 */
void   MKS142_Reset(void);             /* 软件复位：发送 0x88 停止后等待模块进入待命 */
void   MKS142_StartStream(void);       /* 发送 0x8A 命令启动测量 */
void   MKS142_StopStream(void);        /* 发送 0x88 命令停止测量 */

/* === 主循环 tick === */
void   MKS142_Process(void);           /* 消化 UART 环形缓冲并解析 RT_PACK */

/* === 数据访问 === */
void   MKS142_GetData(MKS142_Data_t *out);
int16_t MKS142_GetEcg(void);           /* 返回最近 ECG 采样并清 ecg_updated */
int16_t MKS142_PeekEcg(void);          /* 返回最近 ECG 采样，不影响 ecg_updated */
uint32_t MKS142_GetEcgSeq(void);       /* 多消费者去重：每个新采样递增 */
uint8_t MKS142_GetHR(void);
uint8_t MKS142_GetSpO2(void);
uint8_t MKS142_GetStatus(void);
uint8_t MKS142_HasNewEcg(void);

/* 批量拉取自 *cursor 之后到达的所有 ECG 采样：
 *   - cursor 由调用方维护（首次传 0）
 *   - 返回实际拷贝的样本数；执行后 *cursor 已被更新到最新位置
 *   - 各消费者使用各自独立的 cursor，互不干扰
 *   - 超过 ring 容量时只能拿到最新的 MKS142_ECG_RING_SIZE 个
 */
int    MKS142_PullEcgSince(uint32_t *cursor, int8_t *out, int max);

/* === 诊断计数（用于排查上位机看不到数据的原因）===
 *  - GetRxBytes:    UART3 ISR 累计收到的字节数
 *  - GetRxOverflow: 因 ring 满被丢弃的字节数
 *  - GetFrameTotal: 累计成功解析的 RT_PACK 帧数
 *  - GetFrameDrop:  88B 凑齐但 sane 检查失败的次数
 *  - GetStartRetry: 启动看门狗触发重发 0x8A 的次数
 */
uint32_t MKS142_GetRxBytes(void);
uint32_t MKS142_GetRxOverflow(void);
uint32_t MKS142_GetFrameTotal(void);
uint32_t MKS142_GetFrameDrop(void);
uint32_t MKS142_GetStartRetry(void);
uint8_t  MKS142_GetLSR(void);
uint16_t MKS142_GetPinAlternate(void);   /* R16_PIN_ALTERNATE 原值，bit7=RB_PIN_UART3 */
uint8_t  MKS142_GetPb20Pb21(void);       /* bit0=PB20 电平, bit1=PB21 电平 */

#endif
