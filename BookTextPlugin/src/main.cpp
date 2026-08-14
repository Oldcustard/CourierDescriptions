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

extern "C" __declspec(dllexport) bool SKSEAPI SKSEPlugin_Load(const SKSE::LoadInterface* a_skse)
{
    SKSE::Init(a_skse);            // default logger (logs into the SKSE plugin log)
    logs::info("CDNBookText loaded"sv);

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
