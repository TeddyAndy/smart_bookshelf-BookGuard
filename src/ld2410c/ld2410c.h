/**
 * HLK-LD2410C 毫米波雷达驱动库
 *
 * 非阻塞轮询 + 回调模式，适合主循环集成。
 * 协议: V1.09
 *
 * 使用:
 *   ld2410c_ctx_t ctx;
 *   ld2410c_init(&ctx, "/dev/ttyS1", 115200);
 *   ctx.on_state_change = my_callback;  // 可选
 *   while (1) { ld2410c_poll(&ctx, 100); }
 *   ld2410c_close(&ctx);
 *
 * 硬件:
 *   P9 Pin8  (UART1_TX) → LD2410C RX
 *   P9 Pin10 (UART1_RX) → LD2410C TX
 *   P9 Pin4  (VCC_5V)   → LD2410C VCC
 *   P9 Pin6  (GND)      → LD2410C GND
 */

#ifndef LD2410C_H
#define LD2410C_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * 协议常量
 * ================================================================ */

#define LD2410C_MAX_GATES      9       /* 距离门 0~8 */
#define LD2410C_GATE_WIDTH_CM  75      /* 每门 0.75m */
#define LD2410C_DEFAULT_BAUD   115200  /* 本系统已统一为 115200 */

/* 目标状态 */
#define LD2410C_TARGET_NONE            0x00
#define LD2410C_TARGET_MOVING          0x01
#define LD2410C_TARGET_STATIONARY      0x02
#define LD2410C_TARGET_BOTH            0x03
#define LD2410C_TARGET_NOISE_DETECTING 0x04
#define LD2410C_TARGET_NOISE_SUCCESS   0x05
#define LD2410C_TARGET_NOISE_FAILED    0x06

/* ================================================================
 * 数据结构
 * ================================================================ */

typedef struct {
    /* 基本目标数据 */
    uint8_t  target_state;             /* 0=无 1=运动 2=静止 3=都有 */
    uint16_t moving_distance_cm;       /* 运动目标距离 cm */
    uint8_t  moving_energy;            /* 运动目标能量 0~100 */
    uint16_t still_distance_cm;        /* 静止目标距离 cm */
    uint8_t  still_energy;             /* 静止目标能量 0~100 */
    uint16_t detection_distance_cm;    /* 检测距离 cm */

    /* 工程模式附加 */
    int      is_engineering;
    uint8_t  max_moving_gate;
    uint8_t  max_still_gate;
    uint8_t  moving_gate_energy[LD2410C_MAX_GATES];
    uint8_t  still_gate_energy[LD2410C_MAX_GATES];
    uint8_t  light_sensor;
    uint8_t  out_pin_state;
} ld2410c_data_t;

typedef struct {
    unsigned long total_frames;
    unsigned long valid_frames;
    unsigned long err_header;
    unsigned long err_length;
    unsigned long err_footer;
    unsigned long err_inner;
    unsigned long acks_received;
} ld2410c_stats_t;

/* 回调类型 */
typedef void (*ld2410c_data_cb)(const ld2410c_data_t *data);
typedef void (*ld2410c_state_cb)(uint8_t old_state, uint8_t new_state);

/* 上下文 (调用方分配) */
typedef struct {
    int fd;                          /* UART fd, -1=未打开 */
    char device[64];                 /* 设备路径 */
    int baud_rate;                   /* 波特率 */
    int verbose;                     /* 调试输出 */
    int is_engineering;              /* 当前是否工程模式 */

    ld2410c_data_t latest;           /* 最新帧数据 */
    ld2410c_stats_t stats;           /* 帧统计 */
    uint8_t last_target_state;       /* 上次目标状态 (用于 on_state_change) */

    /* 回调 */
    ld2410c_data_cb  on_data;        /* 每收到有效帧时调用 */
    ld2410c_state_cb on_state_change;/* 目标状态变化时调用 */

    /* 内部缓冲 */
    uint8_t rx_buf[256];
    int     rx_pos;
} ld2410c_ctx_t;

/* ================================================================
 * 生命周期
 * ================================================================ */

/**
 * 初始化雷达
 * @param ctx       调用方分配的上下文
 * @param device    串口设备 (如 "/dev/ttyS1")
 * @param baud_rate 波特率 (建议 115200)
 * @return 0=成功, -1=失败
 */
int  ld2410c_init(ld2410c_ctx_t *ctx, const char *device, int baud_rate);

/**
 * 安全关闭
 */
void ld2410c_close(ld2410c_ctx_t *ctx);

/* ================================================================
 * 非阻塞轮询 (主循环中调用)
 * ================================================================ */

/**
 * 读取并解析数据帧 (非阻塞)
 * @param ctx        上下文
 * @param timeout_ms select 超时 ms (0=立即返回)
 * @return >0=本次解析到有效帧数, 0=无新数据, -1=错误
 */
int  ld2410c_poll(ld2410c_ctx_t *ctx, int timeout_ms);

/** 获取最新一帧数据 */
const ld2410c_data_t *ld2410c_get_data(const ld2410c_ctx_t *ctx);

/** 获取统计 */
const ld2410c_stats_t *ld2410c_get_stats(const ld2410c_ctx_t *ctx);

/* ================================================================
 * 命令 (需要雷达在线)
 * ================================================================ */

/** 设置工程模式 on=1 off=0 */
int ld2410c_set_engineering_mode(ld2410c_ctx_t *ctx, int enable);

/** 读取固件版本 */
int ld2410c_read_version(ld2410c_ctx_t *ctx, uint16_t *fw_type, uint16_t *fw_ver);

/** 设置最大检测距离门和无人上报时间
 * @param gate_moving   运动检测最远距离门 (0~8)
 * @param gate_still    静止检测最远距离门 (0~8)
 * @param unmanned_sec  目标消失后多少秒上报无人 (0~65535)
 */
int ld2410c_set_max_gate(ld2410c_ctx_t *ctx, int gate_moving,
                         int gate_still, int unmanned_sec);

/** 设置灵敏度 (0~100, 全部距离门统一) */
int ld2410c_set_sensitivity(ld2410c_ctx_t *ctx, uint8_t moving_sens,
                            uint8_t still_sens);

/** 重启模块 */
int ld2410c_restart(ld2410c_ctx_t *ctx);

/** 查询底噪检测状态 */
int ld2410c_read_noise_status(ld2410c_ctx_t *ctx);

/* ================================================================
 * 辅助
 * ================================================================ */

/** 获取目标状态中文名称 */
const char *ld2410c_state_name(uint8_t state);

/** 设置调试输出 */
void ld2410c_set_verbose(ld2410c_ctx_t *ctx, int verbose);

#ifdef __cplusplus
}
#endif

#endif /* LD2410C_H */
