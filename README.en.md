# TinyProcessEngine

[中文](README.md)

TinyProcessEngine is a C++17 command-line tool for inspecting and modifying process memory on Windows and Linux. Run it locally or connect to a TinyProcessEngine Server on another machine over a controlled IPv4 network.

- Find and open a process by PID.
- Run an initial scan, filter matches by value or by change, and restore the previous set of matches with `undo`.
- Read live values at matching addresses with `list` and change the value at a specific address with `write`.

## Build from source

You need Git, CMake 3.20 or later, and a toolchain with C++17 support. On the first configure, CMake fetches libhv, spdlog, and GoogleTest from GitHub. Run these commands from the repository root.

### Windows

Use PowerShell with the Visual Studio C++ toolchain and a Visual Studio multi-config generator. If CMake picks a different generator by default, use `-G` with the Visual Studio generator listed by `cmake --help`.

```powershell
cmake -S . -B build/windows -DTPE_BUILD_TESTS=ON
cmake --build build/windows --config Debug --parallel
ctest --test-dir build/windows -C Debug --output-on-failure
.\build\Debug\TinyProcessEngine.exe --help
```

### Linux

These commands use GCC and Make.

```bash
cmake -S . -B build/linux -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Debug -DTPE_BUILD_TESTS=ON
cmake --build build/linux --parallel
ctest --test-dir build/linux --output-on-failure
./build/TinyProcessEngine --help
```

`TPE_BUILD_TESTS=ON` also builds the `Test` process used below.

## Quick start

1. Run `.\build\windows\tests\Debug\Test.exe` on Windows or `./build/linux/tests/Test` on Linux in its own terminal. Leave it running. It prints a `Target address` and an initial value of `100`.
2. In another terminal, run TinyProcessEngine with `--all-processes` to find the test process PID. Run it again with `--open-process <PID>` to enter the scan prompt. Replace the placeholder with the actual PID.
3. Enter `new-scan 100`. Switch to the test process terminal and press Enter once to raise the value to `102`.
4. Return to TinyProcessEngine and enter the following commands. Replace `<address>` with the hexadecimal address printed by the test process. The final `list --all` should show `200` at that address.

   ```text
   next-scan --changed
   list --all
   write <address> 200
   list --all
   exit
   ```

The tool needs permission to read and write the target process's memory. Linux tracing restrictions can affect processes started by the same user.

## Remote use

On the target host, run TinyProcessEngine with `--server -port 8008`. On the client, run the same executable with `--connect 127.0.0.1 -port 8008` to try a loopback connection. For a connection between machines, replace `127.0.0.1` with the Server's IPv4 address.

At the client's `TPE>` prompt, use `--all-processes` to find a PID **on the Server**, then use `--open-process <PID>` to enter the scan prompt. Use the scan commands from the local example. Enter `exit` at the scan prompt to return to `TPE>`, then enter `exit` at `TPE>` to close the connection.

The Server listens on all IPv4 interfaces by default and admits one client at a time. Connections have no application-layer authentication or encryption. Use network access controls to keep the listening port within a controlled environment.

## Documentation and feedback

- [CLI commands and output](docs/plan/cli-interaction-spec.en.md)
- [Remote connection guide](docs/plan/remote-connection-guide.en.md)
- Include the operating system, command, and actual output when reporting a problem. [GitHub Issues](https://github.com/ANITOCE/TinyProcessEngine/issues)

## License

TinyProcessEngine is licensed under the [MIT License](LICENSE). libhv and GoogleTest use the BSD 3-Clause License. spdlog and its bundled fmt use the MIT License. Third-party dependencies are subject to their own license terms.
