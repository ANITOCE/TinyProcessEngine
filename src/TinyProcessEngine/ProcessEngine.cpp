#include "ProcessEngine.h"

ProcessEngine::ProcessEngine()
{
}

ProcessEngine::~ProcessEngine()
{
}

void ProcessEngine::getProcessList() const
{
    for(auto process : m_os->ProcessList)
    {
        std::cout << "PID: " << process->getPid() << " ProcessName: " << process->getProcessName() << std::endl;
    }
}

std::shared_ptr<PlatformProcess> ProcessEngine::openProcess(Pid_t pid)
{
    m_currentProcess = m_os->open(pid);
    if (!m_currentProcess) {
        std::cerr << "Failed to open process with PID " << pid << std::endl;
    }
    return m_currentProcess;
}

std::optional<std::string> ProcessEngine::searchProcess(Pid_t pid) const
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

uint64_t ProcessEngine::searchMemory(const ValueType& type)
{
    if (!m_currentProcess) {
        std::cerr << "No process opened. Use 'open-process <PID>' first." << std::endl;
        return 0;
    }

    // Initialize scan session
    m_session = std::make_unique<ScanSession>(m_currentProcess);
    m_session->beginScan(type);

    // Execute first scan
    ScanOptions options;
    auto results = m_scanner.firstScan(*m_currentProcess, type, options);

    // Commit results to session
    m_session->commitFirstScan(std::move(results));

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

    // Commit to session
    m_session->commitNextScan(condition, std::move(filteredResults));

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
        } catch (const std::exception &e) {
            std::cerr << "Invalid input. Please try again." << std::endl;
        }
    }
}
