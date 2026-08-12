#include "pch.h"
#include "Papyrus.h"

namespace
{
    // Must match the Scriptname in Scripts/Source/CDN_BookText.psc.
    constexpr std::string_view PapyrusClass = "CDN_BookText";

    // Returns the body text of a book/letter (the DESC subrecord), as authored
    // (may include <font>/markup and <Alias=...> tokens). TESObjectBOOK inherits
    // TESDescription, whose GetDescription fills a BSString with the DESC text.
    // Returns "" for a null book or an empty description.
    RE::BSFixedString GetBookText(RE::StaticFunctionTag*, RE::TESObjectBOOK* a_book)
    {
        if (!a_book)
            return RE::BSFixedString("");

        RE::BSString buf;
        a_book->GetDescription(buf, a_book);  // default fieldType 'CSED' == DESC == book text

        const char* str = buf.c_str();
        return RE::BSFixedString(str ? str : "");
    }
    // ---- Sender resolution ---------------------------------------------------
    //
    // Works out WHO sent a letter, so the prompt can pull that NPC's SkyrimNet
    // bio (formid_to_uuid -> render_character_profile). Returns an actor
    // REFERENCE FormID, which is what SkyrimNet's decorators expect - verified
    // against SeverActions, which feeds formid_to_uuid an akActor.GetFormID().
    //
    // WHY THIS IS HERE AND NOT IN PAPYRUS. SkyrimNetApi.FindActorByName exists,
    // but it only searches "within 4000 units of the player" (SkyrimNetApi.psc).
    // A letter's sender is almost never nearby, which is the entire case this
    // feature is built for, so it is useless to us.
    //
    // A WRONG IDENTIFICATION IS WORSE THAN NONE - the courier would confidently
    // describe the wrong person. Hence: unique NPCs only, and any name that
    // matches more than one actor resolves to 0.

    // One entry per unique actor NAME. `ambiguous` means two genuinely different
    // people share it and we refuse to guess - see BuildSenderIndex for why that
    // is NOT the same as two records sharing a name.
    struct SenderEntry
    {
        RE::FormID         id        = 0;  // the actor REFERENCE - what callers get
        RE::FormID         baseId    = 0;  // the base NPC_ record - what we rank on
        const RE::TESFile* file      = nullptr;  // the base record's defining plugin
        bool               ambiguous = false;
    };

    std::once_flag                               g_senderIndexOnce;
    std::unordered_map<std::string, SenderEntry> g_senderIndex;

    // Place names, so "A warning from Ivarstead" is not read as a person called
    // Ivarstead. Built in the same GetAllForms pass as the sender index.
    std::unordered_set<std::string>              g_placeNames;

    std::string ToLower(std::string_view a_in)
    {
        std::string out(a_in);
        std::transform(out.begin(), out.end(), out.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return out;
    }

    // Strips the punctuation a sign-off is decorated with. The UTF-8 dashes are
    // NOT optional: an LLM-composed letter signs off "- Ysolda" about as often as
    // "\xE2\x80\x94 Ysolda", and a multi-byte dash left on the front becomes part of the
    // lookup key, so the name silently fails to resolve. ASCII-only trimming
    // would make that failure look like "no such NPC".
    std::string_view Trim(std::string_view a_in)
    {
        // en dash, em dash, figure dash, horizontal bar - all 3 bytes in UTF-8.
        static constexpr std::string_view dashes[] = {
            "\xE2\x80\x93"sv, "\xE2\x80\x94"sv, "\xE2\x80\x92"sv, "\xE2\x80\x95"sv
        };
        const auto notSpace = [](unsigned char c) { return !std::isspace(c) && c != '-' && c != '~' && c != '*'; };

        bool changed = true;
        while (changed) {
            changed = false;
            while (!a_in.empty() && !notSpace(static_cast<unsigned char>(a_in.front()))) {
                a_in.remove_prefix(1);
                changed = true;
            }
            while (!a_in.empty() && !notSpace(static_cast<unsigned char>(a_in.back()))) {
                a_in.remove_suffix(1);
                changed = true;
            }
            // After the ASCII pass, so "\xE2\x80\x94 Ysolda" loses the dash on the next lap.
            for (const auto& dash : dashes) {
                if (a_in.starts_with(dash)) {
                    a_in.remove_prefix(dash.size());
                    changed = true;
                }
                if (a_in.ends_with(dash)) {
                    a_in.remove_suffix(dash.size());
                    changed = true;
                }
            }
        }
        return a_in;
    }

    // One pass over the form table, cached for the session. Called from
    // ArmDelivery, which runs when the delivery is first QUEUED - typically
    // minutes before the courier reaches the player - so the cost is invisible.
    // Do NOT call this from the 1.5 s armed poll or the closing-line backstop.
    void BuildSenderIndex()
    {
        const auto& [map, lock] = RE::TESForm::GetAllForms();
        if (!map) {
            logs::warn("CDNBookText: no form table; sender lookup disabled"sv);
            return;
        }

        std::size_t              scanned = 0;
        std::size_t              ambiguous = 0;
        std::size_t              merged = 0;
        const RE::BSReadLockGuard guard{ lock };

        for (const auto& [id, form] : *map) {
            if (!form)
                continue;

            const auto type = form->GetFormType();
            if (type == RE::FormType::Location || type == RE::FormType::Cell ||
                type == RE::FormType::WorldSpace) {
                if (const char* placeName = form->GetName(); placeName && *placeName) {
                    auto trimmed = ToLower(Trim(placeName));
                    if (!trimmed.empty())
                        g_placeNames.insert(std::move(trimmed));
                }
                continue;
            }

            if (!form->Is(RE::FormType::ActorCharacter))
                continue;
            auto* actor = form->As<RE::Actor>();
            if (!actor || actor->GetFormID() == 0x14)  // the player is never a sender
                continue;
            auto* base = actor->GetActorBase();
            if (!base || !base->IsUnique())
                continue;

            const char* raw = base->GetName();
            if (!raw || !*raw)
                continue;
            const auto name = ToLower(Trim(raw));
            if (name.empty())
                continue;

            ++scanned;

            const auto  id     = actor->GetFormID();
            const auto  baseId = base->GetFormID();
            const auto* file   = base->GetFile(0);  // the plugin that DEFINES the base record

            auto [it, inserted] = g_senderIndex.try_emplace(name, SenderEntry{ id, baseId, file, false });
            if (inserted || it->second.ambiguous || it->second.id == id)
                continue;

            // Two Unique records share a display name. That is NOT automatically
            // two people: alternate forms are conventionally shipped as a second
            // Unique record with the same name - CS_Coralyn.esp defines both
            // "Coralyn" (00080A) and "Coralyn as Wolf" (000818), which made a real
            // follower invisible to this lookup. Within ONE plugin, treat them as
            // one character and keep the lowest BASE FormID, which is the record
            // the author created first and so the canonical one.
            //
            // ⚠️ Rank on the BASE record, never on the reference. Reference
            // FormIDs are assigned at runtime and carry no authoring order, so
            // ranking on them picks an arbitrary twin - that shipped once and
            // resolved Coralyn to her wolf form, giving "some naked wolf by the
            // name of Coralyn... he seemed disgusted" for a female follower.
            //
            // ACROSS plugins, refuse: two mods each adding their own "Lydia" are
            // genuinely different people, and describing the wrong one is the
            // failure this whole index is built to avoid.
            if (file && it->second.file == file) {
                if (baseId < it->second.baseId) {
                    it->second.id     = id;
                    it->second.baseId = baseId;
                }
                ++merged;
            } else {
                it->second.ambiguous = true;
                ++ambiguous;
            }
        }

        logs::info("CDNBookText: sender index built - {} unique-named actors, {} ambiguous, {} same-plugin variants merged, {} place names"sv,
                   scanned, ambiguous, merged, g_placeNames.size());
    }

    // "Letter from Ysolda" / "Note from Ysolda" -> "Ysolda"
    // "Ysolda's Letter"                          -> "Ysolda"
    // Is this candidate a PERSON's name, or just the thing after the word "from"?
    //
    // "X from Y" fills Y with whatever the author felt like, and only some of it
    // is a name. Everything rejected here was seen in a real delivery or ships as
    // a test letter:
    //   "Summons from the Steward"  -> "the Steward" - an office. Spoken back as
    //                                  "Someone called the Steward".
    //   "A warning from Ivarstead"  -> a town (caught by g_placeNames).
    //   "Letter from Home"          -> "Home"       - would be "someone called Home".
    //
    // An ARTICLE is the reliable signal: no one is called "the Steward" or "a
    // friend", while "Balgruuf the Greater" carries its article in the middle
    // where this does not look. The word list after it is only for the cases an
    // article does not cover; keep it short, and only add words actually observed.
    bool LooksLikePersonName(std::string_view a_candidate)
    {
        static constexpr std::string_view articles[] = { "the "sv, "a "sv, "an "sv };
        static constexpr std::string_view notPeople[] = {
            "home"sv, "family"sv, "work"sv, "mother"sv, "father"sv, "friend"sv,
            "everyone"sv, "someone"sv, "afar"sv, "abroad"sv
        };

        const auto lower = ToLower(a_candidate);
        if (lower.empty())
            return false;

        for (const auto& article : articles) {
            if (lower.starts_with(article))
                return false;
        }
        for (const auto& word : notPeople) {
            if (lower == word)
                return false;
        }
        return !g_placeNames.contains(lower);
    }

    std::string NameFromTitle(std::string_view a_title)
    {
        std::string candidate;

        const auto lower = ToLower(a_title);
        if (const auto pos = lower.rfind(" from "); pos != std::string::npos)
            candidate = std::string(Trim(a_title.substr(pos + 6)));
        else if (const auto pos2 = lower.find("'s "); pos2 != std::string::npos)
            candidate = std::string(Trim(a_title.substr(0, pos2)));

        if (candidate.empty() || !LooksLikePersonName(candidate))
            return {};

        return candidate;
    }

    // Book text is markup ("<font face=...>" ... "</font>"), so the LAST line of
    // every letter is a closing tag, not a signature. Strip tags before looking.
    std::string StripMarkup(std::string_view a_in)
    {
        std::string out;
        out.reserve(a_in.size());
        bool inTag = false;
        for (const char c : a_in) {
            if (c == '<')
                inTag = true;
            else if (c == '>')
                inTag = false;
            else if (!inTag)
                out.push_back(c);
        }
        return out;
    }

    // The signature line: the last non-empty line of the body, minus a leading
    // dash and any "Yours,"/"Signed," lead-in. NarrativeEngine's letter composer
    // instructs its senders to "Sign off with your name on its own line at the
    // end", so this route covers generated letters as well as hand-authored ones.
    std::string NameFromSignature(std::string_view a_body)
    {
        static constexpr std::string_view leadIns[] = {
            "yours truly,", "yours sincerely,", "your friend,", "sincerely,",
            "signed,", "yours,", "regards,", "written by", "by"
        };

        // Two SAFETY bounds, not optimisations. Test letter 3 signs off "- V.";
        // the initial is rejected as too short, and the search then climbs into
        // the letter's body. Any body that happens to mention a real NPC would
        // resolve to the WRONG sender, and the courier would confidently
        // describe someone who never touched the letter. Failing to find a name
        // is fine; naming the wrong person is not.
        //
        // 1. Look at the last TWO non-empty lines only. Two rather than one
        //    because a sign-off may carry its title beneath it ("- Balgruuf" /
        //    "Jarl of Whiterun").
        // 2. A signature line is SHORT. This is the bound that actually holds:
        //    letter 3's "Forty septims, ..." paragraph is the second non-empty
        //    line from the end, so any line budget lets it through, and the
        //    length check further down runs AFTER the comma split - which sees
        //    only the clause "Forty septims" and waves a whole paragraph past.
        //    Measure the line BEFORE splitting it, and prose is refused as
        //    prose.
        constexpr int         kMaxLinesFromEnd  = 2;
        constexpr std::size_t kMaxSignatureLine = 80;

        int examined = 0;
        while (!a_body.empty() && examined < kMaxLinesFromEnd) {
            const auto nl  = a_body.find_last_of("\r\n");
            auto       raw = nl == std::string_view::npos ? a_body : a_body.substr(nl + 1);
            if (nl == std::string_view::npos)
                a_body = {};
            else
                a_body = a_body.substr(0, nl);

            // Tags are dropped here, so a bare "</font>" line falls through to
            // the real last line above it.
            std::string      stripped(StripMarkup(raw));
            std::string_view line = Trim(stripped);
            if (line.empty())
                continue;  // markup-only lines such as "</font>" don't count against the budget

            ++examined;

            // Prose, not a sign-off. Costs its line of budget and is dropped.
            if (line.size() > kMaxSignatureLine)
                continue;

            auto lower = ToLower(line);
            for (const auto& lead : leadIns) {
                if (lower.starts_with(lead)) {
                    line  = Trim(line.substr(lead.size()));
                    lower = ToLower(line);
                    break;
                }
            }
            // "Bersi Honey-Hand, by his own hand." -> "Bersi Honey-Hand".
            // Whatever follows a comma is a title or a flourish, not the name.
            if (const auto comma = line.find(','); comma != std::string_view::npos)
                line = Trim(line.substr(0, comma));
            if (const auto stop = line.find('.'); stop != std::string_view::npos)
                line = Trim(line.substr(0, stop));

            // A 1-2 character leftover is an initial, not a name ("- V." signs
            // test letter 3). Keep walking upward rather than burning the
            // signature route on something that can never resolve.
            if (line.size() < 3 || line.size() > 64)
                continue;

            // A name is a few words at most - "Balgruuf the Greater" is three.
            // Without this, "Sealed this day by the office of the Steward" (44
            // chars, so inside the length guard, and carrying no comma or full
            // stop to split on) was offered as a sender's name.
            const auto words = std::count(line.begin(), line.end(), ' ') + 1;
            if (words > 4)
                continue;

            return std::string(line);
        }
        return {};
    }

    // Exact match first. Failing that, a UNIQUE prefix match on a word boundary,
    // so a letter signed "Aela" still finds "Aela the Huntress" - but "Ma" or any
    // other prefix shared by two actors resolves to nothing.
    RE::FormID LookupSender(const std::string& a_candidate)
    {
        if (a_candidate.empty())
            return 0;

        const auto key = ToLower(a_candidate);
        if (const auto it = g_senderIndex.find(key); it != g_senderIndex.end())
            return it->second.ambiguous ? 0 : it->second.id;

        const auto  prefix = key + " ";
        RE::FormID  hit    = 0;
        for (const auto& [name, entry] : g_senderIndex) {
            if (!name.starts_with(prefix) || entry.ambiguous)
                continue;
            if (hit != 0)
                return 0;  // more than one - refuse
            hit = entry.id;
        }
        return hit;
    }

    // The sender's name AS WRITTEN ON THE LABEL, or "".
    //
    // This is the only name the courier is allowed to speak without knowing the
    // person: a folded note is labelled on the outside, so he can read it without
    // reading the letter. Deliberately independent of FindSenderRef - a title
    // name is speakable whether or not it resolves to an actor ("Letter from
    // Coralyn" resolved to nothing, and he may still say Coralyn). Place names
    // are filtered out inside NameFromTitle.
    RE::BSFixedString SenderNameFromTitle(RE::StaticFunctionTag*, RE::BSFixedString a_title)
    {
        std::call_once(g_senderIndexOnce, BuildSenderIndex);  // NameFromTitle needs g_placeNames
        const std::string_view title = a_title.c_str() ? a_title.c_str() : "";
        return RE::BSFixedString(NameFromTitle(title).c_str());
    }

    // Best-effort. 0 means "no idea", and the prompt degrades to describing an
    // anonymous stranger exactly as it did before this existed.
    std::int32_t FindSenderRef(RE::StaticFunctionTag*, RE::BSFixedString a_title, RE::TESObjectBOOK* a_book)
    {
        std::call_once(g_senderIndexOnce, BuildSenderIndex);
        if (g_senderIndex.empty())
            return 0;

        const std::string_view title = a_title.c_str() ? a_title.c_str() : "";

        auto candidate = NameFromTitle(title);
        auto source    = "title"sv;

        if (LookupSender(candidate) == 0 && a_book) {
            RE::BSString buf;
            a_book->GetDescription(buf, a_book);
            if (const char* body = buf.c_str(); body && *body) {
                if (auto signature = NameFromSignature(body); !signature.empty()) {
                    candidate = std::move(signature);
                    source    = "signature"sv;
                }
            }
        }

        // Both routes declined, so `source` is still its initial value and would
        // otherwise name a route that produced nothing.
        if (candidate.empty())
            source = "neither route"sv;

        const auto id = LookupSender(candidate);
        if (id == 0) {
            // "no sender" used to cover three different causes and there was no
            // way to tell them apart from the log - which is exactly what made a
            // real follower's ambiguous name look like a missing NPC.
            auto why = "no actor of that name"sv;
            if (candidate.empty()) {
                why = "no name found in the title or the signature"sv;
            } else if (const auto it = g_senderIndex.find(ToLower(candidate));
                       it != g_senderIndex.end() && it->second.ambiguous) {
                why = "AMBIGUOUS - two or more different actors share that name"sv;
            }
            logs::info("CDNBookText: no sender for '{}' ({}; best candidate '{}' via {})"sv,
                       title, why, candidate, source);
        } else {
            logs::info("CDNBookText: sender for '{}' is {:08X} ('{}' via {})"sv,
                       title, id, candidate, source);
        }
        return static_cast<std::int32_t>(id);
    }

    // ---- TESTopicInfo response text -----------------------------------------
    //
    // Why this exists: the courier's line is delivered as a real dialogue INFO so
    // it occupies a dialogue slot BEFORE the closing "Looks like that's it."
    // (DirectNarration cannot - it lives outside the dialogue chain, so it always
    // trails). SkyrimNet voices an unvoiced INFO's response text verbatim:
    //
    //   TopicInfoEventHandler: On-demand: Generating TTS for silent response: '<text>'
    //
    // so the generated line has to BE that text - and an INFO's text is baked into
    // the plugin, which Papyrus cannot touch.
    //
    // WHY A HOOK, AND NOT A PLAIN WRITE. Three approaches were tried in-game and
    // the first two are dead ends, recorded so they are not retried:
    //
    //  1. Resolve the INFO by FormID and write its response text.
    //     Dead: a dialogue INFO is NOT in the global form table. Papyrus's
    //     GetFormFromFile(0x803, "...esp") and C++ LookupForm(0x803, "...esp")
    //     both returned nothing, in a session where a Book (0x801) and a Global
    //     (0x802) in that same plugin resolved fine.
    //
    //  2. Reach it through its parent DIAL topic's topicInfos array, then write.
    //     The lookup works, but only once the topic is in use - called 29 s early
    //     the array is still null ("not populated yet (array=null, count=0)").
    //     Worse, even when the write reported success the value did not stick:
    //         wrote:     A nervous-looking Imperial merchant handed me this...
    //         read back: A grey-haired Breton woman handed this over near...
    //     TESTopicInfo owns no response list; GetResponseList BUILDS one per call,
    //     so the write landed in a throwaway.
    //
    //  3. Hook GetResponseList and rewrite the text on the way out. That is this.
    //     Every consumer - SkyrimNet's TTS read, the engine's own playback, the
    //     subtitle - goes through it, so they all see the generated line, and it
    //     needs neither the form table nor a populated topic.

    // One slot per describable item in a delivery. A delivery can carry several
    // unvoiced items and the courier gets a line for each, so several INFOs are
    // armed at once - matching MAX_ITEMS in CDN_Courier_QuestScript.
    constexpr std::size_t kMaxOverrides = 4;

    // The armed FormIDs are atomic so the hook - which runs for EVERY line of
    // dialogue in the game - costs kMaxOverrides relaxed loads in the
    // overwhelmingly common case where nothing is armed, and only takes the lock
    // for one of our own INFOs.
    std::atomic<RE::FormID> g_overrideFormID[kMaxOverrides]{};
    std::mutex              g_textLock;
    RE::BSFixedString       g_overrideText[kMaxOverrides];

    using GetResponseList_t = RE::TESTopicInfo::TESResponseList* (*)(RE::TESTopicInfo*,
                                                                    RE::TESTopicInfo::TESResponseList*);
    GetResponseList_t g_origGetResponseList = nullptr;

    RE::TESTopicInfo::TESResponseList* GetResponseList_Hook(RE::TESTopicInfo*                  a_this,
                                                            RE::TESTopicInfo::TESResponseList* a_list)
    {
        auto* result = g_origGetResponseList(a_this, a_list);

        if (!a_this || !result || !result->head)
            return result;

        // An empty slot holds 0, so a form with no ID must never reach the scan.
        const auto id = a_this->GetFormID();
        if (id == 0)
            return result;

        for (std::size_t i = 0; i < kMaxOverrides; ++i) {
            if (g_overrideFormID[i].load(std::memory_order_relaxed) != id)
                continue;
            std::scoped_lock lock(g_textLock);
            // Re-checked under the lock: SetTopicInfoText may have disarmed this
            // slot between the load above and the lock being taken.
            if (g_overrideFormID[i].load(std::memory_order_relaxed) == id)
                result->head->responseText = g_overrideText[i];
            break;
        }
        return result;
    }

    // Resolves plugin + local FormID to a full FormID WITHOUT needing the form to
    // exist in the lookup table - LookupFormID only consults the load order, so it
    // works for an INFO and works before the topic is populated.
    RE::FormID ResolveFormID(std::string_view a_plugin, std::uint32_t a_localFormID)
    {
        auto* handler = RE::TESDataHandler::GetSingleton();
        if (!handler) {
            logs::warn("CDNBookText: no TESDataHandler"sv);
            return 0;
        }
        auto full = handler->LookupFormID(static_cast<RE::FormID>(a_localFormID), a_plugin);
        if (full == 0) {
            logs::warn("CDNBookText: '{}' is not in the load order (local {:06X})"sv, a_plugin, a_localFormID);
        }
        return full;
    }

    // Arms the override. The text is applied to that INFO from here on, every time
    // the engine asks for its responses. Pass an empty string to disarm.
    bool SetTopicInfoText(RE::StaticFunctionTag*, RE::BSFixedString a_plugin,
                                        std::uint32_t a_localFormID, RE::BSFixedString a_text)
    {
        const auto full = ResolveFormID(a_plugin.c_str() ? a_plugin.c_str() : "", a_localFormID);
        if (full == 0)
            return false;

        const bool clearing = !a_text.c_str() || a_text.empty();

        {
            std::scoped_lock lock(g_textLock);

            // Re-arm this INFO's existing slot if it has one, else take a free
            // one. Keying on the FormID rather than an index passed from Papyrus
            // keeps the two sides from having to agree on slot numbering.
            std::size_t slot     = kMaxOverrides;
            std::size_t freeSlot = kMaxOverrides;
            for (std::size_t i = 0; i < kMaxOverrides; ++i) {
                const auto cur = g_overrideFormID[i].load(std::memory_order_relaxed);
                if (cur == full) {
                    slot = i;
                    break;
                }
                if (cur == 0 && freeSlot == kMaxOverrides)
                    freeSlot = i;
            }
            if (slot == kMaxOverrides)
                slot = freeSlot;
            if (slot == kMaxOverrides) {
                logs::warn("CDNBookText: all {} override slots are armed; {:08X} refused"sv,
                           kMaxOverrides, full);
                return false;
            }

            // Text first, FormID second, both under the lock the hook re-checks
            // in - so the hook can never see an armed FormID paired with a stale
            // string.
            g_overrideText[slot] = clearing ? RE::BSFixedString("") : a_text;
            g_overrideFormID[slot].store(clearing ? 0 : full, std::memory_order_release);
        }

        if (clearing) {
            logs::info("CDNBookText: cleared override for {:08X}"sv, full);
        } else {
            logs::info("CDNBookText: override {:08X} <- '{}'"sv, full, a_text.c_str());
        }
        return true;
    }

    // Reads the INFO's response text back THROUGH the same path the engine uses,
    // so it verifies the hook rather than a private copy. Returns "" if the topic
    // is not populated yet, which is expected before the conversation starts.
    RE::BSFixedString GetTopicInfoText(RE::StaticFunctionTag*, RE::BSFixedString a_topicPlugin,
                                       std::uint32_t a_topicLocalID, RE::BSFixedString a_infoPlugin,
                                       std::uint32_t a_infoLocalID)
    {
        auto* handler = RE::TESDataHandler::GetSingleton();
        if (!handler)
            return RE::BSFixedString("");

        auto* topicForm = handler->LookupForm(static_cast<RE::FormID>(a_topicLocalID),
                                              a_topicPlugin.c_str() ? a_topicPlugin.c_str() : "");
        auto* topic = topicForm ? topicForm->As<RE::TESTopic>() : nullptr;
        if (!topic || !topic->topicInfos || topic->numTopicInfos == 0)
            return RE::BSFixedString("");

        const auto wanted = ResolveFormID(a_infoPlugin.c_str() ? a_infoPlugin.c_str() : "", a_infoLocalID);
        for (std::uint32_t i = 0; i < topic->numTopicInfos; ++i) {
            auto* info = topic->topicInfos[i];
            if (info && info->GetFormID() == wanted) {
                RE::TESTopicInfo::TESResponseList storage{};
                auto* list = info->GetResponseList(&storage);
                if (list && list->head)
                    return list->head->responseText;
                return RE::BSFixedString("");
            }
        }
        return RE::BSFixedString("");
    }
}

// ⚠️ DO NOT go back to SKSE::Trampoline here. The first version of this hook
// used trampoline.write_branch<5> on the function entry and CRASHED THE GAME
// (EXCEPTION_ACCESS_VIOLATION, "Tried to execute memory at 0x7FF6CCF3186A",
// CDNBookText.dll on the stack, ~3.5 min into load).
//
// The cause is not the address - RELOCATION_ID(25083, 25626) is exactly what
// CommonLibSSE's own TESTopicInfo::GetResponseList uses. It is that
// write_branch IS NOT AN ENTRY DETOUR. Read what it does:
//
//     const auto disp   = (std::int32_t*)(a_src + N - 4);
//     const auto nextOp = a_src + N;
//     const auto func   = isNop ? 0 : nextOp + *disp;   // <- "original" target
//     ... overwrite the 5 bytes at a_src ...
//     return func;
//
// It assumes a_src ALREADY HOLDS a 5-byte relative call/jmp - i.e. it patches a
// CALL SITE, reading that instruction's rel32 to recover the old target. It
// never relocates a prologue. Aimed at a function entry it reads the prologue's
// bytes as a displacement and hands back a fabricated address, which is the
// 0x7FF6CCF3186A the hook then called.
//
// CommonLibSSE ships no entry-detour facility, so this uses MinHook, which
// decodes the prologue's instruction lengths and builds a real trampoline.

void CDNBookText::InstallHooks()
{
    if (const auto status = MH_Initialize(); status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        logs::error("CDNBookText: MH_Initialize failed ({})"sv, MH_StatusToString(status));
        return;
    }

    REL::Relocation<std::uintptr_t> target{ RELOCATION_ID(25083, 25626) };  // TESTopicInfo::GetResponseList
    auto* const                     addr = reinterpret_cast<LPVOID>(target.address());

    // Logged so a future crash can be told apart from a mis-resolved address
    // without another guess - compare against the Address Library RVA.
    logs::info("CDNBookText: GetResponseList at {:016X} (base+{:X})"sv,
               target.address(), target.address() - REL::Module::get().base());

    if (const auto status = MH_CreateHook(addr, reinterpret_cast<LPVOID>(&GetResponseList_Hook),
                                          reinterpret_cast<LPVOID*>(&g_origGetResponseList));
        status != MH_OK) {
        logs::error("CDNBookText: MH_CreateHook failed ({}); text override is inert"sv,
                    MH_StatusToString(status));
        return;
    }

    if (const auto status = MH_EnableHook(addr); status != MH_OK) {
        logs::error("CDNBookText: MH_EnableHook failed ({}); text override is inert"sv,
                    MH_StatusToString(status));
        g_origGetResponseList = nullptr;
        return;
    }

    logs::info("CDNBookText: hooked TESTopicInfo::GetResponseList (trampoline {:016X})"sv,
               reinterpret_cast<std::uintptr_t>(g_origGetResponseList));
}

bool CDNBookText::BindPapyrus(RE::BSScript::IVirtualMachine* a_vm)
{
    a_vm->RegisterFunction("GetBookText", PapyrusClass, GetBookText);
    a_vm->RegisterFunction("SetTopicInfoText", PapyrusClass, SetTopicInfoText);
    a_vm->RegisterFunction("GetTopicInfoText", PapyrusClass, GetTopicInfoText);
    a_vm->RegisterFunction("FindSenderRef", PapyrusClass, FindSenderRef);
    a_vm->RegisterFunction("SenderNameFromTitle", PapyrusClass, SenderNameFromTitle);
    logs::info("CDNBookText: registered GetBookText, SetTopicInfoText, GetTopicInfoText, FindSenderRef, SenderNameFromTitle"sv);
    return true;
}
