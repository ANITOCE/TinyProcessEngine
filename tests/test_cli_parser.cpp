#include <gtest/gtest.h>

#include "Platform.h"
#include "CliCommandParser.h"
#include "cmdline.h"

TEST(CliParserTest, OpenProcessAcceptsPidValue)
{
    cmdline::parser parser;
    configureMainCommandParser(parser);

    const std::vector<std::string> args{"TinyProcessEngine", "--open-process", "1234"};

    EXPECT_TRUE(parser.parse(args)) << parser.error_full();
    EXPECT_TRUE(parser.exist("open-process"));
    EXPECT_EQ(parser.get<Pid_t>("open-process"), static_cast<Pid_t>(1234));
}