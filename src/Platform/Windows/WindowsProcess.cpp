#ifdef _WIN32

#include "WindowsProcess.hpp"

namespace tpe::platform {

namespace {

std::string queryProcessName(HANDLE handle)
{
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
        return {};
    }
    WCHAR processPath[MAX_PATH] = {0};
    DWORD bufferSize = MAX_PATH;
    if (!QueryFullProcessImageNameW(handle, 0, processPath, &bufferSize)) {
        return {};
    }
    std::wstring name(processPath, bufferSize);
    const std::size_t pos = name.find_last_of(L"\\/");
    if (pos != std::wstring::npos) {
        name = name.substr(pos + 1);
    }
    return to_byte_string(name);
}

} // namespace

bool can_cheat_page(const MEMORY_BASIC_INFORMATION &page)
{
    return page.State == MEM_COMMIT &&
           page.Type == MEM_PRIVATE &&
           page.Protect == PAGE_READWRITE;
}

WindowsProcess::WindowsProcess(Pid_t pid) : PlatformProcess(pid)
{
    this->m_processHandle = ScopedHandle(OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid));
    this->m_processName = queryProcessName(this->m_processHandle.get());
}

WindowsProcess::WindowsProcess(Pid_t pid, std::string p_name) : PlatformProcess(pid, std::move(p_name))
{
    this->m_processHandle = ScopedHandle(OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid));
}

WindowsProcess::WindowsProcess(Pid_t pid, std::string p_name, ScopedHandle handle)
    : PlatformProcess(pid, std::move(p_name))
    , m_processHandle(std::move(handle))
{
    // 远程严格打开使用调用方已取得的句柄；名称必须与该句柄同源，不能沿用
    // 枚举快照或占位名称。
    const std::string realName = queryProcessName(this->m_processHandle.get());
    if (!realName.empty()) {
        this->m_processName = realName;
    }
}

bool WindowsProcess::isAlive() const
{
    if (!m_processHandle.valid()) {
        return false;
    }
    DWORD exitCode = 0;
    if (!GetExitCodeProcess(m_processHandle.get(), &exitCode)) {
        return false;
    }
    return exitCode == STILL_ACTIVE;
}

std::vector<MemoryPage> WindowsProcess::getCheatablePages() const {
    std::vector<MemoryPage> pages;
    tpe::Address address = 0;
    for (MEMORY_BASIC_INFORMATION m_page;
        VirtualQueryEx(this->m_processHandle.get(), reinterpret_cast<LPCVOID>(address), &m_page, sizeof(m_page)) == sizeof(m_page);
        address = reinterpret_cast<tpe::Address>(m_page.BaseAddress) + m_page.RegionSize)
    {
        if (can_cheat_page(m_page))
        {
            pages.emplace_back(reinterpret_cast<tpe::Address>(m_page.BaseAddress), m_page.RegionSize);
        }
    }

    return pages;
}

Result<tpe::Memory, PlatformError> WindowsProcess::read(MemoryPage page) const {
    tpe::Memory memory(page.size);

    SIZE_T total;
    if (!ReadProcessMemory(this->m_processHandle.get(), reinterpret_cast<LPCVOID>(page.start), memory.data(), page.size, &total))
    {
        return Result<tpe::Memory, PlatformError>::error(
            PlatformError::from_last_error("ReadProcessMemory", m_pid));
    }

    memory.resize(total);
    return Result<tpe::Memory, PlatformError>::success(std::move(memory));
}

Result<void, PlatformError> WindowsProcess::write(tpe::Address address, const tpe::Memory &value) {
    // 缺陷③(FR-009,C-P1):降级只读会话不调用 WriteProcessMemory,
    // 返回明确错误(消息含 read-only;native code = ERROR_ACCESS_DENIED)。
    if (m_readOnly) {
        PlatformError err;
        err.operation   = "WriteProcessMemory";
        err.pid         = m_pid;
        err.native_code = static_cast<int>(ERROR_ACCESS_DENIED);
        err.message     = "Operation '" + err.operation
                        + "' failed on PID " + std::to_string(err.pid)
                        + " (native code: " + std::to_string(err.native_code) + "): "
                          "session opened read-only; write access was denied at open";
        return Result<void, PlatformError>::error(std::move(err));
    }

    if (!WriteProcessMemory(this->m_processHandle.get(), reinterpret_cast<LPVOID>(address), value.data(), value.size(), nullptr))
    {
        return Result<void, PlatformError>::error(
            PlatformError::from_last_error("WriteProcessMemory", m_pid));
    }
    return Result<void, PlatformError>::success();
}

} // namespace tpe::platform

#endif // _WIN32