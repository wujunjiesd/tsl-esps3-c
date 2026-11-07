/**
 * @file lcd_display.c
 * @brief AXS15231B (172x640 QSPI) 仪表显示驱动
 *
 * 结构：
 *   - 帧缓存放在 PSRAM，渲染完成后按条带拷入内部 DMA 缓冲再推屏
 *   - 渲染在独立任务中进行，业务线程只负责更新状态，互不阻塞
 */

#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_axs15231b.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"

#include "board.h"
#include "board_config.h"
#include "lcd_display.h"
#include "ui_draw.h"
#include "ui_pages.h"

static const char *TAG = "LCD";

#define SCR_W BOARD_LCD_H_RES
#define SCR_H BOARD_LCD_V_RES

#define LCD_SPI_HOST    SPI3_HOST      // 与官方板级示例一致
// 官方 ESP-IDF 例程（09_LVGL_V8_Test / 10_LVGL_V9_Test）与 xiaozhi 上游板级均为 40MHz
#define LCD_PIXEL_CLOCK (40 * 1000 * 1000)

/**
 * @brief 面板初始化命令
 *
 * 必须显式传入，不能留 NULL。
 *
 * 驱动内置的 vendor_specific_init_default 是针对 454x454 圆屏调好的电源/伽马表，
 * 末尾还有一条 0x22（All Pixels Off）。直接用到 172x640 这块屏上只会得到灰白噪点。
 * 官方例程与上游板级都只发 SLPOUT(0x11) + DISPON(0x29)，这里保持一致。
 */
static const axs15231b_lcd_init_cmd_t lcd_init_cmds[] = {
    {0x11, (uint8_t[]){0x00}, 0, 100},
    {0x29, (uint8_t[]){0x00}, 0, 100},
};

// 每次 DMA 推送的“物理行”数（面板物理宽 172）。
// 172 * 64 * 2 = 22016 字节，保持在 ESP32-S3 单次 SPI 传输上限（32768）以内。
#define FLUSH_LINES 64
// 横屏旋转的分块边长（PSRAM cache 友好，见 flush_frame）
#define ROT_TILE 16
// 两次刷屏之间的最小间隔，避免高频数据把 CPU 占满
#define MIN_REFRESH_INTERVAL_MS 100
// 刷屏结束后给触摸 I2C 的静默窗（见 flush_frame 结尾的说明）：
// 这几十毫秒内 die 还在收尾 RAMWR，此时去读触摸会把它的触摸引擎打死。
// 不能给 10ms：实测断连等待页（转圈动画）每 100ms 就推满一帧，一帧 QSPI 本身
// 就要 40~60ms，10ms 的静默窗根本跨不过 die 的收尾期，动画期间触摸链路几乎必死
// （日志里"点了按钮 -> 立刻 1C 卡死 -> 反复整片复位"就是这么来的）。
#define TP_FLUSH_QUIET_MS 25

static esp_lcd_panel_handle_t s_panel = NULL;
static i2c_master_dev_handle_t s_tp_dev = NULL;   // 触摸芯片（0x3B），直接走 i2c_master
// 触摸报文长度：2 字节头 + 最多 2 点 * 6 字节，与官方例程一致
#define TP_BUF_LEN 14
// 实际一次读 32 字节：官方例程（09/10/11 三个例程）都是写 11 字节命令、
// 读 32 字节，再只解析前 14 字节。读得比这少，芯片这次事务收不了尾，
// 之后每次都会回同一串固定字节（本工程实测就是 1C 1C 1C ...），
// 表现就是"触摸怎么都不响应"。
#define TP_READ_LEN 32
// 触摸 I2C 速率：官方例程用 300kHz，这里保持一致（200k~400k 都能过）
#define TP_I2C_HZ (300 * 1000)
static const uint8_t s_tp_cmd[11] = {
    0xb5, 0xab, 0xa5, 0x5a, 0x00, 0x00,
    (uint8_t)(TP_BUF_LEN >> 8), (uint8_t)(TP_BUF_LEN & 0xff), 0x00, 0x00, 0x00
};
// I2C 事务超时：43 字节就算在 50kHz 下也不到 10ms，60ms 足够宽松。
// 不能给 1000ms（卡住时把触摸任务整整堵住 1 秒，手势全丢），
// 也不建议压到 20~30ms：这颗 die 的 I2C 引擎一旦在事务中途被掐断，
// 就会卡在"一直回 1C 1C 1C"的状态，只有拉 RSTN 才能救 ——
// 宁可多等一会儿让它自己收尾，也不要在半路 abort 把它打回死态。
#define TP_I2C_TIMEOUT_MS 60
static uint16_t *s_fb = NULL;           // PSRAM 帧缓存，逻辑方向（横屏 640x172）
static uint16_t *s_rot = NULL;          // PSRAM 旋转缓冲，物理方向（172x640）
// 目前只用第一条带做双缓冲之外的搬运；数组长度与回收循环必须用同一个值，
// 否则改其中一个就会漏回收或越界
#define DMA_BUF_COUNT    ((int)(sizeof(s_dma_buf) / sizeof(s_dma_buf[0])))
static uint16_t *s_dma_buf[1] = {NULL};
static SemaphoreHandle_t s_dma_sem = NULL;
// QSPI 总线锁。触摸与显示是同一颗 die：刷屏期间去读触摸 I2C 会把触摸的状态机打断
// ——表现就是之后一直回同一串字节（0x1E 这种），软复位都救不回来。
// 所以触摸读一帧也要拿这把锁，与"推一条带 / 面板复位重初始化"互斥。
// 刷屏按条带取放锁（见 flush_frame），触摸才能在刷屏期间也采到点。
static SemaphoreHandle_t s_lcd_bus_lock = NULL;

// QSPI 总线在多个上下文里被访问（渲染任务推帧、触摸任务里的面板复位、
// 显示自检），每处都要判空再取锁，这里收口成一对函数
static void bus_lock(void)
{
    if (s_lcd_bus_lock) {
        xSemaphoreTake(s_lcd_bus_lock, portMAX_DELAY);
    }
}

static void bus_unlock(void)
{
    if (s_lcd_bus_lock) {
        xSemaphoreGive(s_lcd_bus_lock);
    }
}

// 一整帧正在推送中。整片复位要等它结束再做，否则复位会把剩下的半帧写进乱掉的显存
static volatile bool s_flush_active = false;
// 触摸恢复进行中（touch_recover / touch_hard_recover）。
//
// 这条标志是"整片复位永远叫不醒触摸"的直接原因：
// 恢复流程跑在触摸任务（优先级 6）里，中间全是 vTaskDelay（换速率 5ms、复位后
// 每 25ms 探一次），一延时 CPU 就交给渲染任务（优先级 4），于是恢复期间 QSPI
// 仍在按条带推帧。I2C 读与推屏撞在同一颗 die 上，刚从 RSTN 放出来的触摸引擎
// 立刻又被打死 —— 所以每一轮"整片复位"都以"未唤醒触摸"收场，接着退避再复位，
// 触摸就一直死到下一次上电。
// 有这条标志后，恢复期间渲染任务一次也不会推屏（保持 dirty，恢复结束自动补画），
// die 在这段时间里只面对触摸 I2C，复位才真正有机会成功。
static volatile bool s_tp_recovering = false;

static ui_canvas_t s_canvas = {0};
static ui_state_t s_state = {0};
static SemaphoreHandle_t s_lock = NULL;
static TaskHandle_t s_render_task = NULL;
static TaskHandle_t s_touch_task = NULL;

static volatile bool s_dirty = true;
static volatile bool s_bl_on = false;
// ---- 手势状态 ----
// 一次"按压"= 按下沿 -> 结束（抬手 / 芯片静默 / 手势已经生效且手指停下）。
// 抬手靠"连续几轮读不到触点"判定；但这块屏经常不给抬手帧，
// 所以滑动/双击生效后手指一停下就主动收尾，否则后续手势会被并进同一次按压吞掉。
// 点击在抬手（或按住完全不动够久）时执行，滑动在达到阈值那一刻执行。
static bool s_press_active = false;       // 当前是否处于一次按压中
static bool s_action_taken = false;       // 本次按压已经产生动作（滑动/双击），抬手不再补单击
static TickType_t s_press_start_tick = 0; // 本次按压的起始时刻（防芯片卡在"一直有触点"）
static TickType_t s_last_sample_tick = 0; // 最近一次读到触点的时刻
static TickType_t s_last_flush = 0;

// 诊断用：一次按压期间的采样统计（滑动判不出来时，靠这行日志区分"没采到点"和"采到了但没判出来"）
static int s_seq_samples = 0;
static int s_seq_xfirst = 0;              // 本次按压的落点（滑动方向与跨度都相对它）
// 本次按压的真实落点，swipe_rebase 不会动它。
// 滑动途中芯片漏报会把位移基准拉到当前点（见 touch_task），那时只有这个起点
// 才能反映手指究竟挪了多远——用它来判断"还算不算一次点击"。
static int s_press_x0 = 0;
static int s_press_y0 = 0;
static int s_press_dx_max = 0;   // 按压过程中相对真实落点的最大漂移（横/纵）
static int s_press_dy_max = 0;
// 去掉"最近一帧"之后的漂移峰值。手指抬起那一帧芯片常会吐一个偏几十像素的
// 抬起抖动，把它算进去就会把一次正常点击判成"滑动过"而直接失效。
static int s_press_dx_prev = 0;
static int s_press_dy_prev = 0;
static int s_seq_yfirst = 0;
static int s_seq_xmin = 0;
static int s_seq_xmax = 0;
static int s_seq_ymin = 0;
static int s_seq_ymax = 0;
static int s_prev_dx = 0;                 // 上一个采样点相对落点的位移（诊断用）
static int s_prev_dy = 0;
static int s_last_lx = 0;                 // 上一个采样点（算单步位移）
static int s_last_ly = 0;
static int s_run_dir = 0;                 // 最近一次有效移动的方向（+1 / -1）
static int s_run_len = 0;                 // 连续同向移动的步数：判滑动的核心依据
// 本次按压内各按钮的命中票数（抗坐标噪声）
// 必须是 16 位：手指微动导致按压一直没结束时 ++ 不会停，uint8_t 约 2.55 秒就回绕，
// 票数归零会把"解锁"误判成其它按钮 —— 那是一条真实的车控指令
static uint16_t s_hit_votes[LCD_KEY_SLOTS];
static bool s_tap_blocked = false;        // 同一次接触已下发过，手指抬起或换按钮前不再重复
static int s_tap_last_x = 0;              // 上次下发点击的位置（判断是否换按钮）
static int s_tap_last_y = 0;
// 最近一次"有意义的移动"时刻：点击判定用它算"手指静止了多久"。
// 用"静止时长"而不是"按下时长"是关键——滑动的特征是持续移动，
// 只要还在动，静止时长就一直是 0，永远不会走到点击分支。
static TickType_t s_last_move_tick = 0;
// 本次按压里动作（滑动/双击）生效的时刻：手势到此为止，收尾用它
static TickType_t s_action_tick = 0;
// 滑动冷却到什么时候：防止"一次长滑被切成两段、连切两页"
static TickType_t s_swipe_lock_until = 0;
static TickType_t s_tap_fire_tick = 0;    // 上次下发点击的时刻（防止一帧抖出两次点击）
static TickType_t s_tp_quiet_until = 0;   // 整片复位后给芯片的静默期
// 触摸基线是否已确认（上电 / 整片复位之后芯片常先吐一个假触点，见 touch_disarm）
static bool s_tp_armed = false;
static TickType_t s_tp_arm_since = 0;     // 本轮"等基线"的起始时刻
// 整片复位成功时读到的那一帧是不是"干净无触点"。是的话基线已经稳了，
// 不必再等 touch_task 自己撞上一帧无触点报文（那一段等待会让刚恢复的触摸
// 白丢 2.5s，用户感觉就是"偶发点不动"）。
static bool s_tp_wake_no_touch = false;
// 整片复位之后先压住"补画显存那一帧"：mark_dirty() 会立刻触发一次整屏刷新，
// 而刚放开 RSTN 的 die 正在跑触摸自检，这一帧 QSPI 会把它再打死一次。
// 保持 s_tp_recovering 到触摸重新读到有效帧（或超时），见 TP_FLUSH_HOLD_MS。
#define TP_FLUSH_HOLD_MS     800
static TickType_t s_flush_hold_until = 0;

static void mark_dirty(void);

// ---- 触摸手势：单击（车钥匙按钮）与双击（切换工作模式）----
//
// 判定的总原则（两条都不能破）：
//   A. 一次点击必须"要么抬手、要么按住不动够久"，并且在成立之前就已经排除滑动。
//      滑动的识别天然需要时间（连续同向多步 + 横向够远，几十毫秒起），
//      如果在滑动还没被判出来的时候就允许点击，手指刚落下、刚起步，
//      车控指令就已经发出去了 —— 这就是"钥匙页一滑动就误触按钮"的根因。
//   B. 一次手势必须有明确的"结束点"，不能依赖芯片上报抬手。
//      这块屏的触摸芯片经常不给抬手帧，如果按压状态只能靠抬手帧结束，
//      后续的点击/滑动就会被并进同一次按压里吞掉（连点、连滑不生效）。
//
// 双击只靠时间判定，不卡位置：手指在同一处点两下也会有几十像素抖动。
// 800ms：手慢的人两击间隔常在 500~700ms，600ms 会把第二击判成新的一击。
#define DOUBLE_TAP_MS    800   // 两击最大间隔
#define DOUBLE_TAP_DIST  320   // 两击允许的最大位移（像素，宽松兜底）
// 触摸轮询周期：刚有过触点时 10ms，长时间无触点退回 25ms 省 CPU。
// 异常时也不能放得太慢：100ms 一轮的话，一次点击的按下沿/抬手沿会整轮错过，
// 抬手判定要凑够连续几轮，点一下要等 300ms 才有反应 —— 那本身就是"点了没反应"。
#define TOUCH_POLL_ACTIVE_MS 10
#define TOUCH_POLL_IDLE_MS   25
#define TOUCH_IDLE_AFTER_MS  2000   // 这么久没有触点就退回慢速轮询
// 最近一次采到触点之后，这段时间内算"用户正在用屏幕"（见 touch_busy）：
// 转圈动画要让路，否则刷屏会把触摸帧挤掉。
#define TOUCH_BUSY_HOLD_MS   600
// 上电/整片复位后"等触摸基线稳定"的最长时间。
// 这颗芯片刚初始化时经常先吐一个假触点（日志里"触摸原始报文: C0 01 ..."= 刚上电
// 就报 1 个触点），如果恢复出来的页面是车钥匙页，这个假触点会被当成按下沿压在
// 按钮上，按住 600ms 就真的下发车控指令。所以先等一帧干净的"无触点"再开放触摸；
// 万一基线彻底偏了、一直不报无触点，最多等这么久也放开，不能让触摸永久失效。
#define TP_ARM_TIMEOUT_MS    2500
// 触摸读一帧要拿 QSPI 总线锁（与刷屏互斥），最多等这么久。
#define TOUCH_FLUSH_WAIT_MS  20

// ---- 点击判定 ----
// 点击允许的总漂移：手指挪动超过它就不再当点击处理（交给滑动判定）
#define TAP_MAX_DRIFT        40
// 按住完全不动这么久才允许"按住触发点击"。
// 正常点击走两条快路径：芯片的抬手事件（event=1，抬手那一帧就结算）和 0 触点帧。
// 这条只剩"两条都快路径都没有"时的兜底（芯片既不报抬手事件、也不报 0 触点）。
//
// 原来是 600ms，用户感觉就是"轻点没反应、必须长按"。现在降到 300ms：
// 300ms 静止且漂移 ≤20px 已经排除了真滑动（真滑动这 300ms 里早就走出去几十像素），
// 代价是"按下后发愣 300ms 再滑"会被先算成一次点击 —— 这个代价可接受。
#define TAP_HOLD_STILL_MS    300
// 按住触发时允许的最大漂移：比抬手严得多。静止 300ms 且只挪了 20px，
// 不可能是滑动，此时才敢认为用户就是要按这个按钮。
#define TAP_HOLD_DRIFT       20
// 单步位移小于它算"没动"（芯片静止时本来就有 ±1 抖动），用于累计"静止时长"
#define TAP_MOVE_EPS         5
// 已下发过一次点击后，手指要挪这么远才算"换了按钮"，解除重复下发限制
// （防止手指压在按钮上不动被反复下发指令）
#define TAP_UNBLOCK_MOVE     40
// 同一个按钮的最小重复下发间隔。抬手事件（event=1）偶尔会连着来两帧
// （坐标/事件各差一个字节就算"新帧"），没有这道闸门就会一次轻点发两条车控指令。
// 120ms 比人手连点最短间隔（约 180~200ms）小，不影响快速连点。
#define TAP_REFIRE_MS        120

// ---- 抬手判定 ----
// 判据 = "最后一帧读到了没触点" + "距最后一个触点已经过了 RELEASE_MIN_GAP_MS"。
// 关键是 40ms 这个时间下限，它同时干了两件事：
//   * 健康速率（10ms 一轮）下单帧偶发空窗跨不到 40ms，按住的滑动不会被切碎；
//   * 轮询被拖慢时（通信异常降速到 40ms 一轮），不会因为"凑不够 2 帧"而把两次
//     快速点击并成一次。以前要求连续 3 帧 + 60ms，慢轮询下抬手判定能被拖到 200ms 以上，
//     用户感觉就是"连点第二下永远没反应"。
#define RELEASE_MIN_MISS     1
#define RELEASE_MIN_GAP_MS   40
// 芯片彻底不再上报时的兜底（手指还按着但再也没有触点帧）
#define PRESS_SILENT_MS      400
// 芯片卡在"一直有触点"时的兜底：一次按压超过这么久就强制结束按压状态机。
// 注意这里只是"结束状态机"，不能顺手解除 s_tap_blocked ——
// 芯片一直报触点说明手指可能还压着，解除限制会让按住不放变成每几秒重复控车。
#define PRESS_STUCK_MS       2500

// ---- 滑动判定 ----
// 单步最小位移：小于它算抖动，不参与方向统计
#define SWIPE_STEP_MIN   6
// 连续同向移动的步数达到这么多才判滑动。
// 这是区分"真滑动"与"两次快速点击"的唯一可靠依据：
// 两次点击之间只有一个大跳变（1 步），真正的滑动会连续多帧朝同一方向走。
#define SWIPE_RUN_MIN    3
// 判滑动所需的最小横向位移（相对落点）
#define SWIPE_MIN_DX     56
// 采样极稀（芯片只吐两三个点）时的大跨步兜底
#define SWIPE_JUMP_DX    200
// 相邻两次触点采样间隔在该值以内，仍算同一次按压。
#define SWIPE_LINK_MS    250
// 同一次按压的超时兜底：两次触点间隔超过这个值就不再算同一次按压。
// 曾经是 600ms，这是"连点不生效"最主要的开关：
// 通信抖动时两次采样本来就隔 400~500ms，600ms 会把**两次独立的点击**
// 合并成一次按压 —— 投票被摊到两个按钮上（"按键命中不明确"直接丢弃），
// 同向步数还会跨两次点击累加（run 到 3 就把点击否掉、甚至误判成滑动切页）。
// 真手指按住不动时轮询是 10ms 一轮，间隔不会超过几十毫秒，
// 所以 180ms 已经足够宽松：超过它必然是中间丢了帧，只能是新的一次按压。
#define GESTURE_GAP_MS   180
// 一次滑动生效后，这段时间内不再判滑动：
// 覆盖"手指还在同一次滑动里继续走"的尾巴，避免一次长滑连切两页；
// 又不能太长 —— 连续两次滑动之间只隔一两百毫秒，太长会把第二次吞掉。
#define SWIPE_LOCK_MS        250
// 滑动/双击生效后，手指停下来这么久就结束这次按压（下一次触点即全新手势）。
// 芯片经常不给抬起帧，不靠这个收尾，"连续滑动"会一直被当成同一次按压吞掉。
#define ACTION_END_STILL_MS  150
// 同上，手指一直不停时的兜底（正常滑动几十毫秒内就会停或被判为抬手）
#define ACTION_END_MAX_MS    500

static volatile lcd_ui_mode_t s_mode = LCD_MODE_DASHBOARD;
static volatile uint32_t s_double_taps = 0;

// 车钥匙按钮动作队列。
// 以前只用一个 int：业务层 50ms 才取一次，而且取到之后还要跑一条 BLE 指令，
// 这期间连点第二、第三下会直接覆盖同一个变量 —— 用户看到的就是"连点只有第一下生效"。
// 改成小队列后每一次点击都排队送达，不丢、不乱序。
#define KEY_SLOT_QUEUE_LEN 4
static volatile uint8_t s_key_q[KEY_SLOT_QUEUE_LEN];
static volatile uint8_t s_key_q_head = 0;   // 只由触摸任务推进
static volatile uint8_t s_key_q_tail = 0;   // 只由业务层推进

// 车钥匙页按钮的按下动效。
// 整屏一帧要 150~200ms（整屏转置 + 10 条 QSPI），按时间去插值根本没有中间帧可用，
// 所以动效按"帧"推进：按下后下一帧画"压下去"，再下一帧画"弹回来"，两帧结束。
// 这样无论刷屏快慢，用户都一定能看到一模一样的一次按压反馈。
#define KEY_PRESS_NONE   0
#define KEY_PRESS_DOWN   1   // 下一帧画按下的样子
#define KEY_PRESS_UP     2   // 再下一帧弹回静止
static volatile uint8_t s_key_press_stage = KEY_PRESS_NONE;
static volatile uint8_t s_key_press_slot = 0;

/** 投递一次按键动作（队列满时丢弃最新的一次，避免积压后集中控车） */
static void key_slot_push(int idx)
{
    if (idx < 0 || idx >= LCD_KEY_SLOTS) {
        return;
    }
    uint8_t next = (uint8_t)((s_key_q_head + 1) % KEY_SLOT_QUEUE_LEN);
    if (next == s_key_q_tail) {
        ESP_LOGW(TAG, "按键队列已满，丢弃槽位 %d", idx);
        return;
    }
    s_key_q[s_key_q_head] = (uint8_t)idx;
    portMEMORY_BARRIER();   // 先写数据再发布下标，避免另一个核读到"下标已更新、数据还没写"
    s_key_q_head = next;

    // 屏幕上的反馈：按下的按钮下沉 + 提亮，由渲染任务在紧接的帧里画出来
    s_key_press_slot = (uint8_t)idx;
    s_key_press_stage = KEY_PRESS_DOWN;
    mark_dirty();
}

/** 按下动效是否还在播放（渲染任务据此继续按节拍出帧，直到"弹回"那一帧画完） */
static bool key_press_animating(void)
{
    return s_key_press_stage != KEY_PRESS_NONE;
}

/** 推进按下动效一帧：先画按下，再画弹回 */
static void key_press_step(void)
{
    if (s_key_press_stage == KEY_PRESS_DOWN) {
        s_state.key_press = (int8_t)s_key_press_slot;
        s_key_press_stage = KEY_PRESS_UP;
    } else if (s_key_press_stage == KEY_PRESS_UP) {
        s_state.key_press = -1;
        s_key_press_stage = KEY_PRESS_NONE;
    }
}

// 模式发生变化时通知业务层（用于记住"上次停在哪个页面"，重启后恢复）
static void (*s_mode_change_cb)(lcd_ui_mode_t mode) = NULL;

static bool s_tap_pending = false;
static bool s_tap_wake = false;   // 这一击是用来点亮屏幕的，超时后不执行单击动作
static TickType_t s_tap_tick = 0;
static int s_tap_x = 0;           // 按下沿的落点（双击比较用）
static int s_tap_y = 0;
static int s_last_x = 0;          // 按压期间最新的触点（命中测试用：按下沿那一帧可能是旧坐标）
static int s_last_y = 0;

// ============ 底层：SPI / 面板 ============

static bool lcd_trans_done_cb(esp_lcd_panel_io_handle_t panel_io,
                             esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    BaseType_t woken = pdFALSE;
    if (s_dma_sem) {
        xSemaphoreGiveFromISR(s_dma_sem, &woken);
    }
    return woken == pdTRUE;
}

static esp_err_t lcd_hw_init(void)
{
    esp_err_t ret;

    // 背光引脚（低电平点亮），已在 lcd_display_init 开头点亮，这里只保证方向正确
    gpio_config_t bl_cfg = {
        .pin_bit_mask = 1ULL << board_backlight_gpio(),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&bl_cfg));

    // QSPI 总线
    // 说明：spi_bus_config_t 里 mosi/miso/quadwp/quadhd 与 data0..data3 是同一组
    //       union 成员，这里按 data0..data3 填写（QSPI：D0/D1/D2/D3 + CLK）。
    spi_bus_config_t buscfg = {
        .data0_io_num = BOARD_LCD_D0,
        .data1_io_num = BOARD_LCD_D1,
        .data2_io_num = BOARD_LCD_D2,
        .data3_io_num = BOARD_LCD_D3,
        .sclk_io_num = BOARD_LCD_PCLK,
        // 单次传输量按“物理行宽 x 分块行数”计算：172 * 64 * 2 = 22016 字节
        .max_transfer_sz = BOARD_LCD_PANEL_W * FLUSH_LINES * sizeof(uint16_t) + 64,
    };
    ret = spi_bus_initialize(LCD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI 总线初始化失败: %s", esp_err_to_name(ret));
        return ret;
    }

    // 面板 IO
    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = BOARD_LCD_CS,
        .dc_gpio_num = GPIO_NUM_NC,
        .spi_mode = 3,
        .pclk_hz = LCD_PIXEL_CLOCK,
        .trans_queue_depth = 10,
        .on_color_trans_done = lcd_trans_done_cb,
        .lcd_cmd_bits = 32,
        .lcd_param_bits = 8,
        .flags.quad_mode = true,
    };
    ret = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &io_config, &io);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "面板 IO 创建失败: %s", esp_err_to_name(ret));
        return ret;
    }

    // 面板驱动：init_cmds 必须用官方那两条（0x11 / 0x29）
    const axs15231b_vendor_config_t vendor_config = {
        .init_cmds = lcd_init_cmds,
        .init_cmds_size = sizeof(lcd_init_cmds) / sizeof(lcd_init_cmds[0]),
        .flags.use_qspi_interface = 1,
    };
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = GPIO_NUM_NC,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = BOARD_LCD_BIT_PER_PIXEL,
        .vendor_config = (void *)&vendor_config,
    };
    ret = esp_lcd_new_panel_axs15231b(io, &panel_config, &s_panel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "AXS15231B 面板创建失败: %s", esp_err_to_name(ret));
        return ret;
    }

    // 复位：EXIO5（R73）与 GPIO21（R69）两路候选复位脚同时打脉冲，
    // 无论板上焊的是哪一颗 0R，都能把面板从复位态放出来。
    // 必须放在 esp_lcd_new_panel_axs15231b 之后、esp_lcd_panel_init 之前，
    // 与官方例程顺序一致。
    board_lcd_reset();

    ret = esp_lcd_panel_init(s_panel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "面板初始化失败: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "面板初始化完成 %dx%d（init_cmds 0x11/0x29）", SCR_W, SCR_H);
    return ESP_OK;
}

/** 触摸芯片（0x3B）是否在线；上电初期可能还没准备好，重试几次 */
static bool touch_probe(void)
{
    for (int i = 0; i < 5; i++) {
        if (board_i2c_touch() && i2c_master_probe(board_i2c_touch(), BOARD_TOUCH_ADDR, 100) == ESP_OK) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return false;
}

/**
 * @brief 读一帧触摸原始报文
 *
 * 注意这里必须用 i2c_master_transmit_receive（写命令 + 重复起始 + 读数据，
 * 一次完整事务），不能像 esp_lcd_touch 那样把"写读命令"和"读数据"拆成两次
 * 独立的 I2C 事务——中间插一个 STOP，AXS15231B 的触摸部分会丢掉这次请求，
 * 读回来永远是"0 个触点"，表现就是触摸完全没反应。
 *
 * 报文格式（与官方例程一致，最多 2 点）：
 *   [0] gesture      [1] 触点数 num
 *   [2] X 高 4 位 + event(高 2 位)   [3] X 低 8 位
 *   [4] Y 高 4 位                    [5] Y 低 8 位
 */
static esp_err_t tp_raw_read(uint8_t *buf, size_t len)
{
    if (!s_tp_dev) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2c_master_transmit_receive(s_tp_dev, s_tp_cmd, sizeof(s_tp_cmd), buf, len,
                                       TP_I2C_TIMEOUT_MS);
}

/** 重新挂载触摸设备并指定 I2C 速率 */
static esp_err_t tp_set_speed(uint32_t hz)
{
    if (s_tp_dev) {
        i2c_master_bus_rm_device(s_tp_dev);
        s_tp_dev = NULL;
    }
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BOARD_TOUCH_ADDR,
        .scl_speed_hz = hz,
    };
    return i2c_master_bus_add_device(board_i2c_touch(), &cfg, &s_tp_dev);
}

/**
 * @brief 触摸报文是否"整包字节完全相同且非 0"
 *
 * 正常报文里手势、触点数、坐标各不相同，不可能整包一模一样；
 * 整包同一个字节（0x1C、0x1E 这类）说明这次读根本没拿到触摸数据。
 * 全 0 是"没有触摸"的合法报文，不算异常。
 */
static bool tp_msg_broken(const uint8_t *buf, size_t len)
{
    bool all_zero = true;
    for (size_t i = 0; i < len; i++) {
        if (buf[i] != buf[0]) {
            return false;
        }
        if (buf[i] != 0) {
            all_zero = false;
        }
    }
    return !all_zero;
}

// ---- 报文判读 ----
// 「整包逐字节完全相同」不能当成通信故障！
//   手指按住不动时芯片报的坐标可以连续几百毫秒一字不差（尤其是按钮这种
//   完全静止的按压），以前 8 帧（约 200ms）就判成"芯片卡在回吐旧包"并触发
//   整套恢复流程（复位总线 + 换速率 + 可能整片复位）——一次普通点击就可能
//   把触摸链路打成 1C 1C 1C，之后连着好几秒点不动。
//   现在的处理：这种帧就是"合法但没变化"的采样，照常当按住在报点；
//   只有持续 STALE 到 TP_STALE_MS 以上（真手指不可能这么久一字不差），
//   才当作"假触点"回报成没有触点，让上层把这次按压干净收尾 —— 全程不碰总线。
//
//   注意：stale 帧**不等于抬手**。芯片冻在"一直有触点"时吐的就是这种帧，
//   上层若把它当成"手指抬起"，就变成"抬手 -> 解除重复限制 -> 又收到触点 ->
//   再投一次票 -> 再下发一条车控指令"的循环：一次按压会连着发好几条指令
//   （日志里同一坐标反复触发"空调开关"就是这么来的）。所以 stale 通过
//   tp_poll 的 out 参数单独上报，上层只结束按压、不认定抬手、不下发点击。
static uint8_t s_last_tp_pkt[TP_BUF_LEN];
static TickType_t s_same_pkt_since = 0;
#define TP_STALE_MS         1500
// 触摸报文里每点的 event 编码：0=按下 1=抬起 2=持续接触（AXS15231B 触摸协议）。
// 这是"轻点立刻响应"的关键：抬手那一帧触点数还是 1、坐标还是最后位置，
// 只有 event 位变了。设成 -1 等于关掉这条快路径（只靠 0 触点帧和静止兜底）。
#define TP_EVENT_UP         1
// 同一帧持续超过这个时间：真手指按住不动也会让报文一字不差，但静止到 6 秒
// 还逐字节完全相同的概率极低 —— 这时按"触摸引擎冻死"处理，升级成整片复位。
// 不做这一步的话，芯片冻在最后一帧带触点的数据上会永远回 pressed=false：
// 上层认为"没有触摸"、全程不报错，恢复流程一次都不会启动，触摸一直死到重启。
#define TP_FROZEN_MS        6000
static bool s_tp_frozen = false;   // 已经判定"报文冻住"，直到报文变化才解除
// 这次的失败是不是"整包同一个非零字节"（1C/1E 这种）。这是 die 的 I2C 引擎
// 卡死的特征，软复位救不回来，只有拉 RSTN 有用 —— 恢复流程见到它就别慢慢试速率了。
static bool s_tp_stuck_sig = false;
// 读得到、但一帧要几十上百毫秒（吞吐掉到 5Hz 以下）。这是显示引擎把触摸引擎
// 挤住的特征：每次 I2C 事务都被时钟拉伸，但都在超时之前返回，所以从来不算"错误"，
// 恢复流程一次都不会启动，手势就一直采不到足够密的点 —— 又一种"触摸卡死"。
// 软握手（复位总线/换速率）治不了这个，同样要整片复位。
static bool s_tp_slow_sig = false;

// 触摸总线当前实际可用的速率：某一档跑通一次就记住它，下次恢复先试这一档。
// 这块板标称 300kHz 经常跑不动（日志里"300kHz 仍不通，沿用 100kHz"），
// 每次都从 300k 试起等于每轮恢复都白等一次失败 + 一次重挂载。
static uint32_t s_tp_hz = TP_I2C_HZ;

// 握手探测的单次 I2C 超时。恢复要的是"快"，不是"有耐心"：
// 以前每次 1000ms × 4 档 × 3 轮，卡死时一次恢复能把触摸任务堵十几秒，
// 表现就是"点了半天完全没反应"。
#define TP_PROBE_TIMEOUT_MS 30

/**
 * @brief 在某一档速率上试读一帧
 *
 * 只认官方例程的方式（写解锁命令 + 重复起始读）。以前还试过"写命令后单独读"，
 * 但这条路上芯片根本不出数据、读回来是全 0 —— 而全 0 恰好是合法的"没有触点"报文，
 * 会被当成"通信建立成功"，于是刚报成功 200ms 后又掉线（日志里就是这种反复）。
 * 宁可不认这种方式，也不要假成功。
 */
static bool tp_try_speed(uint32_t hz, uint8_t *out)
{
    if (tp_set_speed(hz) != ESP_OK) {
        return false;
    }
    uint8_t b[TP_READ_LEN];
    memset(b, 0, sizeof(b));
    bool linked = (i2c_master_transmit_receive(s_tp_dev, s_tp_cmd, sizeof(s_tp_cmd),
                                               b, sizeof(b), TP_PROBE_TIMEOUT_MS) == ESP_OK &&
                   !tp_msg_broken(b, TP_BUF_LEN));
    if (out) {
        memcpy(out, b, TP_BUF_LEN);
    }
    return linked;
}

/**
 * @brief 把触摸通信拉起来
 *
 * 这块屏的触摸芯片并不是上电就一定在出数据，而且一旦某次 I2C 事务被打断
 * （从机把 SDA 拉住 / 状态机卡住），之后所有读都会返回同一串固定字节，
 * 只有复位 I2C 总线才能恢复。所以这里按"复位总线 -> 换速率 -> 换事务方式"反复试。
 */
static bool tp_wakeup(const char *why)
{
    // 上次跑通的速率优先；其余作为电气条件变差时的退路（越低越宽容），
    // 400k 比标称更激进，放到最后试。
    static const uint32_t rest[] = {TP_I2C_HZ, 100 * 1000, 50 * 1000, 400 * 1000};
    const int n_rest = (int)(sizeof(rest) / sizeof(rest[0]));
    uint32_t speeds[1 + sizeof(rest) / sizeof(rest[0])];
    int n_speed = 0;
    speeds[n_speed++] = s_tp_hz;
    for (int i = 0; i < n_rest; i++) {
        if (rest[i] != s_tp_hz) {
            speeds[n_speed++] = rest[i];
        }
    }

    uint8_t last[TP_BUF_LEN] = {0};
    // 只跑一轮：两轮等于把触摸任务多堵一倍时间（每档 30ms 超时 + 5ms 间隔）。
    // 一轮扫完还不行就直接交给整片复位，它才是真正能救回 1C 卡死的手段。
    for (int round = 0; round < 1; round++) {
        // 复位 I2C 外设状态机（把被从机拉住的 SDA 释放掉）
        if (board_i2c_touch()) {
            i2c_master_bus_reset(board_i2c_touch());
        }
        vTaskDelay(pdMS_TO_TICKS(2));

        for (int i = 0; i < n_speed; i++) {
            if (tp_try_speed(speeds[i], last)) {
                if (speeds[i] != s_tp_hz) {
                    ESP_LOGW(TAG, "[%s] 触摸速率 %ukHz -> %ukHz", why,
                             (unsigned)(s_tp_hz / 1000), (unsigned)(speeds[i] / 1000));
                    s_tp_hz = speeds[i];
                }
                ESP_LOGI(TAG, "[%s] 触摸通信建立(%ukHz, 第 %d 轮)", why,
                         (unsigned)(s_tp_hz / 1000), round + 1);
                return true;
            }
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }

    tp_set_speed(s_tp_hz);
    ESP_LOGW(TAG, "[%s] 触摸通信仍未建立：读回整包相同字节 %02X %02X %02X %02X",
             why, last[0], last[1], last[2], last[3]);
    return false;
}

// 整片复位后给芯片的静默期：这段时间不去读它（读也全是 1E）
#define TP_QUIET_AFTER_RST_MS 600

/**
 * @brief 整片复位 AXS15231B（触摸 I2C 彻底卡死时的唯一有效恢复手段）
 *
 * 软复位（i2c_master_bus_reset + 换速率）只能救 ESP 侧的状态机，救不了从机：
 * 触摸与显示是同一颗 die，一旦触摸的 I2C 引擎卡住，只有拉 RSTN 才能让它重来。
 * 复位后显存内容会被清空，所以必须通知渲染任务重画一帧。
 */
static bool touch_hard_recover(const char *why)
{
    // 日志必须打在 s_panel 判空之前：面板句柄没就绪时直接 return 的话，
    // 日志里就只剩"触摸通信仍未建立"，根本看不出来到底有没有走到整片复位这一步
    // （实测卡死后要是这一步一直没出现，触摸就会一直死着没人管）。
    ESP_LOGW(TAG, "[%s] 触摸软复位无效 -> 整片复位 AXS15231B", why);
    if (!s_panel) {
        ESP_LOGE(TAG, "[%s] 面板句柄为空，无法整片复位", why);
        return false;
    }

    if (s_tp_dev) {
        i2c_master_bus_rm_device(s_tp_dev);
        s_tp_dev = NULL;
    }
    if (board_i2c_touch()) {
        i2c_master_bus_reset(board_i2c_touch());
    }

    // 先等当前这一帧推完：复位会清掉显存，半帧写进去会留下花屏。
    //
    // 这里原来只等 60ms，而一帧实际要 150~200ms —— 等于经常在一帧 QSPI 中途拉 RSTN。
    // 对这颗 die 来说这是最糟的时机：RSTN 落地时它正把像素往显存里灌，触摸引擎会直接
    // 卡在回同一帧的状态（日志里"整片复位后 600ms 仍无触摸数据: 1E 1E 1E 1E"，
    // 连着三十次复位全是这个结果，最后只有在长时间不退避的空窗里自己缓过来）。
    // 现在等够一整帧（调用方已置 s_tp_recovering，渲染任务会在条带之间立刻收手，
    // 实际通常几毫秒就等到 !s_flush_active）。
    for (int i = 0; i < 200 && s_flush_active; i++) {
        vTaskDelay(pdMS_TO_TICKS(2));
    }

    // 复位 + 重新初始化面板：必须与渲染任务的 QSPI 传输串行
    bus_lock();
    board_lcd_reset();
    esp_lcd_panel_init(s_panel);
    bus_unlock();
    mark_dirty();   // 面板复位后显存归零，重画一帧

    if (tp_set_speed(s_tp_hz) != ESP_OK) {
        ESP_LOGE(TAG, "[%s] 触摸设备重新挂载失败", why);
        return false;
    }

    // 复位后芯片要重新跑触摸自检。以前是"死等 600ms 再去握手"，但实测多数时候
    // 100ms 左右就醒了 —— 死等等于每次卡死都白丢半秒的操作时间。
    // 改成边等边试：一有应答立刻返回，最多等 TP_QUIET_AFTER_RST_MS。
    // 这里只做一次挂载，循环里不再反复 rm/add 设备。
    uint8_t last[TP_READ_LEN] = {0};
    for (int i = 0; i < TP_QUIET_AFTER_RST_MS / 25; i++) {
        vTaskDelay(pdMS_TO_TICKS(25));

        // 先只探地址，再决定读不读。
        // 刚放开 RSTN 时芯片还在做触摸自检，从机会一直拉住 SCL（clock stretching）。
        // 这时候直接发 43 字节事务必然撞上 TP_I2C_TIMEOUT_MS 被掐断，而"事务被半路
        // 掐断"恰恰就是这颗 die 卡死在回固定字节的原因 —— 等于每次复位都亲手把它
        // 再打死一次。地址探活是最短的事务，探不到就说明还没醒，这一轮干脆不读。
        if (!board_i2c_touch() ||
            i2c_master_probe(board_i2c_touch(), BOARD_TOUCH_ADDR, TP_PROBE_TIMEOUT_MS) != ESP_OK) {
            continue;
        }

        memset(last, 0, sizeof(last));
        esp_err_t rerr = tp_raw_read(last, sizeof(last));
        if (rerr == ESP_OK && !tp_msg_broken(last, TP_BUF_LEN)) {
            ESP_LOGI(TAG, "[%s] 整片复位后 %dms 恢复应答", why, (i + 1) * 25);
            // 首帧就是"无触点"= 触摸基线已经稳了，复位后不需要再重新学基线。
            // 交给 touch_task 立刻放开触摸，别让用户在恢复后白等一帧基线确认。
            s_tp_wake_no_touch = ((last[1] & 0x0F) == 0);
            s_tp_quiet_until = 0;
            s_tp_stuck_sig = false;
            s_tp_slow_sig = false;
            s_tp_frozen = false;
            s_same_pkt_since = 0;
            memset(s_last_tp_pkt, 0, sizeof(s_last_tp_pkt));
            return true;
        }
        // 读失败（超时/被掐断）：立刻把总线清干净，不让从机卡在半截事务里，
        // 否则后面每一轮读到的都是上一次的残包，整轮 600ms 全白试。
        if (rerr != ESP_OK && board_i2c_touch()) {
            i2c_master_bus_reset(board_i2c_touch());
        }
    }

    // 一直没醒：给一段静默期，别在这里死循环锤它。
    // 带上最后一次读到的字节：1C/1E 这种整包相同字节说明是 die 侧卡死，
    // 全 0 则是根本没应答（多半是复位没真正打到 RSTN），两者定位方向完全不同。
    ESP_LOGW(TAG, "[%s] 整片复位后 %dms 仍无触摸数据: %02X %02X %02X %02X",
             why, TP_QUIET_AFTER_RST_MS, last[0], last[1], last[2], last[3]);
    s_tp_wake_no_touch = false;
    s_tp_quiet_until = xTaskGetTickCount() + pdMS_TO_TICKS(TP_QUIET_AFTER_RST_MS);
    return false;
}

// 恢复节奏（分级，别一坏就上手）：
//
//   0 ~ 1200ms  ：什么都不做，只让 touch_task 照常全速重读。
//                 实测这颗 die 卡在 1C 之后，多数时候 100~300ms 自己就缓过来了；
//                 这期间去复位总线 / 换速率，等于把它重新摁回去
//                 —— 日志里"刚恢复 200ms 又掉线、1C 连着好几秒"就是这么来的。
//   > 1200ms    ：软握手（复位总线 + 换速率），代价小、不闪屏。
//   软握手无效  ：整片复位（会清显存、闪一下），只有"复位也没救回来"才退避 1.2s→4s。
//
// !!! 这里的每一个恢复动作都会把 touch_task 本身堵住 !!!
// 软握手要跑 4 档速率，硬复位要 1.3s。以前是"连续失败 150ms 就调一次"，
// 结果触摸任务一半时间卡在恢复里，采样间隔被撑到 400~700ms，
// 手势判定拿到的全是稀疏帧 —— 表现就是按钮点了没反应/误判成滑动。
// 所以现在由 s_next_recover_tick 统一安排下一次尝试的时刻：
// 两次动作之间触摸任务只在采样，绝不进恢复流程。
// "读得到但吞吐极低"的判据（见 touch_task 里的慢读检测）：
// 正常一轮 12~30ms（含主动延时的 10/25ms）；被显示引擎挤住时单轮 150~270ms。
// 取 80ms 做界，连续 3 轮、且这种状态已经持续 3 秒以上才认定故障，
// 这样一次整帧刷屏造成的偶发慢轮不会误触发整片复位。
#define TP_SLOW_READ_MS      80
#define TP_SLOW_READ_RUN     3
#define TP_SLOW_ESCALATE_MS  3000
// 整片复位之后，这段时间内不跑转圈动画（整屏刷屏会立刻把刚救回来的触摸打死）
#define TP_ANIM_QUIET_AFTER_RST_MS 5000
#define TP_WAKEUP_MIN_MS     300
#define TP_RECOVER_GRACE_MS  1200
static const uint16_t k_hwrst_cool_ms[] = {1200, 2500, 4000};
// 这么久没再复位过 = 上一次故障期早就过去了，退避档位归零
#define TP_HWRST_STEP_RESET_MS 4000
#define TP_HWRST_COOL_STEPS  ((int)(sizeof(k_hwrst_cool_ms) / sizeof(k_hwrst_cool_ms[0])))
static TickType_t s_last_wakeup_tick = 0;
static TickType_t s_last_hwrst_tick = 0;
static TickType_t s_fail_since = 0;     // 这一轮通信故障从什么时候开始（读通了就清零）
static TickType_t s_next_recover_tick = 0;  // 下一次允许做恢复动作的时刻
static int s_hwrst_step = 0;            // 整片复位的退避档位
static int s_wakeup_fail_run = 0;       // 软恢复连续失败次数（见 touch_recover）

/**
 * 触摸通信坏了之后的统一恢复入口（分级 + 限速 + 退避）
 * @param did_hwrst 出参：这一轮是不是真的拉了 RSTN 做整片复位。
 *                  只有它才会让触摸基线失效、才需要重新学基线；
 *                  软握手 / 空跑（宽限期、退避）都不该把已确认的基线丢掉。
 */
static bool touch_recover(const char *why, bool *did_hwrst)
{
    if (did_hwrst) {
        *did_hwrst = false;
    }
    TickType_t now = xTaskGetTickCount();
    if (s_fail_since == 0) {
        return false;   // 调用方还没登记故障起点
    }
    if (s_last_wakeup_tick != 0 &&
        (now - s_last_wakeup_tick) < pdMS_TO_TICKS(TP_WAKEUP_MIN_MS)) {
        s_next_recover_tick = s_last_wakeup_tick + pdMS_TO_TICKS(TP_WAKEUP_MIN_MS);
        return false;
    }
    int fail_ms = (int)((now - s_fail_since) * portTICK_PERIOD_MS);

    // 第 1 级：故障还很短，先别动手（tp_poll 自己已经重读过 3 次）
    if (fail_ms < TP_RECOVER_GRACE_MS) {
        // 直接安排到宽限期结束，这段时间触摸任务只在全速采样
        s_next_recover_tick = s_fail_since + pdMS_TO_TICKS(TP_RECOVER_GRACE_MS);
        return false;
    }
    s_last_wakeup_tick = now;

    // 第 2 级：软握手。
    // 命中卡死签名（读回整包 1C/1E）或"读得到但吞吐极低"时直接跳过：
    // 前者的总线是通的、从机正常应答，问题在 die 内部的触摸引擎；
    // 后者（s_tp_slow_sig）同样如此 —— 事务都能返回，只是被拖到几十毫秒，
    // 换速率/复位总线治不了它。两种情况都白花几百毫秒，直接整片复位。
    if (!s_tp_stuck_sig && !s_tp_slow_sig) {
        if (tp_wakeup(why)) {
            s_wakeup_fail_run = 0;
            s_same_pkt_since = 0;
            memset(s_last_tp_pkt, 0, sizeof(s_last_tp_pkt));
            s_next_recover_tick = xTaskGetTickCount();   // 通了：下次重新按故障时长判
            return true;
        }
        s_wakeup_fail_run++;
    }

    // 第 3 级：整片复位。只有真卡死或软恢复连续无效才做，
    // 因为它要清显存、要 700ms 左右（复位时序 + 面板初始化）。
    if (!s_tp_stuck_sig && !s_tp_slow_sig && s_wakeup_fail_run < 3) {
        s_next_recover_tick = now + pdMS_TO_TICKS(TP_WAKEUP_MIN_MS);
        return false;
    }
    // 距上次复位已经很久 = 那之后一直是好的，这次仍按最快节奏救。
    if (s_last_hwrst_tick == 0 ||
        (now - s_last_hwrst_tick) >= pdMS_TO_TICKS(TP_HWRST_STEP_RESET_MS)) {
        s_hwrst_step = 0;
    }
    if (s_last_hwrst_tick != 0 &&
        (now - s_last_hwrst_tick) < pdMS_TO_TICKS(k_hwrst_cool_ms[s_hwrst_step])) {
        s_next_recover_tick =
            s_last_hwrst_tick + pdMS_TO_TICKS(k_hwrst_cool_ms[s_hwrst_step]);
        return false;
    }
    s_last_hwrst_tick = now;
    s_tp_stuck_sig = false;
    s_tp_slow_sig = false;
    if (did_hwrst) {
        *did_hwrst = true;
    }
    s_tp_wake_no_touch = false;   // 由 touch_hard_recover 按首帧实际内容重填
    bool ok = touch_hard_recover(why);
    now = xTaskGetTickCount();   // 硬复位要 1.3s，重取时间

    // 退避只在"复位也没救回来"时升档。旧代码不管成败都升档，
    // 几次偶发卡死就把档位一路顶到最高，于是卡死后几十秒都不再动手，
    // 而实际上每一次复位本来都是有效的。
    if (ok) {
        s_wakeup_fail_run = 0;
        s_hwrst_step = 0;
        s_next_recover_tick = now;
        return true;
    }

    if (s_hwrst_step < TP_HWRST_COOL_STEPS - 1) {
        s_hwrst_step++;
        // 复位没唤醒 = die 还没缓过来，这时退避才有意义。日志里带上档位，
        // 下次看串口就能判断到底是"一直没动手"还是"动手了但叫不醒"。
        ESP_LOGW(TAG, "[%s] 整片复位未唤醒触摸，退避 %dms",
                 why, k_hwrst_cool_ms[s_hwrst_step]);
    }
    // 复位后还有静默期，取两者中较晚的那个时刻才允许下一次尝试
    TickType_t next = now + pdMS_TO_TICKS(TP_QUIET_AFTER_RST_MS);
    TickType_t cool = s_last_hwrst_tick + pdMS_TO_TICKS(k_hwrst_cool_ms[s_hwrst_step]);
    s_next_recover_tick = ((int32_t)(next - cool) > 0) ? next : cool;
    return false;
}

/**
 * @brief 读一次触摸
 * @param raw_x/raw_y 芯片原始坐标（有触点时有效）
 * @param pressed     是否有触点
 * @param stale       本次的"没有触点"是芯片重复吐旧帧造成的（抬手未确认）
 * @param lifted      这一帧是芯片明确报的"抬手事件"（event=1）：坐标有效，但手指已经抬起
 * @return I2C 读取结果（报文异常返回 ESP_ERR_INVALID_RESPONSE）
 */
static esp_err_t tp_poll(uint16_t *raw_x, uint16_t *raw_y, bool *pressed, bool *stale, bool *lifted)
{
    uint8_t buf[TP_READ_LEN] = {0};
    if (stale) {
        *stale = false;   // 出错路径上也要有确定值，调用方不必先清
    }
    if (lifted) {
        *lifted = false;
    }
    esp_err_t err = tp_raw_read(buf, sizeof(buf));

    // 偶发卡帧第一步：原样再读一次，不动总线。
    // 绝大多数情况（尤其是刚从刷屏空窗里抢到的那一帧）当场就恢复了，
    // 而且不去折腾从机——不然要等到攒够 300ms 失败才走恢复流程，按键是没反应的。
    if (err != ESP_OK || tp_msg_broken(buf, TP_BUF_LEN)) {
        memset(buf, 0, sizeof(buf));
        err = tp_raw_read(buf, sizeof(buf));
    }

    // 第二步：还不行才复位总线（把被从机拽住的 SDA 放开）再读一次。
    // 关键：只在"真·总线错误"（NACK/超时，从机可能把 SDA 拽住了）时复位。
    // 读回整包相同字节（1C/1E）说明总线是通的、从机正常应答，坏的是 die 内部的
    // 触摸引擎 —— 复位总线治不了这个，反复复位反而会把它往更死的状态摁，
    // 而且每 300ms 摁一次会让它永远缓不过来（日志里"刚恢复又掉线"就是这么来的）。
    if (err != ESP_OK) {
        static TickType_t s_last_bus_reset = 0;
        TickType_t now = xTaskGetTickCount();
        if (s_last_bus_reset == 0 ||
            (now - s_last_bus_reset) >= pdMS_TO_TICKS(300)) {
            s_last_bus_reset = now;
            if (board_i2c_touch()) {
                i2c_master_bus_reset(board_i2c_touch());
            }
            memset(buf, 0, sizeof(buf));
            err = tp_raw_read(buf, sizeof(buf));
            if (err != ESP_OK) {
                return err;
            }
        }
    }

    // 整包同一个字节（0x1C 这类）= 这一帧根本没拿到触摸数据。
    // 必须报错，否则上层会把它当成"当前没有触点"，永远等不到手势，
    // 也就永远不触发恢复流程。
    if (tp_msg_broken(buf, TP_BUF_LEN)) {
        // 记下"卡死签名"：软复位救不回来，恢复流程见到它就直奔整片复位，
        // 不再浪费几百毫秒去挨个试速率。
        s_tp_stuck_sig = true;
        return ESP_ERR_INVALID_RESPONSE;
    }
    s_tp_stuck_sig = false;   // 这一帧读通了，卡死状态已解除
    *pressed = false;
    // 触点数取低 4 位：高 4 位个别固件会用来放状态标志，整字节比较会导致永远读不到点
    uint8_t num = buf[1] & 0x0F;
    if (num == 0 || num > 2) {
        s_same_pkt_since = 0;   // 没有触点时报文本来就是恒定的，不能算卡帧
        return ESP_OK;   // 芯片正常应答，只是当前没有触点
    }

    // 整包逐字节与上一帧完全相同：这**不是**通信故障，而是"手指没动"的正常现象
    // （按钮这种完全静止的按压，报上来的坐标可以连续几百毫秒一字不差）。
    // 以前 8 帧就判成卡帧并触发整套恢复，结果一次普通点击就能把链路打成 1C 1C 1C，
    // 之后连着好几秒点不动 —— 这就是"连续点击不响应"的直接来源。
    // 现在：一律当合法采样继续用；只有持续到 TP_STALE_MS 以上（真手指不可能
    // 一字不差这么久）才回报成"没有触点"并打上 stale 标记，让上层结束这次按压
    // 但**不认定抬手**；再持续到 TP_FROZEN_MS 就判成引擎冻死，升级整片复位。
    TickType_t now = xTaskGetTickCount();
    if (memcmp(buf, s_last_tp_pkt, TP_BUF_LEN) == 0) {
        if (s_same_pkt_since == 0) {
            s_same_pkt_since = now;   // 第一帧不变的时刻
        }
        TickType_t same = now - s_same_pkt_since;
        if (s_tp_frozen || same >= pdMS_TO_TICKS(TP_FROZEN_MS)) {
            if (!s_tp_frozen) {
                ESP_LOGW(TAG, "触摸报文 %dms 一字不差，判定触摸引擎冻死，升级整片复位",
                         TP_FROZEN_MS);
                s_tp_frozen = true;
            }
            s_tp_stuck_sig = true;   // 软握手救不了：恢复流程直奔整片复位
            return ESP_ERR_INVALID_RESPONSE;
        }
        if (same >= pdMS_TO_TICKS(TP_STALE_MS)) {
            if (stale) {
                *stale = true;   // *pressed 已经是 false
            }
            return ESP_OK;
        }
    } else {
        memcpy(s_last_tp_pkt, buf, TP_BUF_LEN);
        s_same_pkt_since = 0;
        s_tp_frozen = false;   // 报文变了：冻死状态（如果真的发生过）已解除

        // 帧变了才有可能是一次新的触摸事件。event 定义（AXS15231B 触摸协议）：
        //   0 = 按下   1 = 抬起   2 = 持续接触
        // buf[2] 的高 2 位就是 event，低 4 位是 X 的高 4 位。
        // 若某批屏的 event 编码不同（症状：日志里每次按压刚开始就报 [抬手事件]、
        // 或者滑动判不出来），把 TP_EVENT_UP 改成对应值，或设成 -1 关掉这条快路径。
        //
        // 这个字节是"快速点击不跟手"的解药：手指抬起时芯片常常只发一帧
        // event=1（触点数还是 1、坐标还是最后位置），之后既不报 0 触点、
        // 也不再更新，于是上层只能等 1.5s 的"重复帧"或 600ms 的"按住不动"才收尾
        // —— 表现就是"轻点没反应，必须长按"。
        // 现在直接认这一帧：坐标仍然参与投票，抬手当场成立，点击立刻下发。
        //
        // 只认"变化过"的帧：同一帧重复吐（芯片卡住）不算新的抬手，
        // 否则每重复一次就当成一次抬手 -> 一次按压被拆成好几次点击。
        if (lifted && ((buf[2] >> 6) & 0x03) == TP_EVENT_UP) {
            *lifted = true;
        }
    }

    *raw_x = (uint16_t)(((buf[2] & 0x0F) << 8) | buf[3]);
    *raw_y = (uint16_t)(((buf[4] & 0x0F) << 8) | buf[5]);

    // 越界坐标一律当噪声丢掉。芯片在通信异常期（1E 1E 1E 前后）会吐随机坐标，
    // 以前把它们 clamp 到屏幕边界上，于是凭空出现跨越整屏的假触点：
    // 假点击、假滑动（"点按钮变成切页"就是这么来的）。
    if (*raw_x >= SCR_W || *raw_y >= SCR_H) {
        *pressed = false;
        if (lifted) {
            *lifted = false;   // 坐标都不可信，抬手事件也不能拿来结算点击
        }
        return ESP_OK;
    }
    *pressed = true;
    return ESP_OK;
}

static esp_err_t touch_hw_init(void)
{
    ESP_LOGI(TAG, "探测触摸芯片 0x%02X ...", BOARD_TOUCH_ADDR);
    if (!touch_probe()) {
        ESP_LOGW(TAG, "触摸芯片 0x%02X 无响应，禁用触摸（不影响显示与蓝牙）", BOARD_TOUCH_ADDR);
        s_tp_dev = NULL;
        return ESP_ERR_NOT_FOUND;
    }

    // 只读扫描触摸总线（SDA17/SCL18），列出应答的地址：
    // 只有 0x3B 应答属正常；一堆地址都应答说明总线电气/线序有问题。
    // 这里只做 probe，不向任何设备写数据，避免影响其它器件。
    {
        char found[64] = {0};
        for (uint8_t addr = 0x08; addr < 0x78; addr++) {
            if (i2c_master_probe(board_i2c_touch(), addr, 20) == ESP_OK) {
                char one[8];
                snprintf(one, sizeof(one), " %02X", addr);
                strncat(found, one, sizeof(found) - strlen(found) - 1);
            }
        }
        ESP_LOGI(TAG, "触摸总线设备:%s", found[0] ? found : " 无");
    }

    esp_err_t ret = tp_set_speed(TP_I2C_HZ);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "触摸设备挂载失败: %s", esp_err_to_name(ret));
        s_tp_dev = NULL;
        return ret;
    }

    // 立刻读一帧并把原始字节打出来：这一行能直接区分
    // "I2C 读不到" / "读到了但触点为 0" / "报文格式与预期不同"
    uint8_t buf[TP_READ_LEN] = {0};
    ret = tp_raw_read(buf, sizeof(buf));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "触摸原始读取失败: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "触摸原始报文: %02X %02X %02X %02X %02X %02X %02X %02X | %02X %02X %02X %02X %02X %02X",
             buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7],
             buf[8], buf[9], buf[10], buf[11], buf[12], buf[13]);

    // 报文整包相同说明通信还没建立，先握一轮手（这块屏实测必须做）；
    // 握手也救不回来就直接整片复位一次。
    if (tp_msg_broken(buf, TP_BUF_LEN) && !tp_wakeup("上电")) {
        touch_hard_recover("上电");
    }

    return ESP_OK;
}

// ============ 刷屏 ============

/**
 * @brief 把 PSRAM 帧缓存按条带推送到面板
 *
 * 必须“一条发完再发下一条”，与官方例程完全一致：
 * AXS15231B 的 QSPI 驱动只在 y_start==0 时发 RAMWR(0x2C)，之后全部走
 * RAMWRC(0x3C) 连续写，并且不发 RASET(0x2B)。也就是说每一块的落地位置
 * 完全依赖面板内部的“写指针继续往后走”。
 * 一旦两条传输并发/乱序（比如双缓冲），写指针顺序错乱，屏幕就会花屏或空白。
 *
 * 正因为“靠写指针顺序推进”，驱动要求每块宽度必须等于面板物理宽度（172）。
 * 所以横屏不能用 MADCTL 的 MV 位交给硬件旋转（CASET 会变成 0..639，
 * 超过物理列数，整屏被折成横线），只能先把逻辑画面转置成物理布局再推。
 */
static void flush_frame(void)
{
    // 触摸正在恢复：这一帧不推。此时推屏会和触摸 I2C 读抢同一颗 die，
    // 刚复位的触摸引擎会被再次打死（详见 s_tp_recovering 的说明）。
    // 保持 s_dirty，恢复结束后的下一轮会自然补画这一帧。
    if (s_tp_recovering) {
        s_dirty = true;
        return;
    }

    // 触摸 I2C 与 QSPI 推屏共用一颗 die，必须互斥。这里改成"整帧一锁到底"：
    // 之前每条带取放一次锁，会让触摸任务（优先级更高）在条带之间插进来读 I2C，
    // 而此刻 die 正在收尾上一条 RAMWR —— 触摸引擎被打死、之后一直回 1C 1C 1C，
    // 正是"连续点击按钮不生效"的根因。整帧锁住后触摸只在推屏前/后（配合静默窗）读。
    //
    // 旋转（纯 CPU + PSRAM，没有 QSPI 流量）仍放在锁外，触摸可以照常读。
    s_flush_active = true;

#if BOARD_LCD_LANDSCAPE
    // 逻辑 (lx, ly) -> 物理 (px = 171 - ly, py = lx)：顺时针旋转 90°
    const int LW = SCR_W;                    // 640
    const int LH = SCR_H;                    // 172
    const int PW = BOARD_LCD_PANEL_W;        // 172

    // 按 ROT_TILE x ROT_TILE 分块转置。两块缓冲都在 PSRAM 上，
    // 逐像素跨步写会让几乎每个 16 位写都触发一次 cache line 填充（一次整屏旋转几十毫秒）。
    // 分块后读写各自只占用 16 条 line 并被反复命中，旋转开销降一个数量级，
    // 直接缩短刷屏时间、给触摸轮询腾出空窗。
    // 注意：旋转期间没有 QSPI 流量，不算"刷屏忙"，触摸可以照常读。
    for (int ty = 0; ty < LH; ty += ROT_TILE) {
        int ty_end = ty + ROT_TILE;
        if (ty_end > LH) {
            ty_end = LH;
        }
        for (int tx = 0; tx < LW; tx += ROT_TILE) {
            int tx_end = tx + ROT_TILE;
            if (tx_end > LW) {
                tx_end = LW;
            }
            for (int ly = ty; ly < ty_end; ly++) {
                const uint16_t *src = s_fb + (uint32_t)ly * LW;
                int px = LH - 1 - ly;
                for (int lx = tx; lx < tx_end; lx++) {
                    uint16_t v = src[lx];
                    s_rot[(uint32_t)lx * PW + px] = (uint16_t)((v >> 8) | (v << 8));  // 面板要求大端
                }
            }
        }
    }
    const uint16_t *panel_fb = s_rot;
    const int panel_w = PW;
    const int panel_h = BOARD_LCD_PANEL_H;
#else
    const uint16_t *panel_fb = s_fb;
    const int panel_w = SCR_W;
    const int panel_h = SCR_H;
#endif

    for (int y = 0; y < panel_h; y += FLUSH_LINES) {
        // 恢复是在刷屏中途开始的（条带之间是放锁的，触摸任务随时可能被唤醒）：
        // 立刻收手，把 die 完整让给触摸。下一帧从 y=0 重新推，写指针不会错乱。
        if (s_tp_recovering) {
            break;
        }
        int lines = panel_h - y;
        if (lines > FLUSH_LINES) {
            lines = FLUSH_LINES;
        }
        int pixels = panel_w * lines;

        uint16_t *dst = s_dma_buf[0];
#if BOARD_LCD_LANDSCAPE
        // 旋转缓冲里已经是物理布局且字节序已换好，直接连续搬运
        memcpy(dst, panel_fb + (uint32_t)y * panel_w, (size_t)pixels * sizeof(uint16_t));
#else
        const uint16_t *src = panel_fb + (uint32_t)y * panel_w;
        for (int i = 0; i < pixels; i++) {
            uint16_t v = src[i];
            dst[i] = (uint16_t)((v >> 8) | (v << 8));
        }
#endif

        // 锁只包住"这一条带真正往面板推数据"的那一小段。
        // 条带之间锁是放开的，触摸任务（优先级更高）会立刻拿到锁把这帧读完，
        // 于是刷屏期间触摸依然采得到点，又绝不会出现"读 I2C 的同时在推 QSPI"。
        bus_lock();

        // 清掉可能残留的完成信号，保证下面等到的是本次传输
        xSemaphoreTake(s_dma_sem, 0);

        esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, 0, y, panel_w, y + lines, dst);
        if (err != ESP_OK) {
            // 这条带没推进面板写指针，继续推后面的条带会把数据错位写到同一批行上，
            // 屏幕出现固定错行（花屏）。出错就停，让下一帧重新从头推。
            ESP_LOGW(TAG, "draw_bitmap 失败: %s (y=%d)，放弃本次刷新", esp_err_to_name(err), y);
            if (s_lcd_bus_lock) {
                xSemaphoreGive(s_lcd_bus_lock);
            }
            break;
        }
        if (xSemaphoreTake(s_dma_sem, pdMS_TO_TICKS(1000)) != pdTRUE) {
            ESP_LOGW(TAG, "等待 QSPI 传输完成超时 (y=%d)，放弃本次刷新", y);
            if (s_lcd_bus_lock) {
                xSemaphoreGive(s_lcd_bus_lock);
            }
            break;
        }

        bus_unlock();
    }

    // 刷完最后一条带之后，die 内部还要把这次 RAMWR 收尾（触摸和显示是同一颗 die）。
    // 这段收尾期里立刻去读触摸 I2C，它的触摸引擎往往还没被调度回来，
    // 会直接回 1C 1C 1C 并卡在"一直回 1C"的状态 —— 之后就只剩整片复位能救。
    // 因此给一个很短的静默窗：代价是触摸少采一帧，换来的是不会莫名把链路打死。
    // （复位的 600ms 静默窗更大，用 max 保住那一段。）
    TickType_t guard = xTaskGetTickCount() + pdMS_TO_TICKS(TP_FLUSH_QUIET_MS);
    if (s_tp_quiet_until == 0 || (int32_t)(guard - s_tp_quiet_until) > 0) {
        s_tp_quiet_until = guard;
    }

    s_flush_active = false;
}

/**
 * @brief 开机显示自检：整屏刷 红→绿→蓝，确认 QSPI 数据通路与背光都正常
 * @note 想关掉它，把下面的 LCD_BOOT_COLOR_TEST 改成 0
 */
#define LCD_BOOT_COLOR_TEST 1

static void lcd_color_selftest(void)
{
#if LCD_BOOT_COLOR_TEST
    static const uint16_t colors[] = {0xF800, 0x07E0, 0x001F};  // 红 / 绿 / 蓝
    const uint32_t px = (uint32_t)SCR_W * SCR_H;

    for (int i = 0; i < 3; i++) {
        for (uint32_t n = 0; n < px; n++) {
            s_fb[n] = colors[i];
        }
        flush_frame();
        vTaskDelay(pdMS_TO_TICKS(200));
        ESP_LOGW(TAG, "显示自检: 颜色 %d (%s)", i, i == 0 ? "红" : (i == 1 ? "绿" : "蓝"));
    }
    ESP_LOGW(TAG, "显示自检结束，若没看到红/绿/蓝，说明是背光或面板硬件通路问题");
#else
    (void)s_fb;
#endif
}

/** 直接操作背光硬件（不依赖渲染任务） */
static void backlight_hw(bool on)
{
    if (on) {
        board_backlight_enable(true);
        gpio_set_level(board_backlight_gpio(), BOARD_LCD_BL_ON_LEVEL);
    } else {
        gpio_set_level(board_backlight_gpio(), !BOARD_LCD_BL_ON_LEVEL);
        board_backlight_enable(false);
    }
    ESP_LOGW(TAG, "背光硬件 -> %s", on ? "开" : "关");
}

// ============ 状态更新 ============

/*
 * 表盘盘心显示状态机：
 *   档位发生变化 -> 临时显示档位 5 秒；其余时间（含 5 秒到期后）显示实时速度。
 * 速度不做事件判定：每帧直接取 s_state.drive.speed_kmh，速度变化自然实时反映。
 * 这两个变量由业务任务（set_drive）写、渲染任务读，统一用 lock_state() 保护。
 */
#define CENTER_GEAR_HOLD_TICKS pdMS_TO_TICKS(5000)
static char s_last_gear;                     // 上一次收到的有效档位（0 = 还没收到过）
static TickType_t s_gear_hold_until_tick;    // 档位提示到期时刻（tick）

static void mark_dirty(void)
{
    s_dirty = true;
    if (s_render_task) {
        xTaskNotifyGive(s_render_task);
    }
}

static void lock_state(void)
{
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

static void unlock_state(void)
{
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
}

// 状态里的字符串字段一律按这个规则写：传 NULL 表示清空，超长则截断并补终止符
static void set_str_field(char *dst, size_t cap, const char *src)
{
    if (src) {
        strncpy(dst, src, cap - 1);
        dst[cap - 1] = '\0';
    } else {
        dst[0] = '\0';
    }
}

void lcd_display_set_link(lcd_link_state_t state)
{
    lock_state();
    s_state.link = state;
    unlock_state();
    mark_dirty();
}

void lcd_display_set_drive(const lcd_drive_info_t *info)
{
    if (!info) {
        return;
    }
    lock_state();
    // 只有档位真的变了才开 5 秒提示；未连上车、或档位为 '-' 的无效读数不触发，
    // 否则断连抖动会弹出一次假档位。上电后第一次拿到有效档位也算一次变化：
    // 先显示 5 秒当前档位，再切到实时速度。
    char g = info->valid ? info->gear : 0;
    if (g && g != '-' && g != s_last_gear) {
        s_gear_hold_until_tick = xTaskGetTickCount() + CENTER_GEAR_HOLD_TICKS;
        s_state.center_show_gear = true;
    }
    s_last_gear = g;
    s_state.drive = *info;
    unlock_state();
    mark_dirty();
}

void lcd_display_set_vin(const char *vin)
{
    lock_state();
    set_str_field(s_state.vin, sizeof(s_state.vin), vin);
    unlock_state();
    mark_dirty();
}

void lcd_display_set_paired(bool paired)
{
    lock_state();
    s_state.paired = paired;
    unlock_state();
    mark_dirty();
}

void lcd_display_set_hint(const char *hint)
{
    lock_state();
    set_str_field(s_state.hint, sizeof(s_state.hint), hint);
    unlock_state();
    mark_dirty();
}

void lcd_display_set_setup_mode(bool enable, const char *ap_ssid)
{
    lock_state();
    s_state.setup_mode = enable;
    if (ap_ssid) {
        set_str_field(s_state.ap_ssid, sizeof(s_state.ap_ssid), ap_ssid);
    }
    unlock_state();
    mark_dirty();
}

void lcd_display_set_backlight(bool on)
{
    if (s_bl_on == on) {
        return;
    }
    s_bl_on = on;
    backlight_hw(on);
    if (on) {
        mark_dirty();
    }
    ESP_LOGI(TAG, "背光 %s", on ? "开" : "关");
}

bool lcd_display_backlight_is_on(void)
{
    return s_bl_on;
}

void lcd_display_set_mode(lcd_ui_mode_t mode)
{
    if (s_mode == mode) {
        return;
    }
    s_mode = mode;
    lock_state();
    s_state.mode = mode;
    // 切页时结束未播完的按下动效，避免回到钥匙页时残留"按下去"的样子
    s_key_press_stage = KEY_PRESS_NONE;
    s_state.key_press = -1;
    unlock_state();
    mark_dirty();
    ESP_LOGI(TAG, "界面模式 -> %s", mode == LCD_MODE_KEY ? "车钥匙" : "仪表盘");

    // 与状态更新分开，业务层在这里落盘；未注册回调时没有额外开销
    if (s_mode_change_cb) {
        s_mode_change_cb(mode);
    }
}

void lcd_display_set_mode_change_cb(void (*cb)(lcd_ui_mode_t mode))
{
    s_mode_change_cb = cb;
}

lcd_ui_mode_t lcd_display_get_mode(void)
{
    return s_mode;
}

int lcd_display_take_key_slot(void)
{
    // 从队列取一个：业务层每次取走一条，连点产生的多条动作按顺序依次送达，
    // 不会像以前那样被下一次点击直接覆盖掉。
    if (s_key_q_tail == s_key_q_head) {
        return -1;
    }
    int slot = s_key_q[s_key_q_tail];
    portMEMORY_BARRIER();   // 先取走数据再推进下标
    s_key_q_tail = (uint8_t)((s_key_q_tail + 1) % KEY_SLOT_QUEUE_LEN);
    return slot;
}

void lcd_display_set_cells(const char *names[LCD_CELL_COUNT],
                           const char *values[LCD_CELL_COUNT],
                           const uint8_t tones[LCD_CELL_COUNT])
{
    lock_state();
    if (!names || !values) {
        s_state.cells_custom = false;
    } else {
        s_state.cells_custom = true;
        for (int i = 0; i < LCD_CELL_COUNT; i++) {
            snprintf(s_state.cell_name[i], sizeof(s_state.cell_name[i]), "%s",
                     names[i] ? names[i] : "");
            // 空字符串也要兜底：字库缺字或指标返回空文本时，直接画空串会
            // 留下一个看起来"没数据"的空白格子，比 "--" 更难排查。
            snprintf(s_state.cell_value[i], sizeof(s_state.cell_value[i]), "%s",
                     (values[i] && values[i][0]) ? values[i] : "--");
            s_state.cell_tone[i] = tones ? tones[i] : 0;
        }
    }
    unlock_state();
    mark_dirty();
}

void lcd_display_set_key_buttons(const uint8_t icons[LCD_KEY_SLOTS],
                                 const char *labels[LCD_KEY_SLOTS])
{
    lock_state();
    for (int i = 0; i < LCD_KEY_SLOTS; i++) {
        s_state.key_icon[i] = icons ? icons[i] : (uint8_t)LCD_ICON_GENERIC;
        snprintf(s_state.key_label[i], sizeof(s_state.key_label[i]), "%s",
                 (labels && labels[i]) ? labels[i] : "");
    }
    unlock_state();
    mark_dirty();
}

uint32_t lcd_display_take_double_tap(void)
{
    uint32_t n = s_double_taps;
    s_double_taps = 0;
    return n;
}

void lcd_display_refresh(void)
{
    mark_dirty();
}


// ============ 任务 ============

/** 扫描等待页有旋转动画，需要按固定节拍重绘（其余页面只在数据变化时重绘） */
static bool wait_page_animating(void)
{
    if (s_state.setup_mode) {
        return false;
    }
    // 车钥匙页是静态页面（没有转圈动画），不需要按节拍连续刷屏。
    // 断连时若还按 link 状态判动画，钥匙页会每 100ms 推满一帧，QSPI 与触摸 I2C
    // 在同一颗 die 上反复撞车，正是"点按钮后触摸卡死"的帮凶之一。
    if (s_state.mode == LCD_MODE_KEY) {
        return false;
    }
    // 触摸链路坏着的时候，转圈动画一律停掉（不是降速，是停）。
    // 转圈是整屏刷新：一帧 150~200ms 的 QSPI，而触摸和显示在同一颗 die 上。
    // 只要还在推屏，触摸引擎就一帧都回不来 —— 实测开机后连续 2 分钟
    // "每 2.5~4 秒整片复位一次、每次都是 1E 1E 1E"，就是动画一直把 die 占着。
    // 停掉之后 die 才有完整空窗，一次复位就能把触摸拉回来。
    if (s_fail_since != 0) {
        return false;
    }
    // 刚整片复位过：再给一段安静期，别刚救回来又被刷屏打死
    if (s_last_hwrst_tick != 0 &&
        (xTaskGetTickCount() - s_last_hwrst_tick) < pdMS_TO_TICKS(TP_ANIM_QUIET_AFTER_RST_MS)) {
        return false;
    }
    switch (s_state.link) {
    case LCD_LINK_DISCONNECTED:
    case LCD_LINK_SCANNING:
    case LCD_LINK_CONNECTING:
    case LCD_LINK_PAIRING:
        return true;
    default:
        return false;
    }
}

/**
 * @brief 当前允许的最小刷屏间隔
 *
 * 触摸链路正处于故障期时进一步拉大间隔。断连等待页的转圈动画平时每 100ms 就要
 * 推满一帧，而一帧 QSPI 本身占 40~60ms，die 几乎没有喘息窗口 —— 这段时间里
 * 触摸 I2C 一直在跟推屏抢同一颗 die，结果是"越修越死"：每一轮整片复位都叫不醒。
 * 触摸坏了的时候先把动画降到 4fps，把 die 让给触摸；好了就立刻恢复原节奏。
 */
static uint32_t min_refresh_interval_ms(void)
{
    return (s_fail_since != 0) ? 250 : MIN_REFRESH_INTERVAL_MS;
}

/**
 * @brief 用户是不是"正在用屏幕"（手指还压着 / 刚刚才采到触点）
 *
 * 用来决定"这一帧转圈动画要不要推"。断连等待页的转圈是按节拍连续整屏刷新的，
 * 而这块屏的触摸和显示在同一颗 die 上：一帧要转置整屏 + 推 10 条 QSPI 条带
 * （实测一帧 150~200ms），这段时间触摸采到的帧会被挤掉。
 * 实测症状就是采样间隔被拉到 180~270ms：一次滑动只有 1~2 个点，
 * "同向 3 步"永远凑不够（日志里"横向 230..230 跨度 0、同向 0 步"），
 * 而且每个点之间都超过 GESTURE_GAP_MS，被当成一次次独立按压 —— 表现就是
 * "滑几次之后手势全部失效、怎么点都没反应"，也就是触摸卡死。
 *
 * 所以手指在屏幕上时让动画停几帧（转圈停一下没人看得出来），
 * 把 die 完整让给触摸；手指离开 600ms 后动画自动继续。
 */
static bool touch_busy(void)
{
    if (s_press_active) {
        return true;
    }
    if (s_last_sample_tick == 0) {
        return false;
    }
    return (xTaskGetTickCount() - s_last_sample_tick) < pdMS_TO_TICKS(TOUCH_BUSY_HOLD_MS);
}

static void render_task(void *arg)
{
    while (true) {
        // 动画期间用更短的等待间隔，保证转圈跟手；静止时可以放心睡久一点
        bool animating = wait_page_animating() || key_press_animating();
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(animating ? 40 : 500));

        if (!s_bl_on) {
            continue;
        }

        // 档位 5 秒提示到期：上层可能已经不再推数据（车停着、或断连后停在仪表页），
        // 这里主动补一帧切回实时速度，否则盘心会一直停在最后一次的档位字母上。
        bool gear_expired = false;
        lock_state();
        if (s_state.center_show_gear &&
            (int32_t)(xTaskGetTickCount() - s_gear_hold_until_tick) >= 0) {
            s_state.center_show_gear = false;
            gear_expired = true;
        }
        unlock_state();
        if (gear_expired) {
            mark_dirty();
        }

        if (!s_dirty && !(wait_page_animating() || key_press_animating())) {
            continue;
        }

        // 手指还在屏幕上：这一帧如果纯粹是为了推进转圈动画，就先不推。
        // 刷满一帧要把 die 占住 150~200ms，期间触摸采不到点（见 touch_busy 的说明），
        // 手势会被拆成一堆 1~2 个采样点的"假按压"。状态有变化（s_dirty）时照常画，
        // 界面上的反馈不能因为手指压着就延迟。
        // 按钮按下动效例外：按住不放时按钮就应该一直是"压下去"的样子，
        // 手指抬起后的回弹帧照常出（那时 touch_busy 已经过去了）。
        if (!s_dirty && touch_busy() && !key_press_animating()) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        // 限制刷新频率
        TickType_t now = xTaskGetTickCount();
        if (s_last_flush != 0) {
            int32_t elapsed = (int32_t)(now - s_last_flush) * 1000 / configTICK_RATE_HZ;
            uint32_t min_gap = min_refresh_interval_ms();
            if (elapsed < (int32_t)min_gap) {
                vTaskDelay(pdMS_TO_TICKS(min_gap - elapsed));
            }
        }

        if (!s_bl_on) {
            continue;
        }
        s_dirty = false;
        s_last_flush = xTaskGetTickCount();

        lock_state();
        // 等待页转圈的角度按时间推进：约 4 秒一圈
        s_state.spin = (uint16_t)((xTaskGetTickCount() * 1000 / configTICK_RATE_HZ / 11) % 360);
        // 车钥匙按钮的按下动效：按帧推进（按下一帧 / 弹回一帧）
        key_press_step();
        ui_state_t snapshot = s_state;
        unlock_state();

        ui_render(&s_canvas, &snapshot);
        flush_frame();

    }
}

/**
 * @brief 把一次单击落到具体动作上
 *
 * 钥匙页：命中四个按钮就下发对应车钥匙指令（返回仪表盘用左右滑动），
 *         没命中就打一条日志（否则"点了没反应"完全没法定位）。
 * 仪表页：轻触不再切页（仪表盘只有驾驶页一屏），避免滑动没识别成手势时
 *         被当成单击翻到别处。
 */
static void handle_single_tap(int x, int y)
{
    if (s_mode != LCD_MODE_KEY) {
        // 仪表盘没有需要单击触发的元素，模式切换一律走左右滑动
        return;
    }
    int idx = ui_key_button_hit(x, y);
    if (idx >= 0 && idx < LCD_KEY_SLOTS) {
        key_slot_push(idx);
        ESP_LOGI(TAG, "按键槽位 %d (%d,%d)", idx, x, y);
    } else {
        ESP_LOGW(TAG, "钥匙页点击 (%d,%d) 没命中任何按钮", x, y);
    }
}

/**
 * @brief 用"本次按压内各按钮的命中票数"决定点了哪个按钮
 *
 * 触摸芯片在通信异常期会吐随机坐标。以前拿最后一个点做命中测试，
 * 一个噪声点就会按到隔壁按钮（表现为"点 A 出 B"，时灵时不灵）。
 * 改成投票：真实落点会被反复命中，零散噪声点永远占不到多数。
 *
 * 还要求**过半数**：手指压在两块按钮交界处、或滑动途中划过两块按钮时，
 * 票数会被摊薄；这种"说不清点的哪个"的情况宁可不下发，也不能发错车控指令。
 */
static void fire_key_tap(void)
{
    int best = -1;
    uint32_t best_n = 0;
    uint32_t total = 0;
    for (int i = 0; i < LCD_KEY_SLOTS; i++) {
        total += s_hit_votes[i];
        if (s_hit_votes[i] > best_n) {
            best_n = s_hit_votes[i];
            best = i;
        }
    }
    if (best >= 0 && best_n * 2 <= total) {
        ESP_LOGW(TAG, "按键命中不明确(最多 %u 票 / 共 %u 票)，忽略本次点击",
                 (unsigned)best_n, (unsigned)total);
        return;
    }
    if (best >= 0) {
        key_slot_push(best);
        ESP_LOGI(TAG, "按键槽位 %d (票数 %u, 末点 %d,%d)",
                 best, (unsigned)best_n, s_last_x, s_last_y);
    } else {
        // 一次有效命中都没有（极短的一次点击，只有一帧采样）：用最后落点兜底
        handle_single_tap(s_last_x, s_last_y);
    }
}

/** 结束一次按压（不管有没有产生动作），并清掉与本次按压相关的统计 */
static void press_end(void)
{
    s_press_active = false;
    s_seq_samples = 0;
    s_run_dir = 0;
    s_run_len = 0;
    s_press_dx_max = 0;
    s_press_dy_max = 0;
    memset(s_hit_votes, 0, sizeof(s_hit_votes));
}

/**
 * @brief 一次按压开始（按下沿）
 *
 * 落点记为第 1 个采样点，但它有可能是上一次按压留下的旧坐标，
 * 所以第 2 个点到达时会重新起算基准（见 touch_task）。
 */
/** 以当前点为新基准重新起算滑动（保留按压本身，只丢弃位移历史） */
static void swipe_rebase(int lx, int ly)
{
    s_seq_xfirst = lx;
    s_seq_yfirst = ly;
    s_seq_xmin = lx;
    s_seq_xmax = lx;
    s_seq_ymin = ly;
    s_seq_ymax = ly;
    // s_last_lx/ly 必须一起重置：它是下一帧 step_x 的基准，
    // 留在旧坐标上会因为一帧里凭空多出几百像素而打乱同向步数统计
    s_last_lx = lx;
    s_last_ly = ly;
    s_prev_dx = 0;
    s_prev_dy = 0;
}

static void press_begin(int lx, int ly)
{
    TickType_t now = xTaskGetTickCount();
    s_press_active = true;
    s_action_taken = false;
    s_press_start_tick = now;
    s_action_tick = now;
    // "静止计时"从按下这一刻开始：手指一直不动就不会再刷新它
    s_last_move_tick = now;
    s_seq_samples = 1;
    swipe_rebase(lx, ly);
    s_press_x0 = lx;
    s_press_y0 = ly;
    s_press_dx_max = 0;
    s_press_dy_max = 0;
    s_press_dx_prev = 0;
    s_press_dy_prev = 0;
    s_run_dir = 0;
    s_run_len = 0;
    memset(s_hit_votes, 0, sizeof(s_hit_votes));
}

/**
 * @brief 重新进入"等基线"状态（上电、整片复位之后调用）
 *
 * 复位/上电后芯片的触摸基线要重新学习，这段时间报上来的"有触点"不可信。
 * 在收到第一帧干净的"无触点"报文之前，一律不建立按压（见 touch_task 里的判据）。
 */
static void touch_disarm(void)
{
    s_tp_armed = false;
    s_tp_arm_since = xTaskGetTickCount();
}

/**
 * 丢弃当前手势状态（触摸通信恢复之后，旧的状态一律作废）
 * @param rearm_baseline 是否重新学触摸基线。只有"整片复位"才需要：
 *        软握手只是重置 ESP 侧总线/换速率，die 的触摸基线没变；
 *        空跑的恢复（宽限期、退避）更是一次都没动过芯片。
 *        以前每轮恢复都调 touch_disarm()，等于在故障期反复把"等基线"
 *        倒计时清零 —— 恢复之后最长 2.5s 内的真实触摸全被当噪声丢掉，
 *        表现就是"偶发点不动"。
 */
static void gesture_reset(bool rearm_baseline)
{
    press_end();
    if (rearm_baseline) {
        touch_disarm();
    }
    s_prev_dx = 0;
    s_prev_dy = 0;
    s_tap_pending = false;
    s_tap_wake = false;
    s_tap_blocked = false;
}

static void touch_task(void *arg)
{
    uint32_t no_touch = 0;        // 连续读不到触点的轮询次数
    uint32_t flush_miss = 0;      // 没等到刷屏空窗的次数（诊断用）
    TickType_t idle_log_tick = 0; // 上次打"长时间无触点"日志的时刻
    int slow_read = 0;            // 连续"读得到但被拖慢"的轮次（见 TP_SLOW_READ_MS）
    TickType_t slow_since = 0;    // 这轮"慢读"是从什么时候开始的

    if (!s_tp_dev) {
        ESP_LOGE(TAG, "触摸设备未就绪，手势功能不可用（检查触摸初始化日志）");
    } else {
        ESP_LOGI(TAG, "触摸轮询已启动（活动中 %d ms / 空闲 %d ms）",
                 TOUCH_POLL_ACTIVE_MS, TOUCH_POLL_IDLE_MS);
    }

    // 上电后先别急着认触点：芯片基线还没稳，第一帧常带一个假触点
    touch_disarm();

    while (true) {
        TickType_t now = xTaskGetTickCount();

        // 整片复位后的"压住补画"必须有兜底出口：触摸万一一直没醒，
        // 不能让渲染任务永远停在 s_tp_recovering 上（屏幕会一直黑着不重画）。
        if (s_flush_hold_until != 0 && (int32_t)(now - s_flush_hold_until) >= 0) {
            s_flush_hold_until = 0;
            s_tp_recovering = false;
            mark_dirty();   // 超时兜底：立刻补画，别让屏幕一直空着
        }

        // 整片复位之后让芯片安静一会儿：它要重新跑触摸自检，这时候去读只会读到 1E
        if (s_tp_quiet_until != 0) {
            if (now < s_tp_quiet_until) {
                vTaskDelay(pdMS_TO_TICKS(5));
                continue;
            }
            s_tp_quiet_until = 0;
        }

        // 刚有过触点就用快速轮询：按下沿和抬手沿都只有短短一瞬，
        // 25ms 一轮时经常被整轮错过，表现就是"点了没反应"。
        bool active = s_press_active ||
                      (now - s_last_sample_tick) < pdMS_TO_TICKS(TOUCH_IDLE_AFTER_MS);
        // 通信异常时不再放慢轮询。以前降到 40ms 是为了"别锤芯片"，
        // 但那同时把抬手沿的采样密度砍掉一半 —— 一次点击只采到一两个点，
        // 抬手判定和投票全都跟着变差。真正该限速的是恢复动作，
        // 那件事现在由 s_next_recover_tick 管，采样本身越密越好。
        uint32_t poll_ms = active ? TOUCH_POLL_ACTIVE_MS : TOUCH_POLL_IDLE_MS;
        vTaskDelay(pdMS_TO_TICKS(poll_ms));

        // 设备句柄丢了（重新挂载失败过）就再挂一次，不要直接躺平
        if (!s_tp_dev) {
            if (tp_set_speed(s_tp_hz) != ESP_OK) {
                continue;
            }
            ESP_LOGI(TAG, "触摸设备重新挂载成功");
        }

        uint16_t raw_x = 0;
        uint16_t raw_y = 0;
        bool pressed = false;
        bool stale = false;   // 本次"没触点"来自芯片重复帧（抬手未确认）
        bool lifted = false;  // 芯片明确报了"抬手事件"（event=1），坐标仍然有效

        // 触摸 I2C 与 QSPI 推屏共用一颗 die，必须用同一把总线锁互斥。
        // 以前靠"刷屏就整轮跳过"来避让，连续刷屏时触摸能断将近 1 秒——这就是"时灵时不灵"的主因。
        // 现在刷屏只在每条带那一小段持锁，这里稍等一下就能拿到，读完立刻放锁（一帧 1~2ms）。
        // 注意：必须放锁之后才能做恢复动作，touch_recover() 内部也要拿这把锁。
        if (s_lcd_bus_lock) {
            if (xSemaphoreTake(s_lcd_bus_lock, pdMS_TO_TICKS(TOUCH_FLUSH_WAIT_MS)) != pdTRUE) {
                if ((++flush_miss % 100) == 0) {
                    ESP_LOGW(TAG, "已 %u 次没等到刷屏空窗（刷屏过于密集）", (unsigned)flush_miss);
                }
                continue;   // 这轮不读：不是通信故障，也不计入失败计数
            }
        }
        TickType_t t_read = xTaskGetTickCount();
        esp_err_t err = tp_poll(&raw_x, &raw_y, &pressed, &stale, &lifted);
        bus_unlock();

        // 读成功了，但一帧花掉几十上百毫秒 = 这颗 die 的触摸引擎被显示引擎挤住，
        // 每次 I2C 事务都被时钟拉伸，只是没到超时而已。这时的症状最迷惑人：
        // 日志里不报任何错误，可采样率掉到 5Hz 以下 —— 一次滑动只采到一两个点，
        // "同向 3 步"永远凑不够（"横向 230..230 跨度 0、同向 0 步"），
        // 而且每个点之间都超过 GESTURE_GAP_MS，被切成一次次独立按压。
        // 以前这种状态永远不会被当成故障，恢复流程一次都不会启动，只能重启，
        // 也就是"滑几次之后就卡死"。这里按通信异常处理（软握手救不了，走整片复位）。
        if (err == ESP_OK) {
            int cost_ms = (int)((xTaskGetTickCount() - t_read) * portTICK_PERIOD_MS);
            if (cost_ms >= TP_SLOW_READ_MS) {
                if (slow_since == 0) {
                    slow_since = t_read;
                }
                if (++slow_read >= TP_SLOW_READ_RUN &&
                    (xTaskGetTickCount() - slow_since) >= pdMS_TO_TICKS(TP_SLOW_ESCALATE_MS)) {
                    ESP_LOGW(TAG, "触摸读取被拖慢到 %dms/轮（链路通但吞吐不足，手势必然失效），"
                                  "按通信异常处理", cost_ms);
                    slow_read = 0;
                    slow_since = 0;
                    s_tp_slow_sig = true;
                    err = ESP_ERR_TIMEOUT;   // 复用下面的分级恢复
                }
            } else {
                slow_read = 0;
                slow_since = 0;
                s_tp_slow_sig = false;
            }
        }

        if (err != ESP_OK) {
            // 读失败与"整包相同字节"都会走到这里。单帧偶发错在 tp_poll 里已经靠重读吸收了，
            // 能攒到这里的都是真断了。
            //
            // 恢复节奏完全交给 s_next_recover_tick 安排（见 touch_recover 的说明）。
            // 以前是"连续失败 150ms 就调一次"，可软握手一次要 300~500ms、
            // 整片复位要 1.3s —— 触摸任务一半时间堵在恢复里，采样间隔被撑到
            // 400~700ms，手势判定拿到的全是稀疏帧，于是就"按钮点了没反应"。
            if (s_fail_since == 0) {
                s_fail_since = now;   // 这一轮故障的起点，供分级恢复判断
                // 宽限期内只读不修：这段时间是全速采样，多数 1C 会自己缓过来
                s_next_recover_tick = now + pdMS_TO_TICKS(TP_RECOVER_GRACE_MS);
            }
            if ((int32_t)(now - s_next_recover_tick) >= 0) {
                ESP_LOGW(TAG, "触摸读取持续失败(%s)，尝试恢复（检查 SDA17/SCL18）",
                         esp_err_to_name(err));
                // 整段恢复期间必须独占 die：见 s_tp_recovering 的说明。
                // 置位后渲染任务不会再推屏，恢复里的每一次 I2C 读才是"干净"的。
                s_flush_hold_until = 0;
                s_tp_recovering = true;
                bool did_hwrst = false;
                touch_recover("运行中", &did_hwrst);
                bool wake_clean = s_tp_wake_no_touch;
                s_tp_wake_no_touch = false;
                // 只有整片复位才需要重新学基线；软握手 / 空跑别把已确认的基线丢掉。
                gesture_reset(did_hwrst);
                if (did_hwrst) {
                    if (wake_clean) {
                        // 复位后读到的第一帧就是干净无触点：基线已稳，立刻放开触摸。
                        // 不再等 touch_task 自己撞上无触点帧 / 2.5s 超时 —— 那段等待
                        // 会把恢复后的头几次点击全吃掉（"偶发点不动"）。
                        s_tp_armed = true;
                        ESP_LOGI(TAG, "整片复位后首帧即无触点，触摸基线已确认");
                    }
                    // 压住 mark_dirty() 触发的整屏补画，直到触摸重新读到有效帧。
                    // 刚放开 RSTN 就推一帧 QSPI，会把正在自检的触摸引擎再打死一次，
                    // 于是"复位后 600ms 仍无触摸数据"连着出现、退避里全是死时间。
                    s_flush_hold_until = xTaskGetTickCount() + pdMS_TO_TICKS(TP_FLUSH_HOLD_MS);
                } else {
                    s_tp_recovering = false;
                }
                no_touch = 0;
            }
            continue;
        }
        s_fail_since = 0;   // 读通了：这一轮故障结束

        // 触摸已经能读到有效帧 = die 醒过来了，压住的那帧补画可以放行。
        // 正常情况下这就是复位后的下一轮轮询（10~25ms），用户几乎看不到黑屏。
        if (s_flush_hold_until != 0) {
            s_flush_hold_until = 0;
            s_tp_recovering = false;
            mark_dirty();   // 显存被复位清掉了，现在 die 已醒，立刻把画面补回来
        }

        now = xTaskGetTickCount();

        // ---- 基线确认（上电 / 整片复位之后）----
        // 收到第一帧干净的"无触点"报文 = 芯片的触摸基线稳了，之后才认触点。
        // 这一条专治上电假触点：日志里刚初始化就报 1 个触点、坐标正好落在
        // 车钥匙页的按钮上，按住 600ms 就会真的发一条车控指令出去。
        if (!s_tp_armed) {
            if (!pressed && !stale) {
                s_tp_armed = true;
                ESP_LOGI(TAG, "触摸基线确认，等了 %dms",
                         (int)((now - s_tp_arm_since) * portTICK_PERIOD_MS));
            } else if ((now - s_tp_arm_since) >= pdMS_TO_TICKS(TP_ARM_TIMEOUT_MS)) {
                // 一直等不到干净的"无触点"：基线可能整体偏移，再等下去触摸就废了
                s_tp_armed = true;
                ESP_LOGW(TAG, "上电后一直没读到干净的\"无触点\"报文，先放开触摸");
            }
        }
        if (pressed && !s_tp_armed) {
            // 打一条限速日志：如果现场还有"点不动"，看这里就知道是不是基线闸门
            // 把真实触点当噪声丢了（以及丢了多久），不用再猜。
            static TickType_t s_disarm_log_tick = 0;
            if ((now - s_disarm_log_tick) >= pdMS_TO_TICKS(1000)) {
                s_disarm_log_tick = now;
                ESP_LOGW(TAG, "基线未确认，忽略触点帧 (%d,%d)，已等 %dms",
                         (int)raw_x, (int)raw_y,
                         (int)((now - s_tp_arm_since) * portTICK_PERIOD_MS));
            }
            pressed = false;   // 基线未确认：这一帧当噪声，不建立按压
        }
        // 孤立的"抬手事件"（前面根本没有按压在手）不能拿来结算点击：
        // 芯片在通信异常期会乱报 event，靠它单独触发就是凭空控车。
        // 这种情况下只把它当普通的一帧触点用（正常建立按压走常规路径）。
        if (lifted && !s_press_active) {
            lifted = false;
        }

        int gap_ms = (int)((now - s_last_sample_tick) * portTICK_PERIOD_MS);

        if (pressed) {
            no_touch = 0;
            s_last_sample_tick = now;

            // 芯片原始坐标：raw_x = 长边 0~640，raw_y = 短边 0~172。
            // 官方竖屏映射是 (x = raw_y, y = 640 - raw_x)，
            // 本工程横屏再按 flush_frame 的转置换算成逻辑坐标：
            //   lx = 640 - raw_x   （长边方向，左右滑动就看它）
            //   ly = 171 - raw_y   （短边方向）
            int lx = SCR_W - 1 - (int)raw_x;
            int ly = SCR_H - 1 - (int)raw_y;

            // 命中测试用最后一个点：按下沿那一帧有可能是上一次按压的旧坐标
            s_last_x = lx;
            s_last_y = ly;

            // 新的一次按压：上次已经抬手，或者中间空了太久（超时兜底）
            bool new_press = !s_press_active || (gap_ms > GESTURE_GAP_MS);
            if (new_press) {
                press_begin(lx, ly);
                // 新的一次按压没有"上一次触点"，不能用开机以来的巨大间隔去判抬手，
                // 否则上电后第一次触摸会立刻被判成"抬手"直接收尾。
                gap_ms = 0;
            } else {
                s_seq_samples++;
                if (s_seq_samples == 2) {
                    // 按下沿那一帧往往是上一次按压留下的旧坐标（芯片那一帧还没更新），
                    // 拿它当基准会凭空多出一大截位移——表现就是"点按钮被判成滑动切了页"。
                    // 所以第二个点到达时重新起算基准，第一个点只用来确认"手指按下了"。
                    swipe_rebase(lx, ly);
                    // 判断"算不算点击"的真实落点同理要以第二个点为准：
                    // 按陈旧首帧算漂移会凭空多出几百像素，按钮就再也点不动了。
                    s_press_x0 = lx;
                    s_press_y0 = ly;
                    s_press_dx_max = 0;
                    s_press_dy_max = 0;
                    s_press_dx_prev = 0;
                    s_press_dy_prev = 0;
                } else if (s_seq_samples > 2) {
                    if (lx < s_seq_xmin) {
                        s_seq_xmin = lx;
                    }
                    if (lx > s_seq_xmax) {
                        s_seq_xmax = lx;
                    }
                    if (ly < s_seq_ymin) {
                        s_seq_ymin = ly;
                    }
                    if (ly > s_seq_ymax) {
                        s_seq_ymax = ly;
                    }
                }
                // 同一次按压内漏报了一段时间：重新起算位移基准，但按压本身继续算同一次
                if (gap_ms > SWIPE_LINK_MS) {
                    swipe_rebase(lx, ly);
                    s_run_dir = 0;
                    s_run_len = 0;
                }
            }

            // 相对真实落点的漂移：上面几次 swipe_rebase 会抹掉位移基准，
            // 只有跟着落点算，才能看出手指到底挪没挪（决定还算不算一次点击）
            int drift_x = abs(lx - s_press_x0);
            int drift_y = abs(ly - s_press_y0);
            // 先留一份"不含本帧"的峰值，供抬手判定使用（见 s_press_dx_prev 的说明）
            s_press_dx_prev = s_press_dx_max;
            s_press_dy_prev = s_press_dy_max;
            if (drift_x > s_press_dx_max) {
                s_press_dx_max = drift_x;
            }
            if (drift_y > s_press_dy_max) {
                s_press_dy_max = drift_y;
            }

            // ---- 单步位移：同向步数（判滑动）+ 静止计时（判点击）----
            // 两次快速点击之间只有一个几百像素的跳变（run=1），
            // 而真正的滑动会连续多帧朝同一方向移动（run 累加）。
            // 所以"同向连续步数"是区分二者最可靠的依据，跨度只做辅助。
            int step_x = lx - s_last_lx;
            int step_y = ly - s_last_ly;
            s_last_lx = lx;
            s_last_ly = ly;
            if (abs(step_x) >= TAP_MOVE_EPS || abs(step_y) >= TAP_MOVE_EPS) {
                // 还在动：静止计时清零。点击判定看的是"手指静止了多久"，
                // 滑动过程里它永远到不了阈值，所以滑动绝不可能被判成点击。
                s_last_move_tick = now;
            }
            if (abs(step_x) >= SWIPE_STEP_MIN) {
                int dir = (step_x > 0) ? 1 : -1;
                if (dir == s_run_dir) {
                    s_run_len++;
                } else {
                    s_run_dir = dir;
                    s_run_len = 1;
                }
            }

            // ---- 钥匙页按钮投票 ----
            // 第 1 个采样点可能是上一次按压留下的旧坐标，不参与投票，
            // 否则"在同一个按钮上连点"的第一帧会把票投到上一个位置。
            if (s_mode == LCD_MODE_KEY && s_seq_samples >= 2) {
                int hit = ui_key_button_hit(lx, ly);
                if (hit >= 0 && hit < LCD_KEY_SLOTS) {
                    s_hit_votes[hit]++;
                }
            }
            // 手指挪到别的按钮上了：解除"同一次接触只下发一次"的限制
            if (s_tap_blocked &&
                (abs(lx - s_tap_last_x) > TAP_UNBLOCK_MOVE ||
                 abs(ly - s_tap_last_y) > TAP_UNBLOCK_MOVE)) {
                s_tap_blocked = false;
            }

            // ================= 左右滑动 =================
            // 冷却期内不判：滑动生效的那一刻手指通常还在继续走，
            // 不冷却会把同一段滑动切成两半、连切两页。
            // 用有符号差值比较：TickType_t 会在 49 天处回绕，直接比大小会在回绕前后失效
            if (!s_action_taken && (int32_t)(now - s_swipe_lock_until) >= 0) {
                int dx = lx - s_seq_xfirst;
                int dy = ly - s_seq_yfirst;
                bool horiz = abs(dx) * 2 >= abs(dy) * 3;   // 横向至少 1.5 倍于纵向
                // 采样够密才能靠"同向步数"判滑动：断流时两三帧就能凑出 3 步，
                // 但那是"丢了帧"不是"手指在滑"，拿它切页会把点击吃掉。
                bool dense = s_seq_samples >= 4;
                // 判定一：连续同向移动 + 位移够大（常规滑动）
                bool by_run = dense && s_run_len >= SWIPE_RUN_MIN && abs(dx) >= SWIPE_MIN_DX && horiz;
                // 判定二：采样很稀（芯片只吐两三个点）时的大跨步兜底
                bool by_jump = s_run_len >= 2 && abs(dx) >= SWIPE_JUMP_DX && horiz;

                if (by_run || by_jump) {
                    s_action_taken = true;
                    s_action_tick = now;
                    s_swipe_lock_until = now + pdMS_TO_TICKS(SWIPE_LOCK_MS);
                    // 这次接触已经是"切页手势"：本次按压绝不能在抬手时再补一次按键
                    s_tap_pending = false;
                    s_tap_blocked = true;
                    s_tap_last_x = lx;
                    s_tap_last_y = ly;
                    ESP_LOGW(TAG, "滑动 %s %dpx (同向 %d 步, 纵向 %d, 采样 %d) -> %s",
                             dx > 0 ? "向右" : "向左", dx, s_run_len, dy, s_seq_samples,
                             s_mode == LCD_MODE_KEY ? "仪表盘" : "车钥匙");
                    lcd_display_set_mode(s_mode == LCD_MODE_KEY ? LCD_MODE_DASHBOARD
                                                                : LCD_MODE_KEY);
                }
            }
            s_prev_dx = lx - s_seq_xfirst;
            s_prev_dy = ly - s_seq_yfirst;

            // ================= 按下沿：登记点击 / 判双击 =================
            if (new_press && !s_action_taken) {
                // 首次收到触摸时明确打一条，方便区分"手势没识别"和"触摸根本没通"
                static bool first_touch_logged = false;
                if (!first_touch_logged) {
                    first_touch_logged = true;
                    ESP_LOGW(TAG, "触摸首次响应 (%d,%d)", lx, ly);
                }

                // 息屏时这一击只负责点亮屏幕，不参与单击/双击判定：
                // 否则"点亮 + 随手再点一下"会被误判成双击，直接跳到车钥匙页。
                bool wake_tap = false;
                if (!s_bl_on) {
                    lcd_display_set_backlight(true);
                    wake_tap = true;
                }

                ESP_LOGI(TAG, "触摸 raw(%d,%d) -> (%d,%d) pending=%d dt=%lums",
                         (int)raw_x, (int)raw_y, lx, ly, s_tap_pending ? 1 : 0,
                         (unsigned long)((now - s_tap_tick) * portTICK_PERIOD_MS));

                if (wake_tap) {
                    s_tap_pending = false;
                } else if (s_mode != LCD_MODE_KEY && s_tap_pending &&
                           (now - s_tap_tick) <= pdMS_TO_TICKS(DOUBLE_TAP_MS) &&
                           abs(lx - s_tap_x) <= DOUBLE_TAP_DIST &&
                           abs(ly - s_tap_y) <= DOUBLE_TAP_DIST) {
                    // 第二击落在时间窗内 -> 双击，取消第一击的单击处理。
                    // 模式切换直接在这里做：触摸任务一定在运行，不依赖业务层的轮询任务；
                    // 否则 ui_action 任务一旦没起来，表现就是"双击完全没反应"。
                    //
                    // 只在仪表盘页判双击：双击是"进钥匙页"的入口，
                    // 钥匙页里的点击必须老老实实当按下去执行，
                    // 否则刚双击进来再点一下按钮会被当成第三次连击，直接又切回仪表盘
                    // ——表现就是"钥匙页点按钮没反应"。
                    s_tap_pending = false;
                    // 这一击已经用来切模式了：本次按压不再判滑动，抬手也不再补单击
                    s_action_taken = true;
                    s_action_tick = now;
                    s_swipe_lock_until = now + pdMS_TO_TICKS(SWIPE_LOCK_MS);
                    // 刚双击切页时手指还压在屏幕上：这一下不允许再被当成按键下发
                    s_tap_blocked = true;
                    s_tap_last_x = lx;
                    s_tap_last_y = ly;
                    s_double_taps++;
                    ESP_LOGW(TAG, "双击 (%d,%d) -> %s", lx, ly,
                             s_mode == LCD_MODE_KEY ? "仪表盘" : "车钥匙");
                    lcd_display_set_mode(s_mode == LCD_MODE_KEY ? LCD_MODE_DASHBOARD
                                                                : LCD_MODE_KEY);
                } else {
                    s_tap_pending = true;
                    s_tap_wake = wake_tap;
                    s_tap_tick = now;
                    s_tap_x = lx;
                    s_tap_y = ly;
                }
            }
        } else {
            // 这一轮没读到触点
            no_touch++;

            // 明确读到"无触点" = 手指真的离开了（按压状态也已经收尾）。
            // 断流（silent）期间保留的 s_tap_blocked 必须在这里解除，
            // 否则断流一次之后，在同一个按钮上继续点就再也点不动了。
            //
            // stale 例外：芯片重复吐同一帧**带触点**的数据时，它并没有说手指
            // 抬起了。这里若照样解除限制，芯片每抖出一条新帧就会重新投一次票、
            // 再下发一条车控指令 —— 一次按压连发好几条"空调开关"就是这么来的。
            if (s_tap_blocked && !s_press_active && !stale) {
                s_tap_blocked = false;
            }

            if ((now - s_last_sample_tick) >= pdMS_TO_TICKS(10000) &&
                (idle_log_tick == 0 || (now - idle_log_tick) >= pdMS_TO_TICKS(10000))) {
                idle_log_tick = now;
                ESP_LOGW(TAG, "触摸连续 10 秒无任何触点（若此刻正按住屏幕，说明通信没建立）");
            }
        }

        // ================= 点击执行 / 按压结束 =================
        // "这次按压什么时候结束"直接决定连点、连滑灵不灵：芯片经常不给抬手帧，
        // 只靠抬手帧收尾就会把后面的点击/滑动全部并进同一次按压吞掉。
        // 所以除了抬手，还有下面两条明确的收尾路径。
        if (s_press_active) {
            bool stuck = (now - s_press_start_tick) >= pdMS_TO_TICKS(PRESS_STUCK_MS);
            // silent = 通信断了导致的"静默"，不是手指抬起（一帧无触点都没读到）。
            // 这两种情况必须分开：silent 时若当成抬手去解除 s_tap_blocked，
            // 手指其实还压着，恢复后会被算成一次新的按压 —— 一次按压重复控车。
            bool silent = (no_touch == 0) && gap_ms >= PRESS_SILENT_MS;
            // stale_hold：芯片在重复吐同一帧带触点的数据。按"结束这次按压"处理，
            // 但它和 silent 一样属于"抬手未确认"，绝不能算成一次点击。
            bool stale_hold = stale && no_touch >= RELEASE_MIN_MISS;
            // lifted：这一帧就是芯片报的"抬手事件"。它是最准、最快的抬手信号 ——
            // 不用等 0 触点帧、不用攒够 40ms，抬手这一帧当场结算点击。
            bool released = lifted ||
                            (no_touch >= RELEASE_MIN_MISS && gap_ms >= RELEASE_MIN_GAP_MS) ||
                            silent || stale_hold || stuck;
            int still_ms = (int)((now - s_last_move_tick) * portTICK_PERIOD_MS);
            // 只有"没滑成"的按压才可能是点击。
            // 漂移用"不含最后一帧"的峰值：抬起抖动不能把一次点击否掉。
            // 这里不再看 s_run_len：滑动是否成立由 dx/dy 单独判定（而且成立时会置
            // s_action_taken），把 run 也算进点击门槛，只会让"手指轻微挪了一下"
            // 的正常点击被判死 —— 日志里"同向 3 步"的按压全都没能触发按键。
            bool settled = s_press_dx_prev <= TAP_MAX_DRIFT &&
                           s_press_dy_prev <= TAP_MAX_DRIFT;
            bool can_tap = !s_action_taken && !s_tap_blocked && s_tap_pending && !s_tap_wake &&
                           !stale_hold && settled && s_mode == LCD_MODE_KEY &&
                           (now - s_tap_fire_tick) >= pdMS_TO_TICKS(TAP_REFIRE_MS);

            // 点击成立只有两种情况：
            //   1) 抬手（正常点击都走这条，最快、也没有歧义）
            //      —— 抬手事件（event=1）算、0 触点帧也算，抬手那一帧当场结算；
            //   2) 手指完全不动地按住够久（前两条都不来时的兜底）
            // 两条都要求"已经排除了滑动"，且第 2 条要求漂移极小 ——
            // 这就是"滑动时不会误触按钮"的根本保证：想滑动的手指不可能静止 300ms 只挪 20px。
            if (can_tap &&
                (released || (still_ms >= TAP_HOLD_STILL_MS &&
                              s_press_dx_max <= TAP_HOLD_DRIFT &&
                              s_press_dy_max <= TAP_HOLD_DRIFT))) {
                fire_key_tap();
                // 同一根手指压着不放时不再重复下发，直到抬手或挪开 TAP_UNBLOCK_MOVE
                s_tap_blocked = true;
                s_tap_last_x = s_last_x;
                s_tap_last_y = s_last_y;
                s_tap_pending = false;
                s_tap_fire_tick = now;   // 同一按钮的最小重复间隔（见 TAP_REFIRE_MS）
            }

            if (released) {
                if (stuck || silent || stale_hold) {
                    // 芯片一直报触点 / 通信断了 / 报文冻住：都不知道手指抬没抬起，
                    // 保留 s_tap_blocked。解除限制会让"按住不放"或断流恢复后
                    // 被算成新的一次按压，同一次接触重复下发车控指令。
                    // 真正的解除交给"明确读到无触点"那一帧（见上面的 else 分支）。
                    s_tap_pending = false;
                } else {
                    s_tap_blocked = false;   // 手指确实抬起来了，允许下一次点击
                }
                if (s_seq_samples > 0) {
                    ESP_LOGI(TAG, "按压结束: 采样 %d 个, 横向 %d..%d (跨度 %d), 同向 %d 步%s",
                             s_seq_samples, s_seq_xmin, s_seq_xmax, s_seq_xmax - s_seq_xmin,
                             s_run_len,
                             stale_hold ? " [芯片重复帧, 抬手未确认]"
                                        : (lifted ? " [抬手事件]"
                                                  : (stuck ? " [按满超时]"
                                                           : (silent ? " [通信静默]" : ""))));
                }
                press_end();
            } else if (s_action_taken &&
                       (still_ms >= ACTION_END_STILL_MS ||
                        (now - s_action_tick) >= pdMS_TO_TICKS(ACTION_END_MAX_MS))) {
                // 滑动/双击已经生效，手指也停下来了 -> 这次手势到此为止。
                // 不收尾的话，下一次滑动/点击会因为是"同一次按压"被直接吞掉
                // —— "连续滑动只生效一次" 就是这条原因。
                if (s_seq_samples > 0) {
                    ESP_LOGI(TAG, "手势收尾: 采样 %d 个, 同向 %d 步, 静止 %dms",
                             s_seq_samples, s_run_len, still_ms);
                }
                press_end();
            }
        }

        // 超过双击时间窗还没等来第二击 -> 结算这一击。
        // 钥匙页的按钮只由上面"按住到位/抬手"那条路径下发，那条路径有漂移校验。
        // 这里绝不能再补一次 handle_single_tap：它会甩开所有判据，滑动没成功时
        // 直接把手指底下那个按钮按下去——左滑误触发空调开关就是这条路径。
        if (s_tap_pending && !s_action_taken &&
            (xTaskGetTickCount() - s_tap_tick) > pdMS_TO_TICKS(DOUBLE_TAP_MS)) {
            s_tap_pending = false;
        }
    }
}

// ============ 初始化 ============

esp_err_t lcd_display_init(void)
{
    // 显式初始化：下面的失败分支会 goto fail 并用 ret 作为返回码，
    // 在缓冲分配阶段失败时它还不会被赋值
    esp_err_t ret = ESP_FAIL;

    // 第一件事：点亮背光。
    // 这样即使后续 LCD / PSRAM 初始化失败，也能通过“有背光但无画面”区分
    // 供电问题与屏幕数据问题，避免整机看起来完全没反应。
    gpio_config_t bl = {
        .pin_bit_mask = 1ULL << board_backlight_gpio(),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&bl);
    board_backlight_enable(true);
    gpio_set_level(board_backlight_gpio(), BOARD_LCD_BL_ON_LEVEL);
    s_bl_on = true;

    s_lock = xSemaphoreCreateMutex();
    s_dma_sem = xSemaphoreCreateBinary();
    s_lcd_bus_lock = xSemaphoreCreateMutex();
    if (!s_lock || !s_dma_sem || !s_lcd_bus_lock) {
        ESP_LOGE(TAG, "信号量创建失败");
        return ESP_ERR_NO_MEM;
    }

    // 静态零初始化会把 key_press 当成"0 号按钮正被按下"，这里显式置成"没有动效"
    s_state.key_press = -1;
    s_key_press_stage = KEY_PRESS_NONE;

    const size_t fb_size = (size_t)SCR_W * SCR_H * sizeof(uint16_t);

    ESP_LOGI(TAG, "PSRAM 可用: %u 字节, 内部 RAM 可用: %u 字节",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    s_fb = heap_caps_malloc(fb_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_fb) {
        ESP_LOGW(TAG, "PSRAM 分配帧缓存失败，改用内部 RAM（240KB）");
        s_fb = heap_caps_malloc(fb_size, MALLOC_CAP_8BIT);
    }
    if (!s_fb) {
        ESP_LOGE(TAG, "帧缓存分配失败，屏幕无法显示");
        backlight_hw(true);   // 至少点亮背光，便于判断供电是否正常
        return ESP_ERR_NO_MEM;
    }
    memset(s_fb, 0, fb_size);
    ESP_LOGI(TAG, "帧缓存 %u 字节 @%p", (unsigned)fb_size, s_fb);

#if BOARD_LCD_LANDSCAPE
    // 横屏需要一块物理方向的旋转缓冲（172x640，与逻辑帧缓存同大小）
    s_rot = heap_caps_malloc(fb_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_rot) {
        ESP_LOGW(TAG, "PSRAM 分配旋转缓冲失败，改用内部 RAM");
        s_rot = heap_caps_malloc(fb_size, MALLOC_CAP_8BIT);
    }
    if (!s_rot) {
        ESP_LOGE(TAG, "旋转缓冲分配失败（需要 %u 字节）", (unsigned)fb_size);
        // 已经拿到帧缓存了，失败退出前必须还回去，否则 440KB 就这么漏掉了
        goto fail;
    }
    memset(s_rot, 0, fb_size);
    ESP_LOGI(TAG, "旋转缓冲 %u 字节 @%p", (unsigned)fb_size, s_rot);
#endif

    s_dma_buf[0] = heap_caps_malloc((size_t)BOARD_LCD_PANEL_W * FLUSH_LINES * sizeof(uint16_t),
                                    MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_dma_buf[0]) {
        ESP_LOGE(TAG, "DMA 缓冲分配失败");
        goto fail;
    }

    s_canvas.buf = s_fb;
    s_canvas.w = SCR_W;
    s_canvas.h = SCR_H;

    ret = lcd_hw_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LCD 硬件初始化失败: %s", esp_err_to_name(ret));
        goto fail;
    }

    ret = touch_hw_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "无触摸输入，继续运行");
    }

    // ---- 二分定位测试 ----
    // 注意：GPIO21 在部分贴片上就是 LCD_RST（R69 焊 / R73 NC），
    //       所以不再把它当作 TE 采样脚使用，否则会破坏复位电平。

    // 步骤 1：背光通路 —— GPIO42（PWM 反相）+ TCA9554 EXIO1（升压使能）
    ESP_LOGW(TAG, "背光闪烁测试: 关 800ms");
    board_backlight_enable(false);
    gpio_set_level(board_backlight_gpio(), !BOARD_LCD_BL_ON_LEVEL);
    vTaskDelay(pdMS_TO_TICKS(800));
    board_backlight_enable(true);
    gpio_set_level(board_backlight_gpio(), BOARD_LCD_BL_ON_LEVEL);
    vTaskDelay(pdMS_TO_TICKS(300));

    // 步骤 2：命令通路 —— 若这段之后屏幕没有明显变黑一次，
    //        说明 QSPI 命令根本没到达面板（查 CS/CLK/D0~D3/mode/复位）
    ESP_LOGW(TAG, "命令通路测试: DISPOFF 700ms");
    esp_lcd_panel_disp_on_off(s_panel, false);
    vTaskDelay(pdMS_TO_TICKS(700));
    esp_lcd_panel_disp_on_off(s_panel, true);
    vTaskDelay(pdMS_TO_TICKS(300));
    ESP_LOGW(TAG, "命令通路测试结束（屏幕应刚闪过一次黑）");

    // 步骤 3：像素数据通路。红→绿→蓝，看不到颜色说明数据没写进显存
    lcd_color_selftest();

    // 同步画一帧并推屏，确保上电即有画面
    lock_state();
    ui_state_t snapshot = s_state;
    unlock_state();
    ui_render(&s_canvas, &snapshot);
    flush_frame();

    s_bl_on = true;
    backlight_hw(true);

    // 两个任务都必须检查返回值：创建失败的话屏上一个像素都不会动，
    // 而 init 仍然返回 ESP_OK，后面只会看到"毫无反应"的显示模块
    if (xTaskCreate(render_task, "lcd_render", 5120, NULL, 4, &s_render_task) != pdPASS) {
        ESP_LOGE(TAG, "渲染任务创建失败");
        goto fail;
    }
    // 触摸任务优先级必须高于渲染任务：刷一帧要做整屏转置 + 10 次 QSPI 传输，
    // 期间渲染任务几乎不阻塞，触摸任务（原优先级 2）会被饿到几十上百毫秒才采到一次点。
    // 这里给到 6，同时高于业务任务（tesla_main=5）：蓝牙收发期间 CPU 会被占住一会儿，
    // 同优先级只能靠时间片轮转，按下沿很容易错过。
    // 栈给足 4KB：触摸任务里有 I2C 调用和带多个参数的日志输出，3072 偏紧
    if (xTaskCreate(touch_task, "lcd_touch", 4096, NULL, 6, &s_touch_task) != pdPASS) {
        ESP_LOGE(TAG, "触摸任务创建失败");
        if (s_render_task) {
            vTaskDelete(s_render_task);
            s_render_task = NULL;
        }
        goto fail;
    }

    ESP_LOGI(TAG, "显示模块初始化完成");
    return ESP_OK;

fail:
    // 统一回收：SPI 总线在这里不 free（esp_lcd 面板句柄已在 lcd_hw_init 内自行处理），
    // 只把已申请的缓冲还给堆，避免失败路径泄漏几百 KB
    if (s_fb) {
        heap_caps_free(s_fb);
        s_fb = NULL;
    }
    if (s_rot) {
        heap_caps_free(s_rot);
        s_rot = NULL;
    }
    for (int i = 0; i < DMA_BUF_COUNT; i++) {
        if (s_dma_buf[i]) {
            heap_caps_free(s_dma_buf[i]);
            s_dma_buf[i] = NULL;
        }
    }
    s_bl_on = true;
    backlight_hw(true);   // 点亮背光，便于判断供电是否正常
    return (ret != ESP_OK) ? ret : ESP_FAIL;
}
