#include "pch.h"
#include "Papyrus.h"

// ⚠️ HOOK INSTALLATION IS DEFERRED TO kDataLoaded ON PURPOSE - see InstallHooks
// in Papyrus.cpp. Installing from SKSEPlugin_Load put us AHEAD of Dynamic String
// Distributor and crashed the game on load for every user who has it.
static void MessageListener(SKSE::MessagingInterface::Message* a_msg)
{
    if (a_msg && a_msg->type == SKSE::MessagingInterface::kDataLoaded) {
        CDNBookText::InstallHooks();
    }
}

// ⚠ `SKSEAPI` IS GONE FROM CommonLibSSE-NG v14 - the entry point is declared
// with the library's own macro now (`SKSE_EXPORT` = extern "C" +
// __declspec(dllexport); the old __cdecl was a no-op on x64 anyway). Writing the
// signature out by hand is what broke the v14 build: the compiler reads an
// undefined `SKSEAPI` as a return type and reports a missing `;`.
SKSE_PLUGIN_LOAD(const SKSE::LoadInterface* a_skse)
{
    SKSE::Init(a_skse);            // default logger (logs into the SKSE plugin log)

    // ⚠ THE GAME BUILD AND THE RUNTIME FAMILY GO IN THE LOG, NOT JUST OUR OWN
    // VERSION. Which family CommonLibSSE resolved the exe to is what picks between
    // the two Address Library IDs for the hook, and 1.7.99 moved the MINOR version,
    // so a library that classifies by it (anything before CommonLibSSE-NG v6.7.0)
    // calls 1.7.104 "SE" and silently resolves the wrong one. Without this line a
    // 1.7.104 report cannot be told from a 1.6.1170 report at all - and neither
    // version can be tested on the dev machine.
    logs::info("CDNBookText loaded - Skyrim {} ({})"sv,
               REL::Module::get().version().string("."sv),
               REL::Module::IsVR() ? "VR"sv : (REL::Module::IsAE() ? "AE"sv : "SE"sv));

    const auto messaging = SKSE::GetMessagingInterface();
    if (!messaging || !messaging->RegisterListener(MessageListener)) {
        SKSE::stl::report_and_fail("CDNBookText: failed to register the SKSE message listener"sv);
    }

    const auto papyrus = SKSE::GetPapyrusInterface();
    if (!papyrus || !papyrus->Register(CDNBookText::BindPapyrus))
    {
        SKSE::stl::report_and_fail("CDNBookText: failed to register Papyrus functions"sv);
    }

    return true;
}
