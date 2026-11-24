---
title: "架构决策记录 (ADR)"
---

# 架构决策记录 (ADR)

本目录记录 SiFli EPD Platform 的关键架构决策、设计理由和技术选型依据。这些文档回答"为什么这样设计"，与其他架构文档中描述的"是什么"和"怎么做"形成互补。

## ADR 索引

| 编号 | 标题 | 状态 | 日期 |
| --- | --- | --- | --- |
| [ADR-001](001-dual-core-architecture.md) | 双核架构与任务分工 | 已采纳 | 2024 |
| [ADR-002](002-memory-partitioning.md) | 内存分区策略 | 已采纳 | 2024 |
| [ADR-003](003-technology-stack.md) | 技术栈选型 | 已采纳 | 2024 |
| [ADR-004](004-dynamic-application-model.md) | 动态应用模型 | 已采纳 | 2024 |
| [ADR-005](005-platform-comparison.md) | 平台定位与差异化 | 已采纳 | 2024 |

## 什么是 ADR

架构决策记录 (Architecture Decision Record, ADR) 是一种轻量级的文档形式，用于记录项目中做出的重要架构决策。每条记录包含：

- **背景**：决策时的技术和业务环境
- **决策**：具体选择的方案
- **理由**：为什么选择这个方案
- **后果**：决策带来的影响（正面和负面）
- **替代方案**：考虑过但未采纳的其他方案

## 文档状态

- **提议中**：正在讨论的方案
- **已采纳**：已实施的决策
- **已废弃**：不再使用的旧决策
- **已取代**：被新决策替换

## 阅读指引

如果你想了解：

- **为何采用双核架构** → 阅读 [ADR-001](001-dual-core-architecture.md)
- **内存为何这样分配** → 阅读 [ADR-002](002-memory-partitioning.md)
- **为何选择 RT-Thread 和 LVGL** → 阅读 [ADR-003](003-technology-stack.md)
- **为何使用动态应用而非静态编译** → 阅读 [ADR-004](004-dynamic-application-model.md)
- **与 Android/PebbleOS 的本质区别** → 阅读 [ADR-005](005-platform-comparison.md)

## 相关文档

- [系统架构总览](../architecture.md)：描述当前系统的整体架构
- [应用运行与生命周期](../application-runtime.md)：应用的运行机制
- [内存与存储](../memory-storage.md)：内存管理的具体实现
- [阅读与双核计算](../reading-architecture.md)：双核协作的实际应用
