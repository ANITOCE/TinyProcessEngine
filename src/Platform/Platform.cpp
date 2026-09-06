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
#endif

PlatformError PlatformError::from_last_error(std::string op, unsigned long pid_val) {
    PlatformError err;
    err.operation   = std::move(op);
    err.pid         = pid_val;
#ifdef _WIN32
    err.native_code = static_cast<int>(GetLastError());
#else
    err.native_code = errno;
#endif
    err.message     = "Operation '" + err.operation
                    + "' failed on PID " + std::to_string(err.pid)
                    + " (native code: " + std::to_string(err.native_code) + ")";
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
    return std::stoull(str);
#elif defined(__linux__)
    return std::stoi(str);
#endif
    
}
