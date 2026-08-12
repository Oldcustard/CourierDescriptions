#pragma once

namespace CDNBookText
{
    // Bound to the Papyrus VM from SKSEPlugin_Load. Registers the native
    // functions on the "CDN_BookText" script class.
    bool BindPapyrus(RE::BSScript::IVirtualMachine* a_vm);

    // Detours TESTopicInfo::GetResponseList so a dialogue INFO's response text can
    // be swapped at runtime. Must run once at load, before any dialogue plays.
    void InstallHooks();
}
