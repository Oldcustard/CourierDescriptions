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
    // Every control character comes back as a space. Vanilla book text is full of
    // CR/LF, and ONE raw CR in the context JSON is fatal: SkyrimNet's parse fails
    // with "control character U+000D (CR) must be escaped", it then renders the
    // prompt with NO variables bound at all, and the model - handed a template
    // still reading {{ itemName }} - answers "Please provide the {{ itemName }},
    // ... so I can write the line." That reached an armed INFO on 2026-08-15.
    //
    // It has to be scrubbed HERE. Papyrus has no \r escape, so CleanForJSON
    // cannot even name the character it needs to drop; its \n and \t branches
    // caught the test letters (authored on one line) and nothing else. Bytes
    // >= 0x80 are left alone so UTF-8 survives.
    RE::BSFixedString GetBookText(RE::StaticFunctionTag*, RE::TESObjectBOOK* a_book)
    {
        if (!a_book)
            return RE::BSFixedString("");

        RE::BSString buf;
        a_book->GetDescription(buf, a_book);  // default fieldType 'CSED' == DESC == book text

        const char* str = buf.c_str();
        if (!str)
            return RE::BSFixedString("");

        std::string text{ str };
        for (auto& ch : text) {
            if (static_cast<unsigned char>(ch) < 0x20 || static_cast<unsigned char>(ch) == 0x7F)
                ch = ' ';
        }
        return RE::BSFixedString(text.c_str());
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
    // people share it and we refuse to guess - see BuildIndexes for why that
    // is NOT the same as two records sharing a name.
    struct SenderEntry
    {
        RE::FormID         id        = 0;  // the actor REFERENCE - what callers get
        RE::FormID         baseId    = 0;  // the base NPC_ record - what we rank on
        const RE::TESFile* file      = nullptr;  // the base record's defining plugin
        bool               ambiguous = false;
    };

    // ONE flag for the single form-table pass that feeds every index. Two flags
    // meant the pass could run twice, and worse, its result depended on which
    // native Papyrus happened to call first.
    std::once_flag                               g_indexOnce;
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

    // ONE pass over the form table, cached for the session, feeding the sender
    // name index and the place names. The table is ~500k forms and walking it is
    // the single most expensive thing this DLL does, so it happens exactly once.
    //
    // ⛔ The voiced-item scan is NOT done here. It was, for one build: matching
    // every INFO's parentTopic against WICourierDeliveries. The table held 143194
    // INFO forms, every one of them with a valid parent topic, and NONE of them
    // under 00039F6C - not even our own four, which the courier had spoken from
    // minutes earlier. INFOs are created with their topic, not at data load, and
    // that topic had not been loaded yet. See EnsureVoicedScan.
    //
    // Called from ArmDelivery, which runs when the delivery is first QUEUED -
    // typically minutes before the courier reaches the player - so the cost is
    // invisible. Do NOT call this from the 1.5 s armed poll or the closing-line
    // backstop.
    void BuildIndexes()
    {
        const auto& [map, lock] = RE::TESForm::GetAllForms();
        if (!map) {
            logs::warn("CDNBookText: no form table; sender lookup disabled"sv);
            return;
        }

        std::size_t scanned   = 0;
        std::size_t ambiguous = 0;
        std::size_t merged    = 0;

        // Scoped: naming a form in the report below re-enters the form table, so
        // the read lock must be gone before it runs.
        {
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

        }  // read lock released

        logs::info("CDNBookText: sender index built - {} unique-named actors, {} ambiguous, {} same-plugin variants merged, {} place names"sv,
                   scanned, ambiguous, merged, g_placeNames.size());
    }

    void EnsureIndexes()
    {
        std::call_once(g_indexOnce, BuildIndexes);
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
        EnsureIndexes();  // NameFromTitle needs g_placeNames
        const std::string_view title = a_title.c_str() ? a_title.c_str() : "";
        return RE::BSFixedString(NameFromTitle(title).c_str());
    }

    // Best-effort. 0 means "no idea", and the prompt degrades to describing an
    // anonymous stranger exactly as it did before this existed.
    std::int32_t FindSenderRef(RE::StaticFunctionTag*, RE::BSFixedString a_title, RE::TESObjectBOOK* a_book)
    {
        EnsureIndexes();
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

    // ---- Which items does the courier ALREADY have a voiced line for? --------
    //
    // We must not describe an item vanilla already describes - the player would
    // hear who sent it twice, once in the courier's recorded voice and once in
    // ours, and the recorded one is right.
    //
    // This was a hardcoded list of nine Skyrim.esm letters in Papyrus. That list
    // is only correct on a load order where nothing but Skyrim.esm touches the
    // courier, which is not a load order anyone plays. WICourierDeliveries is a
    // standard extension point and things pile into it: Hearthfires adds five
    // letters, Dragonborn four, and quest mods however many they like. On the dev
    // machine's own order the shipped list was missing NINETEEN items and wrong
    // about a twentieth - TCBM.esp repoints 023EE1's GetItemCount away from
    // WICastMagic04Letter to one of its own notes, so the letter the list names
    // is no longer the letter that INFO is about.
    //
    // So read it out of the data instead. Any INFO with a line about a specific
    // item is conditioned on the player receiving that item, which makes the
    // answer mechanical: walk the topic's INFOs, take the parameter of every
    // GetItemCount condition, and that set IS the list. It costs one pass at
    // first use and is then a hash lookup.
    //
    // OR-chains need no special handling. A condition block like 071440's -
    // five ORed GetItemCounts covering two vanilla letters and three from mods -
    // means the INFO speaks for any one of them, so every arm belongs in the set;
    // collecting them all is exactly right, not an approximation.

    constexpr std::uint32_t kDeliveriesTopic = 0x039F6C;  // WICourierDeliveries

    // The shipped list, kept as a FLOOR under the scan rather than replaced by
    // it. Two jobs:
    //
    //  1. It covers the window BEFORE the topic is loaded, which is every check
    //     made at ArmDelivery. Anything on this list costs no LLM call at all;
    //     anything not on it and voiced anyway costs one wasted generation and is
    //     then dropped by the second check at the greeting.
    //  2. The scan only sees items named by a CONDITION. If a line ever gates on
    //     its item some other way - a script fragment property, a quest stage -
    //     the scan cannot see it, and the floor stops that becoming a regression
    //     against 1.0.1.
    //
    // ⚠️ EVERY ENTRY IS CONDITION-DERIVED, not remembered. Each one is the
    // GetItemCount parameter of a real INFO under WICourierDeliveries, read out of
    // the plugin. A guessed entry here is worse than a missing one: a false
    // positive makes the courier say NOTHING about a letter he has no line for,
    // and unlike a duplicate it is invisible in testing.
    //
    // The DLC entries were added because most players have the DLC, so leaving
    // them out meant a wasted LLM call on the commonest voiced deliveries there
    // are. Verified identical in vanilla and under USSEP, which overrides four of
    // these INFOs without touching their item conditions. A plugin that is not
    // installed simply resolves to nothing.
    struct FallbackVoicedItem
    {
        std::string_view plugin;
        std::uint32_t    localID;
    };

    constexpr FallbackVoicedItem kFallbackVoicedItems[] = {
        { "Skyrim.esm"sv, 0x05224A },  // DBEntranceLetter             - Mysterious Note
        { "Skyrim.esm"sv, 0x0249AF },  // MS12QuintusLetter
        { "Skyrim.esm"sv, 0x01BFF5 },  // WIKill03InheritanceLetter
        { "Skyrim.esm"sv, 0x023EE5 },  // WICastMagic04Letter          - Letter from a Friend
        { "Skyrim.esm"sv, 0x0A0F46 },  // WIAddItem07Note              - Letter from Calcelmo
        { "Skyrim.esm"sv, 0x0D91D1 },  // MS06FalkFirebeardNote
        { "Skyrim.esm"sv, 0x094D8B },  // DA07MythicDawnMuseumPamphlet
        { "Skyrim.esm"sv, 0x071442 },  // WIKill04RewardLetter
        { "Skyrim.esm"sv, 0x071443 },  // WIKill04ThanksLetter

        // HearthFires - INFOs 003DE5 (2 items), 003DE6, 01770F (2 items)
        { "HearthFires.esm"sv, 0x003F78 },  // BYOHAdoptionStewardCourierNote - "<Alias=Home> Furnishings"
        { "HearthFires.esm"sv, 0x0030A1 },  // BYOHHouseJarlFriendLetter      - from the Steward of <HoldCity>
        { "HearthFires.esm"sv, 0x003F7C },  // BYOHAdoptionConstanceLetter    - "Consider Adoption"
        { "HearthFires.esm"sv, 0x016130 },  // BYOHHouseJarlIntroLetter
        { "HearthFires.esm"sv, 0x010BEF },  // BYOHHouseJarlFriendLetter2

        // Dragonborn - INFOs 036603..036606, one per part
        { "Dragonborn.esm"sv, 0x0365FF },  // DLC2dunKolbjornCourierNote01 - Ralis Sedarys, Part 1
        { "Dragonborn.esm"sv, 0x036600 },  // DLC2dunKolbjornCourierNote02 - Part 2
        { "Dragonborn.esm"sv, 0x036601 },  // DLC2dunKolbjornCourierNote03 - Part 3
        { "Dragonborn.esm"sv, 0x036602 },  // DLC2dunKolbjornCourierNote04 - Part 4
    };

    std::unordered_set<RE::FormID> g_voicedItems;

    // A FormList parameter contributes its MEMBERS, not itself: the condition is
    // satisfied by holding any one of them, so each one is spoken for. Depth
    // limited because a FormList can hold FormLists and nothing prevents a cycle.
    void AddVoicedItem(RE::TESForm* a_form, int a_depth)
    {
        if (!a_form || a_depth > 4)
            return;

        if (a_form->Is(RE::FormType::FormList)) {
            auto* list = a_form->As<RE::BGSListForm>();
            if (!list)
                return;
            for (auto* entry : list->forms)
                AddVoicedItem(entry, a_depth + 1);
            if (list->scriptAddedTempForms) {
                for (const auto id : *list->scriptAddedTempForms)
                    AddVoicedItem(RE::TESForm::LookupByID(id), a_depth + 1);
            }
            return;
        }

        g_voicedItems.insert(a_form->GetFormID());
    }

    // Every GetItemCount parameter in one INFO's condition block. Returns how many
    // such conditions it saw, which is the only number that tells a topic with no
    // item-specific lines apart from one we failed to read.
    std::uint32_t ScanInfoConditions(RE::TESTopicInfo* a_info)
    {
        std::uint32_t hits = 0;
        for (auto* cond = a_info->objConditions.head; cond; cond = cond->next) {
            if (cond->data.functionData.function != RE::FUNCTION_DATA::FunctionID::kGetItemCount)
                continue;
            // With this flag the parameter slot holds an alias INDEX, not a form
            // pointer. Dereferencing it would be reading an integer as an address.
            if (cond->data.flags.usesAliases)
                continue;
            ++hits;
            AddVoicedItem(static_cast<RE::TESForm*>(cond->data.functionData.params[0]), 0);
        }
        return hits;
    }

    std::size_t g_voicedFromList = 0;

    void FinishVoicedScan(std::uint32_t a_infos, std::uint32_t a_hits, std::size_t a_added);

    // Seeds the floor once, then reads the topic's OWN info array - the only route
    // that has ever worked.
    //
    // ⚠️ THE ANSWER IS ONLY AVAILABLE WHILE THE CONVERSATION IS LIVE, so this
    // RETRIES rather than caching a failure. TESTopic::topicInfos is null until the
    // engine loads the topic, and the INFO forms themselves do not exist before
    // that either: a form-table pass at ArmDelivery walked 143194 INFOs, every one
    // with a valid parent topic, and found NOT ONE under WICourierDeliveries -
    // including our own four, which the same session went on to speak. So there is
    // no route that answers early, and the shipped floor is what covers the gap
    // until the greeting, when the array is populated and this returns the real
    // answer. Papyrus asks AGAIN at publish time for exactly that reason.
    //
    // ⛔ AND IT DOES NOT LATCH ON THE FIRST SUCCESS EITHER. The array is filled
    // INCREMENTALLY, so an early scan returns a real but PARTIAL answer, and
    // caching that is the same silent failure as caching a miss. Measured
    // 2026-08-15, one load order, two runs:
    //
    //   20:10:31  scan on attempt 6:  25 INFOs, 29 conditions -> 28 items
    //   20:39:58  scan on attempt 29: 19 INFOs, 24 conditions -> 24 items
    //
    // The second is a strict SUBSET of the first, and what it lost says what
    // happened: every item it found is from plugin index <= 1D, and all four it
    // missed are >= B1 (Suvaris, Mr. Wolf, Cassia, The Northern Special). That is
    // a load-order PREFIX - the engine appends to this array as it works down the
    // order, and the scan caught it mid-fill. Nothing about the caller said so:
    // topicInfos was non-null and numTopicInfos was a perfectly plausible 19.
    //
    // So a pass runs whenever numTopicInfos has CHANGED since the last one, and
    // the results are UNIONED - the set only ever grows, and every entry in it
    // was read from a condition on some pass. Latching would have silenced four
    // mod-added letters the courier voices himself, on a load order where the
    // scan appeared to work perfectly.
    //
    // Rescanning is cheap: a null check until the array exists, then ~25 INFOs'
    // conditions on the handful of passes where the count moves. Nothing here
    // walks the form table.
    bool          g_voicedScanAbandoned = false;  // permanent failure; retrying cannot help
    int           g_voicedScanAttempts  = 0;      // calls that got as far as looking
    int           g_voicedScanPasses    = 0;      // calls that actually read the array
    std::uint32_t g_voicedScannedInfos  = 0;      // numTopicInfos as of the last pass

    void EnsureVoicedScan()
    {
        if (g_voicedScanAbandoned)
            return;

        auto* handler = RE::TESDataHandler::GetSingleton();
        if (!handler) {
            logs::error("CDNBookText: no TESDataHandler; voiced-item set is EMPTY"sv);
            return;
        }

        if (g_voicedScanAttempts++ == 0) {
            for (const auto& [plugin, local] : kFallbackVoicedItems) {
                if (auto* form = handler->LookupForm(static_cast<RE::FormID>(local), plugin))
                    g_voicedItems.insert(form->GetFormID());
            }
            g_voicedFromList = g_voicedItems.size();
            logs::info("CDNBookText: shipped floor resolved {} of {} items "
                       "(the rest are from DLC this order does not have)"sv,
                       g_voicedFromList, std::size(kFallbackVoicedItems));
        }

        // Each failure below gets its OWN line. They were one collapsed message
        // once, and it cost a round trip to find out which of them had fired.
        const auto topicID = handler->LookupFormID(static_cast<RE::FormID>(kDeliveriesTopic), "Skyrim.esm");
        if (topicID == 0) {
            logs::error("CDNBookText: WICourierDeliveries ({:06X}:Skyrim.esm) is not in this "
                        "load order - voiced-item set is the shipped list only ({} items)"sv,
                        kDeliveriesTopic, g_voicedFromList);
            g_voicedScanAbandoned = true;  // no load order change can fix this mid-session
            return;
        }

        auto* topicForm = RE::TESForm::LookupByID(topicID);
        if (!topicForm) {
            logs::error("CDNBookText: WICourierDeliveries resolves to {:08X} but that form does "
                        "not exist"sv,
                        topicID);
            return;
        }
        auto* topic = topicForm->As<RE::TESTopic>();
        if (!topic) {
            logs::error("CDNBookText: {:08X} is a {}, not a Dialogue topic"sv,
                        topicID, static_cast<std::uint32_t>(topicForm->GetFormType()));
            g_voicedScanAbandoned = true;
            return;
        }

        if (!topic->topicInfos || topic->numTopicInfos == 0) {
            // Expected before the courier speaks, and NOT an error - the floor is
            // in force and the next caller tries again. Logged only the first few
            // times: the armed poll asks per item, per delivery.
            if (g_voicedScanAttempts <= 3) {
                logs::info("CDNBookText: WICourierDeliveries not loaded yet (topicInfos {}, "
                           "numTopicInfos {}) - shipped list of {} stands, will retry"sv,
                           topic->topicInfos ? "set" : "null", topic->numTopicInfos,
                           g_voicedFromList);
            }
            return;
        }

        // Nothing has been added to the array since the last pass, so a rescan
        // would read exactly what it read before. This is the ONLY thing standing
        // between the armed poll (which asks per item, per 1.5 s) and a rescan on
        // every call - not a cache of the answer, a cache of the array's size.
        if (topic->numTopicInfos == g_voicedScannedInfos)
            return;

        const auto    before = g_voicedItems.size();
        std::uint32_t infos  = 0;
        std::uint32_t hits   = 0;
        for (std::uint32_t i = 0; i < topic->numTopicInfos; ++i) {
            if (auto* info = topic->topicInfos[i]) {
                ++infos;
                hits += ScanInfoConditions(info);
            }
        }
        g_voicedScannedInfos = topic->numTopicInfos;
        ++g_voicedScanPasses;
        FinishVoicedScan(infos, hits, g_voicedItems.size() - before);
    }

    void FinishVoicedScan(std::uint32_t a_infos, std::uint32_t a_hits, std::size_t a_added)
    {
        logs::info("CDNBookText: voiced-delivery scan pass {} (attempt {}): {} INFOs, {} "
                   "GetItemCount conditions -> {} items, {} new ({} of them from the shipped "
                   "list)"sv,
                   g_voicedScanPasses, g_voicedScanAttempts, a_infos, a_hits,
                   g_voicedItems.size(), a_added, g_voicedFromList);

        // A later pass that found the array grown but the item set unchanged has
        // nothing to itemise, and repeating 25 identical lines would bury the pass
        // that DID find something. The FIRST pass always prints, added or not, so
        // the guarantee below holds even on an order whose whole set is the floor.
        if (a_added == 0 && g_voicedScanPasses > 1)
            return;

        // Logged in full whenever it changes. This set decides which deliveries
        // stay silent, so when a report says "he said nothing about my letter"
        // the answer is either in this list or it is not, and no second round
        // trip is needed to find out. Sorted so two passes can be diffed.
        std::vector<RE::FormID> sorted(g_voicedItems.begin(), g_voicedItems.end());
        std::sort(sorted.begin(), sorted.end());
        for (const auto id : sorted) {
            auto*       form = RE::TESForm::LookupByID(id);
            const char* name = form ? form->GetName() : nullptr;
            logs::info("CDNBookText:   voiced {:08X}  {}"sv, id, (name && *name) ? name : "<no name>");
        }
    }

    const std::unordered_set<RE::FormID>& VoicedItems()
    {
        EnsureVoicedScan();
        return g_voicedItems;
    }

    // True when the courier already has his own line about this item. Deliberately
    // a predicate and not a Form[] getter: the array never crosses into Papyrus,
    // so there is no array to be held in a member variable and poisoned by a save.
    bool IsVoicedDeliveryItem(RE::StaticFunctionTag*, RE::TESForm* a_item)
    {
        if (!a_item)
            return false;
        const auto& voiced = VoicedItems();
        return voiced.find(a_item->GetFormID()) != voiced.end();
    }

    // For the Papyrus log line only. At ArmDelivery this is normally the shipped
    // floor and nothing more - the real set only becomes readable at the greeting,
    // so read this number again after the delivery, not before it.
    std::int32_t GetVoicedDeliveryItemCount(RE::StaticFunctionTag*)
    {
        return static_cast<std::int32_t>(VoicedItems().size());
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
//
// ⚠️ AND THIS MUST INSTALL LATE - Dynamic String Distributor HOOKS THE SAME
// FUNCTION. DSD's Hooks.cpp installs GetResponseListHook on the identical
// RELOCATION_ID(25083, 25626), from kPostPostLoad, with its own
// hook_function_prologue<T, 6>, whose "trampoline" is a VERBATIM byte copy:
//
//     for (size_t i = 0; i < a_originalByteLength; ++i)
//         db(*reinterpret_cast<std::uint8_t*>(a_originalFuncAddr + i));
//     jmp(ptr[rip]); dq(a_originalFuncAddr + a_originalByteLength);
//
// No relocation. So whichever of the two installs FIRST decides who crashes:
// we used to install from SKSEPlugin_Load, which is strictly before
// kPostPostLoad, so the 6 bytes DSD copied were OUR 5-byte `E9 rel32` plus a
// leftover. That displacement is relative to the function entry; executed from
// DSD's buffer it lands in unmapped memory, and every line of dialogue in the
// game goes through here:
//
//     Tried to execute memory at 0x7FF6FECC589B
//     [1] DynamicStringDistributor.dll ... Hooks.cpp:108 GetResponseListHook::thunk
//
// Two user crash reports, ~60 s into load, both this. (DSD 1.2.5 - the dev
// load order's version - has no GetResponseListHook at all, which is why this
// never showed up here; it arrived in 1.4.x.)
//
// Installing at kDataLoaded puts us AFTER DSD, and then it composes, because
// MinHook does what DSD does not: a leading `E9 rel32` is rewritten as a
// JMP_ABS to its true destination (trampoline.c, `(hs.opcode & 0xFD) == 0xE9`).
// So DSD copies the pristine prologue, our trampoline becomes an absolute jump
// to DSD's thunk, and the chain is entry -> us -> DSD -> original.
//
// DO NOT move this back into SKSEPlugin_Load. The residual risk is only a mod
// that byte-copies a prologue at kDataLoaded or later - none known.

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

    // The prologue as we found it. A leading E9/EB/FF25 means another plugin
    // detoured this function before us - the single fact the two crash reports
    // of 2026-08-12 could not supply, and the one that identifies the mod we
    // are layering over.
    {
        const auto* p = reinterpret_cast<const std::uint8_t*>(target.address());
        logs::info("CDNBookText: prologue {:02X} {:02X} {:02X} {:02X} {:02X} {:02X}{}"sv,
                   p[0], p[1], p[2], p[3], p[4], p[5],
                   (p[0] == 0xE9 || p[0] == 0xEB || (p[0] == 0xFF && p[1] == 0x25))
                       ? "  <- ALREADY DETOURED by another plugin"sv
                       : ""sv);
    }

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
    a_vm->RegisterFunction("IsVoicedDeliveryItem", PapyrusClass, IsVoicedDeliveryItem);
    a_vm->RegisterFunction("GetVoicedDeliveryItemCount", PapyrusClass, GetVoicedDeliveryItemCount);
    logs::info("CDNBookText: registered GetBookText, SetTopicInfoText, GetTopicInfoText, FindSenderRef, "
               "SenderNameFromTitle, IsVoicedDeliveryItem, GetVoicedDeliveryItemCount"sv);
    return true;
}
