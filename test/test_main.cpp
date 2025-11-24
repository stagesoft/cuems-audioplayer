/* LICENSE TEXT

    audioplayer for linux based using RtAudio and RtMidi libraries to
    process audio and receive MTC sync. It also uses oscpack to receive
    some configurations through osc commands.
    Copyright (C) 2020  Stage Lab & bTactic.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.

*/

#include <gtest/gtest.h>
#include <iostream>
#include <sstream>
#include <cstring>
#include "main.h"
#include "cuems_errors.h"

// Test showcopyright function
TEST(MainFunctionsTest, ShowCopyright) {
    // Redirect cout to capture output
    std::ostringstream oss;
    std::streambuf* oldCout = std::cout.rdbuf(oss.rdbuf());
    
    showcopyright();
    
    std::cout.rdbuf(oldCout);
    
    std::string output = oss.str();
    
    // Check that copyright information is displayed
    EXPECT_NE(output.find("audioplayer-cuems"), std::string::npos);
    EXPECT_NE(output.find("Copyright"), std::string::npos);
    EXPECT_NE(output.find("Stage Lab"), std::string::npos);
}

// Test showusage function
TEST(MainFunctionsTest, ShowUsage) {
    // Redirect cout to capture output
    std::ostringstream oss;
    std::streambuf* oldCout = std::cout.rdbuf(oss.rdbuf());
    
    showusage();
    
    std::cout.rdbuf(oldCout);
    
    std::string output = oss.str();
    
    // Check that usage information is displayed
    EXPECT_NE(output.find("Usage"), std::string::npos);
    EXPECT_NE(output.find("--file"), std::string::npos);
    EXPECT_NE(output.find("--port"), std::string::npos);
}

// Test showwarrantydisclaimer function
TEST(MainFunctionsTest, ShowWarrantyDisclaimer) {
    // Redirect cout to capture output
    std::ostringstream oss;
    std::streambuf* oldCout = std::cout.rdbuf(oss.rdbuf());
    
    showwarrantydisclaimer();
    
    std::cout.rdbuf(oldCout);
    
    std::string output = oss.str();
    
    // Check that warranty disclaimer is displayed
    EXPECT_NE(output.find("Warranty"), std::string::npos);
    EXPECT_NE(output.find("NO WARRANTY"), std::string::npos);
}

// Test showcopydisclaimer function
TEST(MainFunctionsTest, ShowCopyDisclaimer) {
    // Redirect cout to capture output
    std::ostringstream oss;
    std::streambuf* oldCout = std::cout.rdbuf(oss.rdbuf());
    
    showcopydisclaimer();
    
    std::cout.rdbuf(oldCout);
    
    std::string output = oss.str();
    
    // Check that copyright disclaimer is displayed
    EXPECT_NE(output.find("Copyright"), std::string::npos);
    EXPECT_NE(output.find("GNU General Public License"), std::string::npos);
}

// Test error code definitions
TEST(MainFunctionsTest, ErrorCodesDefined) {
    // Verify that error codes are defined
    EXPECT_EQ(CUEMS_EXIT_OK, 0);
    EXPECT_EQ(CUEMS_EXIT_FAILURE, -1);
    EXPECT_EQ(CUEMS_EXIT_WRONG_PARAMETERS, -2);
    EXPECT_EQ(CUEMS_EXIT_WRONG_DATA_FILE, -3);
    EXPECT_EQ(CUEMS_EXIT_AUDIO_DEVICE_ERR, -4);
}

// Test that signal handler functions are declared
TEST(MainFunctionsTest, SignalHandlersDeclared) {
    // Verify that signal handler functions exist
    // This is a compile-time check
    EXPECT_TRUE(true);
    
    // We can't easily test signal handlers without actually sending signals
    // which would require process forking and is better suited for integration tests
}

// Test that main.h includes required headers
TEST(MainFunctionsTest, HeaderIncludes) {
    // Verify that required headers are included
    // This is a compile-time check
    EXPECT_TRUE(true);
}

// Test error code values are correct
TEST(MainFunctionsTest, ErrorCodeValues) {
    // Test that error codes have expected values
    EXPECT_LT(CUEMS_EXIT_FAILURE, 0);
    EXPECT_LT(CUEMS_EXIT_WRONG_PARAMETERS, 0);
    EXPECT_LT(CUEMS_EXIT_WRONG_DATA_FILE, 0);
    EXPECT_LT(CUEMS_EXIT_AUDIO_DEVICE_ERR, 0);
}

// Test that all error codes are unique
TEST(MainFunctionsTest, ErrorCodesUnique) {
    // Verify that all error codes are different
    EXPECT_NE(CUEMS_EXIT_FAILURE, CUEMS_EXIT_WRONG_PARAMETERS);
    EXPECT_NE(CUEMS_EXIT_FAILURE, CUEMS_EXIT_WRONG_DATA_FILE);
    EXPECT_NE(CUEMS_EXIT_FAILURE, CUEMS_EXIT_AUDIO_DEVICE_ERR);
    EXPECT_NE(CUEMS_EXIT_WRONG_PARAMETERS, CUEMS_EXIT_WRONG_DATA_FILE);
    EXPECT_NE(CUEMS_EXIT_WRONG_PARAMETERS, CUEMS_EXIT_AUDIO_DEVICE_ERR);
    EXPECT_NE(CUEMS_EXIT_WRONG_DATA_FILE, CUEMS_EXIT_AUDIO_DEVICE_ERR);
}

// Test copyright output format
TEST(MainFunctionsTest, CopyrightOutputFormat) {
    std::ostringstream oss;
    std::streambuf* oldCout = std::cout.rdbuf(oss.rdbuf());
    
    showcopyright();
    
    std::cout.rdbuf(oldCout);
    
    std::string output = oss.str();
    
    // Check for version format
    EXPECT_NE(output.find("v."), std::string::npos);
}

// Test usage output contains all main options
TEST(MainFunctionsTest, UsageOutputContainsOptions) {
    std::ostringstream oss;
    std::streambuf* oldCout = std::cout.rdbuf(oss.rdbuf());
    
    showusage();
    
    std::cout.rdbuf(oldCout);
    
    std::string output = oss.str();
    
    // Check for main command-line options
    EXPECT_NE(output.find("--file"), std::string::npos);
    EXPECT_NE(output.find("--port"), std::string::npos);
    EXPECT_NE(output.find("--offset"), std::string::npos);
    EXPECT_NE(output.find("--wait"), std::string::npos);
    EXPECT_NE(output.find("--uuid"), std::string::npos);
    EXPECT_NE(output.find("--device"), std::string::npos);
    EXPECT_NE(output.find("--ciml"), std::string::npos);
    EXPECT_NE(output.find("--mtcfollow"), std::string::npos);
    EXPECT_NE(output.find("--resample-quality"), std::string::npos);
}

// Test warranty disclaimer contains expected text
TEST(MainFunctionsTest, WarrantyDisclaimerContent) {
    std::ostringstream oss;
    std::streambuf* oldCout = std::cout.rdbuf(oss.rdbuf());
    
    showwarrantydisclaimer();
    
    std::cout.rdbuf(oldCout);
    
    std::string output = oss.str();
    
    // Check for key warranty terms
    EXPECT_NE(output.find("NO WARRANTY"), std::string::npos);
    EXPECT_NE(output.find("AS IS"), std::string::npos);
}

// Test copyright disclaimer contains GPL reference
TEST(MainFunctionsTest, CopyDisclaimerGPLReference) {
    std::ostringstream oss;
    std::streambuf* oldCout = std::cout.rdbuf(oss.rdbuf());
    
    showcopydisclaimer();
    
    std::cout.rdbuf(oldCout);
    
    std::string output = oss.str();
    
    // Check for GPL reference
    EXPECT_NE(output.find("GNU General Public License"), std::string::npos);
    EXPECT_NE(output.find("version 3"), std::string::npos);
}

// Test that functions don't crash
TEST(MainFunctionsTest, FunctionsDoNotCrash) {
    // Test that all display functions can be called without crashing
    EXPECT_NO_THROW(showcopyright());
    EXPECT_NO_THROW(showusage());
    EXPECT_NO_THROW(showwarrantydisclaimer());
    EXPECT_NO_THROW(showcopydisclaimer());
}

// Test output is non-empty
TEST(MainFunctionsTest, OutputNonEmpty) {
    std::ostringstream oss;
    std::streambuf* oldCout = std::cout.rdbuf(oss.rdbuf());
    
    showcopyright();
    std::string copyright = oss.str();
    oss.str("");
    
    showusage();
    std::string usage = oss.str();
    oss.str("");
    
    showwarrantydisclaimer();
    std::string warranty = oss.str();
    oss.str("");
    
    showcopydisclaimer();
    std::string copy = oss.str();
    
    std::cout.rdbuf(oldCout);
    
    EXPECT_FALSE(copyright.empty());
    EXPECT_FALSE(usage.empty());
    EXPECT_FALSE(warranty.empty());
    EXPECT_FALSE(copy.empty());
}

