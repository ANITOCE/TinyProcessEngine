#pragma once

#include "ProcessEngine.h"
#include "HelpFunction.h"
#include "cmdline.h"
#include "Platform.h"
#include "ValueType.h"
#include "Matches.h"
#include "ScanTypes.h"
#include "AobPattern.h"
#include "MemoryScanner.h"
#include "CliCommandParser.h"

#include <sstream>
#include <iomanip>

// Helper: parse hex address string → tpe::Address
static tpe::Address parseHexAddr(const std::string& s) {
    return static_cast<tpe::Address>(std::stoull(s, nullptr, 16));
}

// Helper: split string by first space → {first, rest}
static std::pair<std::string, std::string> splitFirst(const std::string& s) {
    size_t pos = s.find(' ');
    if (pos == std::string::npos) return {s, ""};
    return {s.substr(0, pos), s.substr(pos + 1)};
}

int main(int argc, char *argv[])
{
    ProcessEngine engine = ProcessEngine();
    std::string prefix = "";

    while(1) {

        cmdline::parser commandLine;
        configureMainCommandParser(commandLine);

        std::cout << "TinyProcessEngine$ "+ prefix + "> ";
        std::string command = "";
        std::getline(std::cin, command);
        commandLine.parse_check(" " + command);

        if (commandLine.exist("all-processes")) 
            engine.getProcessList();
        else if (commandLine.exist("open-process")) {
            prefix.clear();
            auto proc = engine.openProcess(commandLine.get<Pid_t>("open-process"));
            if (!proc) {
                std::cerr << "Failed to open process." << std::endl;
                continue;
            }
            prefix = proc->getProcessName() + " ";

            // ── Search sub-mode ──
            std::cout << "Select value type:\n";
            const ValueType& type = ValueType::choose_type();

            // Read initial value for scan (optional — user can change per scan)
            std::cout << "\nEnter search value (or 'unknown' for Unknown Initial Value scan): ";
            std::string initVal;
            std::getline(std::cin, initVal);

            bool isUnknownMode = (initVal == "unknown" || initVal == "u");

            if (!isUnknownMode) {
                // Execute first scan immediately with the given value
                uint64_t count = engine.searchMemory(type);
                std::cout << "First scan: " << count << " matches found.\n";
            } else {
                // TODO: Unknown initial value scan — record all readable addresses
                // For now, show placeholder message
                std::cout << "Unknown initial value mode selected.\n";
                std::cout << "Run 'scan u' to start unknown scan when implemented.\n";
            }

            while(1){
                // Status line (FR-014)
                auto* session = engine.session();
                if (session && session->state() == SessionState::Ready) {
                    std::cout << "\n[Round " << session->round()
                              << " | Matches: " << session->resultCount()
                              << " | Type: " << type.name << "]\n";
                }
                std::cout << "[" << proc->getProcessName() << "] ";

                std::string scanCmd;
                std::getline(std::cin, scanCmd);
                if (scanCmd.empty()) continue;

                auto [cmd, args] = splitFirst(scanCmd);

                // ── scan / s ── first scan
                if (cmd == "scan" || cmd == "s") {
                    if (args == "unknown" || args == "u") {
                        // Unknown initial value scan
                        std::cout << "Unknown scan: recording all readable addresses...\n";
                        // For now: use firstScan with Unknown condition
                        // This requires MemoryScanner to support Unknown mode
                        std::cout << "Unknown scan not fully implemented yet. Use 'scan' with a value.\n";
                    } else {
                        uint64_t count = engine.searchMemory(type);
                        std::cout << "Scan complete: " << count << " matches found.\n";
                    }
                }
                // ── next / n ── incremental scan
                else if (cmd == "next" || cmd == "n") {
                    if (!session || session->resultCount() == 0) {
                        std::cout << "No results. Run 'scan' first.\n";
                        continue;
                    }
                    ScanCondition cond;
                    std::optional<tpe::Memory> newVal;

                    if (args == "exact" || args == "e") {
                        cond = ScanCondition::ExactValue;
                        std::cout << "Enter new value: ";
                        std::string valStr;
                        std::getline(std::cin, valStr);
                        newVal = type.askValue();
                    } else if (args == "increased" || args == "inc") {
                        cond = ScanCondition::Increased;
                    } else if (args == "decreased" || args == "dec") {
                        cond = ScanCondition::Decreased;
                    } else if (args == "changed" || args == "ch") {
                        cond = ScanCondition::Changed;
                    } else if (args == "unchanged" || args == "uc") {
                        cond = ScanCondition::Unchanged;
                    } else {
                        std::cout << "Usage: next <exact|increased|decreased|changed|unchanged>\n";
                        continue;
                    }

                    uint64_t count = engine.nextScan(cond, type, newVal);
                    std::cout << "Next scan: " << count << " matches remaining.\n";
                }
                // ── undo / u ──
                else if (cmd == "undo" || cmd == "u") {
                    if (!session || !session->canUndo()) {
                        std::cout << "Nothing to undo.\n";
                        continue;
                    }
                    try {
                        session->undo();
                        std::cout << "Undo: reverted to round " << session->round()
                                  << " (" << session->resultCount() << " matches).\n";
                    } catch (const std::exception& e) {
                        std::cout << "Undo failed: " << e.what() << "\n";
                    }
                }
                // ── list / l ──
                else if (cmd == "list" || cmd == "l") {
                    if (!session || session->resultCount() == 0) {
                        std::cout << "No results. Run 'scan' first.\n";
                        continue;
                    }
                    uint64_t total = session->resultCount();
                    uint64_t perPage = 20;
                    uint64_t totalPages = (total + perPage - 1) / perPage;
                    uint64_t page = 0;

                    if (!args.empty()) {
                        try { page = std::stoull(args) - 1; }
                        catch (...) { page = 0; }
                    }

                    uint64_t start = page * perPage;
                    uint64_t end = (std::min)(start + perPage, total);
                    std::cout << "Page " << (page + 1) << "/" << totalPages
                              << " (Total: " << total << " matches)\n";
                    for (uint64_t i = start; i < end; ++i) {
                        auto rec = session->resultAt(i);
                        if (rec.has_value()) {
                            std::cout << "  0x" << std::hex << std::uppercase
                                      << std::setw(16) << std::setfill('0')
                                      << rec->address << std::dec << std::nouppercase;
                            if (rec->hasSnapshot()) {
                                std::cout << "  [snapshot: " << static_cast<int>(rec->snapshot_size) << " bytes]";
                            }
                            std::cout << "\n";
                        }
                    }
                    std::cout << "(use 'list <page>' to jump, or 'n'/'p' for next/prev)\n";
                }
                // ── read / r ──
                else if (cmd == "read" || cmd == "r") {
                    if (args.empty()) {
                        std::cout << "Usage: read <hex_address>\n";
                        continue;
                    }
                    try {
                        tpe::Address addr = parseHexAddr(args);
                        auto result = session ? session->readMemory(addr, type.askValue().size())
                                              : Result<tpe::Memory, PlatformError>::error(
                                                  PlatformError{"read", 0, 0, "No session"});
                        if (result) {
                            auto& mem = result.value();
                            std::cout << "0x" << std::hex << addr << std::dec << " = ";
                            for (auto b : mem) std::cout << std::hex << std::setw(2)
                                                         << std::setfill('0') << static_cast<int>(b) << " ";
                            std::cout << std::dec << "\n";
                        } else {
                            std::cout << "Read failed: " << result.error().message << "\n";
                        }
                    } catch (const std::exception& e) {
                        std::cout << "Error: " << e.what() << "\n";
                    }
                }
                // ── write / w ──
                else if (cmd == "write" || cmd == "w") {
                    if (args.empty()) {
                        std::cout << "Usage: write <hex_address> <hex_value>\n";
                        continue;
                    }
                    auto [addrStr, valStr] = splitFirst(args);
                    if (addrStr.empty() || valStr.empty()) {
                        std::cout << "Usage: write <hex_address> <hex_value>\n";
                        continue;
                    }
                    try {
                        tpe::Address addr = parseHexAddr(addrStr);
                        // Parse value as hex bytes
                        tpe::Memory value;
                        std::istringstream vss(valStr);
                        std::string byteStr;
                        while (vss >> byteStr) {
                            value.push_back(static_cast<tpe::Byte>(std::stoul(byteStr, nullptr, 16)));
                        }
                        if (value.empty()) {
                            value.push_back(static_cast<tpe::Byte>(std::stoul(valStr, nullptr, 16)));
                        }

                        auto result = session ? session->writeMemory(addr, value)
                                              : Result<void, PlatformError>::error(
                                                  PlatformError{"write", 0, 0, "No session"});
                        if (result) {
                            std::cout << "Write OK: 0x" << std::hex << addr << std::dec << "\n";
                        } else {
                            std::cout << "Write failed: " << result.error().message << "\n";
                        }
                    } catch (const std::exception& e) {
                        std::cout << "Error: " << e.what() << "\n";
                    }
                }
                // ── aob ──
                else if (cmd == "aob") {
                    if (args.empty()) {
                        std::cout << "Usage: aob <hex_pattern_with_??>\n";
                        std::cout << "Example: aob 48 8B ?? ?? 00 10\n";
                        continue;
                    }
                    std::string err;
                    auto pattern = AobPattern::parse(args, &err);
                    if (!pattern.has_value()) {
                        std::cout << "Invalid pattern: " << err << "\n";
                        continue;
                    }
                    MemoryScanner scanner;
                    ScanOptions opts;
                    auto results = scanner.scanAOB(*proc, *pattern, opts);
                    std::cout << "AOB scan: " << results.size() << " matches found.\n";
                    size_t show = (std::min)(results.size(), static_cast<size_t>(20));
                    for (size_t i = 0; i < show; ++i) {
                        std::cout << "  0x" << std::hex << results[i] << std::dec << "\n";
                    }
                    if (results.size() > show)
                        std::cout << "  ... and " << (results.size() - show) << " more.\n";
                }
                // ── export / e ──
                else if (cmd == "export" || cmd == "e") {
                    if (!session || session->resultCount() == 0) {
                        std::cout << "No results to export.\n";
                        continue;
                    }
                    std::string filename = args.empty() ? "scan_results.txt" : args;
                    try {
                        session->exportTo(filename, "");
                        std::cout << "Exported " << session->resultCount()
                                  << " results to " << filename << "\n";
                    } catch (const std::exception& e) {
                        std::cout << "Export failed: " << e.what() << "\n";
                    }
                }
                // ── back / b / exit ──
                else if (cmd == "back" || cmd == "b" || cmd == "exit") {
                    break;
                }
                // ── help / h ──
                else if (cmd == "help" || cmd == "h") {
                    std::cout << "Available commands:\n";
                    std::cout << "  scan / s              First scan with current type+value\n";
                    std::cout << "  scan unknown / su     Unknown initial value scan\n";
                    std::cout << "  next <cond> / n       Next scan (exact|increased|decreased|changed|unchanged)\n";
                    std::cout << "  undo / u              Undo last scan\n";
                    std::cout << "  list [page] / l       Show results (paginated)\n";
                    std::cout << "  read <hex_addr> / r   Read memory at address\n";
                    std::cout << "  write <hex_addr> <val> / w  Write memory\n";
                    std::cout << "  aob <pattern>         Array-of-bytes search\n";
                    std::cout << "  export [file] / e     Export results to file\n";
                    std::cout << "  back / b              Return to main prompt\n";
                    std::cout << "  help / h              Show this help\n";
                }
                else {
                    std::cout << "Unknown command. Type 'help' for available commands.\n";
                }
            }
        }     
        else if (commandLine.exist("serch-process")) {
            std::cout << engine.searchProcess(commandLine.get<Pid_t>("serch-process")) << std::endl;
        }

        if(command == "exit") {
            break;
        }
    }

    return 0;
}