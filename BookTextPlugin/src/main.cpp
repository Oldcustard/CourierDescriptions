#include "pch.h"
#include "Papyrus.h"

extern "C" __declspec(dllexport) bool SKSEAPI SKSEPlugin_Load(const SKSE::LoadInterface* a_skse)
{
    SKSE::Init(a_skse);            // default logger (logs into the SKSE plugin log)
    logs::info("CDNBookText loaded"sv);

    CDNBookText::InstallHooks();

    const auto papyrus = SKSE::GetPapyrusInterface();
    if (!papyrus || !papyrus->Register(CDNBookText::BindPapyrus))
    {
        SKSE::stl::report_and_fail("CDNBookText: failed to register Papyrus functions"sv);
    }

    return true;
}
