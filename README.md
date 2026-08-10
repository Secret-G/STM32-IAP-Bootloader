可以，我给你整理一版**偏求职/GitHub展示风格的 README.md**。

我不会把你的技术细节删掉，因为你的项目亮点就在可靠性；但是会调整结构：

* 首页更像开源项目
* 突出设计思想
* 减少“流水账”
* 增加面试友好的设计说明
* 保留真实实现，不夸大

你可以直接复制成 `README.md`。

---

```markdown
# STM32F407 可靠固件升级系统

> Bootloader + A/B Slot + Run运行区 + Trial/Confirm + 自动Rollback + Qt6升级工具

---

## 项目简介

本项目是一套基于 **STM32F407ZGT6** 的串口 IAP 固件升级系统。

针对传统固件升级过程中可能出现的：

- 升级掉电导致程序损坏
- 固件传输错误
- APP启动失败
- 新版本运行异常无法恢复

等问题，设计了一套可靠升级机制。

系统由三部分组成：

- STM32 Bootloader
- A/B 固件备份槽
- Qt6 PC升级工具


核心设计：

> A/B槽用于保存固件镜像，Run区域作为唯一执行区域。Bootloader负责镜像管理、校验、安装和回滚。


---

# 核心特点

## 1. A/B Slot + Run运行区

- A/B槽保存候选固件和旧版本固件
- APP不直接在备份槽运行
- Bootloader负责搬运镜像到固定Run区域执行
- A/B两个APP采用相同链接地址


## 2. 双Flag事务存储

- 两份Flag副本轮换保存
- Sequence递增选择最新状态
- CRC校验保证Flag有效性
- 支持掉电恢复


## 3. UART可靠升级协议

支持：

- START / DATA / END命令
- CRC16帧校验
- ACK/NACK应答
- 包序号检测
- 超时重发
- 重复数据包处理


## 4. Trial / Confirm机制

新固件升级后不会立即成为正式版本：

```

安装新版本

↓

Trial试运行

↓

APP确认

↓

正式提交

```

如果APP启动异常，则自动回滚。


## 5. IWDG异常检测

APP运行期间开启独立看门狗：

```

APP异常

↓

IWDG Reset

↓

Bootloader检测

↓

Rollback恢复旧版本

````


## 6. Qt6升级工具

实现完整升级闭环：

- BIN选择
- CRC计算
- 版本输入
- 协议组帧
- 固件分包发送
- ACK处理
- 重传
- 升级进度显示


---

# 系统架构

```mermaid
flowchart LR

PC["Qt6升级工具"]

PC -->|"START/DATA/END"| UART["USART1"]

UART --> BOOT["Bootloader"]


BOOT --> SLOT_A["APP Slot A"]

BOOT --> SLOT_B["APP Slot B"]


SLOT_A --> RUN["Run运行区"]

SLOT_B --> RUN


BOOT <--> FLAG0["Flag Copy 0"]

BOOT <--> FLAG1["Flag Copy 1"]


BOOT -->|"TRIAL_REQUEST"| APP["APP"]

APP -->|"CONFIRMED"| BOOT

APP --> IWDG["IWDG Reset"]

IWDG --> BOOT

````

---

# Flash布局

芯片：

```
STM32F407ZGTx
Flash: 1MB
```

| 区域         | Sector | 地址                    | 大小    | 作用      |
| ---------- | ------ | --------------------- | ----- | ------- |
| Bootloader | 0~3    | 0x08000000-0x0800FFFF | 64KB  | 启动与升级控制 |
| Flag Copy0 | 4      | 0x08010000-0x0801FFFF | 64KB  | 状态副本    |
| Run APP    | 5~6    | 0x08020000-0x0805FFFF | 256KB | APP执行区域 |
| APP Slot A | 7~8    | 0x08060000-0x0809FFFF | 256KB | 固件备份    |
| APP Slot B | 9~10   | 0x080A0000-0x080DFFFF | 256KB | 固件备份    |
| Flag Copy1 | 11     | 0x080E0000-0x080FFFFF | 128KB | 状态副本    |

APP链接：

```
APP Address:
0x08020000

VTOR Offset:
0x00020000
```

设计原因：

采用独立Run区域，使A/B镜像保持统一链接地址，降低双镜像启动管理复杂度。

---

# 升级流程

## 正常升级

```
Qt发送固件

↓

START

↓

版本检查

↓

选择非活动Slot

↓

DATA分包写入

↓

END校验CRC

↓

Flag提交PENDING

↓

Boot复位

↓

安装到Run区域

↓

Trial运行

↓

APP Confirm

↓

版本提交
```

## 失败恢复

```
APP启动失败

↓

未收到Confirm

↓

IWDG Reset

↓

Bootloader检测

↓

Rollback

↓

恢复旧版本
```

---

# 状态机

升级状态通过Flag保存：

| 状态            | 说明          |
| ------------- | ----------- |
| IDLE          | 正常运行        |
| PENDING       | 固件接收完成，等待安装 |
| INSTALLING    | 正在搬运镜像      |
| TRIAL_READY   | 等待试运行       |
| TRIAL_RUNNING | 等待APP确认     |
| ROLLBACK      | 执行回滚        |
| ERROR         | 异常状态        |

状态流转：

```
IDLE

↓

PENDING

↓

INSTALLING

↓

TRIAL_READY

↓

TRIAL_RUNNING

↓

CONFIRMED

↓

IDLE


失败：

TRIAL_RUNNING

↓

ROLLBACK

↓

IDLE
```

---

# UART升级协议

通信参数：

```
115200
8N1
无流控
```

协议格式：

```
CMD(2B)

+

LEN(2B)

+

DATA(NB)

+

RESERVE(4B)

+

CRC16(2B)
```

支持命令：

| 命令           | 作用     |
| ------------ | ------ |
| START_UPDATE | 开始升级   |
| DATA_PACKET  | 发送固件数据 |
| END_UPDATE   | 结束升级   |
| ACK          | 成功响应   |
| NACK         | 错误响应   |

可靠机制：

* 分包传输
* ACK确认
* 超时重发
* 重复包检测

---

# 双Flag可靠存储

Flag保存：

* Magic
* 结构版本
* Sequence
* Slot信息
* 固件大小
* CRC
* 版本号
* 当前状态

更新流程：

```
生成新Flag

↓

Sequence++

↓

擦除备用Flag

↓

写入

↓

回读校验

↓

提交成功
```

启动时：

```
读取Flag0

读取Flag1

↓

CRC检查

↓

选择最新有效版本
```

---

# 镜像完整性校验

升级过程中：

1. DATA阶段写入备份槽
2. END阶段检查：

   * 数据大小
   * 包数量
   * CRC16
3. 安装到Run区后再次CRC校验
4. APP启动前检查：

   * 栈地址
   * Reset向量
   * 镜像CRC

---

# Trial / Confirm机制

为什么需要Trial？

CRC只能证明：

> 固件数据正确

不能证明：

> APP能够正常运行

流程：

```
Bootloader安装新版本

↓

写TRIAL_REQUEST

↓

启动APP

↓

APP运行检测

↓

写CONFIRMED

↓

复位

↓

正式提交
```

确认存储：

```
RTC Backup Register
```

---

# 版本管理

版本采用32位整数保存：

```
V3.1.2.6

↓

0x03010206
```

格式：

```
31~24 Major

23~16 Minor

15~8  Patch

7~0   Build
```

升级策略：

| 情况   | 处理   |
| ---- | ---- |
| 首次安装 | 允许   |
| 高版本  | 允许升级 |
| 同版本  | 允许重装 |
| 低版本  | 拒绝   |

版本比较基于：

```
active_slot
```

而不是未确认的新版本。

---

# Qt6升级工具

基于：

* Qt6 Widgets
* Qt SerialPort
* CMake

功能：

* 串口扫描
* BIN读取
* CRC计算
* 版本输入
* 自动分包
* ACK解析
* 超时重发
* 日志显示

特点：

Qt不维护A/B布局。

只发送：

```
升级这个固件
```

目标Slot由Bootloader自动选择。

---

# 测试验证

已完成：

| 测试          | 结果 |
| ----------- | -- |
| 正常A/B升级     | 通过 |
| UART分包接收    | 通过 |
| ACK丢失重传     | 通过 |
| CRC错误检测     | 通过 |
| Flash写入掉电恢复 | 通过 |
| Flag更新掉电恢复  | 通过 |
| 安装阶段掉电恢复    | 通过 |
| Trial失败回滚   | 通过 |
| IWDG异常复位恢复  | 通过 |
| 首次安装失败处理    | 通过 |
| 版本降级拒绝      | 通过 |
| 版本回滚一致性     | 通过 |

---

# 设计取舍

## 为什么采用A/B Slot + Run？

相比双APP直接运行：

优点：

* APP统一链接地址
* 向量表固定
* Bootloader逻辑简单

代价：

* 切换版本需要Flash搬运

---

## 为什么使用双Flag？

单Flag：

```
擦除

↓

写入

↓

掉电
```

可能导致状态丢失。

双Flag：

```
旧状态

+

新状态

```

保证至少存在一个有效记录。

---

## 为什么CRC不能保证安全？

CRC只能检测：

* 数据错误
* Flash损坏

不能验证：

* 固件来源
* 固件是否被篡改

如果产品化，需要增加：

* 数字签名
* 加密验证

---

# 后续扩展

APP层可进一步增加：

* WiFi通信
* Ethernet通信
* 远程升级触发

保持Bootloader升级框架不变。


这样面试官想深入看，也有东西。👍
```
