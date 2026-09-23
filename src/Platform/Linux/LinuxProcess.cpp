#ifdef __linux__

// ============================================================
// TinyProcessEngine — Linux 平台进程实现
// 日志约定: 异常路径使用 std::cerr 输出 [WARN] 或 [ERROR] 前缀
// 格式: [LEVEL] ClassName::methodName: message
// ============================================================

#include "LinuxProcess.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <cstring>
#include <cerrno>
#include <fcntl.h>

namespace {

/// 权限类失败的可操作建议(缺陷⑦;FR-020/C-P4)
constexpr const char* kPermissionAdvice =
    "Try running as root or set /proc/sys/kernel/yama/ptrace_scope to 0";

/// 组装内存访问错误:EPERM/EACCES 在 message 中追加权限建议;
/// 其余错误保持基础消息。不影响重试策略(仅 EPERM、至多一次)。
PlatformError makeMemAccessError(const char* operation, Pid_t pid, int errorCode)
{
    if (errorCode == EPERM || errorCode == EACCES) {
        return PlatformError::from_last_error(
            operation, static_cast<unsigned long>(pid), kPermissionAdvice);
    }
    return PlatformError::from_last_error(operation, static_cast<unsigned long>(pid));
}

} // namespace

// ============================================================
// 构造/析构
// ============================================================
LinuxProcess::LinuxProcess(Pid_t pid, std::string p_name)
    : PlatformProcess(pid, std::move(p_name))
{}

LinuxProcess::~LinuxProcess() {
    ensurePtraceDetached();
    if (m_procMemFd >= 0) {
        close(m_procMemFd);
        m_procMemFd = -1;
    }
}

// ============================================================
// 内存页权限过滤
// ============================================================
bool LinuxProcess::isCheatablePage(const std::string& perms)
{
    // 兼容 3 字符 (rwx) 和 4 字符 (rwxp) 格式
    if (perms.size() < 3) {
        std::cerr << "[WARN] LinuxProcess::isCheatablePage: unexpected perms length="
                  << perms.size() << " value='" << perms << "'" << std::endl;
        return false;
    }
    bool readable  = (perms[0] == 'r');
    bool writable  = (perms[1] == 'w');
    bool isPrivate = (perms.size() == 3) || (perms[3] == 'p');

    if (perms.size() != 3 && perms.size() != 4) {
        std::cerr << "[WARN] LinuxProcess::isCheatablePage: unexpected perms length="
                  << perms.size() << " value='" << perms << "'" << std::endl;
    }
    return readable && writable && isPrivate;
}

// ============================================================
// getCheatablePages() — 解析 /proc/PID/maps + 权限过滤
// ============================================================
std::vector<MemoryPage> LinuxProcess::getCheatablePages() const {
    std::vector<MemoryPage> pages;
    std::ifstream maps_file("/proc/" + std::to_string(m_pid) + "/maps");
    if (!maps_file) {
        std::cerr << "[ERROR] LinuxProcess::getCheatablePages: cannot open /proc/"
                  << m_pid << "/maps" << std::endl;
        return pages;
    }

    std::string line;
    while (std::getline(maps_file, line)) {
        std::istringstream iss(line);
        std::string address_range, perms, offset, dev, inode;
        iss >> address_range >> perms >> offset >> dev >> inode;
        // 仅读取前 5 列，忽略后续未知列

        // 地址范围解析
        size_t dash_pos = address_range.find('-');
        if (dash_pos == std::string::npos) {
            std::cerr << "[WARN] LinuxProcess::getCheatablePages: skipping malformed address range '"
                      << address_range << "'" << std::endl;
            continue;
        }

        // 权限过滤
        if (!isCheatablePage(perms))
            continue;

        // 地址转换
        try {
            tpe::Address start = static_cast<tpe::Address>(
                std::stoull(address_range.substr(0, dash_pos), nullptr, 16));
            tpe::Address end   = static_cast<tpe::Address>(
                std::stoull(address_range.substr(dash_pos + 1), nullptr, 16));
            if (end <= start) {
                std::cerr << "[WARN] LinuxProcess::getCheatablePages: skipping invalid range "
                          << address_range << std::endl;
                continue;
            }
            tpe::Size size = end - start;
            pages.emplace_back(start, size);
        } catch (const std::exception&) {
            std::cerr << "[WARN] LinuxProcess::getCheatablePages: skipping non-hex address '"
                      << address_range << "'" << std::endl;
            continue;
        }
    }

    return pages;
}

// ============================================================
// 内存读写后端探测
// ============================================================
void LinuxProcess::detectMemBackend() const {
    if (m_memBackend != MemBackend::AutoDetect)
        return;

    struct iovec local_empty  = { nullptr, 0 };
    struct iovec remote_empty = { nullptr, 0 };
    ssize_t ret = process_vm_readv(static_cast<pid_t>(m_pid), &local_empty, 0, &remote_empty, 0, 0);

    if (ret == 0 || (ret == -1 && errno != ENOSYS)) {
        m_memBackend = MemBackend::ProcessVm;
    } else {
        m_memBackend = MemBackend::ProcMem;
        std::cerr << "[WARN] LinuxProcess::detectMemBackend: process_vm_readv not available"
                  << " (ENOSYS), falling back to /proc/PID/mem" << std::endl;
    }
}

// ============================================================
// process_vm_readv 实现
// ============================================================
Result<tpe::Memory, PlatformError> LinuxProcess::readViaProcessVm(MemoryPage page) const {
    tpe::Memory buffer(page.size);

    struct iovec local_iov;
    local_iov.iov_base = buffer.data();
    local_iov.iov_len  = page.size;

    struct iovec remote_iov;
    remote_iov.iov_base = reinterpret_cast<void*>(static_cast<uintptr_t>(page.start));
    remote_iov.iov_len  = page.size;

    ssize_t nread = process_vm_readv(static_cast<pid_t>(m_pid), &local_iov, 1, &remote_iov, 1, 0);

    if (nread < 0) {
        // EPERM: 尝试 ptrace attach 后重试一次(FR-022:仅 EPERM、至多一次)
        if (errno == EPERM && !m_ptraceAttached) {
            auto attachResult = ensurePtraceAttached();
            if (attachResult) {
                nread = process_vm_readv(static_cast<pid_t>(m_pid), &local_iov, 1, &remote_iov, 1, 0);
            }
            if (nread < 0) {
                return Result<tpe::Memory, PlatformError>::error(
                    makeMemAccessError("process_vm_readv", m_pid, errno));
            }
        } else {
            return Result<tpe::Memory, PlatformError>::error(
                makeMemAccessError("process_vm_readv", m_pid, errno));
        }
    }

    if (static_cast<size_t>(nread) < page.size) {
        std::cerr << "[WARN] LinuxProcess::readViaProcessVm: partial read "
                  << nread << "/" << page.size << " bytes at 0x"
                  << std::hex << page.start << std::dec << std::endl;
        buffer.resize(static_cast<size_t>(nread));
    }

    return Result<tpe::Memory, PlatformError>::success(std::move(buffer));
}

// ============================================================
// process_vm_writev 实现
// ============================================================
Result<void, PlatformError> LinuxProcess::writeViaProcessVm(
    tpe::Address address, const tpe::Memory& value) const
{
    struct iovec local_iov;
    local_iov.iov_base = const_cast<tpe::Byte*>(value.data());
    local_iov.iov_len  = value.size();

    struct iovec remote_iov;
    remote_iov.iov_base = reinterpret_cast<void*>(static_cast<uintptr_t>(address));
    remote_iov.iov_len  = value.size();

    ssize_t nwritten = process_vm_writev(static_cast<pid_t>(m_pid), &local_iov, 1, &remote_iov, 1, 0);

    if (nwritten < 0) {
        // EPERM: 尝试 ptrace attach 后重试一次(FR-022:仅 EPERM、至多一次)
        if (errno == EPERM && !m_ptraceAttached) {
            auto attachResult = ensurePtraceAttached();
            if (attachResult) {
                nwritten = process_vm_writev(static_cast<pid_t>(m_pid), &local_iov, 1, &remote_iov, 1, 0);
            }
            if (nwritten < 0) {
                return Result<void, PlatformError>::error(
                    makeMemAccessError("process_vm_writev", m_pid, errno));
            }
        } else {
            return Result<void, PlatformError>::error(
                makeMemAccessError("process_vm_writev", m_pid, errno));
        }
    }

    if (static_cast<size_t>(nwritten) < value.size()) {
        std::cerr << "[WARN] LinuxProcess::writeViaProcessVm: partial write "
                  << nwritten << "/" << value.size() << " bytes at 0x"
                  << std::hex << address << std::dec << std::endl;
        return Result<void, PlatformError>::error(
            PlatformError::from_last_error("process_vm_writev partial write", m_pid));
    }

    return Result<void, PlatformError>::success();
}

// ============================================================
// /proc/PID/mem 句柄惰性升级(缺陷④;FR-011/FR-012/C-P2)
// ============================================================
bool LinuxProcess::ensureProcMemFd(bool needWrite) const {
    const std::string path = "/proc/" + std::to_string(m_pid) + "/mem";

    if (m_procMemFd >= 0) {
        if (!needWrite || m_procMemFdWritable) {
            return true;
        }
        // 已有只读句柄但本次需要写入:关闭后以 O_RDWR 升级重开
        close(m_procMemFd);
        m_procMemFd = -1;
        m_procMemFdWritable = false;
    }

    if (needWrite) {
        m_procMemFd = open(path.c_str(), O_RDWR);
        m_procMemFdWritable = (m_procMemFd >= 0);
    } else {
        // 读路径:优先可写句柄(后续写入免重开);无写权限时降级只读
        m_procMemFd = open(path.c_str(), O_RDWR);
        if (m_procMemFd >= 0) {
            m_procMemFdWritable = true;
        } else {
            m_procMemFd = open(path.c_str(), O_RDONLY);
            m_procMemFdWritable = false;
        }
    }

    return m_procMemFd >= 0;
}

// ============================================================
// /proc/PID/mem 降级读写
// ============================================================
Result<tpe::Memory, PlatformError> LinuxProcess::readViaProcMem(MemoryPage page) const {
    if (!ensureProcMemFd(/*needWrite=*/false)) {
        return Result<tpe::Memory, PlatformError>::error(
            makeMemAccessError("open /proc/PID/mem", m_pid, errno));
    }

    tpe::Memory buffer(page.size);
    ssize_t nread = pread(m_procMemFd, buffer.data(), page.size,
                          static_cast<off_t>(page.start));

    if (nread < 0) {
        return Result<tpe::Memory, PlatformError>::error(
            makeMemAccessError("pread /proc/PID/mem", m_pid, errno));
    }

    if (static_cast<size_t>(nread) < page.size) {
        buffer.resize(static_cast<size_t>(nread));
    }

    return Result<tpe::Memory, PlatformError>::success(std::move(buffer));
}

Result<void, PlatformError> LinuxProcess::writeViaProcMem(
    tpe::Address address, const tpe::Memory& value) const
{
    // fd 不存在或为只读时升级为 O_RDWR;升级失败返回明确错误(不使用 EBADF 只读句柄)
    if (!ensureProcMemFd(/*needWrite=*/true)) {
        return Result<void, PlatformError>::error(
            makeMemAccessError("open /proc/PID/mem for write", m_pid, errno));
    }

    ssize_t nwritten = pwrite(m_procMemFd, value.data(), value.size(),
                              static_cast<off_t>(address));

    if (nwritten < 0) {
        return Result<void, PlatformError>::error(
            makeMemAccessError("pwrite /proc/PID/mem", m_pid, errno));
    }

    if (static_cast<size_t>(nwritten) < value.size()) {
        return Result<void, PlatformError>::error(
            PlatformError::from_last_error("pwrite /proc/PID/mem partial write", m_pid));
    }

    return Result<void, PlatformError>::success();
}

// ============================================================
// Public read() — 自动后端选择 + 大块分块
// ============================================================
Result<tpe::Memory, PlatformError> LinuxProcess::read(MemoryPage page) const {
    detectMemBackend();

    constexpr tpe::Size CHUNK_SIZE = 64 * 1024;
    if (page.size <= CHUNK_SIZE) {
        if (m_memBackend == MemBackend::ProcessVm) {
            return readViaProcessVm(page);
        } else {
            return readViaProcMem(page);
        }
    }

    // 分块循环读取
    tpe::Memory full(page.size);
    tpe::Address currentAddr = page.start;
    tpe::Size remaining = page.size;
    tpe::Size offset = 0;

    while (remaining > 0) {
        tpe::Size chunkSize = (remaining > CHUNK_SIZE) ? CHUNK_SIZE : remaining;
        MemoryPage chunk(currentAddr, chunkSize);

        Result<tpe::Memory, PlatformError> result;
        if (m_memBackend == MemBackend::ProcessVm) {
            result = readViaProcessVm(chunk);
        } else {
            result = readViaProcMem(chunk);
        }

        if (!result) {
            return result;
        }

        auto& chunkData = result.value();
        std::memcpy(full.data() + offset, chunkData.data(), chunkData.size());
        offset += chunkData.size();
        currentAddr += static_cast<tpe::Address>(chunkData.size());
        remaining -= chunkData.size();

        if (chunkData.size() < chunkSize)
            break;
    }

    full.resize(offset);
    return Result<tpe::Memory, PlatformError>::success(std::move(full));
}

// ============================================================
// Public write() — 自动后端选择
// ============================================================
Result<void, PlatformError> LinuxProcess::write(
    tpe::Address address, const tpe::Memory& value)
{
    detectMemBackend();

    if (m_memBackend == MemBackend::ProcessVm) {
        return writeViaProcessVm(address, value);
    } else {
        return writeViaProcMem(address, value);
    }
}

// ============================================================
// ptrace 权限管理
// ============================================================
Result<void, PlatformError> LinuxProcess::ensurePtraceAttached() const {
    if (m_ptraceAttached)
        return Result<void, PlatformError>::success();

    if (ptrace(PTRACE_ATTACH, static_cast<pid_t>(m_pid), nullptr, nullptr) == -1) {
        int e = errno;
        const char* advice = "";
        switch (e) {
            case EPERM:
                advice = "Permission denied. Try running as root or set "
                         "/proc/sys/kernel/yama/ptrace_scope to 0";
                break;
            case ESRCH:
                advice = "Target process no longer exists";
                break;
            case EBUSY:
            case EEXIST:
                advice = "Target is already being traced by another debugger";
                break;
            default:
                advice = "ptrace PTRACE_ATTACH failed";
                break;
        }
        // FR-021/C-P4:三类诊断必须进入错误消息本身(不依赖 stderr 日志)
        std::cerr << "[ERROR] LinuxProcess::ensurePtraceAttached: " << advice << std::endl;
        return Result<void, PlatformError>::error(
            PlatformError::from_last_error("ptrace PTRACE_ATTACH", m_pid, advice));
    }

    // 等待目标进程 SIGSTOP 就绪，5 秒超时
    int status = 0;
    constexpr int TIMEOUT_MS = 5000;
    constexpr int POLL_MS   = 100;
    int elapsed = 0;
    pid_t waited = 0;

    while (elapsed < TIMEOUT_MS) {
        waited = waitpid(static_cast<pid_t>(m_pid), &status, WNOHANG);
        if (waited > 0) {
            if (WIFSTOPPED(status))
                break;
        }
        usleep(POLL_MS * 1000);
        elapsed += POLL_MS;
    }

    if (waited <= 0 || !WIFSTOPPED(status)) {
        std::cerr << "[ERROR] LinuxProcess::ensurePtraceAttached: waitpid timeout after PTRACE_ATTACH"
                  << std::endl;
        ptrace(PTRACE_DETACH, static_cast<pid_t>(m_pid), nullptr, nullptr);
        return Result<void, PlatformError>::error(
            PlatformError::from_last_error("waitpid timeout after PTRACE_ATTACH", m_pid));
    }

    m_ptraceAttached = true;
    return Result<void, PlatformError>::success();
}

void LinuxProcess::ensurePtraceDetached() const {
    if (!m_ptraceAttached)
        return;
    ptrace(PTRACE_DETACH, static_cast<pid_t>(m_pid), nullptr, nullptr);
    m_ptraceAttached = false;
}

#endif // __linux__