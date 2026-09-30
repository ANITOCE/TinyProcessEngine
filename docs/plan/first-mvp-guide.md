> 🔒 **本指南已封存(只读)**:全部 Phase 已完成并验收通过。禁止修改本文件任何内容;后续迭代请新建开发指南。

# TinyProcessEngine First MVP 开发指南

> **存放位置**: `docs/plan/first-mvp-guide.md`(**公开区**,纳入 git)
> **下游关系**: 本指南第 4 节的路线图条目是各 Phase 文档(按 `docs/templates/phase-template.md` 实例化,存放于 `docs/phases/`)与各 Phase 制品(`specs/<分支名>/`)的唯一事实源。任何实现与本指南冲突时,先修订本指南(走评审),再调整实现。
> **只读封存**: 全部 Phase 完成并验收通过后,必须按第 9 节将本文档**封存为只读**;此后禁止修改或删除本文档的任何内容,功能变更一律新建开发指南。

---

## 文档信息

| 字段 | 内容 |
|---|---|
| 项目/功能名 | TinyProcessEngine — First MVP |
| 指南版本 | 1.0.4 |
| 状态 | `done(已封存,只读)`(2026-09-30 全部 Phase 完成并验收通过,项目级 DoD §6.2 逐项通过,按 §9 封存;原 2026-09-18 评审通过) |
| 负责人(owner) | anitoce |
| 创建日期 | 2026-09-18 |
| 最近更新 | 2026-09-30 |
| 关联仓库 | 本地 Git 仓库(未配置远端;如建立公开远端,按第 3 节公开边界执行) |
| 工具链 | VS Code Copilot Chat(不使用 Copilot CLI);spec-kit `0.16.1`(copilot 集成,PowerShell 脚本) |
| 场景 | 存量迭代 |

---

## 1. 概述

### 1.1 背景与动机

TinyProcessEngine 是跨平台(Windows/Linux)的进程内存扫描与编辑工具(类 Cheat Engine),当前提供 CLI 前端;CE_UI 图形界面与远程连接等长线方向由后续新指南承接。Phase 01–03(基础重构、核心内存扫描、Linux 平台支持)已交付,其制品位于主检出 `specs/001–003`(私有区);但 2026-09 的独立审计发现 7 项功能性缺陷(详见 §4.2 Phase 05)与若干规范偏差;**CLI 旧式交互流程已完成重构(Phase 04,2026-09-21 合并),审计缺陷已全部清零(Phase 05,2026-09-24 合并),规范偏差已收敛(Phase 06,2026-09-26 合并)**。本指南取代旧版《DEVELOPMENT_GUIDE.md》v0.2.0(`docs/temp/`,随本指南落地删除;其第 11 章 CLI 交互规范已迁移为 `docs/plan/cli-interaction-spec.md`),聚焦 **First MVP** 落地:CLI 交互重构(已完成)→ 审计修复(已完成)→ 规范收敛(已完成)→ MVP 验收,为后续新指南建立干净基线。

### 1.2 目标

- **G1 CLI 交互重构**:按《CLI 交互规范》(`docs/plan/cli-interaction-spec.md`,§11.2–§11.10)完成终端旗标命令、REPL 命令全集、`startup` 分层与 Test.exe 测试工具;旧交互流程(类型选择菜单、`Enter value` 追问)全部移除。可验证:规范中各命令行为与退出码逐项通过(测试或运行记录)。
- **G2 审计缺陷清零**:Phase 01–03 审计的 7 项缺陷逐项"先复现、后修复、带回归测试";增量扫描语义正确;Windows/Linux 读写路径可用;测试资产纳入 CTest 且可被顶层发现。可验证:缺陷—证据对照表逐项关闭。
- **G3 规范符合性收敛**:公共 API 归入 `tpe::` 命名空间;头文件 `.hpp` + `#pragma once`;库层无 `exit`/以异常作常规控制流;构建告警(UTF-8 代码页、被弃用 API)清零。可验证:全量构建零新增告警 + 全量测试通过。
- **G4 MVP 达成**:CLI 全流程(扫描 → 过滤 → 定位 → 修改)可用且无占位。可验证:MVP 验收清单通过并附本次运行输出;通过后封存本指南。

### 1.3 非目标(Non-Goals)

- 不实现 CE_UI 图形界面、远程连接(各自另立新指南)。
- 不做工程完善类工作:CI/CD、覆盖率报告、静态分析集成、打包分发(另立新指南)。
- 不做高级功能:地址冻结、指针链扫描、SIMD 加速、性能专项优化(另立新指南)。
- 不支持 macOS;不采用内核驱动级方案;不引入账号/多租户体系。

### 1.4 范围(In / Out of Scope)

| 类型 | 内容 |
|---|---|
| In-Scope | CLI 交互重构(Phase 04);Phase 01–03 审计缺陷修复(Phase 05);规范符合性收敛(Phase 06);MVP 功能补齐与验收(Phase 07) |
| Out-of-Scope | CE_UI、远程连接、工程完善、高级功能(由后续新指南承接);macOS、内核驱动方案、账号/多租户体系 |

### 1.5 术语表

| 术语 | 含义 |
|---|---|
| tpe | TinyProcessEngine 的命名空间前缀(`tpe::core`、`tpe::platform` 等) |
| REPL | 读取—求值—打印循环;本项目中 `--open-process` 进入的驻留交互模式 |
| 首轮扫描 / 增量扫描 | 在全进程内存中搜索初始值 / 在上一轮结果上按条件过滤 |
| 审计 | 2026-09 对 Phase 01–03 的独立审查(7 项缺陷 + 规范偏差,见 §4.2 Phase 05) |
| MVP | 最小可用版本:CLI 全流程可用且无占位 |
| 封存 | 全部 Phase 完成验收后将指南转只读(第 9 节),禁止修改或删除 |
| spec-kit | GitHub spec-kit 工具链(specify CLI 0.16.1),提供 `/speckit.*` 规格驱动流程 |

---

## 2. Constitution(项目治理准则)

> **本节是最高开发准则,所有 Phase 的规格、计划、任务与实现都必须符合。**
> 说明:本指南首次落地时 `.specify/memory/constitution.md` 为空模板,故本节按“新项目”方式给出全文,并已经 `/speckit.constitution` 落地为 `.specify/memory/constitution.md`(初次落地 2026-09-18;2026-09-21 修订为 v1.0.1;2026-09-24 修订为 v1.0.2);两处内容须保持同步。
> 任何条款变更必须记录理由、评审人与日期(写入第 8 节),并同步落地文件。

### 2.1 测试准则(必填,NON-NEGOTIABLE)

- **Test-First 强制**:任何生产代码必须先有失败的测试;顺序固定:写测试 → 人批准 → 确认测试失败(红)→ 最小实现(绿)→ 清理(重构)。先写实现后补测试的代码一律**删除重来**。
- **缺陷修复先行复现**:修复缺陷必须先写复现该缺陷的失败测试,再修复;修复必须带回归测试。
- **红-绿-重构逐步提交**,信息格式固定:

  ```text
  test: add failing test for <X>      # 红
  feat: implement <X>                 # 绿
  refactor: clean up <X>              # 重构
  ```

- **全量验证**:每个 Phase 收尾前必须运行全量测试并留存输出证据(`N 通过 / 0 失败`);禁止"应该没问题"式措辞。
- 禁止删除、禁用、绕过测试;禁止空断言"刷绿"。

### 2.2 工作空间与版本控制准则(必填)

- **单检出**:每个 Phase 独占一个特性分支,在**既有工作目录**内切换(本仓库为既有 worktree `TinyProcessEngine.worktrees/feat-first-mvp`,开发指南、`docs/templates/`、`.specify/` 等仅存在于该目录);**不新建 worktree**(私有区文件不会随分支进入新 worktree,会破坏 spec-kit 流程);禁止多会话在同一检出目录并发写文件。
- **git 纪律**:主干受保护;不强制推送;每个提交是最小可理解状态;Phase 收尾由人三选一(本地合并 / 推送建 PR / 保留分支),合并后必须再跑一次全量测试。
- **提交面检查**:每次提交前执行 `git status --porcelain`,输出必须只含公开区路径(见第 3 节公开边界)。
- 依赖环境按项目隔离,锁文件入库。

### 2.3 代码质量标准

- 命名:命名空间小写(`tpe::core`、`tpe::platform` 等);类/结构体 PascalCase;接口 `I` 前缀;函数/方法 camelCase;成员变量 `m_` 前缀;常量 `k` 前缀或 ALL_CAPS;强类型 `enum class`。
- 头文件 `.hpp` 扩展名 + `#pragma once`;类型别名用 `using`;空指针用 `nullptr`。
- 库代码禁止 `exit()` / `std::abort()`;错误经 `Result`/错误码传播,不以异常作常规控制流;OS 句柄 RAII;避免裸 `new`/`delete`。
- 风格:4 空格缩进;行宽 100;K&R 大括号。
- 构建:全量构建零新增告警,含 UTF-8 代码页告警与弃用 API 告警。

### 2.4 评审与验收闸门(必填)

- **闸门① 开发指南评审**:本指南经需求方评审通过后方可开工。
- **闸门② Phase 三件套评审**:每个 Phase 的 spec / plan / tasks / checklist 经人工评审通过后方可进入实现(checklist 由评审人打勾,AI 不得自行勾选)。
- **闸门③ 完成前取证**:任何完成声明必须附带本次运行的验证输出。
- 评审人:当前为项目负责人(anitoce);后续如有团队,按团队约定扩展评审人名单。

### 2.5 其他条款

- **证据缺口处理**:Linux 运行时验证受开发环境限制(见第 7 节-1);涉及 Linux 行为的修复必须给出可复现证据(WSL、Docker 容器或独立 Linux 机器;Phase 05 已实证容器流程),确无法验证时记录"证据缺口"并经人批准例外。
- **公开边界**:公开提交只含代码、测试与 `docs/plan/` 公开文档;私有区文件(见第 3 节)绝不入库;公开 PR 可引用私有区路径,不得粘贴其内容。

### 2.6 修订流程

- 修订必须记录理由、评审人与日期(第 8 节变更记录),并同步更新 `.specify/memory/constitution.md`。

---

## 3. 全局技术约束

| 项 | 约束 | 理由 |
|---|---|---|
| 技术栈 | C++17(`CMAKE_CXX_STANDARD 17`);Windows(Win32 API)+ Linux(`/proc` + `process_vm_*`) | 既有技术选型;C++17 在主流编译器支持面广,当前不使用 C++20 特性 |
| 构建系统 | CMake(最低 `3.14`);`FetchContent_MakeAvailable` 管理第三方依赖 | 统一、无额外包管理器;3.14 为 MakeAvailable 最低要求(Phase 06 起) |
| 测试框架 | GoogleTest `v1.14.0`(FetchContent) | 已集成;GTest 生态成熟 |
| 依赖策略 | `FetchContent` 固定 tag;新增依赖必须同步修订本指南(走评审) | 版本可控、可复现 |
| 目录约定 | `docs/plan/`(开发指南,公开区)、`docs/phases/`(Phase 文档,私有)、`specs/<分支名>/`(制品,私有);单检出分支切换(在既有 worktree 内操作,**不新建 worktree**) | 统一治理结构 |
| 工具链 | VS Code Copilot Chat(不使用 Copilot CLI);spec-kit `0.16.1`(copilot 集成,PowerShell 脚本) | 项目既定 |
| 公开边界 | 私有区(一律 gitignore):`.github/skills/`、`.github/agents/`、`.github/prompts/`、`.github/copilot-instructions.md`、`.specify/`、`specs/`、`docs/superpowers/`、`.superpowers/`、`docs/phases/`、`docs/templates/`、防御性 `.worktrees/`。公开区:实现代码(`src/`、`include/`、`tests/`)、公开文档、`docs/plan/`、常规构建配置 | 隐私与治理要求 |
| 后续技术(不在本指南) | Qt 6(CE_UI)与 libhv、MessagePack(远程连接)仅在后继指南中引入;本指南不新增这些依赖 | 控制当前阶段范围与依赖面 |

---

## 4. 开发路线图(核心章节)

> 本节把第 1 节的总目标分割为 4 个 Phase。每个 Phase = 一轮完整流程(P0–P13,见 `docs/templates/phase-template.md` 第 3 节),粒度 2–5 个工作日当量;Phase 之间依赖闭环、无环;每个 Phase 产出可独立测试、可独立验收。

### 4.1 路线图总览

| Phase | 名称 | 目标(一句话) | 依赖 | 产出 | 状态 |
|---|---|---|---|---|---|
| Phase 04 | CLI 交互重构 | 按《CLI 交互规范》落地终端旗标命令、REPL、startup 分层与 Test.exe,废弃旧交互流程 | 无(前置 Phase 01–03 已完成) | `specs/004-cli-interaction-refactor` | `done`(2026-09-30) |
| Phase 05 | 遗留缺陷修复 | 清零 7 项审计缺陷 + 1 项 Phase 04 移交缺陷(`--all-processes` 枚举失败信号),每项先复现后修复并带回归测试 | Phase 04 | `specs/005-defect-remediation` | `done`(2026-09-30) |
| Phase 06 | 规范符合性收敛 | 收敛审计规范偏差:命名空间、头文件风格、库层异常、构建告警 | Phase 05 | `specs/006-spec-compliance` | `done`(2026-09-30) |
| Phase 07 | 功能补齐 → MVP | 补齐 `--unknown`/`--greater`/`--less` 首扫、`list` 实时重读,占位清零,完成 MVP 验收 | Phase 06 | `specs/007-mvp-completion` | `done`(2026-09-30) |

状态机: `planned → in-progress → in-review → merged → done`(由 Phase 收尾步骤更新)。

> **项目级收口(2026-09-30)**:全部 Phase 完成并验收通过后,各状态由 `merged` 转 `done`;项目级 DoD(§6.2)逐项通过;本指南按 §9 封存为只读。

**合并策略**(2026-09-21 决定,适用于 Phase 05–07):各 Phase 分支先 `--no-ff` 本地合并至集成分支 `feat/first-mvp`;全部 Phase 完成后由 `feat/first-mvp` 一次性合入 `main`,随后各 Phase 状态由 `merged` 转 `done`、执行项目级 DoD 校验并封存本指南。

**编号说明**:本指南延续项目既有 Phase 编号(01–03 已完成,制品见主检出 `specs/001–003`)。旧版指南(已删除)的 Phase 4 / Phase 5 对应本指南 Phase 04 / Phase 07;Phase 05(缺陷修复)与 Phase 06(规范收敛)为本次新增插入,位置按要求置于 CLI 重构之后、MVP 补齐之前。各 Phase 产出目录名以 `/speckit.specify` 实际生成为准。

### 4.2 Phase 条目明细

#### Phase 04:CLI 交互重构

- **目标**: 按《CLI 交互规范》(`docs/plan/cli-interaction-spec.md` §11.2–§11.10)完成 CLI 交互重构:终端旗标命令、REPL 命令全集与提示符、`startup` 分层、Test.exe 测试工具;废弃旧交互流程(类型选择菜单与 `Enter value` 追问)。
- **范围**: in-scope 终端侧命令(`--all-processes` / `--search-process` / `--open-process` / `--version` / `--help` 及无参数、参数错误约定)、REPL(`new-scan` / `next-scan` / `list` / `write` / `undo` / `help` / `exit`)、`src/startup/` 分层、测试工具 `Test.exe`(独立目标)、相关单元测试;out-of-scope 未实现扫描条件(`--unknown` / `--greater` / `--less` 首扫逻辑,按规范保持占位,Phase 07 补齐)、GUI 实现。
- **依赖**: 无(前置 Phase 01–03 已完成)。
- **交付物**: `specs/004-*` 制品;更新后的本路线图状态;CLI 测试与手工验证记录。
- **验收**: §11.2 各命令行为与退出码逐项符合;REPL 命令与提示符状态(§11.3–§11.8)按规范工作;`startup` / `startup_cli` / `startup_gui`(占位)分层落地;Test.exe 可用并与扫描流程联调通过;旧交互流程无残留;全量测试通过(附输出)。
- **状态**: `done`(2026-09-30 项目级收口:全流程完成并验收通过;此前 2026-09-21 本地合并至 `feat/first-mvp`,**无 PR**;验证证据:需求方人工验证完全通过 + 全量测试 **187 通过 / 0 失败**;Phase 文档与制品位于私有区 `docs/phases/first-mvp-phase-04.md`、`specs/004-cli-interaction-refactor/`;遗留的 Phase 05 依赖项见 Phase 文档 P10 移交记录)

#### Phase 05:遗留缺陷修复(Phase 01–03 审计)

- **目标**: 清零 Phase 01–03 审计的 7 项缺陷:①增量扫描快照缺失与比较语义错误(`Changed`/`Unchanged`/`Increased`/`Decreased`、整数按无符号比较);②`u8` 序列化回退到 4 字节模式;③Windows 打开进程未请求写权限;④Linux `/proc/<pid>/mem` 降级路径写失败(`O_RDONLY` 缓存问题);⑤大结果集磁盘后端(`ResultStorage`)未接入 `ScanSession`;⑥CTest 未注册、Linux 测试未纳入构建目标;⑦Linux 权限错误消息与 `EACCES` 处理不符合规格;⑧Phase 04 移交的新增缺陷:`--all-processes` 枚举失败信号不可达(现有平台 API 无法上报枚举失败;其余移交项与审计 ①③ 重合)。每项先复现后修复并带回归测试。
- **范围**: in-scope 上述 8 项缺陷(7 项审计 + 1 项 Phase 04 移交)的复现测试、修复与证据;测试基础设施修复(顶层测试发现、测试目标纳入 `test_linux_*.cpp` 等);out-of-scope 规范风格类偏差(Phase 06)、任何新功能。
- **依赖**: Phase 04。
- **交付物**: `specs/005-*` 制品;缺陷—测试—证据对照表。
- **验收**: 8 项逐项关闭并附复现/回归测试;顶层 `ctest` 可发现并运行全部用例(`N 通过 / 0 失败`);Windows 上 `modifyMemory` 写入流程可验证(附运行证据);Linux 相关项附复现证据,或按 §2.5 记录获批的证据缺口例外。
- **状态**: `done`(2026-09-30 项目级收口:全流程完成并验收通过;此前 2026-09-24 本地合并至 `feat/first-mvp`,合并提交 `4db7b5f`,**无 PR**;验证证据:需求方人工验证**完全通过** + 全量测试 **Windows 208 通过 / 0 失败、Linux(容器)215 通过 / 0 失败、双平台 ctest 100% passed**;8 项缺陷逐项红→绿,缺陷—测试—证据对照表见私有区 `specs/005-defect-remediation/evidence-matrix.md`,Phase 文档见 `docs/phases/first-mvp-phase-05.md`;过程中附带修复 3 项既有缺陷(CMake `LINUX` 变量平台选择恒假致 Linux 平台源未入构、Linux 进程名测试 exec 竞态、`ValueType.h`(现 `ValueType.hpp`)两处 -Werror 告警),详见 Phase 文档 P9 记录)

#### Phase 06:规范符合性收敛

- **目标**: 收敛审计列出的规范偏差:公共 API 归入 `tpe::` 命名空间;头文件 `.hpp` + `#pragma once`;库层异常使用清理(改为 `Result`/错误码);`FetchContent_Populate` 弃用替换;清零既有编译告警:UTF-8 代码页(C4819)、弃用 API(C4996)及 Phase 05 移交的转换告警(C4267)。
- **范围**: in-scope 命名空间与文件风格迁移、异常语义清理、CMake 现代化(弃用 API 替换)、构建告警清零(含 Phase 05 移交的 C4819 / C4996 / C4267,清单见 `specs/005-defect-remediation/evidence-matrix.md`);out-of-scope 任何行为变更(纯结构收敛,全量测试保持绿色通过)。
- **依赖**: Phase 05。
- **交付物**: `specs/006-*` 制品;构建零告警证据。
- **验收**: 全量构建零告警(覆盖 UTF-8 代码页、弃用 API 与 Phase 05 移交告警 C4819 / C4996 / C4267;或仅剩经批准的例外);全量测试通过;公共 API 均在 `tpe::` 命名空间;头文件全部 `.hpp` + `#pragma once`。
- **状态**: `done`(2026-09-30 项目级收口:全流程完成并验收通过;此前 2026-09-26 本地合并至 `feat/first-mvp`,合并提交 `3eb12a2`,**无 PR**;验证证据:需求方人工验证**完全通过** + 全量测试 **Windows 211 通过 / 0 失败、Linux(容器)218 通过 / 0 失败、双平台 ctest 100% passed**;五类编译告警 308→0、配置零弃用告警、外部行为零差异(对照 Phase 06 基线);收敛明细与 SC/FR 证据映射见私有区 `specs/006-spec-compliance/evidence-matrix.md`,Phase 文档见 `docs/phases/first-mvp-phase-06.md`)

#### Phase 07:功能补齐 → MVP

- **目标**: 补齐 MVP 全部功能并移除占位:`new-scan --unknown`(未知初始值首扫)、`new-scan --greater` / `--less`(首轮大小比较)、`list` 实时重读(显示当前内存值)、规范中其余占位清零;完成 MVP 全流程验收。
- **范围**: in-scope 上述功能与验收记录,《CLI 交互规范》§11.8 占位条款全部消除;out-of-scope GUI / 远程连接 / 工程完善 / 高级功能(超出本指南)。
- **依赖**: Phase 06。
- **交付物**: `specs/007-*` 制品;MVP 验收记录(扫描 → 过滤 → 定位 → 修改全流程运行输出)。
- **验收**: MVP 验收清单通过并附本次运行输出;CLI 全流程无占位(`not implemented` 类提示不再出现);全量测试通过;项目级 DoD(§6.2)达成;由人确认后封存本指南。
- **状态**: `done`(2026-09-30 项目级收口:全流程完成并验收通过;同日本地合并至 `feat/first-mvp`,合并提交 `eced045`,**无 PR**;验证证据:需求方人工验证**完全通过** + 全量测试 **Windows 247 通过 / 0 失败、Linux(容器)254 通过 / 0 失败、双平台 ctest 100% passed**(合并前/后各复跑一次);三功能面(`--unknown` 首扫 + `--greater`/`--less` 首扫 + `list` 实时重读)与两处契约变更(C-D4/C-D5)落地、占位清零、帮助文本与规范 §11 更新;收敛判定 `/speckit.converge` = converged(0 新增任务);证据见私有区 `specs/007-mvp-completion/evidence-matrix.md`、`evidence/mvp-acceptance.md`,Phase 文档见 `docs/phases/first-mvp-phase-07.md`)

### 4.3 并行与顺序

Phase 04 → 05 → 06 → 07 为**硬依赖链,全部串行**:修复基于重构后的 CLI 基线;规范收敛在缺陷修复后一次性执行命名空间与文件迁移,避免与缺陷修复互相冲突;MVP 收口最后进行。本指南不设并行 Phase(单检出纪律,且四个 Phase 连续改动同一代码区域)。Phase 内部任务可在 `tasks.md` 中标 `[P]` 并行执行。

---

## 5. Phase 通用规则

1. 每个 Phase 开工前,按 `docs/templates/phase-template.md` 实例化生成 `docs/phases/first-mvp-phase-<NN>.md`(私有区,不入公开仓库);其第 1 节必须**逐字**复制本指南 §4.2 中对应条目的目标与范围。
2. 每个 Phase 完整执行 phase 模板 3.1 节的流程级任务(P0–P13):规格 → 消歧 → 方案 → 质量清单 → 任务 → 一致性分析 → 评审 → 隔离与基线 → TDD 实现 → 收敛 → 取证 → 收尾 → 回写。
3. Phase 之间通过 §4.1 依赖衔接;前置 Phase 未 merged,后置 Phase 不开工。
4. 路线图调整(增删 Phase、改依赖、改范围)必须先行修订本指南并评审,禁止只改 Phase 文档不动本指南。

---

## 6. 评审与验收总纲

### 6.1 评审闸门

| 闸门 | 内容 | 评审人 | 通过条件 |
|---|---|---|---|
| 开发指南评审 | 本指南全部章节 | 需求方(anitoce) | 无占位符残留;目标可测;路线图闭环 |
| Phase 规格评审 | `specs/<分支>/spec.md` | 需求方 | 聚焦 WHAT/WHY;无 `[NEEDS CLARIFICATION]` 残留;需求可测试;与路线图条目一致 |
| Phase 方案/任务评审 | `plan.md` + `tasks.md` + checklist | 需求方 | Phase 文档"评审标准"节逐项通过 |
| 代码/PR 评审 | 提交历史 + 改动面 + 测试证据 | 需求方 | Phase 文档"验收标准"节逐项通过 |

### 6.2 项目级完成定义(DoD)

所有 Phase 达成各自 Phase 文档"验收标准"后,还必须满足:

- [x] 路线图全部 Phase 状态为 `done`,无悬空依赖(2026-09-30:Phase 04–07 全部收口)
- [x] 每个 Phase 的验收标准与 §4.2 对应条目一致,且有证据(测试输出、运行记录)(各 Phase 文档 §5 逐项确认 + 双平台 `ctest`/`N 通过 / 0 失败` 输出 + 私有区证据矩阵)
- [x] Constitution 条款(§2.1–§2.6)全部符合,无未声明的例外(证据缺口例外已记录并经批准)(各 Phase plan Constitution Check 6 PASS 且 Phase -1 门禁通过;无例外记录)
- [x] 风险与开放问题(第 7 节)全部闭环或转经评审接受的遗留清单(#2/#3/#4/#7 已关闭;#1(已缓解)、#5、#6 为经评审接受的遗留项)
- [x] 已合并分支已删除(`004/005/006/007` 均已删除;保留集成分支 `feat/first-mvp`)
- [x] 公开仓库提交不含私有区文件(见第 3 节公开边界)(各 Phase 提交均为公开区;`git status --porcelain` 干净)
- [x] **本指南已按第 9 节封存为只读**(2026-09-30 封存提交 `docs: seal development guide first-mvp`)

### 6.3 回归策略(存量迭代必填)

- **开工基线**:每次 Phase 开工前运行全量测试建立基线,非绿色先报告、不得带病开发。
- **测试口径**(自 2026-09-24,Phase 05 起):以顶层 `ctest` 为准(Windows 多配置需 `-C Debug`);Windows 本机与 Docker 容器(ubuntu:22.04)双平台执行;直跑 `tpe_tests.exe` 为 2026-09-24 前的历史口径。
- **合并回归**:每个 Phase 合并后必须再跑一次全量测试;凡涉及旧行为的变更(命名空间迁移、文件重命名、异常语义变更等)必须在提交说明或 PR 中声明。
- **证据留存**:每次全量测试保留"命令 + `N 通过 / 0 失败`"输出。

---

## 7. 风险与开放问题

| # | 风险/问题 | 影响 | 缓解措施 | 状态 |
|---|---|---|---|---|
| 1 | Linux 运行时验证环境缺失(当前开发机为 Windows) | Linux 相关缺陷无法在本机 Windows 复现验证 | 已建立 **Docker 容器(ubuntu:22.04)双平台验证流程**(Phase 05 实证:容器 215 通过 / 0 失败、ctest 100%);后续 Phase 的 Linux 行为必须附容器证据(§2.5) | `open`(已缓解,2026-09-24) |
| 2 | 本 worktree `.specify/` 缺 `feature.json`(及扩展) | Phase 04 P0 无法定位特性目录 | 已修复(2026-09-20,Phase 04 P0;Phase 05 P0 已按规则重指向 `specs/005-*`);各 Phase P0 须重指向当期特性目录(Phase 06 → `specs/006-*`) | `closed`(2026-09-20) |
| 3 | CMake 4.4 弃用 `FetchContent_Populate`,现构建有弃用告警 | 未来 CMake 升级导致构建中断 | **已修复(Phase 06)**:CMake 最低版本上调至 `3.14`,改用 `FetchContent_MakeAvailable`;配置期零弃用告警(CMP0169 消除) | `closed`(2026-09-26) |
| 4 | 测试基础设施待修复:CTest 未注册、Linux 测试未纳入目标 | 顶层 `ctest` 不可用,基线只能直接运行测试可执行文件 | **已修复(Phase 05 缺陷⑥)**:顶层 `enable_testing` + Linux 用例纳入;双平台 ctest 100% passed(2026-09-24);口径切换见 §6.3 | `closed`(2026-09-24) |
| 5 | 长线方向(CE_UI、远程连接、工程完善、高级功能)不在本指南范围 | 后续工作暂无总纲可依 | MVP 验收封存后,按 §1.3/§1.4 划界新建开发指南 | `open` |
| 6 | 既有 `specs/001–003` 为旧流程产物,与现行模板存在差异 | 追溯口径不完全一致 | 保持只读引用;新 Phase 一律走现行模板流程;发现不一致时以现行模板为准 | `open` |
| 7 | 既有编译告警未清零:C4819 / C4996 / C4267 / C4244 / C4101 | 与 §2.3"零新增告警"口径存在差距,构建噪声 | **已清零(Phase 06)**:五类告警 308 → 0(MSVC 统一 `/utf-8`,无任何抑制开关);证据见 `specs/006-spec-compliance/evidence-matrix.md` | `closed`(2026-09-26) |

---

## 8. 变更记录

> 本表可编辑时间范围:`draft` 至 `done` 之前;一旦封存(第 9 节),本表同样冻结。

| 版本 | 日期 | 变更内容 | 修订人 | 评审人 |
|---|---|---|---|---|
| 1.0.0 | 2026-09-18 | 初始创建:取代旧《DEVELOPMENT_GUIDE.md》v0.2.0;第 11 章 CLI 交互规范迁移为 `docs/plan/cli-interaction-spec.md`;审计缺陷与规范偏差修复插入为 Phase 05/06(CLI 重构后优先);删除 `docs/temp/`(含 `CE_UI.png`) | GitHub Copilot | anitoce |
| 1.0.0 | 2026-09-21 | 路线图回写:P04 状态 `planned` → `merged`(本地合并至 `feat/first-mvp`,无 PR;验证证据:人工验证通过 + 全量测试 187 通过 / 0 失败);同批公开提交 `docs: update dev guide roadmap (phase 04)` | GitHub Copilot | anitoce |
| 1.0.1 | 2026-09-21 | 一致性修订(PATCH):spec-kit 版本勘误 `0.11.9` → `0.16.1`(文档信息/术语表/§3);单检出表述澄清(既有 worktree 内操作、不新建 worktree;§2.2/§3);§4.1 新增"合并策略"段;Phase 05 条目补充 Phase 04 移交缺陷 ⑧(§4.1/§4.2);§7 风险 #2 关闭、#4 措辞同步;文档信息版本/最近更新同步 | GitHub Copilot | anitoce |
| 1.0.2 | 2026-09-24 | 一致性修订(PATCH):§1.1 完成状态与 §2.5 容器验证口径同步(constitution v1.0.2);§4.2 Phase 06 条目纳入 Phase 05 移交告警(C4819/C4996/C4267);§6.3 测试口径切换为顶层 `ctest`(Windows + Docker 双平台);§7 风险 #1 缓解更新、#2 注记、#4 关闭、新增 #7;文档信息版本/最近更新同步 | GitHub Copilot | anitoce |
| 1.0.3 | 2026-09-26 | 一致性修订(PATCH):§1.1 完成状态同步(Phase 06 规范收敛);§3 构建系统更新为 CMake ≥ 3.14 + `FetchContent_MakeAvailable`;§7 风险 #3、#7 关闭(五类告警 308 → 0、配置零弃用告警);§4.2 Phase 05 历史记录补注现名(`ValueType.hpp`);文档信息版本/最近更新同步 | GitHub Copilot | anitoce |
| 1.0.4 | 2026-09-30 | 项目级收口与封存:路线图 Phase 04–07 状态 `merged` → `done`;§6.2 项目级 DoD 逐项通过并勾选;按 §9 封存为只读(封存横幅 + 文件只读属性 + 提交 `docs: seal development guide first-mvp`);Phase 07 路线图回写(提交 `7f02cc4`,合并提交 `eced045`) | GitHub Copilot | anitoce |

---

## 9. 封存规则(只读,强制)

> 由 AI 在**全部 Phase 完成且项目级验收通过后**执行,并由人确认。

**触发条件**:第 4 节路线图全部 Phase 状态为 `done`,且第 6.2 节项目级 DoD 逐项通过。

**封存三步**:

1. 文档信息表状态改为 `done(已封存,只读)`,并在本文件**正文最顶部**写入封存横幅:

   ```markdown
   > 🔒 **本指南已封存(只读)**:全部 Phase 已完成并验收通过。禁止修改本文件任何内容;后续迭代请新建开发指南。
   ```

2. 设置文件只读属性:Windows `attrib +R "docs/plan/first-mvp-guide.md"`;Unix/macOS `chmod 444 "docs/plan/first-mvp-guide.md"`。
3. 提交 `docs: seal development guide first-mvp`(公开区提交)。

**封存后的不可变规则**:

- 禁止修改、禁止删除本文档的任何内容(目标、范围、Constitution、路线图、验收结论、风险、变更记录一律冻结);**无例外**。
- 文档本身如有错误,不修改本文档,在**新指南**中以"勘误"条目标注。

**后续迭代规则**:

- 需要**修改或删除已有功能**时,必须按 `docs/templates/development-guide-template.md` **新建一份开发指南**(`docs/plan/<新名称>-guide.md`),在**新指南**第 1 节中显式声明与被取代的已封存指南的关系(取代/修订范围);旧指南保持只读不动。
- 即使只是追加功能,只要涉及既有已封存指南中的行为变更,同样走新建指南的方式,不做旧文档的追加修改。
- 本指南范围外的长线方向(CE_UI、远程连接、工程完善、高级功能)天然由新指南承接。

---

*本指南是 First MVP 阶段所有开发工作的总纲;封存前若实现与本文档冲突,先修订本文档(走评审)再调整实现;封存后一律通过新建指南完成变更。*
