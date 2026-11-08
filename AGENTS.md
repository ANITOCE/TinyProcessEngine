# 协作开发入口

将本文件以 `AGENTS.md` 放在目标项目根目录，将随附的 `templates/` 目录复制到目标项目的 `docs/` 下，形成 `docs/templates/`。支持 `AGENTS.md` 的宿主会读取本文件作为项目指令。新开会话时，用启动提示指定角色和当前工作项。也可以按需读取英文说明 `AGENTS.en.md`。

启动提示应说明角色、流程、当前工作项及阶段。默认使用中文。指定英文时，读取对应的 `.en.md` 文件。工作项和阶段尚未确定时，先确认其含义，再进入相应流程。

1. 读取 [共同流程规范](docs/templates/spec-code.md) 或其 [英文版](docs/templates/spec-code.en.md)。
2. 按角色读取下列操作规范。始终使用与第一步相同的语言版本。
3. 按共同规范选择流程并核对当前阶段所需的制品。根据已有文档和验证记录恢复状态。

| 角色 ID | 职责 | 操作规范 |
| --- | --- | --- |
| `codex-brainstorm` | 讨论项目目标、架构和跨 Phase 决策 | [中文](docs/templates/codex-instructions-template.md) · [English](docs/templates/codex-instructions-template.en.md) |
| `codex-review` | 独立评审 SDD 的 P7 / P11 或其他流程的对应节点 | [中文](docs/templates/codex-instructions-template.md) · [English](docs/templates/codex-instructions-template.en.md) |
| `copilot-lead` | 维护项目进度和会话交接 | [中文](docs/templates/copilot-instructions-template.md) · [English](docs/templates/copilot-instructions-template.en.md) |
| `copilot-exec` | 执行当前 Phase 或工作项 | [中文](docs/templates/copilot-instructions-template.md) · [English](docs/templates/copilot-instructions-template.en.md) |

流程 ID 为 `sdd`（功能和行为变更）、`bug`（诊断和修复既有问题）、`assess`（判断是否值得投入）。阶段以所选流程的共同定义为准。

> 读取根目录的 `AGENTS.md`。角色 `codex-review`，流程 `sdd`，工作项 `payments`，Phase 02，阶段 P11，语言中文。以 Phase 文档和评审交接记录为输入。

> Read the root `AGENTS.md`. Role `copilot-exec`, workflow `bug`, item `login-timeout`, stage `bug.assess`, language English.
