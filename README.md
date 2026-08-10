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
