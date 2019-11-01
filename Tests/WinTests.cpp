#include <boost/test/unit_test.hpp>

#include <Windows.h>

#include "GLib/Win/DebugStream.h"
#include "GLib/Win/DebugWrite.h"
#include "GLib/Win/FileSystem.h"
#include "GLib/Win/NativeExceptionPrinter.h"
#include "GLib/Win/Process.h"
#include "GLib/Win/Registry.h"
#include "GLib/Win/Symbols.h"
#include "GLib/Win/Uuid.h"

#include "TestUtils.h"

namespace
{
	LONG WINAPI Filter(std::ostream & s, EXCEPTION_POINTERS * exceptionInfo)
	{
		GLib::Win::Symbols::Print(s, exceptionInfo, 100);
		return EXCEPTION_EXECUTE_HANDLER;
	}

	template <typename Function>
	void GetStackTrace(std::ostream & s, const Function & function)
	{
		__try
		{
			function();
		}
		__except(Filter(s, GetExceptionInformation()))
		{
		}
	}

	template <typename Function>
	std::string GetStackTrace(const Function & function)
	{
		std::ostringstream s;
		GetStackTrace(s, function);
		//std::cout << s.str();
		return s.str();
	}
}

using namespace GLib::Win;

// split up
// clone hg tests in
BOOST_AUTO_TEST_SUITE(WinTests)

	BOOST_AUTO_TEST_CASE(TestDriveInfo)
	{
		auto ld = FileSystem::LogicalDrives();
		(void)ld;
		auto dm = FileSystem::DriveMap();

		std::filesystem::path tempFilePath = std::filesystem::temp_directory_path() / GLib::Cvt::a2w(to_string(Util::Uuid::CreateRandom()) + ".tmp");
		Handle h(FileSystem::CreateAutoDeleteFile(tempFilePath.u8string()));
		std::string pathOfHandle = FileSystem::PathOfFileHandle(h.get(), VOLUME_NAME_NT);

		BOOST_TEST(!exists(std::filesystem::u8path(pathOfHandle)));
		auto normalisedPath = FileSystem::NormalisePath(pathOfHandle, dm);
		BOOST_TEST(exists(std::filesystem::u8path(normalisedPath)));
	}

	BOOST_AUTO_TEST_CASE(TestPathOfModule)
	{
		std::string path = FileSystem::PathOfModule(Process::CurrentModule());
		BOOST_TEST("Tests.exe" == std::filesystem::path(path).filename().u8string());
	}

	BOOST_AUTO_TEST_CASE(TestPathOfhandle)
	{
		std::filesystem::path tempFilePath = std::filesystem::temp_directory_path()
			/ GLib::Cvt::a2w(to_string(Util::Uuid::CreateRandom()) + "\xE2\x82\xAC.tmp");

		Handle h(FileSystem::CreateAutoDeleteFile(tempFilePath.u8string()));
		std::string pathOfHandle = FileSystem::PathOfFileHandle(h.get(), 0);
		BOOST_TEST(true == exists(std::filesystem::u8path(pathOfHandle)));
		h.reset();
		BOOST_TEST(false == exists(std::filesystem::u8path(pathOfHandle)));
	}

	BOOST_AUTO_TEST_CASE(TestDebugStream)
	{
		// capture output
		Debug::Stream() << "Hello!" << std::endl;
		Debug::Write("DebugStreamTest1");
		Debug::Write("DebugStreamTest2 {0} {1} {2}", 1, 2, 3);
		Debug::Write("Utf8 \xE2\x82\xAC");
		::OutputDebugStringA("Write utf8 \xE2\x82\xAC directly for Debugger test\r\n");
	}

	BOOST_AUTO_TEST_CASE(TestProcess)
	{
		// capture output
		Process p(R"(c:\windows\system32\cmd.exe)"); //  /c echo hello
		{
			auto scopedTerminator = p.ScopedTerminator();
			BOOST_TEST(p.IsRunning());
			(void)scopedTerminator;
		}

		BOOST_TEST(!p.IsRunning());
		BOOST_TEST(1U == p.ExitCode());
	}

	BOOST_AUTO_TEST_CASE(TestErrorCheck)
	{
		::SetLastError(ERROR_ACCESS_DENIED);
		GLIB_CHECK_EXCEPTION(Util::AssertTrue(false, "test fail"),
			WinException, "test fail : Access is denied. (5)");
	}

	BOOST_AUTO_TEST_CASE(RegistryKeyExists)
	{
		BOOST_TEST(RegistryKeys::LocalMachine.KeyExists("Software"));
		BOOST_TEST(!RegistryKeys::LocalMachine.KeyExists("MissingFooBar"));
	}

	BOOST_AUTO_TEST_CASE(RegistryGetString)
	{
		auto key = RegistryKeys::CurrentUser / "Environment";
		std::string regValue = key.GetString("TEMP");

		auto envValue = *GLib::Compat::GetEnv("TEMP");
		BOOST_TEST(envValue == regValue);
	}

	BOOST_AUTO_TEST_CASE(RegistryMissingValue)
	{
		GLIB_CHECK_EXCEPTION(RegistryKeys::LocalMachine.GetString("MissingFooBar"),
			WinException, "RegGetValue : The system cannot find the file specified. (2)");
	}

	BOOST_AUTO_TEST_CASE(RegistryGet)
	{
		auto regValue = RegistryKeys::CurrentUser / "Environment" & "TEMP";

		std::string envValue = *GLib::Compat::GetEnv("TEMP");
		BOOST_TEST(std::holds_alternative<std::string>(regValue));
		BOOST_TEST(envValue == std::get<std::string>(regValue));
	}

	BOOST_AUTO_TEST_CASE(RegistryTestComItf)
	{
		std::string keyPath = GLib::Formatter::Format("Interface\\{0}", Util::Uuid{IID_IUnknown});
		BOOST_TEST(RegistryKeys::ClassesRoot.KeyExists(keyPath));
		BOOST_TEST(RegistryKeys::ClassesRoot.OpenSubKey(keyPath).GetString("") == "IUnknown");
	}

	BOOST_AUTO_TEST_CASE(RegistryCreateKey)
	{
		const auto & rootKey = RegistryKeys::CurrentUser;
		constexpr const char TestKey[] = "Software\\CrapolaCppUnitTests";

		auto key = rootKey.CreateSubKey(TestKey);
		SCOPE(Delete, [&]()
		{
			rootKey.DeleteSubKey(TestKey);
		});

		BOOST_TEST(rootKey.KeyExists(TestKey));
		key.SetInt32("Int32Value", 1234567890);
		key.SetInt64("Int64Value", 12345678901234567890);
		key.SetString("StringValue", "plugh");

		BOOST_TEST(1234567890u == key.GetInt32("Int32Value"));
		BOOST_TEST(12345678901234567890u == key.GetInt64("Int64Value"));
		BOOST_TEST(1234567890u == key.GetInt64("Int32Value"));
		BOOST_TEST("plugh" == key.GetString("StringValue"));

		GLIB_CHECK_RUNTIME_EXCEPTION(key.GetInt32("Int64Value"), "RegQueryValueEx : More data is available. (234)");
		GLIB_CHECK_RUNTIME_EXCEPTION(key.GetInt32("StringValue"), "RegQueryValueEx : More data is available. (234)");
		GLIB_CHECK_RUNTIME_EXCEPTION(key.GetInt64("StringValue"), "RegQueryValueEx : More data is available. (234)");
		GLIB_CHECK_RUNTIME_EXCEPTION(key.GetString("Int32Value"), "RegGetValue : Data of this type is not supported. (1630)");
		GLIB_CHECK_RUNTIME_EXCEPTION(key.GetString("Int64Value"), "RegGetValue : Data of this type is not supported. (1630)");

		BOOST_TEST(1234567890u == std::get<uint32_t>(key.Get("Int32Value")));
		BOOST_TEST(12345678901234567890u == std::get<uint64_t>(key.Get("Int64Value")));
		BOOST_TEST("plugh" == std::get<std::string>(key.Get("StringValue")));

		rootKey.DeleteSubKey(TestKey);
		BOOST_TEST(!rootKey.KeyExists(TestKey));
	}

	BOOST_AUTO_TEST_CASE(PrintNativeException1)
	{
		auto s = GetStackTrace([]() { throw std::runtime_error("!"); });

		BOOST_TEST(s.find("Unhandled exception at") != std::string::npos);
		BOOST_TEST(s.find("(code: E06D7363) : C++ exception of type: 'class std::runtime_error'") != std::string::npos);
		BOOST_TEST(s.find("RaiseException") != std::string::npos);
		BOOST_TEST(s.find("CxxThrowException") != std::string::npos);
		BOOST_TEST(s.find("GetStackTrace") != std::string::npos);
		BOOST_TEST(s.find("WinTests::PrintNativeException1") != std::string::npos);
	}

	BOOST_AUTO_TEST_CASE(PrintNativeException2)
	{
		auto s = GetStackTrace([]() { throw 12345678; });

		BOOST_TEST(s.find("Unhandled exception at") != std::string::npos);
		BOOST_TEST(s.find("(code: E06D7363) : C++ exception of type: 'int'") != std::string::npos);
		BOOST_TEST(s.find("RaiseException") != std::string::npos);
		BOOST_TEST(s.find("CxxThrowException") != std::string::npos);
		BOOST_TEST(s.find("GetStackTrace") != std::string::npos);
		BOOST_TEST(s.find("WinTests::PrintNativeException2") != std::string::npos);
	}

	BOOST_AUTO_TEST_CASE(PrintNativeException3)
	{
		const int * p = nullptr;
		int result{};
		auto s = GetStackTrace([&]() { result = *p++; });

		BOOST_TEST(s.find("Unhandled exception at") != std::string::npos);
		BOOST_TEST(s.find("(code: C0000005) : Access violation reading address 00000000") != std::string::npos);
		BOOST_TEST(s.find("GetStackTrace") != std::string::npos);
		BOOST_TEST(s.find("WinTests::PrintNativeException3") != std::string::npos);
	}

BOOST_AUTO_TEST_SUITE_END()