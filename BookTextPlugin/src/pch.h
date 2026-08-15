#pragma once

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <MinHook.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace std::literals;   // the generated SKSEPluginInfo TU needs "...sv literals
namespace logs = SKSE::log;
