// TEMPORARY verification test for Courier Descriptions - not part of CommonLibSSE.
//
// Asserts the three things CDNBookText depends on, across every runtime the one
// DLL claims to serve: the runtime family the library derives from the exe
// version, which Address Library ID RELOCATION_ID(25083, 25626) then selects,
// and that the selected ID actually resolves out of the shipped database for
// that version - including the format-5 database that 1.7.99+ ships.
#include "catch2/catch_all.hpp"

#include "REL/REL.h"
#include "SKSE/SKSE.h"

namespace
{
	// TESTopicInfo::GetResponseList, as CDNBookText (and CommonLibSSE itself) ask
	// for it. The hook is the whole mod; if this ID picks wrong, the courier goes
	// silent or the game crashes.
	std::uint64_t CourierHookID()
	{
		return RELOCATION_ID(25083, 25626).id();
	}

	REL::Module::Runtime ClassifyWithoutHint(REL::Version a_version)
	{
		// a_runtime = Unknown makes mock() run the SAME classification path that
		// load_version() uses against a real SkyrimSE.exe.
		REQUIRE(REL::Module::mock(a_version, REL::Module::Runtime::Unknown, L"SkyrimSE.exe", 0x1000));
		return REL::Module::GetRuntime();
	}
}

TEST_CASE("CDN/RuntimeFamilyFromExeVersion")
{
	SECTION("1.7.104 (the new AE line) classifies as AE")
	{
		CHECK(ClassifyWithoutHint(SKSE::RUNTIME_SSE_1_7_104) == REL::Module::Runtime::AE);
		CHECK(REL::Module::IsAE());
		CHECK_FALSE(REL::Module::IsSE());
		CHECK_FALSE(REL::Module::IsVR());
		CHECK(CourierHookID() == 25626);
	}
	SECTION("1.7.99 classifies as AE")
	{
		CHECK(ClassifyWithoutHint(SKSE::RUNTIME_SSE_1_7_99) == REL::Module::Runtime::AE);
		CHECK(CourierHookID() == 25626);
	}
	SECTION("1.6.1170 still classifies as AE")
	{
		CHECK(ClassifyWithoutHint(SKSE::RUNTIME_SSE_1_6_1170) == REL::Module::Runtime::AE);
		CHECK(CourierHookID() == 25626);
	}
	SECTION("1.6.640 still classifies as AE")
	{
		CHECK(ClassifyWithoutHint(SKSE::RUNTIME_SSE_1_6_640) == REL::Module::Runtime::AE);
		CHECK(CourierHookID() == 25626);
	}
	SECTION("1.5.97 still classifies as SE")
	{
		CHECK(ClassifyWithoutHint(SKSE::RUNTIME_SSE_1_5_97) == REL::Module::Runtime::SE);
		CHECK(REL::Module::IsSE());
		CHECK(CourierHookID() == 25083);
	}
	SECTION("1.4.15 still classifies as VR")
	{
		REQUIRE(REL::Module::mock(REL::Version(1, 4, 15, 0), REL::Module::Runtime::Unknown, L"SkyrimVR.exe", 0x1000));
		CHECK(REL::Module::GetRuntime() == REL::Module::Runtime::VR);
		CHECK(CourierHookID() == 25083);  // VR shares the SE ID in RELOCATION_ID
	}
	REL::Module::reset();
}

TEST_CASE("CDN/HookIDResolvesInEveryDatabase")
{
	SECTION("1.7.99 - format 5")
	{
		REQUIRE(REL::Module::mock(SKSE::RUNTIME_SSE_1_7_99, REL::Module::Runtime::AE, L"SkyrimSE.exe", 0x1000));
		REQUIRE(REL::IDDB::inject(
			L"Data\\SKSE\\Plugins\\versionlib-1-7-99-0.bin", REL::IDDB::Format::SSEv5, SKSE::RUNTIME_SSE_1_7_99));
		CHECK(REL::IDDB::get().id2offset(25626) == 0x3F3300);
		CHECK(REL::ID(CourierHookID()).offset() == 0x3F3300);
		REL::Module::reset();
	}
	SECTION("1.6.1170 - format 2")
	{
		REQUIRE(REL::Module::mock(SKSE::RUNTIME_SSE_1_6_1170, REL::Module::Runtime::AE, L"SkyrimSE.exe", 0x1000));
		REQUIRE(REL::IDDB::inject(
			L"Data\\SKSE\\Plugins\\versionlib-1-6-1170-0.bin", REL::IDDB::Format::SSEv2, SKSE::RUNTIME_SSE_1_6_1170));
		const auto offset = REL::ID(CourierHookID()).offset();
		CHECK(offset != 0);
		WARN(std::string("1.6.1170 GetResponseList (id 25626) offset = ") + std::to_string(offset));
		REL::Module::reset();
	}
	SECTION("1.5.97 - format 1")
	{
		REQUIRE(REL::Module::mock(SKSE::RUNTIME_SSE_1_5_97, REL::Module::Runtime::SE, L"SkyrimSE.exe", 0x1000));
		REQUIRE(REL::IDDB::inject(
			L"Data\\SKSE\\Plugins\\version-1-5-97-0.bin", REL::IDDB::Format::SSEv1, SKSE::RUNTIME_SSE_1_5_97));
		const auto offset = REL::ID(CourierHookID()).offset();
		CHECK(offset != 0);
		WARN(std::string("1.5.97 GetResponseList (id 25083) offset = ") + std::to_string(offset));
		REL::Module::reset();
	}
}

TEST_CASE("CDN/AutoDetectPicksTheRightFormat")
{
	// Real loads pass no expected format - the reader dispatches on the format
	// byte in the file. This is the path the game takes.
	SECTION("format 5 auto-detects")
	{
		REQUIRE(REL::Module::mock(SKSE::RUNTIME_SSE_1_7_99, REL::Module::Runtime::AE, L"SkyrimSE.exe", 0x1000));
		REQUIRE(REL::IDDB::inject(L"Data\\SKSE\\Plugins\\versionlib-1-7-99-0.bin", SKSE::RUNTIME_SSE_1_7_99));
		CHECK(REL::IDDB::get().id2offset(25626) == 0x3F3300);
		REL::Module::reset();
	}
	SECTION("format 2 auto-detects")
	{
		REQUIRE(REL::Module::mock(SKSE::RUNTIME_SSE_1_6_1170, REL::Module::Runtime::AE, L"SkyrimSE.exe", 0x1000));
		REQUIRE(REL::IDDB::inject(L"Data\\SKSE\\Plugins\\versionlib-1-6-1170-0.bin", SKSE::RUNTIME_SSE_1_6_1170));
		CHECK(REL::IDDB::get().id2offset(25626) != 0);
		REL::Module::reset();
	}
}
