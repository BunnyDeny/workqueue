# ⚙️ MCU Workqueue

> 🎯 参考 Linux 内核工作队列接口设计的嵌入式简化版，零动态内存、支持延迟执行。

---

## 📦 快速开始

```bash
make      # 编译
make run  # 运行测试
make clean
```

---

## 🏗️ 核心概念

| 术语 | 说明 |
|------|------|
| `work_struct` | 🔧 一个工作项（要延迟执行的函数） |
| `delayed_work` | ⏰ 带定时器的工作项，到 tick 才执行 |
| `workqueue_struct` | 📥 工作队列，管理所有待执行的工作 |
| `jiffies` | 🕐 全局 tick 计数器，由你的定时器中断维护 |

---

## 📝 API 使用指南

### 1️⃣ 初始化

```c
#include "workqueue.h"

static struct work_struct my_work;
static struct delayed_work my_dwork;

void my_handler(struct work_struct *work)
{
    printf("工作执行了!\n");
}

void init(void)
{
    workqueue_init();                       // 初始化默认队列
    INIT_WORK(&my_work, my_handler);        // 普通工作
    INIT_DELAYED_WORK(&my_dwork, my_handler); // 延迟工作
}
```

### 2️⃣ 调度工作（中断/任务中调用）

```c
/* 立即执行，FIFO 顺序 */
schedule_work(&my_work);

/* 延迟 100 ticks 后执行 */
schedule_delayed_work(&my_dwork, 100);

/* 投递到指定队列 */
struct workqueue_struct *wq = create_workqueue("my_wq");
queue_work(wq, &my_work);
queue_delayed_work(wq, &my_dwork, 100);
```

> ⚡ 所有 `schedule_*` / `queue_*` 函数都可**安全地在中断上下文中调用**。

### 3️⃣ 主循环消费（后台任务中调用）

```c
void main_loop(void)
{
    while (1) {
        /* 处理所有 pending 的工作 */
        workqueue_run_all(system_wq);

        /* 进低功耗，等待中断唤醒 */
        cpu_wfi();
    }
}
```

### 4️⃣ Tick 中断处理

```c
volatile unsigned long jiffies = 0;

void TIM_IRQHandler(void)
{
    jiffies++;
    /* 把到期的延迟工作移入执行队列 */
    workqueue_tick_handler(system_wq, jiffies);
}
```

### 5️⃣ 取消工作

```c
/* 如果工作还在队列中，移除它；如果正在执行，等它完成 */
cancel_work_sync(&my_work);
cancel_delayed_work_sync(&my_dwork);
```

### 6️⃣ 刷新（等待完成）

```c
/* 等待指定工作执行完毕 */
flush_work(&my_work);

/* 刷新整个队列，直到空 */
flush_workqueue(system_wq);
```

---

## 🔒 互斥锁移植（重要）

本库通过两个**弱定义**钩子实现同步，默认空实现（适合单线程/关中断场景）：

```c
unsigned long wq_platform_lock_irqsave(void);
void wq_platform_unlock_irqrestore(unsigned long flags);
```

**在你的项目中覆盖它们即可**，例如：

### 🛡️ 单核 MCU（关中断）

```c
unsigned long wq_platform_lock_irqsave(void)
{
    unsigned long flags = __get_PRIMASK();
    __disable_irq();
    return flags;
}

void wq_platform_unlock_irqrestore(unsigned long flags)
{
    __set_PRIMASK(flags);
}
```

### 🖥️ PC Linux / RTOS（互斥锁）

```c
#include <pthread.h>
static pthread_mutex_t wq_mutex = PTHREAD_MUTEX_INITIALIZER;

unsigned long wq_platform_lock_irqsave(void)
{
    pthread_mutex_lock(&wq_mutex);
    return 0;
}

void wq_platform_unlock_irqrestore(unsigned long flags)
{
    (void)flags;
    pthread_mutex_unlock(&wq_mutex);
}
```

> 💡 由于使用了 `__attribute__((weak))`，你只需在任意一个 `.c` 文件中定义这两个函数，链接时会自动覆盖库中的默认空实现。

---

## 📋 完整示例

```c
#include "workqueue.h"

volatile unsigned long jiffies = 0;
static struct work_struct irq_work;
static struct delayed_work debounce_work;

/* 中断里要做的事 */
void irq_handler(struct work_struct *work)
{
    (void)work;
    printf("处理中断数据\n");
}

/* 消抖：按键中断后 50ms 再确认 */
void debounce_handler(struct work_struct *work)
{
    (void)work;
    printf("按键稳定，执行动作\n");
}

/* UART 接收中断 */
void UART_IRQHandler(void)
{
    schedule_work(&irq_work);               // 立即处理数据
    schedule_delayed_work(&debounce_work, 50); // 50ms 后消抖
}

int main(void)
{
    workqueue_init();
    INIT_WORK(&irq_work, irq_handler);
    INIT_DELAYED_WORK(&debounce_work, debounce_handler);

    while (1) {
        workqueue_run_all(system_wq);
        cpu_wfi();
    }
}
```

---

## 📁 文件说明

| 文件 | 作用 |
|------|------|
| `list.h` | 🔗 链表基础（从 Linux 内核移植） |
| `workqueue.h` | 📋 接口头文件 |
| `workqueue.c` | ⚙️ 实现 |
| `main.c` | 🧪 测试用例（含多线程并发压测） |

---

## 🚫 设计约束

- 不支持递归调度（工作回调里不能再 `schedule_work` 自己）
- 单核 MCU 场景为主，多核需扩展锁实现
- `jiffies` 回绕周期取决于你的 tick 频率和变量位宽

---

## 📜 License

GPL-2.0
