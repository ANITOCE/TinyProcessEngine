#include "Platform.h"
#include "TinyProcessEngine/Result.h"

#ifdef _WIN32
#include "Platform/Windows/WindowsOS.h"
#include "Platform/Windows/WindowsProcess.h"
#include <Windows.h>
#elif defined(__linux__)
#include "Platform/Linux/LinuxOS.h"
#include "Platform/Linux/LinuxProcess.h"
#include <cerrno>
#include <cstring>
#endif

PlatformError PlatformError::from_last_error(std::string op, unsigned long pid_val) {
    PlatformError err;
    err.operation   = std::move(op);
    err.pid         = pid_val;
#ifdef _WIN32
    err.native_code = static_cast<int>(GetLastError());
    err.message     = "Operation '" + err.operation
                    + "' failed on PID " + std::to_string(err.pid)
                    + " (native code: " + std::to_string(err.native_code) + ")";
#else
    err.native_code = errno;
    // 缺陷⑦(FR-020/C-P4):Linux 消息恒定包含 strerror 原文(native code 与文本一致)
    err.message     = "Operation '" + err.operation
                    + "' failed on PID " + std::to_string(err.pid)
                    + " (native code: " + std::to_string(err.native_code)
                    + ", " + std::strerror(err.native_code) + ")";
#endif
    return err;
}

PlatformError PlatformError::from_last_error(std::string op, unsigned long pid_val,
                                             std::string_view advice) {
    PlatformError err = from_last_error(std::move(op), pid_val);
    if (!advice.empty()) {
        // 缺陷⑦(C-P4):strerror 原文在前、可操作建议在后
        err.message += "; ";
        err.message.append(advice);
    }
    return err;
}

std::shared_ptr<PlatformProcess> createPlatformProcess(Pid_t pid, std::string p_name)
{
#ifdef _WIN32
    return std::make_shared<WindowsProcess>(pid, p_name);
#elif defined(__linux__)
    return std::make_shared<LinuxProcess>(pid, p_name);
#endif
}

std::unique_ptr<PlatformOS> createPlatformOS()
{
#ifdef _WIN32
    return std::make_unique<WindowsOS>();
#elif defined(__linux__)
    return std::make_unique<LinuxOS>();
#endif
}

Pid_t str_to_pid(std::string str)
{
#ifdef _WIN32
    // 显式窄化转换：stoull 结果按 Pid_t 收敛（FR-005/C-B3）
    return static_cast<Pid_t>(std::stoull(str));
#elif defined(__linux__)
    return std::stoi(str);
#endif
    
}
