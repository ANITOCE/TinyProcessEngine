# TinyProcessEngine

[English](README.en.md)

TinyProcessEngine 是一个用于查看和修改进程内存的 C++17 命令行工具，支持 Windows 和 Linux。你可以在本机运行，也可以在受控 IPv4 网络中连接另一台机器上的 TinyProcessEngine Server。

- 按 PID 查找并打开进程。
- 执行首轮扫描，按值或变化继续筛选，并用 `undo` 恢复上一轮匹配。
- 用 `list` 读取匹配地址的当前值，用 `write` 修改指定地址。

## 从源码构建

需要 Git、CMake 3.20 或更新版本，以及支持 C++17 的工具链。首次配置时，CMake 会从 GitHub 获取 libhv、spdlog 和 GoogleTest。以下命令在仓库根目录运行。

### Windows

在 PowerShell 中使用 Visual Studio 的 C++ 工具链和多配置生成器。如果 CMake 默认选择了其他生成器，使用 `cmake --help` 列出的 Visual Studio 生成器，通过 `-G` 指定。

```powershell
cmake -S . -B build/windows -DTPE_BUILD_TESTS=ON
cmake --build build/windows --config Debug --parallel
ctest --test-dir build/windows -C Debug --output-on-failure
.\build\Debug\TinyProcessEngine.exe --help
```

### Linux

以下命令使用 GCC 和 Make。

```bash
cmake -S . -B build/linux -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Debug -DTPE_BUILD_TESTS=ON
cmake --build build/linux --parallel
ctest --test-dir build/linux --output-on-failure
./build/TinyProcessEngine --help
```

`TPE_BUILD_TESTS=ON` 会额外构建下方示例使用的 `Test` 进程。

## 快速上手

1. 在一个终端运行 Windows 的 `.\build\windows\tests\Debug\Test.exe` 或 Linux 的 `./build/linux/tests/Test`。保留该终端。程序会显示目标变量的 `Target address` 和初始值 `100`。
2. 在另一终端以 `--all-processes` 运行 TinyProcessEngine，找到测试进程的 PID。再以 `--open-process <PID>` 运行，进入扫描提示符。用实际 PID 替换占位符。
3. 输入 `new-scan 100`。回到测试进程的终端按一次回车，将值增加到 `102`。
4. 回到 TinyProcessEngine，依次输入下列命令。用测试进程打印的十六进制地址替换 `<address>`。最后一条 `list --all` 应在该地址显示 `200`。

   ```text
   next-scan --changed
   list --all
   write <address> 200
   list --all
   exit
   ```

目标进程需要允许当前工具读写其内存。Linux 的进程跟踪限制也可能影响同一用户启动的进程。

## 远程使用

在目标主机运行 TinyProcessEngine 并传入 `--server -port 8008`。在客户端运行同一个程序并传入 `--connect 127.0.0.1 -port 8008` 来测试本机连接。跨主机连接时，将 `127.0.0.1` 替换为 Server 的 IPv4 地址。

在客户端的 `TPE>` 提示符中，用 `--all-processes` 查找 **Server 上**的 PID，再用 `--open-process <PID>` 进入扫描层。扫描命令与本地示例相同。扫描层的 `exit` 返回 `TPE>`，在 `TPE>` 输入 `exit` 结束连接。

Server 默认监听所有 IPv4 接口，一次只接纳一个客户端。连接无应用层鉴权和加密，请通过网络访问控制将监听端口限制在受控环境中。

## 文档与反馈

- [CLI 命令及输出](docs/plan/cli-interaction-spec.md)
- [远程连接指南](docs/plan/remote-connection-guide.md)
- 报告问题时请附上操作系统、运行命令和实际输出。[GitHub Issues](https://github.com/ANITOCE/TinyProcessEngine/issues)

## 许可证

TinyProcessEngine 使用 [MIT 许可证](LICENSE)。libhv 和 GoogleTest 使用 BSD 3-Clause 许可证，spdlog 及其内置 fmt 使用 MIT 许可证。第三方依赖适用各自的许可证。
