# TinyProcessEngine CLI 交互规范

> **来源**: 迁移自旧版《DEVELOPMENT_GUIDE.md》v0.2.0 第 11 章(2026-09-18 迁移);旧文档随 `docs/temp/` 一并删除,本文件为其在公开区的权威副本。章节编号沿用原第 11 章(§11.x),便于与历史记录对照。
> **地位**: 本文件是 **Phase 04(CLI 交互重构)的规格输入**(Phase 04 已于 2026-09-21 合并);继续作为 **CLI 行为契约**,供 Phase 07(占位补齐)与后续迭代引用。与开发指南(`docs/plan/first-mvp-guide.md`)冲突时,以开发指南为准并先行修订指南(走评审)。
> 核心原则:**终端优先、单行自包含、子模式为主体**。

---

## 11.1 设计原则

1. **终端优先**: 程序从终端启动,通过命令行旗标选择操作;查询类命令输出即退出
2. **单行自包含**: 每条命令 = 一次完整操作,绝不交互式追问;缺参数时报用法错误
3. **子模式为主体**: `--open-process <PID>` 进入 REPL 后即为全部交互,无嵌套模式
4. **状态可见**: 提示符实时反映当前扫描类型、数值类型与待扫描值
5. **占位可扩展**: 未实现的功能保持语法可识别,执行时打印占位提示

## 11.2 启动方式(终端侧)

| 命令 | 行为 | 退出码 |
|------|------|--------|
| `TinyProcessEngine.exe --all-processes` | 列出所有进程(PID + 名称),打印后退出 | 0 |
| `TinyProcessEngine.exe --search-process <PID>` | 按 PID 查找进程名,打印后退出 | 0 |
| `TinyProcessEngine.exe --open-process <PID>` | 打开进程并进入 REPL 交互模式 | 0(`exit` 退出) |
| `TinyProcessEngine.exe --version` | 打印版本号,退出 | 0 |
| `TinyProcessEngine.exe --help` | 打印终端侧命令帮助,退出 | 0 |
| `TinyProcessEngine.exe`(无参数) | 等同 `--help` | 0 |
| 参数错误 | 显示帮助 | 2 |
| 运行期失败(进程不存在、枚举失败等) | 打印错误信息(不进入 REPL) | 1 |

> 规则:无参数或参数错误时默认显示 `help`;**只有 `--open-process` 进入驻留的 REPL**;运行期失败打印错误并以退出码 1 结束,REPL 内命令失败不退出、状态保持不变。

## 11.3 REPL 交互模式

进入方式:`TinyProcessEngine.exe --open-process <PID>`

**提示符格式**:

```
<process-name>-<scan-type>-<value-type>-[<value>]>
```

| 段 | 含义 | 默认值 |
|----|------|--------|
| `<process-name>` | 目标进程名 | — |
| `<scan-type>` | 当前扫描类型 | `equal` |
| `<value-type>` | 当前数值类型 | `i32` |
| `[<value>]` | 当前待扫描值(仅扫描带值后显示) | 不显示 |

**示例**:

```
notepad.exe-equal-i32> new-scan 100
notepad.exe-equal-i32-100> next-scan --greater
notepad.exe-greater-i32-100> list 1
```

**`[<value>]` 显示规则**:
- 显示"最近一次带值扫描传入的值"
- `next-scan --changed` / `--unchanged`(不传值)执行后 → 隐藏
- `new-scan` 销毁之前的扫描进度,回到默认不显示状态
- 最近一次扫描带值 → 显示该值;否则隐藏

## 11.4 REPL 命令表

| 命令 | 功能 |
|------|------|
| `help` | 显示 REPL 命令帮助;无参数或参数错误时默认调用 |
| `exit` | 退出 REPL,终止进程返回终端(退出码 0) |
| `new-scan <value>` | 新建扫描(默认 equal + 当前 value-type),销毁之前进度 |
| `new-scan [<scan-type>] [<value-type>] [<value>]` | 新建扫描,可指定扫描类型和数值类型 |
| `next-scan <value>` | 在上一轮结果上继续扫描(默认 equal + 当前 value-type) |
| `next-scan [<scan-type>] [<value-type>] [<value>]` | 继续扫描,可指定扫描类型和数值类型 |
| `list --all` | 列出全部匹配结果(上限 10000) |
| `list [<N>]` | 分页显示第 N 页匹配(每页 20 条,默认第 1 页) |
| `write <address> <new-value>` | 修改指定地址的数值 |
| `undo` | 撤销上一轮扫描,恢复之前的匹配集 |

## 11.5 命令参数语法

统一 `--` 前缀风格:`<scan-type>` 与 `<value-type>` 均为旗标,最后一个非 `--` 参数为待扫描值。

**`new-scan` 扫描类型**:

| 旗标 | 含义 | 值 | 状态 |
|------|------|----|------|
| `--equal` | 精确值 | 必传 | ✅ 已实现 |
| `--unknown` | 未知初始值 | 不传 | ⏳ 占位(Phase 07 补齐) |
| `--greater` | 大于 | 必传 | ⏳ 占位(Phase 07 补齐) |
| `--less` | 小于 | 必传 | ⏳ 占位(Phase 07 补齐) |

**`next-scan` 扫描类型**:

| 旗标 | 含义 | 值 | 状态 |
|------|------|----|------|
| `--equal` | 精确值 | 必传 | ✅ 已实现 |
| `--greater` | 值变大 | 不传 | ✅ 已实现 |
| `--less` | 值变小 | 不传 | ✅ 已实现 |
| `--changed` | 值变化 | 不传 | ✅ 已实现 |
| `--unchanged` | 值未变化 | 不传 | ✅ 已实现 |

**数值类型旗标**:`--u8` / `--i16` / `--i32` / `--i64` / `--float` / `--double` / `--string`(默认 `--i32`)

**示例**:

```
new-scan 100                    # equal + i32 + 100
new-scan --equal --i32 100      # 显式等价
next-scan --changed             # 无值
next-scan --less --i64          # 无值(值变小)
new-scan --unknown              # 占位(打印 not implemented)
```

**缺值规则**:
- 需要值却未提供(如 `new-scan`、`next-scan --equal`)→ 打印用法错误(含 Usage 提示),不执行
- 不传值的旗标(`--changed` / `--unchanged` / `--unknown`,以及 `next-scan` 的 `--greater` / `--less`)缺值不报错,正常执行
- **永不交互式追问**

## 11.6 list 显示格式

```
<process>-equal-i32-100> list --all
Total: 15234 matches
  0x00000000001C0A10 | 100
  0x00000000018120C8 | 200
  ...
```

- 地址:16 位十六进制,`0x` 前缀
- 值:按当前 `<value-type>` 格式化(快照值):
  - `u8` / `i16` / `i32` / `i64` → 十进制整数
  - `float` / `double` → 十进制浮点
  - `string` → 原样字符串
- `list --all`:先打印 `Total: N matches`;上限 10000 条,超出显示 `... and N more`
- `list <N>`:每页 20 条,默认第 1 页
- 值来源:当前为扫描快照;实时重读留待 Phase 07

## 11.7 write 命令格式

```
write <address> <new-value>
```

- 地址:十六进制,`0x` 前缀可选(`0x1C0A10` 或 `1C0A10`)
- 值:按当前 `<value-type>` 解析:
  - 整数 → 十进制
  - 浮点 → 十进制浮点
  - `string` → 地址后剩余整行(允许空格)
- 解析失败 → 打印用法错误,不执行写入

## 11.8 未实现功能占位

功能未实现时打印:

```
This feature is not implemented yet.
```

占位功能(`new-scan --unknown` / `--greater` / `--less`)保持语法可识别、提示符可切换对应 scan-type,底层逻辑在 Phase 07 补齐。

## 11.9 应用启动分层

`main` 只负责启动程序,应用组件创建与装配由 `startup` 系列文件负责:

```
src/
├── main.cpp                    # 仅调用 tpe::app::run(argc, argv)
└── startup/
    ├── startup.hpp             # namespace tpe::app
    ├── startup.cpp             # int run(argc, argv) —— 模式分发(CLI/GUI)
    ├── startup_cli.hpp/.cpp    # int runCli() —— CLI 装配 + REPL 主循环
    └── startup_gui.hpp/.cpp    # int runGui() —— GUI 装配(占位,由后续 GUI 指南落地)
```

职责边界:
- `main`:零逻辑入口
- `startup`:解析全局旗标,决定 CLI / GUI 启动路径
- `startup_cli`:CLI 世界全部装配(ProcessEngine 创建、REPL 循环)
- `startup_gui`:未来 GUI 世界装配点

## 11.10 Test.exe 测试工具

独立于 TinyProcessEngine 的手动测试程序(非 GTest 单元测试):

- 位置:`tests/tools/Test.cpp`,目标名 `Test`,产出 `Test.exe`
- **完全独立**:不链接 TinyProcessEngine 库,不影响 tpe 生成
- 构建:仅当 `TPE_BUILD_TESTS=ON` 时构建;初次构建完成后默认 `off`
- 行为:
  - 启动显示 `100`(按 i32 存储)
  - 按空格 / 回车 → 数值 +2
  - 按 Esc → 退出
- 用途:配合 `TinyProcessEngine.exe --open-process <Test.exe 的 PID>` 进行真实扫描/修改流程验证
- 输入读取需平台非缓冲输入(Windows `_getch()` / Linux `termios`)

---

## 勘误记录

> 本文件为公开区规格输入文档。以下勘误由 Phase 04 消歧(P2,2026-09-20)定稿驱动,并已经 **P7 人工评审闸门确认生效**(2026-09-20,评审人 anitoce);2026-09-24 追加一条退出码口径补充(源自 Phase 04 P2 定稿与 Phase 05 缺陷⑧ 实现,行为已实现并经人工验证);2026-09-25 追加一条头文件扩展名勘误(源自 Phase 06 头文件风格迁移,待 P7 复核)。

| 日期 | 位置 | 原文 | 勘误后 | 依据 | 评审状态 |
|---|---|---|---|---|---|
| 2026-09-20 | §11.3 示例 | `next-scan --increased`(提示符 `increased`) | `next-scan --greater`(提示符 `greater`) | Phase 04 P2 定稿:旗标名以 §11.5 旗标表为准,`--increased` 视为笔误 | 已确认(P7,2026-09-20) |
| 2026-09-20 | §11.3 `[<value>]` 显示规则 | 含 `next-scan --unknown` | 仅保留 `--changed` / `--unchanged` | Phase 04 P2 定稿:`next-scan` 不支持 `--unknown`(`Unknown` 仅首轮) | 已确认(P7,2026-09-20) |
| 2026-09-20 | §11.5 `next-scan` 表 | `--greater` / `--less` 值列 = 必传 | 改为**不传** | **新增勘误(未经 P2 提问)**:值变大/值变小为快照比较条件,不需要外部值——与 §11.3 示例、§11.5 缺值规则举例(如 `next-scan --equal`)及引擎现有条件(`ScanCondition::Increased` / `Decreased`,见 `src/TinyProcessEngine/MemoryScanner.cpp`)一致;原“必传”疑为自 `new-scan` 表复制 | 已确认(P7 重点评审通过,2026-09-20) |
| 2026-09-20 | §11.5 缺值规则 / 示例 | 示例 `next-scan --less --i64 42`;缺值规则未列 `--greater` / `--less` | 示例改 `next-scan --less --i64`;缺值规则补入 `next-scan` 的 `--greater` / `--less` | 同上 | 已确认(P7,2026-09-20) |
| 2026-09-24 | §11.2 表 / 规则 | 仅列成功(0)与参数错误(2),未定义运行期失败退出码 | 增加"运行期失败 → 打印错误,退出码 1"行与规则句(REPL 内失败不退出、状态保持) | Phase 04 P2 定稿(2026-09-20);Phase 05 缺陷⑧ 接通 `--all-processes` 枚举失败信号(`473521b` / `6fc4111`);双平台 ctest 验证 | 已生效(实现+验证,2026-09-24) |
| 2026-09-25 | §11.9 目录树 | 3 处头文件名后缀为 `.h` | 后缀改为 `.hpp`(随 Phase 06 迁移同步) | Phase 06 头文件风格迁移:全部项目头文件统一 `.h → .hpp` + `#pragma once`(共 24 个;FR-009/FR-010) | 待复核(P7) |

---

*本文件已随 Phase 04 落地(2026-09-21 合并);如需求变更,先修订开发指南(`docs/plan/first-mvp-guide.md`,走评审)再修订本文件。*
