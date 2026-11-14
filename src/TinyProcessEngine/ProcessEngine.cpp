#include "ProcessEngine.hpp"

#include <cassert>
#include <string>
#include <utility>

namespace tpe {

namespace {

/// 渲染单个进程的列表行(T016):本地 `getProcessList()` 与 `processListText()`
/// 共用同一渲染器,防止两处格式漂移;行分隔固定 LF,行尾带换行。
std::string formatProcessListLine(const tpe::platform::PlatformProcess& process)
{
    return "PID: " + std::to_string(process.getPid()) +
           " ProcessName: " + process.getProcessName() + '\n';
}

} // namespace

ProcessEngine::ProcessEngine()
    : m_os(tpe::platform::createPlatformOS())
{
}

ProcessEngine::ProcessEngine(std::unique_ptr<tpe::platform::PlatformOS> os)
    : m_os(std::move(os))
{
}

ProcessEngine::~ProcessEngine()
{
}

Result<void, PlatformError> ProcessEngine::getProcessList() const
{
    // T016:成功/失败均复用 processListText(),保证本地 stdout 与远程文本逐字节一致
    // (FR-016/C-P5);枚举失败不打印任何进程行(缺陷⑧)。
    const Result<std::string, PlatformError> text = processListText();
    if (!text.has_value()) {
        return Result<void, PlatformError>::error(text.error());
    }

    std::cout << text.value();
    std::cout.flush(); // 与旧实现逐行 std::endl 的最终可见性一致;不改变输出内容。
    return Result<void, PlatformError>::success();
}

Result<std::string, PlatformError> ProcessEngine::processListText() const
{
    // 缺陷⑧(FR-023/C-P5):平台枚举失败原样透传;不得以空列表伪装成功。
    const std::optional<PlatformError>& enumerationError = m_os->enumerationError();
    if (enumerationError.has_value()) {
        return Result<std::string, PlatformError>::error(*enumerationError);
    }

    // 按 ProcessList 顺序拼接;空列表返回空字符串;不打印、不做 UTF-8/长度校验
    // (校验归 T017 响应层,契约 contracts/cli.md §5)。
    std::string text;
    for (const std::shared_ptr<tpe::platform::PlatformProcess>& process : m_os->ProcessList) {
        text += formatProcessListLine(*process);
    }
    return Result<std::string, PlatformError>::success(std::move(text));
}

std::shared_ptr<tpe::platform::PlatformProcess> ProcessEngine::openProcess(tpe::platform::Pid_t pid)
{
    m_currentProcess = m_os->open(pid);
    if (!m_currentProcess) {
        std::cerr << "Failed to open process with PID " << pid << std::endl;
    }
    return m_currentProcess;
}

Result<OpenProcessInfo, PlatformError> ProcessEngine::openProcessResult(
    tpe::platform::Pid_t pid, bool strict)
{
    std::shared_ptr<tpe::platform::PlatformProcess> process = m_os->open(pid);
    if (!process) {
        PlatformError error{"OpenProcess", static_cast<unsigned long>(pid), 0,
                            "process does not exist or cannot be opened"};
        return Result<OpenProcessInfo, PlatformError>::error(std::move(error));
    }

    const std::string name = process->getProcessName();
    if (strict) {
        if (name.empty() || name == "Unknown" || name == "<unknown>") {
            PlatformError error{"OpenProcess", static_cast<unsigned long>(pid), 0,
                                "process name is not available"};
            return Result<OpenProcessInfo, PlatformError>::error(std::move(error));
        }
        if (!process->bindIdentity() || !process->hasMemoryReadAccess()) {
            PlatformError error{"OpenProcess", static_cast<unsigned long>(pid), 0,
                                "memory read access was denied"};
            return Result<OpenProcessInfo, PlatformError>::error(std::move(error));
        }
    }

    m_session.reset();
    m_currentProcess = process;
    OpenProcessInfo info;
    info.process = std::move(process);
    info.name = name;
    info.readOnly = info.process->isReadOnly();
    return Result<OpenProcessInfo, PlatformError>::success(std::move(info));
}

void ProcessEngine::closeProcess()
{
    m_session.reset();
    m_currentProcess.reset();
}

std::optional<std::string> ProcessEngine::searchProcess(tpe::platform::Pid_t pid) const
{
    for(auto process : m_os->ProcessList)
    {
        if (process->getPid() == pid)
        {
            return process->getProcessName();
        }
    }
    return std::nullopt;
}

uint64_t ProcessEngine::searchMemory(const ValueType& type, const tpe::Memory& pattern)
{
    if (!m_currentProcess) {
        std::cerr << "No process opened. Use 'open-process <PID>' first." << std::endl;
        return 0;
    }

    // Initialize scan session
    m_session = std::make_unique<ScanSession>(m_currentProcess);
    m_session->beginScan(type);

    // Execute first scan (pattern is provided by the caller; no interactive prompt)
    ScanOptions options;
    auto results = m_scanner.firstScan(*m_currentProcess, type, pattern, options);

    // Commit results to session
    m_session->commitFirstScan(std::move(results));

    return m_session->resultCount();
}

uint64_t ProcessEngine::searchUnknown(const ValueType& type)
{
    if (!m_currentProcess) {
        std::cerr << "No process opened. Use 'open-process <PID>' first." << std::endl;
        return 0;
    }

    // Initialize scan session
    m_session = std::make_unique<ScanSession>(m_currentProcess);
    m_session->beginScan(type);

    // Execute unknown-initial-value first scan（无目标值;按类型宽度对齐枚举候选）
    ScanOptions options;
    auto results = m_scanner.firstScanUnknown(*m_currentProcess, type, options);

    // Commit results with the correct first-round condition (C-D11)
    m_session->commitFirstScan(std::move(results), ScanCondition::Unknown);

    return m_session->resultCount();
}

uint64_t ProcessEngine::searchComparison(const ValueType& type, ScanCondition condition,
                                         const tpe::Memory& target)
{
    if (!m_currentProcess) {
        std::cerr << "No process opened. Use 'open-process <PID>' first." << std::endl;
        return 0;
    }

    // Initialize scan session
    m_session = std::make_unique<ScanSession>(m_currentProcess);
    m_session->beginScan(type);

    // Execute first-round comparison scan（严格 GT/LT;采集步进=类型宽度对齐）
    ScanOptions options;
    auto results =
        m_scanner.firstScanComparison(*m_currentProcess, type, condition, target, options);

    // Commit results with the correct first-round condition (C-D2/C-D11)
    m_session->commitFirstScan(std::move(results), condition);

    return m_session->resultCount();
}

uint64_t ProcessEngine::nextScan(ScanCondition condition, const ValueType& type,
                                  const std::optional<tpe::Memory>& newValue)
{
    if (!m_session || m_session->state() != SessionState::Ready) {
        std::cerr << "No scan results available. Run 'scan' first." << std::endl;
        return 0;
    }

    // Collect previous results from session
    std::vector<ScanRecord> previousResults;
    uint64_t count = m_session->resultCount();
    previousResults.reserve(static_cast<size_t>((std::min)(count, static_cast<uint64_t>(1000000))));
    for (uint64_t i = 0; i < count; ++i) {
        auto rec = m_session->resultAt(i);
        if (rec.has_value()) {
            previousResults.push_back(*rec);
        }
    }

    // Execute next scan
    auto filteredResults = m_scanner.nextScan(*m_currentProcess, previousResults,
                                               condition, type, newValue);

    // Commit to session(前置已检查 Ready,失败为不可达路径;C-E3)
    const Result<void, SessionError> committed =
        m_session->commitNextScan(condition, std::move(filteredResults));
    assert(committed.has_value());
    (void)committed;

    return m_session->resultCount();
}

void ProcessEngine::modifyMemory() {
    if (!m_currentProcess) {
        std::cerr << "No process opened. Use 'open-process <PID>' first." << std::endl;
        return;
    }

    while (true) {
        std::cout << "Enter memory address (hex) to modify or 'exit' to quit: ";
        std::string input;
        std::getline(std::cin, input);

        if (input == "exit") {
            break;
        }

        try {
            tpe::Address address = std::stoull(input, nullptr, 16);

            std::cout << "Enter new value (hex): ";
            std::getline(std::cin, input);
            uint8_t new_value = static_cast<uint8_t>(std::stoul(input, nullptr, 16));

            tpe::Memory value(1, static_cast<tpe::Byte>(new_value));
            auto result = m_currentProcess->write(address, value);

            if (result) {
                std::cout << "Memory at address 0x" << std::hex << address
                          << " modified to 0x" << std::hex << static_cast<int>(new_value) << "\n";
            } else {
                std::cerr << "Write failed: " << result.error().message << std::endl;
            }
        } catch (const std::exception &) {
            std::cerr << "Invalid input. Please try again." << std::endl;
        }
    }
}

} // namespace tpe
