#include "CliCommandParser.h"

void configureMainCommandParser(cmdline::parser& parser)
{
    parser.add("all-processes", '\0', "All process name");
    parser.add<Pid_t>("serch-process", '\0', "Search process name", false, 0, cmdline::range<Pid_t>(0, 65536));
    parser.add<Pid_t>("open-process", '\0', "Open process", false, 0);
}