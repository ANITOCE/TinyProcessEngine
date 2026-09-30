// Test.exe — TinyProcessEngine 手工测试工具(规范 §11.10;Phase 04 US3 / T028)。
//
// 用途:提供一个数值随按键变化的真实目标进程,配合
//   TinyProcessEngine.exe --open-process <Test.exe 的 PID>
// 完成"扫描 → 过滤 → 定位 → 修改"的端到端联调。
//
// 行为:
//   - 启动打印 "Value: 100"(i32 视图);每次数值变化后打印新值行(如 "Value: 102")
//   - 空格 / 回车 → 数值 +2;Esc → 退出
//   - stdin 为 TTY 时用平台非缓冲输入(Windows `_getch()` / Linux `termios`);
//     否则(重定向 / 管道,自动化场景)退化为按行读取:
//     空行 = 回车(数值 +2),`q` = 退出,EOF = 退出;其它行忽略。
//
// 独立性:不链接 TinyProcessEngine / Platform 库,零第三方依赖;
// 仅当 TPE_BUILD_TESTS=ON 时由 tests/CMakeLists.txt 引入本目录构建。

#include <cstdint>
#include <cstdio>
#include <iostream>
#include <string>

#ifdef _WIN32
#include <conio.h>
#include <io.h>
#else
#include <termios.h>
#include <unistd.h>
#endif

namespace {

constexpr std::int32_t kInitialValue = 100;
constexpr std::int32_t kStep = 2;

/// 打印当前值(自动化捕获依赖此格式;重定向时标准输出为全缓冲,必须刷新)。
void printValue(std::int32_t value)
{
    std::printf("Value: %d\n", static_cast<int>(value));
    std::fflush(stdout);
}

/// stdin 是否为终端(重定向/管道时自动退化为行模式)。
bool stdinIsTerminal()
{
#ifdef _WIN32
    return _isatty(_fileno(stdin)) != 0;
#else
    return ::isatty(STDIN_FILENO) != 0;
#endif
}

enum class Key { None, Increase, Quit };

#ifdef _WIN32

/// 非缓冲单键读取;功能键以 0/224 前缀双字节到达,读入并丢弃第二字节。
Key readKey()
{
    const int ch = _getch();
    if (ch == 0 || ch == 224) {
        _getch();
        return Key::None;
    }
    if (ch == 27) { // Esc
        return Key::Quit;
    }
    if (ch == ' ' || ch == '\r' || ch == '\n') {
        return Key::Increase;
    }
    return Key::None;
}

#else

/// Linux:进入原始模式并在退出时恢复(RAII)。
class RawModeGuard
{
public:
    RawModeGuard()
    {
        if (::tcgetattr(STDIN_FILENO, &m_old) != 0) {
            return;
        }
        termios raw = m_old;
        raw.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO);
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        m_active = ::tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0;
    }

    ~RawModeGuard()
    {
        if (m_active) {
            ::tcsetattr(STDIN_FILENO, TCSANOW, &m_old);
        }
    }

    RawModeGuard(const RawModeGuard&) = delete;
    RawModeGuard& operator=(const RawModeGuard&) = delete;

private:
    termios m_old{};
    bool m_active = false;
};

/// 非缓冲单键读取(termios 原始模式由 RawModeGuard 设置)。
Key readKey()
{
    unsigned char ch = 0;
    if (::read(STDIN_FILENO, &ch, 1) <= 0) {
        return Key::Quit; // EOF / 读取失败:退出
    }
    if (ch == 27) { // Esc
        return Key::Quit;
    }
    if (ch == ' ' || ch == '\r' || ch == '\n') {
        return Key::Increase;
    }
    return Key::None;
}

#endif // _WIN32

/// TTY 交互模式:非缓冲按键驱动。
int runInteractive(volatile std::int32_t& value)
{
#ifndef _WIN32
    const RawModeGuard guard;
#endif
    for (;;) {
        switch (readKey()) {
        case Key::Quit:
            return 0;
        case Key::Increase:
            value += kStep;
            printValue(value);
            break;
        case Key::None:
            break;
        }
    }
}

/// 非 TTY(管道/重定向)回退:按行读取;空行 = 回车(数值 +2),`q` = 退出,EOF = 退出。
int runLineMode(volatile std::int32_t& value)
{
    std::string line;
    while (std::getline(std::cin, line)) {
        const std::size_t begin = line.find_first_not_of(" \t\r\n\v\f");
        if (begin == std::string::npos) {
            value += kStep; // 空行 = 回车
            printValue(value);
            continue;
        }
        const std::size_t end = line.find_last_not_of(" \t\r\n\v\f");
        const std::string token = line.substr(begin, end - begin + 1);
        if (token == "q") {
            return 0;
        }
        // 其它输入在自动化模式下忽略(等价于无效按键)
    }
    return 0; // EOF
}

} // namespace

int main()
{
    // 数值驻留内存:volatile 保证不被优化进寄存器,便于被扫描/写入(i32 视图)。
    volatile std::int32_t value = kInitialValue;
    printValue(value);

    if (!stdinIsTerminal()) {
        return runLineMode(value);
    }
    return runInteractive(value);
}
