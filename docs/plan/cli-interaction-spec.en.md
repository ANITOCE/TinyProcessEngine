# TinyProcessEngine CLI Interaction Specification

[中文](cli-interaction-spec.md)

> **Origin.** Migrated on 2026-09-18 from Chapter 11 of the former Development Guide v0.2.0. This is the authoritative public copy. The original chapter numbering is retained for historical references.
> **Status.** Sections 11.1–11.10 record the [First MVP](first-mvp-guide.md) CLI contract. Section 11.11 records the CLI design draft in the [remote connection guide](remote-connection-guide.en.md). Behavior changes go through a revision and review of the corresponding development guide before being reflected here.
> The CLI starts from the terminal, takes complete commands one line at a time, and uses the scan REPL for target interaction.

---

## 11.1 Design Principles

1. **Terminal first.** Launch the program from a terminal and select an operation with command-line flags. Local terminal query commands print their results and exit.
2. **One complete operation per line.** Each command performs one complete operation. Missing arguments produce a usage error without prompting for more input.
3. **Scan REPL.** After opening a target process, run scan commands in the scan REPL. For a remote connection, select the target at `TPE>` as described in Section 11.11.
4. **Visible state.** The prompt shows the current scan type, value type, and value to scan for.
5. **Consistent types.** `new-scan` sets the scan session's value type using an explicit flag or the default `i32`. On `next-scan`, a type flag that matches the current type has no effect. A different type flag produces a usage error.

## 11.2 Local Terminal Startup

| Command | Behavior | Exit code |
| --- | --- | --- |
| `TinyProcessEngine.exe --all-processes` | List all processes with their PIDs and names, then exit. | 0 |
| `TinyProcessEngine.exe --search-process <PID>` | Look up the process name by PID, print it, then exit. | 0 |
| `TinyProcessEngine.exe --open-process <PID>` | Open the process and enter the interactive REPL. | 0 (on `exit`) |
| `TinyProcessEngine.exe --version` | Print the version and exit. | 0 |
| `TinyProcessEngine.exe --help` | Print help for terminal commands and exit. | 0 |
| `TinyProcessEngine.exe` (no arguments) | Same as `--help`. | 0 |
| Invalid arguments | Display help. | 2 |
| Runtime failure, such as process not found or enumeration failed | Print an error and exit before entering the REPL. | 1 |

> Local startup displays help for missing or invalid arguments. `--open-process` enters the scan REPL. A runtime failure at startup prints an error and exits with code 1. A command failure inside the scan REPL preserves its current state. See Section 11.11 for remote startup.

## 11.3 Interactive REPL

To enter the local REPL, run `TinyProcessEngine.exe --open-process <PID>`. The prompt has the following form.

```text
<process-name>-<scan-type>-<value-type>-[<value>]>
```

| Segment | Meaning | Default |
| --- | --- | --- |
| `<process-name>` | Target process name | — |
| `<scan-type>` | Current scan type | `equal` |
| `<value-type>` | Current value type | `i32` |
| `[<value>]` | Value to scan for, shown after a scan with a value | Hidden |

```text
notepad.exe-equal-i32> new-scan 100
notepad.exe-equal-i32-100> next-scan --greater
notepad.exe-greater-i32-100> list 1
```

The prompt displays the value supplied to the latest scan when that scan takes a value. `next-scan --changed` and `next-scan --unchanged` hide the value. `new-scan` discards previous scan progress and resets the value display before applying its own result.

## 11.4 REPL Commands

| Command | Behavior |
| --- | --- |
| `help` | Display help for REPL commands. The REPL displays it by default for missing or invalid arguments. |
| `exit` | In local mode, return to the terminal with exit code 0. In remote mode, return to `TPE>` as described in Section 11.11. |
| `new-scan <value>` | Start a new scan with `equal` and the current value type, and discard previous progress. |
| `new-scan [<scan-type>] [<value-type>] [<value>]` | Start a new scan, optionally specifying the scan type and value type. |
| `next-scan <value>` | Continue from the previous results with `equal` and the current session's value type. |
| `next-scan [<scan-type>] [<value>]` | Continue scanning, optionally specifying the scan type. The value type set by `new-scan` stays fixed. |
| `list --all` | List matches, up to 10000. |
| `list [<N>]` | Display page N of the matches, with 20 entries per page. The default is page 1. |
| `write <address> <new-value>` | Change the value at the given address. |
| `undo` | Undo the previous scan and restore the earlier set of matches. |

## 11.5 Scan Command Arguments

The `<scan-type>` and `<value-type>` flags use the `--` prefix. The last argument without that prefix is the value to scan for.

Scan types for `new-scan`.

| Flag | Meaning | Value | Status |
| --- | --- | --- | --- |
| `--equal` | Exact value | Required | Implemented |
| `--unknown` | Unknown initial value | Omit | Implemented |
| `--greater` | Greater than the supplied value | Required | Implemented |
| `--less` | Less than the supplied value | Required | Implemented |

Scan types for `next-scan`.

| Flag | Meaning | Value | Status |
| --- | --- | --- | --- |
| `--equal` | Exact value | Required | Implemented |
| `--greater` | Value increased | Omit | Implemented |
| `--less` | Value decreased | Omit | Implemented |
| `--changed` | Value changed | Omit | Implemented |
| `--unchanged` | Value stayed the same | Omit | Implemented |

The value type flags are `--u8`, `--i16`, `--i32`, `--i64`, `--float`, `--double`, and `--string`. The default is `--i32`. The value type set by `new-scan` stays fixed for the session.

`--string` supports equality scans (`--equal`). The CLI rejects `--string` with `new-scan --unknown`, `--greater`, or `--less`, and with `next-scan --greater`, `--less`, `--changed`, or `--unchanged`. These combinations produce a usage error and preserve scan state.

```text
new-scan 100                    # equal + i32 + 100
new-scan --equal --i32 100       # Explicit equivalent
next-scan --changed             # No value
next-scan --less                # Value decreased. Uses the current session type.
new-scan --unknown              # Unknown initial value. Records all candidate addresses.
```

If a required value is missing, as in `new-scan` or `next-scan --equal`, the CLI prints a usage error with a Usage hint and skips execution. Flags that take no value (`--changed`, `--unchanged`, `--unknown`, and `--greater` or `--less` on `next-scan`) run with the arguments already supplied. The CLI never prompts for more arguments.

## 11.6 `list` Output Format

```text
<process>-equal-i32-100> list --all
Total: 15234 matches
  0x00000000001C0A10 | 100
  0x00000000018120C8 | 200
  ...
```

- Display addresses as 16 hexadecimal digits with a `0x` prefix.
- Format values according to the current `<value-type>`. Display `u8`, `i16`, `i32`, and `i64` as decimal integers, `float` and `double` as decimal floating-point numbers, and `string` as stored text.
- `list --all` prints `Total: N matches` first and displays up to 10000 matches. If the count exceeds the limit, display `... and N more`.
- `list <N>` displays 20 entries per page and defaults to page 1.
- Read the current value from the target process when displaying each entry. Show `??` if the read fails. For `string`, use the recorded snapshot width as the read length.

## 11.7 `write` Command Format

```text
write <address> <new-value>
```

- Parse the address as hexadecimal, with an optional `0x` prefix (`0x1C0A10` or `1C0A10`).
- Parse the value according to the current `<value-type>`. Use decimal notation for integers and floating-point values. For `string`, take the rest of the line after the address, including spaces.
- A parse failure prints a usage error and skips the write.

## 11.8 Feature Placeholders (Retired)

Phase 07 retired the original placeholder text and mechanism on 2026-09-26. This section retains its number for existing references.

## 11.9 Application Startup Layers

The following tree describes the First MVP startup structure. `main` starts the program, and the `startup` files create and assemble the application components.

```text
src/
├── main.cpp                    # Only calls tpe::app::run(argc, argv)
└── startup/
    ├── startup.hpp             # namespace tpe::app
    ├── startup.cpp             # int run(argc, argv) dispatches to CLI or GUI
    ├── startup_cli.hpp/.cpp    # int runCli() sets up the CLI and runs the REPL
    └── startup_gui.hpp/.cpp    # int runGui() is the entry point for future GUI setup
```

- `main` is the entry point.
- `startup` parses global flags and selects the CLI or GUI startup path.
- `startup_cli` assembles the CLI, including `ProcessEngine` creation and the REPL loop.
- `startup_gui` is the entry point for future GUI setup.

## 11.10 `Test.exe` Test Tool

`tests/tools/Test.cpp` is a standalone manual test program. It builds as target `Test`, produces `Test.exe`, and runs independently of the TPE library and GoogleTest suite.

- Build the tool with `TPE_BUILD_TESTS=ON`. The default build setting is `off`.
- The tool displays `100` at startup and stores the value as `i32`.
- Press Space or Enter to add 2 to the value. Press Esc to exit.
- Use `TinyProcessEngine.exe --open-process <PID of Test.exe>` to verify scanning and writing against a live process.
- Input uses platform-specific unbuffered input, `_getch()` on Windows or `termios` on Linux.

## 11.11 Remote Connection (Design Draft)

This section mirrors Section 3 of the remote connection guide. Both remote startup modes use the same TPE executable. The Server parses and executes commands entered on the Client. The Client displays the result and next prompt. [Remote connection guide](remote-connection-guide.en.md)

### 11.11.1 Startup and Connection Outcomes

```text
TinyProcessEngine.exe --server [-port <PORT>]
TinyProcessEngine.exe --connect <IP> -port <PORT>
```

- `--server` and `--connect` are mutually exclusive. Use the corresponding executable name on Linux. The Server defaults to port `8008` when `-port` is omitted and listens on all IPv4 interfaces by default. The Client requires the Server's IPv4 address and port. The port is a decimal integer from `1` through `65535`.
- After it starts listening, the Server displays `Waiting for connection ...` and waits for a Client. On admission, the Server displays `Connection successful.` and then shows operation logs for that Client on the same console. It logs connections, rejections, disconnections, and command outcomes. Operation log fields are limited to the time, peer, command name, target PID when available, and a result summary.
- The Client displays `Connection successful.` and enters `TPE>` after the Server admits it. The Server serves one Client at a time. While a session is active, another Client displays `Connection failed.` and the reason `Server is busy.`, then exits with code `1`. The active session continues.
- Invalid startup arguments display the specific reason and usage and exit with code `2`. Bind, handshake, or connection failures display the reason and exit with code `1`. Before session establishment, a connection failure on the Client also displays `Connection failed.`.
- The first release runs on controlled IPv4 networks, with deployment controlling which machines can reach the port. The Server runs without application authentication and uses its own system permissions to handle memory operations from an admitted Client.

### 11.11.2 Connection and Scan Levels

- At `TPE>`, `--all-processes` and `--search-process <PID>` query processes on the Server and return to `TPE>` after displaying the result. A successful `--open-process <PID>` enters the scan REPL at `<process-name>-equal-i32>`. A failed open reports the reason and returns to `TPE>`.
- At `TPE>`, enter `help` or `--help` to list the commands available at that level. Phase 01 provides remote process listing and lookup. Phase 02 adds process opening and the scan REPL.
- The scan REPL uses the commands, syntax, and prompts from Sections 11.3–11.7. Scanning, listing results, and writing operate on the target process on the Server. An ordinary command failure reports the reason and stays at the current level.
- A scan-level `exit` closes the target and clears the scan state, then returns to `TPE>` on the same connection. An `exit` at `TPE>` or input EOF at either level exits the Client with code `0`. The Server releases the session and keeps listening.
- On detecting an unexpected disconnect, the Client reports the reason and exits with code `1`. If it is waiting for terminal input, it reports a detected disconnect by the next input. If a `write` response was pending, it reports that the write outcome is unknown. On detecting a disconnect, the Server discards commands that have yet to start, allows any command in progress to finish, and cleans up before admitting a new Client.

### 11.11.3 Two-Machine Example

A has IPv4 address `192.168.1.20` and runs a test process with PID `4321`.

```text
# A
TinyProcessEngine.exe --server
Waiting for connection ...
Connection successful.

# B
TinyProcessEngine.exe --connect 192.168.1.20 -port 8008
Connection successful.
TPE> --search-process 4321
Test.exe
TPE> --open-process 4321
Test.exe-equal-i32> new-scan 100
Test.exe-equal-i32-100> exit
TPE> exit
```

A prints the success message when it admits B. Its console then shows B's operation logs.

---

## Errata

> The Phase 04 clarifications below took effect after manual approval at P7 on 2026-09-20, with anitoce as reviewer. Subsequent entries record the exit code clarification, header extension migration, and Phase 07 behavior changes.

| Date | Location | Original text | Correction | Basis | Review status |
| --- | --- | --- | --- | --- | --- |
| 2026-09-20 | §11.3 example | `next-scan --increased` (`increased` in the prompt) | `next-scan --greater` (`greater` in the prompt) | Phase 04 P2. Use the flag names in the §11.5 table. | Confirmed (P7, 2026-09-20) |
| 2026-09-20 | §11.3 rules for displaying `[<value>]` | Included `next-scan --unknown`. | Retain only `--changed` and `--unchanged`. | Phase 04 P2. `Unknown` applies to the initial scan. | Confirmed (P7, 2026-09-20) |
| 2026-09-20 | §11.5 `next-scan` table | Marked the values for `--greater` and `--less` as required. | Mark them as **Omit**. | These flags compare snapshots, consistent with §11.3, §11.5, and `ScanCondition::Increased` and `Decreased` in `src/TinyProcessEngine/MemoryScanner.cpp`. | Confirmed (focused P7 review, 2026-09-20) |
| 2026-09-20 | §11.5 missing-value rules and examples | Used `next-scan --less --i64 42` and omitted `--greater` and `--less` from the missing-value rules. | Change the example to `next-scan --less --i64` and add both flags to the rules. | Same basis as the preceding entry. | Confirmed (P7, 2026-09-20) |
| 2026-09-24 | §11.2 table and rules | Listed success (0) and invalid arguments (2). | Add exit code 1 for a startup runtime failure. A REPL command failure preserves state. | Phase 04 P2 and the Phase 05 fix for defect ⑧ (`473521b`, `6fc4111`). Verified with `ctest` on both platforms. | Effective (2026-09-24) |
| 2026-09-25 | §11.9 directory tree | Three header filenames ended in `.h`. | Update their extensions to `.hpp`. | Phase 06 migrated 24 headers to `.hpp` with `#pragma once` (FR-009 and FR-010). | Confirmed (P7, 2026-09-25) |
| 2026-09-26 | §11.1, §11.5, §11.6, §11.8 | Placeholder principle, three placeholder entries, example comments, snapshot value source, and the placeholder section. | Apply R-1, R-3, R-4, R-6, and R-7. | Phase 07 P2 and implementation. Tests passed on both platforms. | Confirmed (P7, 2026-09-28) |
| 2026-09-26 | §11.4, §11.5 | `next-scan` could set a value type, including the `--less --i64` example. | Apply R-2 and R-5. The type set by `new-scan` stays fixed. | Phase 07 P2 type consistency decision. | Confirmed (P7, 2026-09-28) |
| 2026-09-26 | §11.4, §11.5 | `string` could be used with all scan conditions. | Apply R-5. `--string` supports only `--equal`. | Phase 07 P2 and implementation. Unsupported combinations produce an explicit error. | Confirmed (P7, 2026-09-28) |

---

*The local CLI contract merged with Phase 04 on 2026-09-21. The remote CLI contract is reviewed with the remote connection guide.*
