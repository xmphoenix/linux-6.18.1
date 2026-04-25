# Linux Kernel LED 子系统深度解析 (基于 Linux 6.18.1)

---

## 目录

1. [LED 框架历史变迁](#1-led-框架历史变迁)
2. [LED 子系统软件架构](#2-led-子系统软件架构)
3. [核心数据结构](#3-核心数据结构)
4. [核心 API 接口函数](#4-核心-api-接口函数)
5. [LED Trigger 机制](#5-led-trigger-机制)
6. [LED 命名规范](#6-led-命名规范)
7. [关键知识点汇总](#7-关键知识点汇总)
8. [QEMU 实验设计](#8-qemu-实验设计)

---

## 1. LED 框架历史变迁

### 1.1 早期阶段 (2005-2006, Linux 2.6.x)

- **创建者**: John Lenz 和 Richard Purdie (Openedhand Ltd.)
- 首次引入 LED class 和 LED trigger 框架
- 提供 sysfs 接口 `/sys/class/leds/`，暴露 `brightness` 和 `max_brightness` 属性
- 支持软件闪烁 (software blink)，使用内核定时器实现
- 最初的 trigger 包括: `timer`、`default-on`、`heartbeat`

### 1.2 硬件闪烁支持 (2007-2010, Linux 2.6.x)

- 引入 `blink_set()` 回调，支持硬件加速闪烁
- 添加 `oneshot` 触发器
- GPIO LED 驱动 (`leds-gpio`) 成为最常用的 LED 驱动模板
- 增加了 `LED_CORE_SUSPENDRESUME` 标志，支持电源管理

### 1.3 LED Flash 子类 (2015, Linux 4.x)

- Samsung 的 Jacek Anaszewski 引入 Flash LED class (`led-class-flash.c`)
- 支持相机闪光灯特有属性: flash_brightness、flash_timeout、flash_strobe 等
- 添加 `LED_DEV_CAP_FLASH` 标志

### 1.4 Userspace LED 驱动 (2016, Linux 4.x)

- David Lechner 引入 `uleds` (userspace LED) 驱动
- 类似 `uinput`，允许用户空间创建虚拟 LED 设备
- 通过 `/dev/uleds` 字符设备接口操作

### 1.5 多色 LED 支持 (2019-2020, Linux 5.x)

- Texas Instruments 的 Dan Murphy 引入 LED Multicolor class (`led-class-multicolor.c`)
- 支持多色 LED (如 RGB LED) 的独立颜色强度控制
- 新增 `multi_intensity` 和 `multi_index` sysfs 属性
- 引入 `LED_MULTI_COLOR` 标志

### 1.6 现代化改进 (2020-2025, Linux 5.x - 6.x)

- **DT 命名标准化**: 采用 `<devicename:color:function>` 命名规范
- **`led_init_data`**: 统一的初始化数据结构，从 DT 解析 color、function、label 属性
- **`led_compose_name()`**: 自动合成 LED 名称
- **hw_control 接口**: 支持硬件自主控制 LED（如网口 PHY LED），引入 `hw_control_is_supported()`、`hw_control_set()`、`hw_control_get()` 回调
- **`brightness_set_blocking()`**: 分离阻塞和非阻塞亮度设置路径
- **devm_* 资源管理 API**: 全面支持 device-managed 资源
- **LED consumer API**: `led_get()`/`devm_led_get()` 允许其他子系统获取 LED 设备
- **KUnit 测试** (2025): 引入 `led-test.c` KUnit 测试用例
- **LED RGB 子类**: `drivers/leds/rgb/` 目录下的 RGB LED 驱动

### 1.7 Linux 6.18.1 当前状态

当前版本的 LED 子系统包含：
- **核心文件**: `led-core.c`, `led-class.c`, `led-triggers.c`
- **子类**: `led-class-flash.c`, `led-class-multicolor.c`
- **70+ 个硬件驱动**: GPIO、I2C、SPI、平台设备等
- **14 种 trigger**: timer, heartbeat, default-on, netdev, cpu, disk, panic, backlight, camera, oneshot, pattern, transient, tty, input-events
- **子目录**: `trigger/`, `flash/`, `rgb/`, `blink/`, `simatic/`

---

## 2. LED 子系统软件架构

### 2.1 分层架构

```
┌──────────────────────────────────────────────────────────┐
│                  User Space                               │
│   echo 255 > /sys/class/leds/xxx/brightness              │
│   cat /sys/class/leds/xxx/trigger                        │
│   /dev/uleds (userspace LED)                             │
└──────────────────────┬───────────────────────────────────┘
                       │ sysfs / char dev
┌──────────────────────┴───────────────────────────────────┐
│              LED Class Layer (led-class.c)                │
│  - sysfs 属性: brightness, max_brightness, trigger       │
│  - 设备注册/注销                                          │
│  - PM suspend/resume                                      │
│  - LED lookup table                                       │
├──────────────────────────────────────────────────────────┤
│              LED Core Layer (led-core.c)                  │
│  - 亮度设置 (sleep/nosleep/sync/nopm)                    │
│  - 软件闪烁 (timer-based blink)                          │
│  - 名称合成 (led_compose_name)                            │
│  - DT 属性解析                                            │
├──────────────────────────────────────────────────────────┤
│           LED Trigger Layer (led-triggers.c)              │
│  - trigger 注册/注销                                      │
│  - trigger 绑定/解绑 LED                                  │
│  - simple trigger API                                     │
│  - complex trigger API                                    │
├──────────┬───────────┬────────────┬──────────────────────┤
│  Flash   │Multicolor │   RGB      │     Blink HW         │
│  Class   │  Class    │  Drivers   │     Drivers          │
├──────────┴───────────┴────────────┴──────────────────────┤
│             LED Hardware Drivers                          │
│  leds-gpio, leds-pwm, leds-syscon, leds-pca9532, ...    │
│  - brightness_set() / brightness_set_blocking()          │
│  - brightness_get()                                       │
│  - blink_set() (硬件闪烁)                                 │
│  - hw_control_set/get (硬件自主控制)                       │
└──────────────────────────────────────────────────────────┘
```

### 2.2 核心源文件组织

| 文件 | 功能 |
|------|------|
| `led-core.c` | 核心亮度控制、闪烁定时器、名称合成、DT 解析 |
| `led-class.c` | sysfs 属性、设备注册/注销、PM、LED consumer API |
| `led-triggers.c` | trigger 注册/绑定/事件分发 |
| `led-class-flash.c` | Flash LED 子类 |
| `led-class-multicolor.c` | 多色 LED 子类 |
| `leds.h` (内部头文件) | 内部函数声明 |
| `uleds.c` | 用户空间 LED 驱动 |
| `led-test.c` | KUnit 测试 |

### 2.3 亮度设置路径

```
led_set_brightness(led_cdev, value)     [不可睡眠，可从 IRQ 调用]
    │
    ├── 正在软件闪烁? ──是──> 设置 work_flags, 让 work 处理
    │
    └── 否 ──> led_set_brightness_nosleep(led_cdev, value)
                  │
                  └── led_set_brightness_nopm(led_cdev, value)
                        │
                        ├── brightness_set() 可用? ──是──> 直接调用 (不可睡眠)
                        │
                        └── 否 ──> 委托给 work queue
                              │
                              └── set_brightness_delayed() [work handler]
                                    │
                                    └── brightness_set_blocking() [可睡眠]

led_set_brightness_sync(led_cdev, value)  [同步阻塞版本]
    └── 直接调用 brightness_set_blocking()
```

**关键设计**: 亮度设置分为两条路径：
- **非阻塞路径** (`brightness_set`): 不可睡眠，适用于 GPIO 等快速操作
- **阻塞路径** (`brightness_set_blocking`): 可以睡眠，适用于 I2C/SPI 等总线操作
- 当只有 `brightness_set_blocking` 时，通过 **workqueue** 异步执行

### 2.4 软件闪烁机制

```
led_blink_set(delay_on, delay_off)
    │
    ├── 硬件支持? ──是──> blink_set() 回调
    │
    └── 否 ──> led_set_software_blink()
                  │
                  └── 启动 blink_timer
                        │
                        └── led_timer_function() [定时器回调]
                              │
                              ├── 当前亮 ──> 关闭, delay = delay_off
                              │
                              └── 当前灭 ──> 打开, delay = delay_on
                              │
                              └── mod_timer(jiffies + delay)
```

---

## 3. 核心数据结构

### 3.1 `struct led_classdev` — LED 核心设备结构

```c
/* 定义在 include/linux/leds.h */
struct led_classdev {
    const char      *name;              /* LED 名称 */
    unsigned int     brightness;        /* 当前亮度 */
    unsigned int     max_brightness;    /* 最大亮度 */
    unsigned int     color;             /* 颜色 ID (LED_COLOR_ID_xxx) */
    int              flags;             /* 状态和控制标志 */

    /* === 驱动回调 === */

    /* 非阻塞亮度设置 (不可睡眠, 适用于 GPIO) */
    void (*brightness_set)(struct led_classdev *led_cdev,
                           enum led_brightness brightness);

    /* 阻塞亮度设置 (可睡眠, 适用于 I2C/SPI) */
    int  (*brightness_set_blocking)(struct led_classdev *led_cdev,
                                    enum led_brightness brightness);

    /* 获取当前亮度 */
    enum led_brightness (*brightness_get)(struct led_classdev *led_cdev);

    /* 硬件闪烁 */
    int  (*blink_set)(struct led_classdev *led_cdev,
                      unsigned long *delay_on, unsigned long *delay_off);

    /* 模式设置 */
    int  (*pattern_set)(struct led_classdev *led_cdev,
                        struct led_pattern *pattern, u32 len, int repeat);
    int  (*pattern_clear)(struct led_classdev *led_cdev);

    /* === 内部管理 === */
    struct device           *dev;               /* 关联的 device */
    struct list_head         node;              /* 全局 leds_list 链表节点 */
    const char              *default_trigger;   /* 默认 trigger 名称 */

    /* 闪烁相关 */
    unsigned long    blink_delay_on, blink_delay_off;
    struct timer_list blink_timer;
    int              blink_brightness;
    int              new_blink_brightness;

    /* workqueue 相关 */
    struct workqueue_struct *wq;
    struct work_struct       set_brightness_work;
    int                      delayed_set_value;

    /* Trigger 相关 (CONFIG_LEDS_TRIGGERS) */
    struct rw_semaphore  trigger_lock;
    struct led_trigger  *trigger;
    struct list_head     trig_list;
    void                *trigger_data;
    bool                 activated;

    /* hw_control 相关 */
    const char *hw_control_trigger;
    int (*hw_control_is_supported)(struct led_classdev *led_cdev, unsigned long flags);
    int (*hw_control_set)(struct led_classdev *led_cdev, unsigned long flags);
    int (*hw_control_get)(struct led_classdev *led_cdev, unsigned long *flags);
    struct device *(*hw_control_get_device)(struct led_classdev *led_cdev);

    struct mutex led_access;    /* 一致性访问锁 */
};
```

**重要标志位 (flags)**:

| 标志 | 含义 |
|------|------|
| `LED_SUSPENDED` (BIT 0) | LED 已挂起 |
| `LED_UNREGISTERING` (BIT 1) | LED 正在注销 |
| `LED_CORE_SUSPENDRESUME` (BIT 16) | 自动挂起/恢复 |
| `LED_SYSFS_DISABLE` (BIT 17) | 禁用 sysfs 接口 |
| `LED_DEV_CAP_FLASH` (BIT 18) | 具有 Flash 能力 |
| `LED_HW_PLUGGABLE` (BIT 19) | 可热插拔 |
| `LED_PANIC_INDICATOR` (BIT 20) | 内核 panic 指示灯 |
| `LED_BRIGHT_HW_CHANGED` (BIT 21) | 硬件亮度变化通知 |
| `LED_RETAIN_AT_SHUTDOWN` (BIT 22) | 关机保持状态 |
| `LED_INIT_DEFAULT_TRIGGER` (BIT 23) | 初始化默认 trigger |
| `LED_REJECT_NAME_CONFLICT` (BIT 24) | 拒绝名称冲突 |
| `LED_MULTI_COLOR` (BIT 25) | 多色 LED |

### 3.2 `struct led_trigger` — LED 触发器

```c
struct led_trigger {
    const char       *name;                 /* trigger 名称 */
    int             (*activate)(struct led_classdev *led_cdev);   /* 激活回调 */
    void            (*deactivate)(struct led_classdev *led_cdev); /* 去激活回调 */
    enum led_brightness brightness;         /* 默认亮度 */
    struct led_hw_trigger_type *trigger_type; /* 私有 trigger 类型 */
    spinlock_t        leddev_list_lock;     /* 保护 led_cdevs 列表 */
    struct list_head  led_cdevs;            /* 受此 trigger 控制的 LED 列表 */
    struct list_head  next_trig;            /* 全局 trigger 链表 */
    const struct attribute_group **groups;  /* trigger 自定义 sysfs 属性组 */
};
```

### 3.3 `struct led_init_data` — LED 初始化数据

```c
struct led_init_data {
    struct fwnode_handle *fwnode;    /* 设备固件节点 */
    const char *default_label;      /* 默认标签 (兼容旧驱动) */
    const char *devicename;         /* 设备名部分 */
    bool devname_mandatory;         /* 设备名是否必须 */
};
```

### 3.4 `struct led_classdev_mc` — 多色 LED

```c
struct led_classdev_mc {
    struct led_classdev led_cdev;
    unsigned int num_colors;
    struct mc_subled *subled_info;
};

struct mc_subled {
    unsigned int color_index;     /* LED_COLOR_ID_xxx */
    unsigned int intensity;       /* 颜色强度 (0 ~ max_brightness) */
    unsigned int brightness;      /* 计算后的亮度 */
    unsigned int channel;         /* 硬件通道 */
};
```

### 3.5 `struct led_lookup_data` — LED 查找表

```c
struct led_lookup_data {
    struct list_head list;
    const char *provider;   /* LED classdev 名称 */
    const char *dev_id;     /* 消费者设备名 */
    const char *con_id;     /* 连接标识 */
};
```

### 3.6 颜色 ID 常量

```c
/* 定义在 dt-bindings/leds/common.h */
#define LED_COLOR_ID_WHITE    0
#define LED_COLOR_ID_RED      1
#define LED_COLOR_ID_GREEN    2
#define LED_COLOR_ID_BLUE     3
#define LED_COLOR_ID_AMBER    4
#define LED_COLOR_ID_VIOLET   5
#define LED_COLOR_ID_YELLOW   6
#define LED_COLOR_ID_IR       7
#define LED_COLOR_ID_MULTI    8
#define LED_COLOR_ID_RGB      9
#define LED_COLOR_ID_PURPLE   10
#define LED_COLOR_ID_ORANGE   11
#define LED_COLOR_ID_PINK     12
#define LED_COLOR_ID_CYAN     13
#define LED_COLOR_ID_LIME     14
```

---

## 4. 核心 API 接口函数

### 4.1 LED 设备注册/注销

```c
/* 注册 LED (带初始化数据) */
int led_classdev_register_ext(struct device *parent,
                              struct led_classdev *led_cdev,
                              struct led_init_data *init_data);

/* 注册 LED (简单版本) */
static inline int led_classdev_register(struct device *parent,
                                        struct led_classdev *led_cdev);

/* devm 资源管理版本 */
int devm_led_classdev_register_ext(struct device *parent,
                                   struct led_classdev *led_cdev,
                                   struct led_init_data *init_data);
static inline int devm_led_classdev_register(struct device *parent,
                                             struct led_classdev *led_cdev);

/* 注销 */
void led_classdev_unregister(struct led_classdev *led_cdev);
void devm_led_classdev_unregister(struct device *parent,
                                  struct led_classdev *led_cdev);
```

### 4.2 亮度控制

```c
/* 设置亮度 (不可睡眠, 可从 IRQ 调用) */
void led_set_brightness(struct led_classdev *led_cdev, unsigned int brightness);

/* 同步设置亮度 (可睡眠) */
int led_set_brightness_sync(struct led_classdev *led_cdev, unsigned int value);

/* 不跳过 PM 的亮度设置 */
void led_set_brightness_nopm(struct led_classdev *led_cdev, unsigned int value);

/* 不可睡眠的亮度设置 */
void led_set_brightness_nosleep(struct led_classdev *led_cdev, unsigned int value);

/* 更新亮度 (从硬件读取) */
int led_update_brightness(struct led_classdev *led_cdev);

/* 多色 LED 亮度设置 */
void led_mc_set_brightness(struct led_classdev *led_cdev,
                           unsigned int *intensity_value,
                           unsigned int num_colors, unsigned int brightness);
```

### 4.3 闪烁控制

```c
/* 设置闪烁 (可睡眠, 优先使用硬件加速) */
void led_blink_set(struct led_classdev *led_cdev,
                   unsigned long *delay_on, unsigned long *delay_off);

/* 设置闪烁 (不可睡眠) */
void led_blink_set_nosleep(struct led_classdev *led_cdev,
                           unsigned long delay_on, unsigned long delay_off);

/* 单次闪烁 */
void led_blink_set_oneshot(struct led_classdev *led_cdev,
                           unsigned long *delay_on, unsigned long *delay_off,
                           int invert);

/* 停止软件闪烁 */
void led_stop_software_blink(struct led_classdev *led_cdev);
```

### 4.4 Trigger API

```c
/* 注册/注销 trigger */
int led_trigger_register(struct led_trigger *trigger);
void led_trigger_unregister(struct led_trigger *trigger);
int devm_led_trigger_register(struct device *dev, struct led_trigger *trigger);

/* 简易 trigger 注册 */
void led_trigger_register_simple(const char *name, struct led_trigger **trigger);
void led_trigger_unregister_simple(struct led_trigger *trigger);

/* trigger 事件 (向所有关联的 LED 发送亮度事件) */
void led_trigger_event(struct led_trigger *trigger, enum led_brightness event);
void led_mc_trigger_event(struct led_trigger *trig,
                          unsigned int *intensity_value,
                          unsigned int num_colors,
                          enum led_brightness brightness);

/* trigger 闪烁 */
void led_trigger_blink(struct led_trigger *trigger,
                       unsigned long delay_on, unsigned long delay_off);
void led_trigger_blink_oneshot(struct led_trigger *trigger,
                               unsigned long delay_on, unsigned long delay_off,
                               int invert);

/* trigger 绑定/解绑 */
int led_trigger_set(struct led_classdev *led_cdev, struct led_trigger *trigger);
void led_trigger_remove(struct led_classdev *led_cdev);
void led_trigger_set_default(struct led_classdev *led_cdev);

/* trigger 数据存取 */
static inline void led_set_trigger_data(struct led_classdev *led_cdev, void *data);
static inline void *led_get_trigger_data(struct led_classdev *led_cdev);

/* 快捷宏 */
#define DEFINE_LED_TRIGGER(x)        static struct led_trigger *x;
#define DEFINE_LED_TRIGGER_GLOBAL(x) struct led_trigger *x;
#define module_led_trigger(__led_trigger) \
    module_driver(__led_trigger, led_trigger_register, led_trigger_unregister)
```

### 4.5 LED Consumer API

```c
/* 获取 LED 设备 (通过 DT 或 lookup table) */
struct led_classdev *led_get(struct device *dev, char *con_id);
struct led_classdev *devm_led_get(struct device *dev, char *con_id);
struct led_classdev *devm_of_led_get(struct device *dev, int index);
struct led_classdev *devm_of_led_get_optional(struct device *dev, int index);
void led_put(struct led_classdev *led_cdev);

/* lookup table 管理 */
void led_add_lookup(struct led_lookup_data *led_lookup);
void led_remove_lookup(struct led_lookup_data *led_lookup);
```

### 4.6 PM 和辅助函数

```c
/* 电源管理 */
void led_classdev_suspend(struct led_classdev *led_cdev);
void led_classdev_resume(struct led_classdev *led_cdev);

/* sysfs 控制 */
void led_sysfs_disable(struct led_classdev *led_cdev);
void led_sysfs_enable(struct led_classdev *led_cdev);
static inline bool led_sysfs_is_disabled(struct led_classdev *led_cdev);

/* 名称合成 */
int led_compose_name(struct device *dev, struct led_init_data *init_data,
                     char *led_classdev_name);
const char *led_get_color_name(u8 color_id);

/* 默认模式 */
u32 *led_get_default_pattern(struct led_classdev *led_cdev, unsigned int *size);
enum led_default_state led_init_default_state_get(struct fwnode_handle *fwnode);

/* 硬件亮度变化通知 */
void led_classdev_notify_brightness_hw_changed(struct led_classdev *led_cdev,
                                               unsigned int brightness);

/* Multicolor 辅助 */
int led_mc_calc_color_components(struct led_classdev_mc *mcled_cdev,
                                 enum led_brightness brightness);
```

---

## 5. LED Trigger 机制

### 5.1 内置 Trigger 列表

| Trigger | 文件 | 功能 |
|---------|------|------|
| `timer` | `ledtrig-timer.c` | 定时闪烁，可配置 delay_on/delay_off |
| `heartbeat` | `ledtrig-heartbeat.c` | 心跳模式，频率随系统负载变化 |
| `default-on` | `ledtrig-default-on.c` | 激活时点亮 LED |
| `netdev` | `ledtrig-netdev.c` | 网络设备状态指示 (link/tx/rx) |
| `cpu` | `ledtrig-cpu.c` | CPU 活动指示 |
| `disk` | `ledtrig-disk.c` | 磁盘活动指示 (读/写) |
| `panic` | `ledtrig-panic.c` | 内核 panic 时闪烁 |
| `backlight` | `ledtrig-backlight.c` | 背光 trigger |
| `camera` | `ledtrig-camera.c` | 相机 flash/torch trigger |
| `oneshot` | `ledtrig-oneshot.c` | 单次闪烁 |
| `pattern` | `ledtrig-pattern.c` | 自定义亮度模式 |
| `transient` | `ledtrig-transient.c` | 临时激活 |
| `tty` | `ledtrig-tty.c` | 串口活动指示 |
| `input-events` | `ledtrig-input-events.c` | 输入事件指示 |
| `activity` | `ledtrig-activity.c` | 系统活动指示 |
| `gpio` | `ledtrig-gpio.c` | GPIO 输入联动 |
| `mtd` | `ledtrig-mtd.c` | MTD Flash 活动指示 |

### 5.2 Trigger 工作流

```
1. 注册 Trigger:
   led_trigger_register(trig)
     -> 加入全局 trigger_list
     -> 遍历 leds_list，匹配 default_trigger 的 LED 自动绑定

2. 用户绑定:
   echo "heartbeat" > /sys/class/leds/xxx/trigger
     -> led_trigger_write()
       -> led_trigger_set(led_cdev, trig)
         -> 旧 trigger deactivate
         -> 新 trigger activate
         -> 添加 trigger 的 sysfs groups

3. 事件分发:
   led_trigger_event(trig, LED_FULL)
     -> 遍历 trig->led_cdevs (RCU 保护)
       -> led_set_brightness() for each LED

4. 解绑:
   echo "none" > /sys/class/leds/xxx/trigger
     -> led_trigger_set(led_cdev, NULL)
       -> deactivate, 设置 LED_OFF
```

### 5.3 自定义 Trigger 示例

```c
static int my_trig_activate(struct led_classdev *led_cdev)
{
    /* 分配私有数据 */
    struct my_data *data = kzalloc(sizeof(*data), GFP_KERNEL);
    if (!data)
        return -ENOMEM;
    led_set_trigger_data(led_cdev, data);
    led_set_brightness(led_cdev, LED_FULL);
    return 0;
}

static void my_trig_deactivate(struct led_classdev *led_cdev)
{
    struct my_data *data = led_get_trigger_data(led_cdev);
    kfree(data);
}

static struct led_trigger my_trigger = {
    .name       = "my-trigger",
    .activate   = my_trig_activate,
    .deactivate = my_trig_deactivate,
};
module_led_trigger(my_trigger);
```

---

## 6. LED 命名规范

### 6.1 标准格式

```
<devicename:color:function>
```

- **devicename**: 设备名 (可选，热插拔设备必须)
- **color**: 颜色名 (white, red, green, blue, amber, ...)
- **function**: 功能描述 (status, power, disk-activity, ...)

### 6.2 DT 属性

```dts
leds {
    compatible = "gpio-leds";

    led-0 {
        gpios = <&gpio0 0 GPIO_ACTIVE_HIGH>;
        color = <LED_COLOR_ID_GREEN>;
        function = LED_FUNCTION_STATUS;
        linux,default-trigger = "heartbeat";
    };

    led-1 {
        gpios = <&gpio0 1 GPIO_ACTIVE_HIGH>;
        color = <LED_COLOR_ID_RED>;
        function = LED_FUNCTION_POWER;
        default-state = "on";
    };
};
```

### 6.3 sysfs 接口

```
/sys/class/leds/<led-name>/
├── brightness          # 读写亮度 (0 ~ max_brightness)
├── max_brightness      # 只读，最大亮度
├── trigger             # 读写，当前/可用 trigger 列表
├── delay_on            # timer trigger: 亮时间 (ms)
├── delay_off           # timer trigger: 灭时间 (ms)
├── brightness_hw_changed  # 硬件亮度变化通知 (需 CONFIG)
├── multi_intensity     # multicolor: 各色强度
└── multi_index         # multicolor: 颜色索引
```

---

## 7. 关键知识点汇总

### 7.1 并发与锁机制

| 锁 | 类型 | 保护对象 |
|----|------|----------|
| `leds_list_lock` | rwsem | 全局 LED 设备链表 |
| `triggers_list_lock` | rwsem | 全局 trigger 链表 |
| `led_cdev->led_access` | mutex | 单个 LED 的 sysfs 访问 |
| `led_cdev->trigger_lock` | rwsem | LED 的 trigger 绑定关系 |
| `trig->leddev_list_lock` | spinlock | trigger 控制的 LED 列表 |
| `led_cdev->work_flags` | atomic bits | 异步操作标志位 |

### 7.2 workqueue 设计

- 所有 LED 共享一个全局 workqueue (`leds_wq`)
- `set_brightness_work` 处理三种延迟操作:
  - `LED_SET_BRIGHTNESS`: 设置亮度
  - `LED_SET_BRIGHTNESS_OFF`: 关闭 LED (优先处理)
  - `LED_SET_BLINK`: 设置硬件闪烁
- 使用 `smp_mb__before_atomic()` 确保 `delayed_set_value` 对 work 可见

### 7.3 RCU 在 trigger 中的应用

- `trig->led_cdevs` 链表使用 RCU 保护
- `led_trigger_event()` 在 RCU read-side 遍历
- 添加/删除 LED 使用 `list_add_tail_rcu()`/`list_del_rcu()` + `synchronize_rcu()`
- 确保 trigger 事件分发的高性能和无锁读

### 7.4 devm 资源管理

推荐使用 devm_* 版本 API，自动在设备卸载时清理:
- `devm_led_classdev_register()` / `devm_led_classdev_register_ext()`
- `devm_led_trigger_register()`
- `devm_led_get()` / `devm_of_led_get()`

### 7.5 hw_control 机制 (硬件自主控制)

现代 LED 驱动（如网口 PHY LED）支持硬件自主控制模式：
- `hw_control_trigger`: 声明支持的 trigger 名称
- `hw_control_is_supported()`: 检查硬件是否支持请求的模式
- `hw_control_set()`: 配置硬件控制模式
- `hw_control_get()`: 读取当前硬件控制模式
- `hw_control_get_device()`: 获取关联的设备

---

## 8. QEMU 实验设计

### 实验 1: GPIO LED 基础控制

**目标**: 在 QEMU virt 机器上通过 DT overlay 添加 GPIO LED，验证 sysfs 控制

**步骤**:

1. 编写内核模块，使用平台设备模拟 GPIO LED:

```c
/* led_exp1_gpio_sim.c */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/leds.h>

struct sim_led {
    struct led_classdev cdev;
    unsigned int current_brightness;
};

static void sim_led_brightness_set(struct led_classdev *cdev,
                                    enum led_brightness value)
{
    struct sim_led *led = container_of(cdev, struct sim_led, cdev);
    led->current_brightness = value;
    pr_info("LED [%s] brightness set to %u\n", cdev->name, value);
}

static enum led_brightness sim_led_brightness_get(struct led_classdev *cdev)
{
    struct sim_led *led = container_of(cdev, struct sim_led, cdev);
    return led->current_brightness;
}

static struct sim_led my_leds[] = {
    {
        .cdev = {
            .name = "green:status",
            .max_brightness = 255,
            .brightness_set = sim_led_brightness_set,
            .brightness_get = sim_led_brightness_get,
            .default_trigger = "heartbeat",
            .flags = LED_CORE_SUSPENDRESUME,
        },
    },
    {
        .cdev = {
            .name = "red:power",
            .max_brightness = 1,
            .brightness_set = sim_led_brightness_set,
            .brightness_get = sim_led_brightness_get,
            .default_trigger = "default-on",
        },
    },
    {
        .cdev = {
            .name = "blue:disk",
            .max_brightness = 255,
            .brightness_set = sim_led_brightness_set,
            .brightness_get = sim_led_brightness_get,
        },
    },
};

static int __init sim_led_init(void)
{
    int i, ret;
    for (i = 0; i < ARRAY_SIZE(my_leds); i++) {
        ret = led_classdev_register(NULL, &my_leds[i].cdev);
        if (ret) {
            pr_err("Failed to register LED %s\n", my_leds[i].cdev.name);
            while (--i >= 0)
                led_classdev_unregister(&my_leds[i].cdev);
            return ret;
        }
    }
    pr_info("sim_led: %zu LEDs registered\n", ARRAY_SIZE(my_leds));
    return 0;
}

static void __exit sim_led_exit(void)
{
    int i;
    for (i = 0; i < ARRAY_SIZE(my_leds); i++)
        led_classdev_unregister(&my_leds[i].cdev);
    pr_info("sim_led: all LEDs unregistered\n");
}

module_init(sim_led_init);
module_exit(sim_led_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Simulated LED driver for QEMU testing");
```

**验证命令**:
```bash
# 加载模块
insmod led_exp1_gpio_sim.ko

# 查看注册的 LED
ls /sys/class/leds/

# 控制亮度
echo 128 > /sys/class/leds/green:status/brightness
cat /sys/class/leds/green:status/brightness

# 查看触发器
cat /sys/class/leds/green:status/trigger

# 切换触发器
echo timer > /sys/class/leds/green:status/trigger
echo 500 > /sys/class/leds/green:status/delay_on
echo 200 > /sys/class/leds/green:status/delay_off

# 查看 dmesg 中的亮度变化日志
dmesg | grep "LED \[" | tail -20

# 关闭 LED
echo 0 > /sys/class/leds/red:power/brightness

# 卸载模块
rmmod led_exp1_gpio_sim
```

---

### 实验 2: 自定义 LED Trigger

**目标**: 编写自定义 trigger，每次写入 sysfs 属性时闪烁 LED

```c
/* led_exp2_custom_trigger.c */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/leds.h>
#include <linux/slab.h>
#include <linux/timer.h>

struct pulse_data {
    struct led_classdev *led_cdev;
    struct timer_list timer;
    unsigned int count;
    unsigned int phase;
};

static void pulse_timer_fn(struct timer_list *t)
{
    struct pulse_data *pd = timer_container_of(pd, t, timer);

    if (pd->count == 0)
        return;

    if (pd->phase % 2 == 0) {
        led_set_brightness_nosleep(pd->led_cdev,
                                    pd->led_cdev->max_brightness);
    } else {
        led_set_brightness_nosleep(pd->led_cdev, LED_OFF);
        pd->count--;
    }
    pd->phase++;

    if (pd->count > 0)
        mod_timer(&pd->timer, jiffies + msecs_to_jiffies(200));
}

static ssize_t pulse_count_store(struct device *dev,
                                  struct device_attribute *attr,
                                  const char *buf, size_t size)
{
    struct led_classdev *led_cdev = led_trigger_get_led(dev);
    struct pulse_data *pd = led_get_trigger_data(led_cdev);
    unsigned int count;
    int ret;

    ret = kstrtouint(buf, 10, &count);
    if (ret)
        return ret;

    pd->count = count;
    pd->phase = 0;
    mod_timer(&pd->timer, jiffies + 1);

    return size;
}

static ssize_t pulse_count_show(struct device *dev,
                                 struct device_attribute *attr, char *buf)
{
    struct led_classdev *led_cdev = led_trigger_get_led(dev);
    struct pulse_data *pd = led_get_trigger_data(led_cdev);

    return sprintf(buf, "%u\n", pd->count);
}

static DEVICE_ATTR_RW(pulse_count);

static struct attribute *pulse_trig_attrs[] = {
    &dev_attr_pulse_count.attr,
    NULL,
};
ATTRIBUTE_GROUPS(pulse_trig);

static int pulse_activate(struct led_classdev *led_cdev)
{
    struct pulse_data *pd;

    pd = kzalloc(sizeof(*pd), GFP_KERNEL);
    if (!pd)
        return -ENOMEM;

    pd->led_cdev = led_cdev;
    timer_setup(&pd->timer, pulse_timer_fn, 0);
    led_set_trigger_data(led_cdev, pd);

    return 0;
}

static void pulse_deactivate(struct led_classdev *led_cdev)
{
    struct pulse_data *pd = led_get_trigger_data(led_cdev);

    timer_delete_sync(&pd->timer);
    kfree(pd);
}

static struct led_trigger pulse_trigger = {
    .name       = "pulse",
    .activate   = pulse_activate,
    .deactivate = pulse_deactivate,
    .groups     = pulse_trig_groups,
};
module_led_trigger(pulse_trigger);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Pulse LED trigger for QEMU testing");
```

**验证命令**:
```bash
# 先加载实验1的 LED 模块
insmod led_exp1_gpio_sim.ko

# 加载自定义 trigger
insmod led_exp2_custom_trigger.ko

# 查看 trigger 列表
cat /sys/class/leds/blue:disk/trigger
# 应看到 [none] ... pulse ...

# 绑定 trigger
echo pulse > /sys/class/leds/blue:disk/trigger

# 触发 3 次闪烁
echo 3 > /sys/class/leds/blue:disk/pulse_count

# 查看闪烁日志
dmesg | grep "LED \[blue:disk\]" | tail -10

# 切换回 none
echo none > /sys/class/leds/blue:disk/trigger
```

---

### 实验 3: 使用 uleds 用户空间 LED 驱动

**目标**: 通过 `/dev/uleds` 在用户空间创建虚拟 LED，演示用户空间 LED 驱动机制

**前置条件**: 内核配置 `CONFIG_LEDS_USER=y`

```c
/* led_exp3_uleds_test.c - 用户空间程序 */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <linux/uleds.h>

int main(void)
{
    struct uleds_user_dev udev;
    int fd, ret, brightness;

    /* 打开 uleds 设备 */
    fd = open("/dev/uleds", O_RDWR);
    if (fd < 0) {
        perror("open /dev/uleds");
        return 1;
    }

    /* 配置虚拟 LED */
    memset(&udev, 0, sizeof(udev));
    strncpy(udev.name, "user::virtual", sizeof(udev.name) - 1);
    udev.max_brightness = 100;

    /* 注册 LED */
    ret = write(fd, &udev, sizeof(udev));
    if (ret < 0) {
        perror("write (register)");
        close(fd);
        return 1;
    }

    printf("Virtual LED 'user::virtual' registered!\n");
    printf("Check: ls /sys/class/leds/user::virtual/\n");
    printf("Set:   echo 50 > /sys/class/leds/user::virtual/brightness\n\n");

    /* 持续读取亮度变化 */
    printf("Listening for brightness changes (Ctrl+C to quit)...\n");
    while (1) {
        ret = read(fd, &brightness, sizeof(brightness));
        if (ret < 0) {
            perror("read");
            break;
        }
        printf("  brightness changed to: %d\n", brightness);
    }

    close(fd);
    return 0;
}
```

**验证步骤**:
```bash
# 编译用户空间程序
# aarch64-linux-gnu-gcc -static -o uleds_test led_exp3_uleds_test.c

# 在 QEMU 中运行
./uleds_test &

# 另一个终端/shell 中操作
ls /sys/class/leds/user::virtual/
echo 50 > /sys/class/leds/user::virtual/brightness
echo 0 > /sys/class/leds/user::virtual/brightness
echo heartbeat > /sys/class/leds/user::virtual/trigger

# 观察 uleds_test 输出的亮度变化
```

---

### 实验 4: LED 与内核子系统联动 (blocking vs non-blocking)

**目标**: 对比 `brightness_set` (非阻塞) 和 `brightness_set_blocking` (阻塞) 两条路径

```c
/* led_exp4_blocking_test.c */
#include <linux/module.h>
#include <linux/leds.h>
#include <linux/delay.h>
#include <linux/workqueue.h>

static struct led_classdev fast_led;
static struct led_classdev slow_led;

/* 非阻塞: 直接设置, 不可睡眠 */
static void fast_led_set(struct led_classdev *cdev, enum led_brightness value)
{
    pr_info("fast_led: set to %u (in_atomic=%d, pid=%d)\n",
            value, in_atomic(), current->pid);
}

/* 阻塞: 模拟 I2C 慢速设备 */
static int slow_led_set_blocking(struct led_classdev *cdev,
                                  enum led_brightness value)
{
    pr_info("slow_led: blocking set start, value=%u (pid=%d)\n",
            value, current->pid);
    msleep(50);  /* 模拟 I2C 传输延迟 */
    pr_info("slow_led: blocking set done, value=%u\n", value);
    return 0;
}

static int __init blocking_test_init(void)
{
    int ret;

    fast_led.name = "test:green:fast";
    fast_led.max_brightness = 255;
    fast_led.brightness_set = fast_led_set;

    slow_led.name = "test:red:slow";
    slow_led.max_brightness = 255;
    slow_led.brightness_set_blocking = slow_led_set_blocking;

    ret = led_classdev_register(NULL, &fast_led);
    if (ret)
        return ret;

    ret = led_classdev_register(NULL, &slow_led);
    if (ret) {
        led_classdev_unregister(&fast_led);
        return ret;
    }

    pr_info("blocking_test: registered fast_led and slow_led\n");
    return 0;
}

static void __exit blocking_test_exit(void)
{
    led_classdev_unregister(&fast_led);
    led_classdev_unregister(&slow_led);
}

module_init(blocking_test_init);
module_exit(blocking_test_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("LED blocking vs non-blocking test");
```

**验证命令**:
```bash
insmod led_exp4_blocking_test.ko

# 快速设置 (直接调用 brightness_set)
echo 100 > /sys/class/leds/test:green:fast/brightness

# 慢速设置 (通过 workqueue 调用 brightness_set_blocking)
echo 100 > /sys/class/leds/test:red:slow/brightness

# 设置 timer trigger 观察 workqueue 行为
echo timer > /sys/class/leds/test:red:slow/trigger
echo 100 > /sys/class/leds/test:red:slow/delay_on
echo 100 > /sys/class/leds/test:red:slow/delay_off

# 对比 dmesg 日志中的 pid 和 in_atomic 信息
dmesg | grep -E "fast_led|slow_led" | tail -20
```

---

### 实验 5: LED Trigger 事件机制——模拟内核事件驱动 LED

**目标**: 编写一个内核模块，注册 simple trigger 并用内核定时器模拟事件驱动 LED

```c
/* led_exp5_event_trigger.c */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/leds.h>
#include <linux/timer.h>

DEFINE_LED_TRIGGER(my_event_trigger);

static struct timer_list event_timer;
static unsigned int event_count;

static void event_timer_fn(struct timer_list *t)
{
    event_count++;
    pr_info("event_trigger: event #%u fired\n", event_count);

    /* 模拟: 偶数事件亮灯, 奇数事件灭灯 */
    if (event_count % 2)
        led_trigger_event(my_event_trigger, LED_FULL);
    else
        led_trigger_event(my_event_trigger, LED_OFF);

    /* 每 2 秒触发一次, 持续 20 次 */
    if (event_count < 20)
        mod_timer(&event_timer, jiffies + msecs_to_jiffies(2000));
    else
        pr_info("event_trigger: all events done\n");
}

static int __init event_trigger_init(void)
{
    led_trigger_register_simple("my-event", &my_event_trigger);
    if (!my_event_trigger) {
        pr_err("Failed to register my-event trigger\n");
        return -ENOMEM;
    }

    event_count = 0;
    timer_setup(&event_timer, event_timer_fn, 0);
    mod_timer(&event_timer, jiffies + msecs_to_jiffies(2000));

    pr_info("event_trigger: module loaded, trigger 'my-event' registered\n");
    return 0;
}

static void __exit event_trigger_exit(void)
{
    timer_delete_sync(&event_timer);
    led_trigger_unregister_simple(my_event_trigger);
    pr_info("event_trigger: module unloaded\n");
}

module_init(event_trigger_init);
module_exit(event_trigger_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Event-driven LED trigger demo");
```

**验证命令**:
```bash
# 加载 LED 设备模块 (实验1)
insmod led_exp1_gpio_sim.ko

# 加载事件 trigger
insmod led_exp5_event_trigger.ko

# 绑定 trigger
echo my-event > /sys/class/leds/blue:disk/trigger

# 观察日志 (每 2 秒一次事件)
dmesg -w | grep -E "event_trigger|LED \[blue"

# 等待 40 秒后查看
dmesg | grep "event_trigger" | tail -25
```

---

### 实验 6: PL061 GPIO + 自定义 DTB 真实硬件链路实验

**目标**: 利用 QEMU virt 内置的 PL061 GPIO 控制器，通过自定义 DTB 添加 `gpio-leds` 节点，走完 **DT 解析 → platform driver match → GPIO 子系统 → LED 子系统** 的完整硬件驱动链路，与真实开发板体验一致。

**前置条件**:
- `CONFIG_GPIO_PL061=y` (已在 ybzhang_defconfig 中启用)
- `CONFIG_LEDS_GPIO=y` (已启用)
- 安装 `dtc` (device tree compiler)

#### 步骤 1: 导出 QEMU virt 自动生成的 DTB

QEMU `-machine virt` 会在内部动态生成设备树，不传 `-dtb` 时 Guest 使用的就是这棵树。先把它导出来：

```bash
# 导出 DTB (不启动 Guest，只生成 DTB 文件)
qemu-system-aarch64 -machine virt,dumpdtb=virt-orig.dtb \
    -cpu cortex-a57 -m 1024 -smp 4 -nographic

# 反编译为可编辑的 dts 文本
dtc -I dtb -O dts -o virt-orig.dts virt-orig.dtb

# 查看其中的 PL061 GPIO 控制器节点 (记下 phandle 和地址)
grep -A5 "pl061" virt-orig.dts
# 典型输出:
#   pl061@9030000 {
#       compatible = "arm,pl061", "arm,primecell";
#       reg = <0x00 0x9030000 0x00 0x1000>;
#       interrupts = <0x00 0x07 0x04>;
#       gpio-controller;
#       #gpio-cells = <0x02>;
#       phandle = <0x8005>;   ← 记住这个 phandle 值
#   };
```

#### 步骤 2: 添加 gpio-leds 节点

```bash
# 复制原始 dts
cp virt-orig.dts virt-leds.dts
```

编辑 `virt-leds.dts`，在根节点 (`/ { ... }`) 的**末尾、闭合 `};` 之前**添加 gpio-leds 节点：

```dts
/*
 * 在 virt-leds.dts 的根节点末尾添加以下内容
 * 注意: <0x8005> 是 pl061 的 phandle，根据步骤 1 实际值替换
 */
gpio-leds {
    compatible = "gpio-leds";

    led-green {
        gpios = <0x8005 0 0>;       /* pl061 phandle, GPIO 0, active high */
        label = "green:status";
        linux,default-trigger = "heartbeat";
    };

    led-red {
        gpios = <0x8005 1 0>;       /* GPIO 1, active high */
        label = "red:power";
        default-state = "on";
    };

    led-blue {
        gpios = <0x8005 2 0>;       /* GPIO 2, active high */
        label = "blue:activity";
        linux,default-trigger = "none";
    };

    led-yellow {
        gpios = <0x8005 3 0>;       /* GPIO 3, active high */
        label = "yellow:disk";
        linux,default-trigger = "disk-activity";
    };
};
```

> **phandle 说明**: QEMU 每次生成的 phandle 值可能不同。务必用步骤 1 中 `grep` 输出的实际值。也可以在 dts 中给 pl061 节点加一个 label，然后用 `&label` 引用：
> ```dts
> /* 在 pl061 节点上添加 label */
> gpio0: pl061@9030000 { ... };
> /* 然后在 gpio-leds 中引用 */
> gpios = <&gpio0 0 0>;
> ```

#### 步骤 3: 编译自定义 DTB

```bash
dtc -I dts -O dtb -o virt-leds.dtb virt-leds.dts
```

如果 dtc 报 Warning 可加 `-W no-unit_address_vs_reg` 等忽略。

#### 步骤 4: 用自定义 DTB 启动 QEMU

修改 `launch.sh` 或直接运行：

```bash
qemu-system-aarch64 -machine virt -cpu cortex-a57 -m 1024 -smp 4 \
    -kernel arch/arm64/boot/Image \
    -dtb virt-leds.dtb \
    --append "nokaslr rdinit=/linuxrc console=ttyAMA0" \
    -nographic \
    --fsdev local,id=kmod_dev,path=$PWD/kmodules,security_model=none \
    -device virtio-9p-device,fsdev=kmod_dev,mount_tag=kmod_mount
```

关键区别：增加了 **`-dtb virt-leds.dtb`** 参数。

#### 步骤 5: 验证 — Guest 内核中操作

```bash
# === 1. 确认 LED 设备已注册 ===
ls /sys/class/leds/
# 预期输出: blue:activity  green:status  red:power  yellow:disk

# === 2. 确认走的是 gpio-leds 驱动 ===
cat /sys/class/leds/green:status/device/driver/uevent
# 或
ls -la /sys/class/leds/green:status/device/driver
# 应指向 gpio-leds

# === 3. 查看 GPIO 使用状态 (完整硬件链路证据) ===
cat /sys/kernel/debug/gpio
# 预期输出:
# gpiochip0: GPIOs 0-7, parent: platform/9030000.pl061, pl061_gpio:
#  gpio-0   (green:status        ) out hi    ← heartbeat 在闪
#  gpio-1   (red:power           ) out hi    ← default-state on
#  gpio-2   (blue:activity       ) out lo    ← 初始关闭
#  gpio-3   (yellow:disk         ) out lo    ← 等待磁盘活动

# === 4. 手动控制亮度 ===
echo 0 > /sys/class/leds/red:power/brightness
cat /sys/kernel/debug/gpio | grep gpio-1
#  gpio-1   (red:power           ) out lo    ← GPIO 电平变低

echo 1 > /sys/class/leds/red:power/brightness
cat /sys/kernel/debug/gpio | grep gpio-1
#  gpio-1   (red:power           ) out hi    ← GPIO 电平恢复

# === 5. trigger 操作 ===
# heartbeat 已在闪烁，观察 GPIO 电平在 hi/lo 之间切换
watch -n 0.5 'grep "gpio-0" /sys/kernel/debug/gpio'

# 切换 trigger
echo timer > /sys/class/leds/blue:activity/trigger
echo 200 > /sys/class/leds/blue:activity/delay_on
echo 800 > /sys/class/leds/blue:activity/delay_off
watch -n 0.3 'grep "gpio-2" /sys/kernel/debug/gpio'

# === 6. 查看完整驱动链路 ===
# DT 节点
ls /sys/firmware/devicetree/base/gpio-leds/
# 输出: compatible  led-blue  led-green  led-red  led-yellow  name

cat /sys/firmware/devicetree/base/gpio-leds/led-green/linux,default-trigger
# 输出: heartbeat
```

#### 步骤 6: 从 QEMU 宿主机侧观察 GPIO 寄存器 (可选)

```bash
# 启动 QEMU 时加 monitor 端口
qemu-system-aarch64 ... \
    -monitor telnet:localhost:4444,server,nowait

# 在宿主机另一个终端连接 QEMU monitor
telnet localhost 4444

# 读取 PL061 数据寄存器 (基地址 0x09030000, 偏移 0x3FC = 全位掩码读)
(qemu) xp /1xb 0x090303FC
# 输出示例: 0x090303fc: 0x03
# 解析: bit0=1(green亮) bit1=1(red亮) bit2=0(blue灭) bit3=0(yellow灭)

# Guest 中关掉 red LED 后再读
# Guest: echo 0 > /sys/class/leds/red:power/brightness
(qemu) xp /1xb 0x090303FC
# 输出: 0x090303fc: 0x01
# bit1 变为 0，说明 GPIO1 电平确实变了
```

> **PL061 寄存器说明**: 地址 `base + 0x3FC` 是数据寄存器的全位掩码读地址（读取 GPIO[7:0] 所有引脚电平）。单个位 N 的掩码地址是 `base + (1 << (N+2))`。

#### 完整驱动链路图

```
Device Tree (virt-leds.dtb)
  └─ gpio-leds { compatible = "gpio-leds"; led-green { gpios = <&pl061 0 0>; } }
       │
       │  platform_bus match by compatible
       ▼
leds-gpio.c  (drivers/leds/leds-gpio.c)
  └─ gpio_leds_create()
       └─ create_gpio_led()
            ├─ devm_led_classdev_register_ext()  →  LED Class 注册
            │    └─ /sys/class/leds/green:status/
            └─ gpiod_direction_output()  →  GPIO 子系统
                 └─ pl061_direction_output()  →  PL061 GPIO 驱动
                      └─ 写 MMIO 寄存器 0x09030000+offset
                           └─ QEMU PL061 设备模型处理
```

#### 方案对比

| 方面 | 内核模块模拟 (实验 1-5) | PL061 GPIO + DTB (本实验) |
|------|------------------------|---------------------------|
| 驱动链路 | 直接调用 `led_classdev_register` | DT → platform match → GPIO → LED |
| GPIO 子系统 | 不经过 | **完整经过** |
| 硬件寄存器 | 无 | PL061 MMIO 寄存器可读 |
| DT 解析 | 无 | **完整 DT 解析** |
| 状态观察 | `printk` / dmesg | `debugfs/gpio` + QEMU monitor |
| 与真实板子差异 | 缺少硬件层 | **几乎一致** |
| 适合学习 | LED 子系统框架 | **完整 LED 驱动开发流程** |

---

### 实验 7: Kconfig 和 Makefile 模板

**用于将上述实验模块集成到内核构建系统中**:

```makefile
# drivers/leds/experiments/Makefile
obj-$(CONFIG_LED_EXP1_GPIO_SIM)     += led_exp1_gpio_sim.o
obj-$(CONFIG_LED_EXP2_CUSTOM_TRIG)  += led_exp2_custom_trigger.o
obj-$(CONFIG_LED_EXP4_BLOCKING)     += led_exp4_blocking_test.o
obj-$(CONFIG_LED_EXP5_EVENT_TRIG)   += led_exp5_event_trigger.o
```

或使用外部模块构建:

```makefile
# Makefile (放在实验目录)
KDIR ?= /path/to/linux-6.18.1
ARCH ?= arm64
CROSS_COMPILE ?= aarch64-linux-gnu-

obj-m += led_exp1_gpio_sim.o
obj-m += led_exp2_custom_trigger.o
obj-m += led_exp4_blocking_test.o
obj-m += led_exp5_event_trigger.o

all:
	make -C $(KDIR) M=$(PWD) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) modules

clean:
	make -C $(KDIR) M=$(PWD) ARCH=$(ARCH) clean
```

---

## 附录: 快速参考

### sysfs 操作速查

```bash
# 列出所有 LED
ls /sys/class/leds/

# 查看/设置亮度
cat /sys/class/leds/<led>/brightness
echo <value> > /sys/class/leds/<led>/brightness

# 查看最大亮度
cat /sys/class/leds/<led>/max_brightness

# 查看可用 trigger (当前 trigger 用 [] 标记)
cat /sys/class/leds/<led>/trigger

# 设置 trigger
echo heartbeat > /sys/class/leds/<led>/trigger
echo none > /sys/class/leds/<led>/trigger

# timer trigger 参数
echo 500 > /sys/class/leds/<led>/delay_on
echo 300 > /sys/class/leds/<led>/delay_off
```

### 驱动开发 Checklist

- [ ] 选择 `brightness_set` (非阻塞) 还是 `brightness_set_blocking` (可阻塞)
- [ ] 使用 `devm_led_classdev_register_ext()` + `led_init_data` (推荐)
- [ ] 设置合适的 `max_brightness`
- [ ] 设置 `default_trigger` (可选)
- [ ] 设置 `LED_CORE_SUSPENDRESUME` 标志 (如需自动 PM)
- [ ] 热插拔设备设置 `LED_HW_PLUGGABLE` 和 `devname_mandatory`
- [ ] 实现 `brightness_get()` 以支持状态读回
- [ ] 支持硬件闪烁时实现 `blink_set()`
- [ ] DT binding 使用 `color` + `function` 属性命名
