/*
 * bypass_state.h —— 主程序与注入 DLL 共享的运行状态
 *
 * 设计:
 *   - 持久配置在 bin\bypass_config.ini（人工可读，主程序负责原子写）；
 *   - 运行期状态/一次性命令走命名共享内存 + 事件（Local\ 前缀，同一会话）；
 *   - bypass_hook.off 仍是独立的文件安全阀，不进入这份状态。
 *
 * 32 位 DLL 与 64 位主程序必须使用完全一致的结构布局：只用 int/LONG，
 * 不用指针、size_t、BOOL，并固定 4 字节对齐。
 */
#ifndef BYPASS_STATE_H
#define BYPASS_STATE_H

#include <windows.h>

#define BYPASS_STATE_MAGIC    0x42505331u   /* 'BPS1' */
#define BYPASS_STATE_VERSION  1u
#define BYPASS_STATE_MAP_NAME "Local\\BypassTool_State_v1"
#define BYPASS_STATE_EVT_NAME "Local\\BypassTool_Event_v1"
#define BYPASS_CONFIG_FILE    "bypass_config.ini"

#pragma pack(push, 4)
typedef struct BypassState {
    unsigned int magic;
    unsigned int version;
    unsigned int size;
    volatile LONG seq;          /* seqlock: 奇数=写入中, 偶数=稳定 */

    int window_mode;            /* 1 = 窗口模式（默认） */
    int title_autohide;         /* 1 = 最大化后自动隐藏标题栏（默认） */
    int minimize_tray;          /* 0 = 最小化到任务栏（默认）, 1 = 收起 */

    int small_w;                /* >0 且 small_h>0: 固定外框尺寸 */
    int small_h;
    int small_permille;         /* 350 = 屏宽 0.35; 与 small_w/h 二选一 */

    unsigned int show_seq;      /* reserved: 手动「小窗化/显示」按钮已移除, 不再自增 */
    unsigned int small_seq;     /* reserved: 同上 (保留字段以维持共享内存布局) */
    unsigned int config_seq;    /* 配置代次（诊断用） */

    unsigned int reserved[7];
} BypassState;
#pragma pack(pop)

#endif /* BYPASS_STATE_H */
