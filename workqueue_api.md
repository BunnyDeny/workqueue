# 🔧 MCU 工作队列接口设计文档

> 基于 `list.h` 的嵌入式简化版 Linux workqueue 实现方案

---

## 🎯 设计目标

| 需求 | 方案 |
|------|------|
| ⚡ 中断上下文安全投递工作 | 关中断 + `list_add_tail` 保证原子性 |
| ⏰ 延迟执行（ms/tick 级） | 全局 tick 计数器 + 到期时间比对 |
| 📦 FIFO 顺序执行 | `list_add_tail` 入队，从头到尾消费 |
| 🔒 线程/任务间互斥 | 关中断即是最强互斥（单核 MCU） |
| 🚫 零动态内存 | 工作项静态定义，`container_of` 访问 |

---

## 🏗️ 核心数据结构

```c
#include "list.h"

/* ───── 工作项 ───── */
struct work_struct {
    struct list_head entry;
    void (*func)(struct work_struct *work);
    unsigned long flags;
};

/* ───── 延迟工作项 ───── */
struct delayed_work {
    struct work_struct work;
    unsigned long tick_expire;      // 到期 tick
    struct workqueue_struct *wq;    // 所属队列（cancel 时需要）
};

/* ───── 工作队列 ───── */
struct workqueue_struct {
    struct list_head work_list;         // 立即执行链表
    struct list_head delayed_list;      // 延迟执行链表
    const char *name;
    unsigned int nr_running;            // 正在执行的数量
    // 锁：单核 MCU 下关中断即可，多核需要自旋锁
};
```

---

## 📡 全局默认队列

```c
/* 系统默认工作队列，类似 Linux 的 system_wq */
extern struct workqueue_struct *system_wq;

/* 在中断/任务中直接投递到默认队列 */
bool schedule_work(struct work_struct *work);
bool schedule_delayed_work(struct delayed_work *dwork, unsigned long delay_ticks);
```

---

## 🔌 初始化 API

```c
/* 初始化一个普通工作项 */
#define INIT_WORK(_work, _func)                     \
    do {                                            \
        INIT_LIST_HEAD(&(_work)->entry);            \
        (_work)->func = (_func);                    \
        (_work)->flags = 0;                         \
    } while (0)

/* 初始化一个延迟工作项 */
#define INIT_DELAYED_WORK(_dwork, _func)            \
    do {                                            \
        INIT_WORK(&(_dwork)->work, (_func));        \
        (_dwork)->tick_expire = 0;                  \
        (_dwork)->wq = NULL;                        \
    } while (0)
```

---

## 📤 调度 / 入队 API

```c
/* 投递到指定队列，立即执行（FIFO 顺序） */
bool queue_work(struct workqueue_struct *wq, struct work_struct *work);

/* 投递到指定队列，延迟执行 */
bool queue_delayed_work(struct workqueue_struct *wq,
                        struct delayed_work *dwork,
                        unsigned long delay_ticks);

/* 投递到默认队列 system_wq */
bool schedule_work(struct work_struct *work);
bool schedule_delayed_work(struct delayed_work *dwork, unsigned long delay_ticks);
```

> 💡 **返回值**：如果工作项已经在 pending 状态（已入队但未执行），返回 `false`，不会重复入队。

---

## ❌ 取消 API

```c
/* 取消一个待执行的工作项，成功返回 true */
bool cancel_work_sync(struct work_struct *work);
bool cancel_delayed_work_sync(struct delayed_work *dwork);
```

> ⚠️ `cancel_work_sync` 的含义：
> - 如果工作项在队列中（pending），将其移除，返回 `true`
> - 如果工作项正在执行，**等待其完成**（busy-wait 或任务让步），然后返回 `true`
> - 如果工作项未调度，返回 `false`
>
> MCU 简化版中，"等待其完成"可通过轮询 `WQ_STAT_RUNNING` 标志实现。

---

## 🔄 刷新 API

```c
/* 等待指定工作项执行完毕 */
void flush_work(struct work_struct *work);
void flush_delayed_work(struct delayed_work *dwork);

/* 刷新整个队列，直到队列为空 */
void flush_workqueue(struct workqueue_struct *wq);
```

---

## 🏃 执行引擎 API（在任务/主循环中调用）

```c
/* 处理一个工作项（非阻塞） */
void workqueue_run_one(struct workqueue_struct *wq);

/* 处理所有 pending 的工作项（直到队列为空） */
void workqueue_run_all(struct workqueue_struct *wq);

/* 在 tick 中断中调用，将到期的延迟工作移入执行队列 */
void workqueue_tick_handler(struct workqueue_struct *wq, unsigned long current_tick);
```

### 典型主循环用法

```c
void main_loop(void)
{
    while (1) {
        /* 1. 处理立即执行的工作 */
        workqueue_run_all(system_wq);

        /* 2. 进低功耗或让出 CPU */
        cpu_idle();
    }
}
```

### 典型 tick 中断用法

```c
volatile unsigned long jiffies = 0;

void TIM_IRQHandler(void)
{
    jiffies++;
    workqueue_tick_handler(system_wq, jiffies);
}
```

---

## ⚙️ 队列管理 API

```c
/* 创建/销毁队列 */
struct workqueue_struct *create_workqueue(const char *name);
void destroy_workqueue(struct workqueue_struct *wq);
```

> 🎯 MCU 场景下，通常只使用一个全局的 `system_wq`，不需要动态创建。

---

## 🔐 同步机制设计

### 单核 MCU 策略：关中断 = 最强互斥

```c
/* 伪代码 */
static inline unsigned long wq_lock_irqsave(void)
{
    unsigned long flags;
    __disable_irq();        // CMSIS: __disable_irq()
    flags = 0;              // 实际实现保存 PRIMASK
    return flags;
}

static inline void wq_unlock_irqrestore(unsigned long flags)
{
    (void)flags;
    __enable_irq();         // CMSIS: __enable_irq()
}
```

| 操作 | 保护方式 |
|------|---------|
| `queue_work()` | 关中断，链表操作，开中断 |
| `cancel_work_sync()` | 关中断，检查并移除；若正在运行则忙等 |
| `workqueue_tick_handler()` | **在中断里调用，天然原子** |
| `workqueue_run_one()` | 关中断出队，开中断后执行回调 |

> 💡 执行回调时**必须开中断**，否则中断饿死。出队操作关中断保护，实际执行函数时开中断。

---

## ⏱️ Tick / 延迟执行机制

```c
/* 全局 jiffies，由定时器中断每 1ms/10ms 自增 */
extern volatile unsigned long jiffies;

/* delay_ticks = 延迟的 tick 数 */
bool queue_delayed_work(struct workqueue_struct *wq,
                        struct delayed_work *dwork,
                        unsigned long delay_ticks)
{
    dwork->tick_expire = jiffies + delay_ticks;
    list_add_tail(&dwork->work.entry, &wq->delayed_list);
    dwork->wq = wq;
    return true;
}

/* 在 tick 中断中检查到期 */
void workqueue_tick_handler(struct workqueue_struct *wq, unsigned long current_tick)
{
    struct delayed_work *pos, *n;

    list_for_each_entry_safe(pos, n, &wq->delayed_list, work.entry) {
        if (time_after_eq(current_tick, pos->tick_expire)) {
            list_del_init(&pos->work.entry);
            queue_work(wq, &pos->work);   // 移入立即执行队列
        }
    }
}
```

> 🎯 `time_after_eq(a, b)` 是一个防回绕宏，参考 Linux 内核实现：
> ```c
> #define time_after_eq(a, b)     ((long)((a) - (b)) >= 0)
> ```

---

## 📝 完整使用示例

```c
#include "workqueue.h"

/* 1. 定义工作项 */
static struct work_struct my_work;
static struct delayed_work my_dwork;

/* 2. 工作回调 */
static void my_work_handler(struct work_struct *work)
{
    printf("work executed!\n");
}

static void my_delayed_handler(struct work_struct *work)
{
    printf("delayed work executed after timeout!\n");
}

/* 3. 初始化 */
void init(void)
{
    system_wq = create_workqueue("sys_wq");

    INIT_WORK(&my_work, my_work_handler);
    INIT_DELAYED_WORK(&my_dwork, my_delayed_handler);
}

/* 4. 中断中投递 */
void UART_IRQHandler(void)
{
    /* 收到数据，触发工作 */
    schedule_work(&my_work);

    /* 或者：收到数据，500ms 后再处理 */
    schedule_delayed_work(&my_dwork, 500);  // 500 ticks
}

/* 5. 主循环消费 */
int main(void)
{
    init();

    while (1) {
        workqueue_run_all(system_wq);
        cpu_wfi();  /* 等待中断唤醒 */
    }
}
```

---

## 🗂️ 与 Linux 接口对照表

| Linux 接口 | 本设计接口 | 差异说明 |
|-----------|-----------|---------|
| `INIT_WORK()` | `INIT_WORK()` | ✅ 完全一致 |
| `INIT_DELAYED_WORK()` | `INIT_DELAYED_WORK()` | ✅ 完全一致 |
| `schedule_work()` | `schedule_work()` | ✅ 完全一致 |
| `schedule_delayed_work()` | `schedule_delayed_work()` | ✅ 完全一致 |
| `queue_work()` | `queue_work()` | ✅ 完全一致 |
| `cancel_work_sync()` | `cancel_work_sync()` | 简化版，忙等替代睡眠 |
| `flush_work()` | `flush_work()` | 简化版，轮询替代等待队列 |
| `create_workqueue()` | `create_workqueue()` | 无多线程绑定 |
| `process_one_work()` | `workqueue_run_one()` | 改名，语义一致 |

---

## 🚫 明确不支持的 Linux 特性

| Linux 特性 | 不支持原因 |
|-----------|-----------|
| 🔴 多线程绑定（`kworker`） | MCU 无 OS 线程 |
| 🔴 工作项递归调度 | 简化设计，避免死锁 |
| 🔴 `work_busy()` 复杂状态机 | 只需 `pending` / `running` 两态 |
| 🔴 `cmwq`（Concurrency Managed WQ） | 单核 MCU 不需要并发管理 |
| 🔴 `WQ_HIGHPRI` / `WQ_UNBOUND` | 无调度器，无优先级队列 |

---

## ✅ 总结

> 🎯 这是一个**裁剪到骨头**的 Linux workqueue 移植：
> - 保留最核心的 `struct work_struct` + `struct delayed_work`
> - 保留 `schedule_work` / `schedule_delayed_work` 经典接口
> - 用**关中断**替代自旋锁、等待队列、信号量
> - 用**轮询 jiffies** 替代 hrtimer
> - 适配裸机 / RTOS / 简单前后台系统的 MCU 场景
