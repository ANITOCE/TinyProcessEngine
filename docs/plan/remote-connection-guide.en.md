# TinyProcessEngine Remote Connection Development Guide

[中文](remote-connection-guide.md)

## Document information

- Guide ID `remote-connection`, version `0.1.1`, status `accepted` (owner approval, 2026-11-08)
- Owner anitoce, created 2026-11-08 and last updated 2026-11-14
- Extension of the existing TinyProcessEngine project. See the [public repository](https://github.com/ANITOCE/TinyProcessEngine).
- Workflow 4.0, Spec Kit 1.2.5 (Copilot, PowerShell), Superpowers 7.0.1

## 1. Overview

### 1.1 Background and motivation

The local TinyProcessEngine CLI lists and opens processes, then reads and writes memory in an interactive scanning session. Remote connection lets a user on machine B inspect and modify a target process on machine A with the same commands. Both machines run the same program and select Server or Client mode at startup.

### 1.2 Verifiable goals

- **G1 Connection.** A listens on the default or specified port. B supplies A's IP address and port. Once the Server admits the session, both sides print the success message and B enters `TPE>`.
- **G2 Session.** The Server serves one Client at a time. A second Client receives a clear busy reason. When the active connection ends, the next connection starts at a clean `TPE>`.
- **G3 Remote operations.** B can list, look up, and open processes on A and run existing scanning commands to read and write memory. The end-to-end scenario scans a known initial value in a test process on A, reads it, writes a new value, and reads it back.
- **G4 Observability and regression.** The Server console records connections and operation outcomes. The Client reports concrete failure reasons. Full builds and tests pass on Windows and Linux, with regression coverage for the existing local CLI contract.

### 1.3 Non-goals

Concurrent clients, accounts and application authentication, transport encryption, automatic reconnection and session restoration, host discovery, a GUI, and public internet deployment.

### 1.4 Scope

The first release covers loopback and controlled IPv4 networks on Windows and Linux. The Server lists, opens, reads, and writes processes within its existing platform permissions. The Client operates on the currently connected Server. Public and untrusted networks are outside scope.

### 1.5 Terminology and prior guide

- A **Server** is a TPE instance that holds the target process and scanning state. A **Client** is an instance that receives commands and displays results.
- A **connection session** runs from Server admission until the connection ends. A **scan session** runs from successfully opening a target until a scan-level `exit` or the end of the connection.
- The sealed [First MVP guide](first-mvp-guide.md) is the prior baseline. The [CLI interaction specification](cli-interaction-spec.en.md) defines local commands and scanning behavior. Its Section 11.11 mirrors the remote CLI contract in Section 3 of this guide.

### 1.6 Approaches, trade-offs, and assumptions

- **Forward command lines, selected.** The Client sends the command line entered by the user. The Server reuses the local command parsing and execution logic, then returns the result and next prompt. Local and remote argument validation and error behavior share a single implementation.
- **Parse on the Client and send operation-specific requests.** This makes a structured interface easier for future front ends to consume. It requires defining requests and responses for each scan operation and validating the data on the Server in the first release.
- The owner chose command-line forwarding and prioritized consistency with the existing CLI. Acceptance covers results, prompts, and failures for the same command in local and remote modes.

## 2. Constitution

Follow the approved First MVP governance principles and the common workflow. P0 checks the Constitution in the execution environment against the approved version. If the current checkout contains a template, restore the approved principles and verify the result. The owner approves any changes to those principles and records the decision.

- Write tests first, observe the expected RED after human approval, then complete GREEN and any applicable refactoring. Keep full verification evidence at the end of each Phase.
- Use a dedicated feature branch per Phase in the existing checkout, with one writer at a time. Protect the main branch and have the owner choose how to finish each Phase.
- Follow the existing C++ style, error propagation, and resource lifecycle conventions. Full Windows and Linux builds must introduce zero new warnings. Pin third-party dependencies through CMake and verify platform compatibility and licensing in P3.
- Run the Server without application authentication only on a controlled network with network-level access restrictions. Any Client that can reach the listening port and is admitted can request memory operations within the Server's permissions.

## 3. Technical constraints and CLI contract

### 3.1 Technology and responsibilities

Use the project's existing C++17, CMake, and GoogleTest setup. TCP uses [libhv](https://github.com/ithewei/libhv#tcp), and Server console logging uses [spdlog](https://github.com/gabime/spdlog#features). P3 selects dependency versions, frame encoding, and size limits, and verifies the project's minimum CMake version.

The Server reuses local command parsing and execution. Command execution returns a result and next interaction state for display by the local terminal or remote Client. The Server owns the target process, scan state, and next prompt. The Client sends one command at a time and waits for its response before sending the next. TCP messages have explicit boundaries and can carry multiline results. The Server sends an explicit application response to admit or reject a session and can return a busy response while a scan runs. P3 defines the startup handshake deadline, frame layout, and version field.

### 3.2 Startup and connection

```text
TinyProcessEngine.exe --server [-port <PORT>]
TinyProcessEngine.exe --connect <IP> -port <PORT>
```

- The two startup modes are mutually exclusive. Use the corresponding executable name on Linux. The default Server port is `8008` when `-port` is omitted. The Client requires the Server's IPv4 address and port. The port is a decimal integer from `1` through `65535`.
- The Server displays `Waiting for connection ...` once it is listening and waits for a Client. It displays `Connection successful.` when it admits the Client. The same console then shows operation logs for that Client. It binds all IPv4 interfaces by default to allow direct connections between machines. Deployment controls which machines can reach the port.
- The Client displays `Connection successful.` and `TPE>` after receiving admission. A busy Server rejects the new session. The second Client displays `Connection failed.` with `Server is busy.` as the reason, and the active session continues.
- Invalid startup arguments produce usage and a concrete reason with exit code `2`. Bind, handshake, and connection failures give a concrete reason with exit code `1`. The Client also displays `Connection failed.` for connection failures before session establishment.

### 3.3 Interaction and session end

- `TPE>` accepts `--all-processes`, `--search-process <PID>`, and `--open-process <PID>`. The first two display Server results and return to `TPE>`. A successful open displays `<process-name>-equal-i32>`. A failed open reports the reason and returns to `TPE>`.
- At `TPE>`, `help` or `--help` displays the commands for that level. The scan level uses the existing `help`, `new-scan`, `next-scan`, `list`, `write`, and `undo` commands and scan prompt rules. An ordinary command failure reports the reason and accepts the next command at the same level.
- A scan-level `exit` releases the target process and scan state, then returns to `TPE>` on the same connection. An `exit` at `TPE>` or input EOF at either level ends the Client normally with exit code `0`. The Server keeps listening.
- On detecting an unexpected disconnect, the Client reports the reason and exits with code `1`. It reports the failure immediately while waiting for a command response and by the next input while waiting at a terminal prompt. If a write response was pending, it reports that the write outcome is unknown. The user initiates another connection. Idle disconnect detection relies on transport notification.
- On detecting a disconnect, the Server discards commands that have not started. A command already in progress may finish, after which the Server releases the session and can admit another Client. The Server continues to return busy until cleanup is complete.
- The Server logs connections, rejections, disconnections, and operation outcomes to its console. Operation logs contain the time, peer, command name, target PID when available, and a summary of the outcome. Log fields are limited to this metadata. Command responses carry scan data and written values.

### 3.4 Two-machine example

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

A displays `Connection successful.` when it admits B. A's console then shows the operation logs for B.

## 4. Roadmap

### 4.1 Phase overview

| Phase | Name | Goals | Dependency | Public deliverables | Status |
| --- | --- | --- | --- | --- | --- |
| 01 | Remote connection and process queries | G1, G2, G4 | First MVP | Startup modes, single-client admission, process queries, logging | `done` |
| 02 | Remote scanning and memory access | G3, G4 | Phase 01 | Process opening, scan REPL, two-level exit, end-to-end verification | `done` |

### 4.2 Phase entries

#### Phase 01 · Remote connection and process queries

- Goals and scope — G1, G2, G4. Deliver startup arguments, application admission, `TPE>`, process listing and lookup, connection-level exit, and logging. The `TPE>` help lists the available commands. Other commands use the unknown-command behavior. Process opening, scanning, and memory operations follow in Phase 02.
- Dependency — The delivered First MVP baseline.
- Public deliverables — Two remote startup modes, remote process queries, and single-client admission.
- Acceptance — The default and a custom port accept a Client. The Server displays the waiting message first, and both sides display success when the Server admits the first Client. The first Client can query Server processes. The second Client receives the busy reason and exits with code `1`. A new Client can connect after exit or disconnect. Missing arguments, invalid ports, an occupied port, and connection failure produce the specified outputs and codes. Verify connection and query logs and run local regression tests on both platforms.
- Status — `done`. Merged locally into `feat/add-remote-connection` on 2026-11-12 at `d7af096`. After the merge, all 416 Windows tests and 423 Linux tests passed.

#### Phase 02 · Remote scanning and memory access

- Goals and scope — G3, G4. Deliver remote process opening, scan commands, memory reads and writes, scan-level exit, and session cleanup.
- Dependency — Phase 01 merged and verified after merge.
- Public deliverables — Full remote scan CLI, two-level exit, and remote memory operation verification.
- Acceptance — B opens a controlled test process on A, scans for a known value with existing commands, reads the live value with `list`, writes a new value with `write`, and reads it back. Cover subsequent scans, undo, open failure, opening a different process after scan-level `exit`, and rejecting a second Client during a scan. Cover command handling on disconnect and reconnecting after cleanup. Verify that old results do not carry into new sessions. Exercise the Server and Client on both Windows and Linux. Complete at least one real write and readback over a cross-platform connection.
- Status — `done`. Merged locally into `feat/add-remote-connection` on 2026-11-14 at `52d3123`. After the merge, all 419 Windows tests and 426 Linux tests passed. A Windows Client wrote to a controlled Linux process and read back the new value.

### 4.3 Sequence

Phase 01 establishes the connection and query contract, with independent verification before merge. Phase 02 adds scanning and writes on top of that baseline. Each Phase follows P0–P13.

## 5. Shared Phase rules

Copy the corresponding Section 4.2 entry verbatim in the working language using the project's Phase template. The `copilot-lead` manages dependencies and handoffs. A new `copilot-exec` session executes each Phase. The owner approves changes to scope, goals, dependencies, or acceptance before artifacts are synchronized. P13 updates both guides.

## 6. Review and acceptance

### 6.1 Review gates

P0 follows owner approval of the guide. P7 and P11 each use a fresh independent `codex-review` session, with human approval of the corresponding artifact revision or candidate commit. The owner chooses the integration path at P12, and a full verification run follows merge. P13 reflects the actual merge and verification result.

### 6.2 Project acceptance

- [ ] Both Phases have reached `done`, with acceptance evidence for G1–G4.
- [ ] Governance and build gates passed. Open issues are closed or approved for follow-up work.
- [ ] Branch cleanup was authorized and completed, or the decision to retain the branch was recorded.
- [ ] Both language editions agree on content, status, and public references. The proposed changes passed the public artifact boundary check.

### 6.3 Regression strategy

Run the full suite with the existing GoogleTest and CTest setup. Add coverage for CLI arguments, single-client admission, disconnect cleanup, and opening another process. End-to-end acceptance uses a real TCP connection and a controlled target process. Compare existing local startup commands, scan syntax, output, prompts, and exit codes.

## 7. Risks and open issues

| ID | Issue | Impact | Handling | Status |
| --- | --- | --- | --- | --- |
| R1 | Listening port reachability | A reachable Client can request reads and writes within Server permissions | Deploy on a controlled network and restrict reachability through network access controls | Boundary defined |
| R2 | Disconnect during a write | If the response is lost, the Client cannot determine whether the write completed | Report an unknown outcome and have the user reconnect and read back the value | Behavior defined |
| R3 | Dependencies and transport boundaries | Build compatibility, message framing, and output length affect implementation | P3 verifies dependency versions, message limits, and the handshake deadline | Pending P3 |

## 8. Change history

| Version | Date | Summary | Author | Review |
| --- | --- | --- | --- | --- |
| 0.1.0 | 2026-11-08 | Record the remote CLI, single-client admission, command forwarding, and Phase roadmap | Codex | Approved (anitoce, 2026-11-08) |
| 0.1.1 | 2026-11-08 | Specify the Server success message and logging order, and synchronize the CLI specification | Codex | Approved (anitoce, 2026-11-08) |
| 0.1.1 | 2026-11-12 | Record the Phase 01 local merge and verification on both platforms | Codex | Phase 01 closed |
| 0.1.1 | 2026-11-14 | Record the Phase 02 local merge, full verification on both platforms, and cross-platform write and readback | Codex | Phase 02 closed |

## 9. Seal both language editions

After project acceptance, the owner confirms sealing both editions. Synchronize their `done` status, sealing date, and read-only banner. Set both local files to read-only and verify the sealing commit. Later revisions use a new guide that records its relationship to this version.
