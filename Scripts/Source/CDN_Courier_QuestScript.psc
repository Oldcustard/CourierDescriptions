Scriptname CDN_Courier_QuestScript Extends Quest

; =============================================================================
; Courier Descriptions  -  SkyrimNet plugin  (quest script)
;
; When the vanilla courier delivers items he has no "who sent it" voiceline for,
; he remarks on who handed them to him, in his own voice, during the delivery.
;
; HOW THE LINE IS PRODUCED  (this is the part that is easy to get wrong)
; ---------------------------------------------------------------------
; SkyrimNet has no API that makes an NPC speak a string verbatim - TriggerPlayerTTS
; is player-only, and RegisterDialogue only records that speech happened, without
; voicing it. SkyrimNetApi.DirectNarration registers a FACT that the originator
; actor then RESPONDS to. So we must not hand it a finished line: doing that makes
; the courier answer our line instead of saying it.
;
; Therefore the split:
;   * our LLM prompt ("courier_sender_line") returns a FACT - a third-person
;     description of the person who handed the courier the item, inferred from the
;     letter's text;
;   * DirectNarration(fact, courier, player) hands that fact to SkyrimNet, which
;     generates and voices the courier's own line about it.
; The letter's body text NEVER reaches the narration - only the sender description
; derived from it - which is what keeps the courier from quoting mail he hasn't read.
;
; TIMING
; ------
; The fact is generated EARLY: a poll notices WICourierItemCount rising (the moment
; some quest queues a delivery, usually long before the courier finds the player)
; and pre-generates then, so the fact is cached and ready. The narration is fired
; the moment the courier enters dialogue with the player - i.e. at the greeting -
; so SkyrimNet's own generation and TTS overlap the vanilla delivery lines instead
; of trailing the closing "Looks like that's it."
;
; The overridden closing fragment (TIF__00039F81) remains only as a BACKSTOP: if
; the conversation was never caught by the poll, it fires the narration before the
; handover rather than losing it.
;
; Requires: SkyrimNet (SkyrimNetApi), vanilla WICourier (WICourierScript).
; =============================================================================

WICourierScript wiQuest = None
Int WICOURIER_FORMID = 0x00039F82          ; WICourier quest (verified in this load order)
Int TEST_LETTER_FORMID = 0x00000801        ; CDN_TestLetter (test trigger)
Int DEBUG_DELIVERY_GV_FORMID = 0x00000802  ; CDN_DebugDelivery (console test trigger)
Int PROBE_INFO_FORMID = 0x00000803         ; CDN_CourierProbeInfo (the first spoken INFO)
Int LINE_READY_GV_FORMID = 0x00000806      ; CDN_LineReady - gates those INFOs' conditions
String PLUGIN_NAME = "CDN_CourierDescriptions.esp"
Int COURIER_TOPIC_FORMID = 0x00039F6C      ; WICourierDeliveries - the INFO's parent topic
String COURIER_TOPIC_PLUGIN = "Skyrim.esm"

Float POLL_IDLE  = 10.0   ; watching for a delivery to be queued
Float POLL_ARMED = 1.5    ; delivery queued; watching for the courier to start talking

Int MAX_ITEMS = 4         ; per delivery; extras are described generically

; --- current delivery -------------------------------------------------------
; Slot i is the item dItem<i> described by the fact dFact<i>. A fact of "" is not
; generated yet; genIndex is the slot the LLM is working towards.
;
; ⚠️ These are SCALARS ON PURPOSE - see the comment on GetDeliveryItem. Do not
; "tidy" them back into a Form[]/String[] pair, however much it wants to be one.
Form dItem0
Form dItem1
Form dItem2
Form dItem3
String dFact0
String dFact1
String dFact2
String dFact3
; Actor REFERENCE FormID of whoever sent slot i's item, or 0 if we could not work
; it out. Resolved once at scan time (see ScanContainer) and handed to the prompt,
; which turns it into a SkyrimNet bio via formid_to_uuid/render_character_profile.
Int dSender0 = 0
Int dSender1 = 0
Int dSender2 = 0
Int dSender3 = 0
; The sender's name as written on slot i's LABEL, or "". The only name the
; courier may speak without knowing the person - see rule 1a in the prompt, which
; branches on it. Kept apart from dSenderN because a title name is speakable even
; when the sender never resolved to an actor.
String dTitleName0
String dTitleName1
String dTitleName2
String dTitleName3

Int pendingCount = 0
Int genIndex = 0
Bool bAwaitingLLM = false
Bool bArmed = false        ; the delivery slots reflect a queued delivery
Bool bNarrated = false     ; the narration for this delivery has gone out
Bool bWarmedTTS = false    ; PrepareNPCDialogue already asked for this delivery
Bool bLinePublished = false ; the line is in the INFO the courier speaks
Actor currentCourier = None

; =============================================================================
; Init
; =============================================================================

Event OnInit()
    RegisterForSingleUpdate(POLL_IDLE)
    if GetDebugMode()
        Debug.Notification("[CDN] Debug on. In the console: 'set CDN_DebugDelivery to 1' to send a test courier.")
    endIf
    DBG("OnInit complete")
EndEvent

; =============================================================================
; ⚠️ THIS SCRIPT KEEPS NO ARRAYS IN MEMBER VARIABLES. THAT IS THE WHOLE POINT.
;
; A member array on a quest script is a permanent one-way trap. If ANY save is
; ever written while the array is None, the saved slot loses its array type, and
; from then on `new Form[8]` against that variable fails FOREVER in that save
; line - every load, every playthrough - with:
;
;     Error: Cannot create an array into a non-array variable
;     Error: Cannot cast from None to Form[]
;
; Proven by diagnostic (run 2026-08-10 23:25). In the SAME function, on the SAME
; script, on the same tick:
;
;     [CDN] ARRAY DIAG: local `new Form[4]` OK, length 4
;     Error: Cannot create an array into a non-array variable
;     [CDN] ARRAY DIAG: member deliveryItems is None after `new Form[8]`
;
; A LOCAL array is allocated from the .pex alone and works fine. The MEMBER, which
; is restored from save data, cannot be created at all. The .pex is not at fault -
; decompiling the deployed file shows `Form[] deliveryItems` declared correctly.
;
; RENAMING DOES NOT FIX IT, and that misdiagnosis cost hours. Fresh names buy
; exactly one session: the moment the game autosaves with the new array still
; None, the new name is poisoned too, and the next load is back to square one.
; `if x == None` cannot repair it either - the variable is not an array to begin
; with, so there is nothing for `new` to write into.
;
; How it started here: Utility.CreateFormArray returned None (see ArmDelivery),
; that None got saved, and every build since inherited a dead slot.
;
; So: delivery state lives in SCALAR members with accessor functions, and any
; genuinely array-shaped work uses LOCALS built fresh inside the function. It is
; less pretty and it is the only shape that survives a save.
; =============================================================================

Form Function GetDeliveryItem(Int i)
    if i == 0
        return dItem0
    elseif i == 1
        return dItem1
    elseif i == 2
        return dItem2
    elseif i == 3
        return dItem3
    endIf
    return None
EndFunction

Function SetDeliveryItem(Int i, Form f)
    if i == 0
        dItem0 = f
    elseif i == 1
        dItem1 = f
    elseif i == 2
        dItem2 = f
    elseif i == 3
        dItem3 = f
    endIf
EndFunction

String Function GetDeliveryFact(Int i)
    if i == 0
        return dFact0
    elseif i == 1
        return dFact1
    elseif i == 2
        return dFact2
    elseif i == 3
        return dFact3
    endIf
    return ""
EndFunction

Function SetDeliveryFact(Int i, String s)
    if i == 0
        dFact0 = s
    elseif i == 1
        dFact1 = s
    elseif i == 2
        dFact2 = s
    elseif i == 3
        dFact3 = s
    endIf
EndFunction

Int Function GetDeliverySender(Int i)
    if i == 0
        return dSender0
    elseif i == 1
        return dSender1
    elseif i == 2
        return dSender2
    elseif i == 3
        return dSender3
    endIf
    return 0
EndFunction

Function SetDeliverySender(Int i, Int formId)
    if i == 0
        dSender0 = formId
    elseif i == 1
        dSender1 = formId
    elseif i == 2
        dSender2 = formId
    elseif i == 3
        dSender3 = formId
    endIf
EndFunction

String Function GetDeliveryTitleName(Int i)
    if i == 0
        return dTitleName0
    elseif i == 1
        return dTitleName1
    elseif i == 2
        return dTitleName2
    elseif i == 3
        return dTitleName3
    endIf
    return ""
EndFunction

Function SetDeliveryTitleName(Int i, String senderName)
    if i == 0
        dTitleName0 = senderName
    elseif i == 1
        dTitleName1 = senderName
    elseif i == 2
        dTitleName2 = senderName
    elseif i == 3
        dTitleName3 = senderName
    endIf
EndFunction

; =============================================================================
; Poll - pre-generate at queue time, narrate when the conversation starts
; =============================================================================

Event OnUpdate()
    if GetDebugMode()
        CheckDebugTrigger()
    endIf
    RegisterForSingleUpdate(PollDelivery())
EndEvent

; Returns the delay until the next poll.
Float Function PollDelivery()
    WICourierScript wi = GetWICourier()
    if wi == None || wi.pWICourierItemCount == None
        return POLL_IDLE
    endIf

    if wi.pWICourierItemCount.GetValue() < 1.0
        ; Nothing queued: the previous delivery (if any) is over and done.
        if bArmed
            DBG("delivery finished - disarming")
            ClearDelivery()
        elseIf IsLineReady()
            ; Self-heal. The gate is a save-persisted global but the DLL's text
            ; override is memory-only, so a save taken mid-delivery reloads with
            ; the condition OPEN and no line behind it - which would put the
            ; INFOs' authored placeholder text in the courier's mouth. Nothing is
            ; queued here, so the gate has no business being open.
            DBG("stale CDN_LineReady found with no delivery - closing it")
            SetLineReady(0)
        endIf
        return POLL_IDLE
    endIf

    if !bArmed
        ArmDelivery(wi)
        return POLL_ARMED
    endIf

    if bNarrated
        return POLL_IDLE    ; he is already speaking; nothing left to watch for
    endIf

    ; The courier's container keeps filling after the delivery is first queued, so
    ; this has to run on every armed poll - see RescanDelivery.
    RescanDelivery(wi)

    if pendingCount == 0
        ; Nothing of ours in this delivery yet (everything in it is vanilla-voiced).
        ; Still worth looking again: another quest may queue something.
        return POLL_IDLE
    endIf

    ; Armed. Fire as soon as the courier starts talking, so SkyrimNet's own
    ; generation + TTS run alongside the vanilla delivery lines.
    Actor courier = currentCourier
    if courier != None && courier.IsInDialogueWithPlayer()
        ; The topic's info array only exists once the topic is in use, so the
        ; early attempt at fact-ready time finds nothing to write to. This is the
        ; one that lands - and it is still ~6 s ahead of our INFO firing.
        ;
        ; Re-arm unconditionally rather than trusting the earlier publish: the
        ; DLL's override is memory-only, so if the player saved after the line was
        ; generated and reloaded before the courier arrived, bLinePublished is
        ; still true from the save while the DLL holds nothing.
        bLinePublished = false
        PublishLineToInfo()
        VerifyPublishedLine()
        WarmSilentTTS()
        ; The INFO speaks the line from here. Nothing else may make a sound
        ; during the delivery - see NarrateDelivery for why DirectNarration is
        ; no longer called.
        bNarrated = true
        DBG("courier is in dialogue - INFO armed, leaving the line to DBVO")
        return POLL_IDLE
    endIf
    return POLL_ARMED
EndFunction

; =============================================================================
; Pacing - why there is no hold here
;
; The engine advances a dialogue chain on the CURRENT line's audio duration, and
; an unvoiced line's duration has nothing to do with how long the generated
; speech will be. Measured: the line fired at 21:46:50.48 and its audio only
; began at 21:46:54.93, so the vanilla closing line started 1.2 s before ours
; finished - and SkyrimNet then logged "actor silenced (TTS playback in
; progress)", losing the closing line outright.
;
; Nothing in SkyrimNet holds this path: its menu-blocking loop is player-click
; only, and PrepareNPCDialogue refuses to pre-generate here (see WarmSilentTTS).
;
; An adaptive hold WAS built - a CDN_Speaking global gating a self-relinking
; spacer INFO - and it is gone because it cannot be driven. Both Papyrus signals
; are blind to this audio: across a whole delivery GetSpeechQueueSize() returned
; 0 on every call while the generated line was audibly playing, and
; GetTimeSinceLastAudioEnded() counted straight through the playback without ever
; resetting (266222 ms -> 269228 ms). Silent-INFO TTS runs through
; SilentDialogueManager, not the queue those two report, so "speech started" and
; "speech finished" can never be observed and the hold could only ever expire on
; a timeout - which is exactly what it did.
;
; So the slot length comes from Fuz Ro D-oh's placeholder, which is a HARD
; REQUIREMENT: without it an unvoiced line has no duration at all and the whole
; conversation blows past before the audio exists. Its slot is
; ceil(words / WordsPerSecondSilence) seconds, default rate 2.0, capped at 10
; (Stock_10). Speech runs ~3.6 words/sec, so the slot grows FASTER than the
; speech - a longer line has more margin, right up to the cap:
;
;     words   slot    needed (~4.9 s latency + words/3.6)   margin
;     10      5 s     7.7 s                                 -2.7 s
;     15      8 s     9.1 s                                 -1.1 s
;     20      10 s    10.5 s                                -0.5 s   <- prompt cap
;     25      10 s    11.9 s                                -1.9 s
;
; 20 words is therefore the best the default settings allow, and it is what the
; prompt caps at. The residual ~0.5 s overlap is a known limitation, documented
; for users, and goes away on its own if SkyrimNet fixes the underlying bug (see
; docs/skyrimnet-bug-report.md) or if the player lowers WordsPerSecondSilence in
; their OWN Fuz Ro D'oh.ini - which this mod does not ship or touch.
; =============================================================================

; Ask SkyrimNet to pre-generate TTS for the silent INFO before it is reached.
;
; PrepareNPCDialogue scans the SELECTED topic's TopicInfos, evaluates their
; conditions, and generates TTS for any silent response it finds - so calling it
; the moment the courier starts his greeting gives generation a head start over
; the vanilla lines instead of beginning when our INFO is already on screen.
; That latency is the whole problem: on the probe run the INFO fired at 29.42 and
; its audio only started at 34.31, which is why the closing line talked over it.
;
; TESTED, AND IT DOES NOT WORK HERE - kept deliberately. SkyrimNet logs
;   "PrepareNPCDialogue: Base game dialogue detected, no VAD needed"
; and returns without scanning: no "SelectedTopic", no "Found silent response",
; and generation still happened on-demand when the line fired. The C++ builds its
; DialogueContext from the dialogue menu's SELECTED topic, and a forced greeting
; has neither a menu nor a player click.
;
; Left in because it costs one no-op call per delivery and would start working
; for free if SkyrimNet ever widens that trigger - which is the first of the
; three fixes asked for in docs/skyrimnet-bug-report.md. Until then nothing
; closes the latency gap; see the pacing note above for what carries the slot.
Function WarmSilentTTS()
    if bWarmedTTS
        return
    endIf
    bWarmedTTS = true
    Int res = SkyrimNetApi.PrepareNPCDialogue("")
    DBG("PrepareNPCDialogue (pre-warm silent INFO) returned " + res)
EndFunction

; A delivery has been queued. Snapshot what the courier is carrying and start
; generating a sender description for anything we don't already have one for.
Function ArmDelivery(WICourierScript wi)
    ClearDelivery()
    bArmed = true

    if wi.pCourier != None
        currentCourier = wi.pCourier as Actor
    endIf

    ObjectReference cont = wi.pCourierContainer
    if cont == None
        DBG("ArmDelivery: no courier container")
        return
    endIf

    DBG("ArmDelivery: " + cont + " holds " + cont.GetNumItems() + " item(s)")
    if ScanContainer(cont, true) > 0
        GenerateNextFact()
    endIf
EndFunction

; Appends every unvoiced item in the container that is not already in a delivery
; slot, and returns how many it added.
;
; This runs on EVERY armed poll, not just at ArmDelivery, because the courier's
; container keeps FILLING after the delivery is queued: WICourier accumulates
; items until he actually reaches the player, so a second quest can queue one
; minutes after the first while he is still walking. Snapshotting once meant that
; item was handed over with no line about it - confirmed in the 12:41 run, where
; a letter queued 14 s after arming was delivered silently.
;
; Walks the container's LIVE inventory index by index. Deliberately NO
; intermediate Form[]: every array route out of this container failed. Papyrus has
; no zero-length arrays, so an empty native result arrives as None, and both array
; APIs handed one back - GetContainerForms because it reads the BASE container's
; static item list (empty for WICourierContainer; deliveries live in the
; reference's runtime inventory), and Utility.CreateFormArray for no logged reason
; at all, even with GetNumItems reporting 1. Each produced "Cannot cast from None
; to Form[]" and a silent zero-item delivery.
Int Function ScanContainer(ObjectReference cont, Bool bVerbose)
    Int added = 0
    Int n = cont.GetNumItems()

    ; The loop below stops dead at MAX_ITEMS, so anything past the fourth item is
    ; handed over with no line and no explanation. Say so once: a delivery of five
    ; is rare, but "the letter arrived and he said nothing about it" is otherwise
    ; indistinguishable from a bug.
    if n > MAX_ITEMS && pendingCount == 0
        DBG("scan: container holds " + n + " items, only the first " + MAX_ITEMS + " get a line")
    endIf

    Int i = 0
    while i < n && pendingCount < MAX_ITEMS
        Form item = cont.GetNthForm(i)
        if item == None
            if bVerbose
                DBG("scan: slot " + i + " is empty")
            endIf
        elseIf IsAlreadyQueued(item)
            ; Already described (or being described) - the common case on a
            ; re-scan, so it is not logged.
        elseIf IsVoicedItem(item)
            if bVerbose
                DBG("scan: skipping voiced item " + item.GetName())
            endIf
        else
            SetDeliveryItem(pendingCount, item)
            SetDeliveryFact(pendingCount, LookupKnownSender(item))
            ; Resolved here, once, rather than at generation time: the first call
            ; builds the DLL's name index, and scanning happens when the delivery
            ; is QUEUED - usually minutes before the courier finds the player - so
            ; the cost lands where nothing is waiting on it.
            SetDeliverySender(pendingCount, ResolveSender(item))
            SetDeliveryTitleName(pendingCount, CDN_BookText.SenderNameFromTitle(item.GetName()))
            DBG("scan: queued " + item.GetName())
            pendingCount += 1
            added += 1
        endIf
        i += 1
    endWhile

    ; Ran out of slots with items still in the container - the case above only
    ; catches a delivery that arrives oversized in one go, not one that grows past
    ; MAX_ITEMS while the courier is walking (RescanDelivery, every armed poll).
    if pendingCount >= MAX_ITEMS && i < n
        DBG("scan: all " + MAX_ITEMS + " slots full, " + (n - i) + " item(s) will get no line")
    endIf

    return added
EndFunction

Bool Function IsAlreadyQueued(Form item)
    Int i = 0
    while i < pendingCount
        if GetDeliveryItem(i) == item
            return true
        endIf
        i += 1
    endWhile
    return false
EndFunction

; Picks up items queued after the delivery was armed. Only worth doing before the
; courier starts talking - once he is speaking, the INFO chain is already being
; selected and a new line has nowhere to go.
Function RescanDelivery(WICourierScript wi)
    if pendingCount >= MAX_ITEMS
        return
    endIf
    ObjectReference cont = wi.pCourierContainer
    if cont == None
        return
    endIf
    if ScanContainer(cont, false) > 0
        ; Re-open the publish so the new item gets an INFO and CDN_LineReady is
        ; raised to the new count.
        bLinePublished = false
        GenerateNextFact()
    endIf
EndFunction

Function ClearDelivery()
    Int i = 0
    while i < MAX_ITEMS
        SetDeliveryItem(i, None)
        SetDeliveryFact(i, "")
        SetDeliverySender(i, 0)
        SetDeliveryTitleName(i, "")
        i += 1
    endWhile
    pendingCount = 0
    genIndex = 0
    bAwaitingLLM = false
    bArmed = false
    bNarrated = false
    bWarmedTTS = false
    if bLinePublished
        ; Leave every INFO carrying its authored text between deliveries, so a
        ; line generated for one delivery can never be spoken at an unrelated one.
        Int j = 0
        while j < MAX_ITEMS
            CDN_BookText.SetTopicInfoText(PLUGIN_NAME, InfoFormID(j), "")
            j += 1
        endWhile
    endIf
    bLinePublished = false
    ; Shut the gate too. Belt and braces: even if a DLL override somehow survived,
    ; a zero count keeps every one of our INFOs out of the topic entirely.
    SetLineReady(0)
    currentCourier = None
EndFunction

; =============================================================================
; Fact generation (curated map -> LLM), one item at a time
; =============================================================================

Function GenerateNextFact()
    if bAwaitingLLM
        return    ; the callback drives the next one
    endIf
    ; Skip anything already described by the curated map.
    while genIndex < pendingCount && GetDeliveryFact(genIndex) != ""
        genIndex += 1
    endWhile
    if genIndex >= pendingCount
        DBG("all sender facts ready")
        PublishLineToInfo()
        return
    endIf
    if !IsAIFallbackEnabled()
        SetDeliveryFact(genIndex, GenericFact())
        genIndex += 1
        GenerateNextFact()
        return
    endIf

    Form item = GetDeliveryItem(genIndex)
    String context = BuildContextJSON(genIndex, item, Game.GetPlayer())
    DBG("SendCustomPromptToLLM item=" + item.GetName() + " variant=" + GetLLMVariant())
    bAwaitingLLM = true
    Int res = SkyrimNetApi.SendCustomPromptToLLM("courier_sender_line", GetLLMVariant(), context, self, "CDN_Courier_QuestScript", "ProcessLLMLineCallback")
    if res != 1
        DBG("LLM queue failed (res=" + res + "); generic sender")
        bAwaitingLLM = false
        SetDeliveryFact(genIndex, GenericFact())
        genIndex += 1
        GenerateNextFact()
    endIf
EndFunction

; Async callback from SkyrimNet. Signature fixed: (String response, int success).
Function ProcessLLMLineCallback(String response, Int success)
    bAwaitingLLM = false
    if genIndex >= pendingCount
        return    ; delivery ended while we were waiting
    endIf
    String fact = response
    if success != 1
        DBG("LLM failed (success=" + success + "); generic sender")
        fact = GenericFact()
    endIf
    fact = TrimFact(fact)
    if fact == ""
        fact = GenericFact()
    endIf
    DBG("sender fact: " + fact)
    SetDeliveryFact(genIndex, fact)
    genIndex += 1
    GenerateNextFact()
EndFunction

; =============================================================================
; DBVO - write the generated line into the INFO the courier actually speaks
;
; This is the whole reason the line is an INFO rather than a DirectNarration.
; An INFO occupies a real dialogue slot, so it plays BEFORE the closing "Looks
; like that's it"; DirectNarration sits outside the dialogue chain and can only
; ever trail it. SkyrimNet voices an unvoiced INFO's response text verbatim
; ("On-demand: Generating TTS for silent response: '<text>'"), so the generated
; line has to BE that text - and an INFO's text is baked into the plugin, which
; is what CDN_BookText.SetTopicInfoText exists to get around.
;
; VERIFICATION BUILT IN: TESTopicInfo owns no response list of its own, so a
; write may or may not survive - GetTopicInfoText reads it straight back and
; logs both. If the read-back does not match what was written, the write is
; being discarded per call and the DLL needs a hook at playback time instead.
; =============================================================================

; One INFO per described item, exactly as vanilla gives each delivered item its
; own line. A single INFO carries a single response text, so a delivery of three
; unvoiced items needs three INFOs - not one line that tries to cover them all.
;
; Ready facts are packed into CONSECUTIVE INFO slots rather than mapped
; item-slot-to-INFO-slot, so an item still waiting on the LLM costs its own line
; and nothing else: the gate is a count, and INFO n only fires on
; CDN_LineReady >= n, so a hole in the middle would silence everything after it.
Function PublishLineToInfo()
    if bLinePublished
        return
    endIf

    Int published = 0
    Int i = 0
    while i < pendingCount && published < MAX_ITEMS
        String fact = GetDeliveryFact(i)
        if GetDeliveryItem(i) != None && fact != ""
            if IsGenericFact(fact)
                ; Generation failed for this item, so there is no line to speak.
                ; GenericFact() is a bare phrase written for BuildNarration to
                ; embed mid-sentence - spoken verbatim it is a lowercase
                ; fragment. The INFO's OWN authored text is the designed fallback
                ; here: a real line, in his voice, deliberately sender-neutral.
                ; Clear the override rather than merely skipping it, so the
                ; authored text stands whatever this INFO carried before.
                CDN_BookText.SetTopicInfoText(PLUGIN_NAME, InfoFormID(published), "")
                DBG("INFO " + published + ": no generated line, authored fallback stands")
                published += 1    ; still counts - the item gets its line, just not ours
            elseIf CDN_BookText.SetTopicInfoText(PLUGIN_NAME, InfoFormID(published), AsSpokenLine(fact))
                DBG("override armed on INFO " + published + ": " + AsSpokenLine(fact))
                published += 1
            else
                DBG("SetTopicInfoText failed for INFO " + published + " (see CDNBookText.log)")
            endIf
        endIf
        i += 1
    endWhile

    if published == 0
        DBG("PublishLineToInfo: no line to publish")
        return
    endIf

    bLinePublished = true
    ; Opens the INFOs' conditions, and this count IS how many lines the courier
    ; speaks. Until it is at least 1 no line can be selected at all, so a delivery
    ; we have nothing to say about (every item vanilla already voices) plays
    ; exactly as it does without this mod.
    SetLineReady(published)

    ; Arming needs neither the form table nor a populated topic - the DLL hooks the
    ; engine's response lookup, so this runs as soon as the lines exist, ~30 s
    ; before the courier arrives. The READ-BACK does need a populated topic, so it
    ; comes back "" this early; that is expected, not a failure.
    String readBack = CDN_BookText.GetTopicInfoText(COURIER_TOPIC_PLUGIN, COURIER_TOPIC_FORMID, \
                                                    PLUGIN_NAME, PROBE_INFO_FORMID)
    if readBack == ""
        DBG("read-back empty (topic not populated yet) - re-checked at the greeting")
    else
        DBG("read-back: " + readBack)
    endIf
EndFunction

; The INFOs the courier speaks, in the order the topic selects them: chained by
; PreviousDialog 000803 -> 00080B -> 00080C -> 00080D, each gated on
; CDN_LineReady >= its own index+1, all sitting before the closing line 039F81.
;
; An if-chain, not a parallel array - see the comment on GetDeliveryItem for why
; this script keeps no arrays in member variables.
Int Function InfoFormID(Int i)
    if i == 1
        return 0x0000080B      ; CDN_CourierProbeInfo2
    elseIf i == 2
        return 0x0000080C      ; CDN_CourierProbeInfo3
    elseIf i == 3
        return 0x0000080D      ; CDN_CourierProbeInfo4
    endIf
    return PROBE_INFO_FORMID   ; 0x803 CDN_CourierProbeInfo
EndFunction

; CDN_LineReady is a COUNT of described items, not a flag: INFO n fires on
; GetGlobalValue CDN_LineReady >= n, which is what lets one delivery carry a line
; per item. That gate is also what turns this from a test-only line into a real
; one - the condition used to be GetItemCount CDN_TestLetter, so it could only
; ever fire for the console test.
Function SetLineReady(Int count)
    GlobalVariable gv = Game.GetFormFromFile(LINE_READY_GV_FORMID, PLUGIN_NAME) as GlobalVariable
    if gv == None
        DBG("CDN_LineReady missing - the INFOs can never fire")
        return
    endIf
    gv.SetValue(count as Float)
EndFunction

Bool Function IsLineReady()
    GlobalVariable gv = Game.GetFormFromFile(LINE_READY_GV_FORMID, PLUGIN_NAME) as GlobalVariable
    return gv != None && gv.GetValue() >= 1.0
EndFunction

; Confirms, once the topic IS populated, that the engine reports our line rather
; than the authored one. This is the check that actually proves the hook.
Function VerifyPublishedLine()
    String readBack = CDN_BookText.GetTopicInfoText(COURIER_TOPIC_PLUGIN, COURIER_TOPIC_FORMID, \
                                                    PLUGIN_NAME, PROBE_INFO_FORMID)
    DBG("read-back at greeting: " + readBack)
EndFunction

; Note: the facts PublishLineToInfo writes are the lines the courier SPEAKS,
; first person, as authored by the LLM. That is the opposite of what
; BuildNarration produces, which wraps the same material in a THIRD-person fact
; for DirectNarration - DBVO needs the finished line, DirectNarration must never
; get one (it would answer it instead of saying it).

; Stands for "we have no generated line for this item", and the two consumers
; render that differently. BuildNarration embeds it as text, mid-sentence, which
; is what it was written for. PublishLineToInfo treats it as a SENTINEL and
; publishes nothing - see IsGenericFact.
String Function GenericFact() Global
    return "someone who left no name"
EndFunction

; A stored fact that is really "generation failed". Kept as a function so the two
; sides cannot drift: change the string above and this still matches.
Bool Function IsGenericFact(String s) Global
    return s == GenericFact()
EndFunction

; The prompt asks for a bare third-person phrase, but models like to wrap it in
; quotes or end it with a period; both read badly inside the narration sentence.
String Function TrimFact(String s) Global
    if s == ""
        return ""
    endIf
    Int len = StringUtil.GetLength(s)
    Int first = 0
    while first < len && IsTrimmable(StringUtil.GetNthChar(s, first))
        first += 1
    endWhile
    Int last = len - 1
    while last >= first && IsTrimmable(StringUtil.GetNthChar(s, last))
        last -= 1
    endWhile
    if last < first
        return ""
    endIf
    return StringUtil.Substring(s, first, last - first + 1)
EndFunction

; No "\r" here - Papyrus only knows the \n \t \" \\ escapes, and a literal \r is a
; compile error rather than a carriage return.
Bool Function IsTrimmable(String ch) Global
    return ch == " " || ch == "." || ch == "\"" || ch == "'" || ch == "\n" || ch == "\t"
EndFunction

; Puts the full stop back for the DBVO path.
;
; TrimFact strips a trailing period ON PURPOSE - BuildNarration embeds the fact
; inside a larger sentence, where one reads badly. The INFO text is the opposite
; case: it IS the finished spoken line, and the subtitle shows it verbatim, so
; without terminal punctuation it reads as cut off mid-sentence.
;
; Done here, where the fact becomes speech, rather than in TrimFact - the stored
; fact stays a bare phrase and TrimFact's contract with BuildNarration is
; unchanged. "!" and "?" are left alone; the prompt asks for a flat statement, but
; a model that returns one of those has already terminated the sentence.
String Function AsSpokenLine(String s) Global
    Int len = StringUtil.GetLength(s)
    if len == 0
        return s
    endIf
    String tail = StringUtil.GetNthChar(s, len - 1)
    if tail == "." || tail == "!" || tail == "?"
        return s
    endIf
    return s + "."
EndFunction

; =============================================================================
; Narration - hand SkyrimNet the fact and let the courier speak about it
; =============================================================================

; ⚠️ NOT CALLED DURING A DELIVERY, AND MUST NOT BE. Kept only so the reasoning
; survives with the code.
;
; DirectNarration does not merely add a second line - it DESTROYS the delivery.
; Its TTS occupies the actor, and SkyrimNet then suppresses every dialogue line
; that fires while that audio plays. Run 2026-08-12 11:15, with Probe Mode off
; and this still wired in:
;
;   11:15:27.335  DirectNarration called
;   11:15:31.557  Dropping suppressed line from Eifbrir [Courier]: Let's see here...
;   11:15:31.810  Dropping suppressed line from Eifbrir [Courier]: A frantic looking merchant...   <- OUR OWN DBVO LINE
;   11:15:34.317  Started engine playback: Here.
;   11:15:35.979  Started engine playback: A merchant practically threw this at me and bolted.
;   11:15:36.408  Dropping suppressed line from Eifbrir [Courier]: Looks like that's it. Got to go.
;
; Three lines deleted - including the one this mod exists to deliver - and
; replaced by two SkyrimNet inventions. "Here." is the documented failure mode in
; its purest form: the courier ANSWERING the fact instead of speaking it.
;
; So it is not a usable fallback either. If the DBVO path fails, the INFO's
; authored text is the fallback: a real line, in his own voice, with the vanilla
; delivery intact. Silence beats sabotage.
Function NarrateDelivery(Bool abForce)
    if bNarrated || pendingCount == 0
        return
    endIf

    if bAwaitingLLM && !abForce
        DBG("narration held - still generating")
        return
    endIf
    bNarrated = true

    Actor player = Game.GetPlayer()
    Actor courier = currentCourier
    String narration = BuildNarration(player)
    if narration == ""
        return
    endIf

    ; originator = the courier, so HE is the one who responds to (i.e. speaks
    ; about) the fact. Without an originator SkyrimNet picks a speaker itself,
    ; which is the right fallback if the courier is already despawning.
    if courier != None && !courier.IsDead() && !courier.IsDisabled()
        DBG("DirectNarration (courier): " + narration)
        SkyrimNetApi.DirectNarration(narration, courier, player)
    else
        DBG("DirectNarration (system-pick): " + narration)
        SkyrimNetApi.DirectNarration(narration)
    endIf
EndFunction

; One narration per delivery, however many items it holds - a separate event per
; item would have the courier answer himself several times over.
String Function BuildNarration(Actor player)
    String playerName = "the traveller"
    if player != None && player.GetActorBase() != None
        playerName = player.GetActorBase().GetName()
    endIf

    String s = ""
    Int i = 0
    while i < pendingCount
        Form item = GetDeliveryItem(i)
        String fact = GetDeliveryFact(i)
        if fact == ""
            fact = GenericFact()
        endIf
        if item != None
            if s != ""
                s += " "
            endIf
            s += "The courier is delivering " + item.GetName() + " to " + playerName + \
                 "; it was handed to him by " + fact + "."
        endIf
        i += 1
    endWhile
    if s == ""
        return ""
    endIf
    ; The courier has not read any of it - this is what stops him inventing contents.
    return s + " He never read it, and knows nothing of what it says, but he remembers the person who gave it to him."
EndFunction

; =============================================================================
; Backstop - called from the overridden closing fragment (TIF__00039F81) just
; before GiveItemsToPlayer. Only does anything if the poll never caught the
; conversation, e.g. the quest was mid-save-load when the courier arrived.
; =============================================================================

Function TriggerDeliveryLine(Actor akCourier) Global
    CDN_Courier_QuestScript q = (Game.GetFormFromFile(0x00000800, "CDN_CourierDescriptions.esp") as Quest) as CDN_Courier_QuestScript
    if q
        q.HandleDelivery(akCourier)
    endIf
EndFunction

Function HandleDelivery(Actor akCourier)
    if akCourier != None
        currentCourier = akCourier
    endIf
    if !bArmed
        ; The poll never saw this delivery (script updated mid-save, or the
        ; delivery was queued and handed over inside one poll interval).
        DBG("backstop: delivery was never armed - arming now")
        WICourierScript wi = GetWICourier()
        if wi != None
            ArmDelivery(wi)
            if akCourier != None
                currentCourier = akCourier
            endIf
        endIf
    endIf
    ; No narration backstop. It used to call NarrateDelivery(true) here; that is
    ; now actively harmful (see NarrateDelivery). If the poll never armed the
    ; INFO, the courier simply speaks its authored fallback text - a real line in
    ; his own voice, and the vanilla delivery plays intact.
    bNarrated = true
    ; Make sure the poll is alive even on a save whose OnInit never started it.
    RegisterForSingleUpdate(POLL_ARMED)
EndFunction

; =============================================================================
; Helpers
; =============================================================================

WICourierScript Function GetWICourier()
    if wiQuest != None
        return wiQuest
    endIf
    Quest q = Game.GetFormFromFile(WICOURIER_FORMID, "Skyrim.esm") as Quest
    if q != None
        wiQuest = q as WICourierScript
    endIf
    return wiQuest
EndFunction

; Vanilla courier letters that already get a named sender line in the courier
; dialogue - we must NOT double-speak for these.
;
; The skip-list is a LOCAL array, rebuilt per call. That is deliberate: a member
; array cannot survive a save here (see the big warning above), and this runs at
; most MAX_ITEMS times per delivery, which is nothing.
Bool Function IsVoicedItem(Form item)
    if item == None
        return false
    endIf
    Form[] skip = new Form[9]
    skip[0] = Game.GetFormFromFile(0x05224A, "Skyrim.esm")  ; Dark Brotherhood note
    skip[1] = Game.GetFormFromFile(0x0249AF, "Skyrim.esm")  ; Quintus Navale letter
    skip[2] = Game.GetFormFromFile(0x01BFF5, "Skyrim.esm")  ; Inheritance letter
    skip[3] = Game.GetFormFromFile(0x023EE5, "Skyrim.esm")  ; Letter from a Friend
    skip[4] = Game.GetFormFromFile(0x0A0F46, "Skyrim.esm")  ; Calcelmo note
    skip[5] = Game.GetFormFromFile(0x0D91D1, "Skyrim.esm")  ; Falk Firebeard letter
    skip[6] = Game.GetFormFromFile(0x094D8B, "Skyrim.esm")  ; Museum pamphlet
    skip[7] = Game.GetFormFromFile(0x071442, "Skyrim.esm")  ; generic fallback item 1
    skip[8] = Game.GetFormFromFile(0x071443, "Skyrim.esm")  ; generic fallback item 2

    Int i = 0
    while i < skip.Length
        if skip[i] == item
            return true
        endIf
        i += 1
    endWhile
    return false
EndFunction

; Curated item-to-sender map: items vanilla does NOT voice but where we know the
; sender. Empty today, so every non-voiced item goes to the LLM. To curate one,
; add a branch returning a SENDER DESCRIPTION (third person, not a spoken line):
;
;     if item == Game.GetFormFromFile(0x........, "Skyrim.esm")
;         return "the Jarl's steward, all official about it"
;     endIf
;
; An if-chain rather than parallel arrays, for the same save-safety reason.
String Function LookupKnownSender(Form item)
    if item == None
        return ""
    endIf
    return ""
EndFunction

; Context JSON for the LLM prompt. Top-level keys become Inja vars in
; courier_sender_line.prompt ({{ itemName }} {{ letterContent }} {{ location }}
; {{ playerName }} {{ gameHour }} {{ senderFormId }}). Values escaped/truncated
; via CleanForJSON.
; The letter body text comes from the CDNBookText SKSE plugin (CDN_BookText.GetBookText);
; if that plugin is absent the call returns "" and the prompt degrades to title-based.
; letterContent feeds the PROMPT only - it must never reach the narration.
;
; senderFormId is numeric and UNQUOTED, like gameHour - the prompt hands it
; straight to formid_to_uuid, which wants a number. 0 means "sender unknown" and
; the prompt's bio block renders nothing at all.
String Function BuildContextJSON(Int slot, Form item, Actor player)
    Int senderFormId = GetDeliverySender(slot)
    String itemName = ""
    String letterContent = ""
    if item != None
        itemName = item.GetName()
        Book theBook = item as Book
        if theBook != None
            letterContent = CDN_BookText.GetBookText(theBook)
        endIf
    endIf
    String locationName = ""
    Location loc = Game.GetPlayer().GetCurrentLocation()
    if loc != None
        locationName = loc.GetName()
    endIf
    String playerName = ""
    if player != None && player.GetActorBase() != None
        playerName = player.GetActorBase().GetName()
    endIf
    Float gameHour = Utility.GetCurrentGameTime() * 24.0
    return "{\"itemName\": \"" + CleanForJSON(itemName, 120) + "\", \"letterContent\": \"" + CleanForJSON(letterContent, 1500) + "\", \"location\": \"" + CleanForJSON(locationName, 120) + "\", \"playerName\": \"" + CleanForJSON(playerName, 120) + "\", \"gameHour\": " + gameHour + ", \"senderFormId\": " + senderFormId + ", \"titleSenderName\": \"" + CleanForJSON(GetDeliveryTitleName(slot), 80) + "\"}"
EndFunction

; Actor REFERENCE FormID of this item's sender, or 0 when we cannot tell.
;
; Verified 2026-08-12: formid_to_uuid and render_character_profile both resolve an
; actor who is NOT loaded - Aela's bio rendered from across the province - so this
; works for a sender anywhere in Skyrim, which is the normal case.
;
; SkyrimNetApi.FindActorByName is NOT usable here: it only searches within 4000
; units of the player. The DLL does a name-index lookup over the whole form table
; instead, and refuses any name that matches more than one actor.
Int Function ResolveSender(Form item)
    if item == None
        return 0
    endIf
    Book theBook = item as Book
    Int senderRef = CDN_BookText.FindSenderRef(item.GetName(), theBook)
    if senderRef != 0
        DBG("sender for " + item.GetName() + " resolved to " + senderRef)
    endIf
    return senderRef
EndFunction

; Escape a string for safe JSON embedding and cap it at maxOut output chars.
; Escapes " and \, drops CR, turns LF/tab into a space. Needs SKSE StringUtil.
String Function CleanForJSON(String s, Int maxOut) Global
    if s == ""
        return ""
    endIf
    String result = ""
    Int len = StringUtil.GetLength(s)
    Int i = 0
    while i < len
        if StringUtil.GetLength(result) >= maxOut
            result += "..."
            return result
        endIf
        String ch = StringUtil.GetNthChar(s, i)
        if ch == "\""
            result += "\\\""
        elseif ch == "\\"
            result += "\\\\"
        elseif ch == "\n"
            result += " "
        elseif ch == "\t"
            result += " "
        else
            result += ch
        endIf
        i += 1
    endWhile
    return result
EndFunction

; =============================================================================
; Config (read from the manifest via SkyrimNet)
; =============================================================================

String Function ConfigKey() Global
    return "Plugin_Courier Descriptions"
EndFunction

Bool Function GetDebugMode() Global
    return SkyrimNetApi.GetConfigBool(ConfigKey(), "debug.enabled", false)
EndFunction

Bool Function IsAIFallbackEnabled() Global
    return SkyrimNetApi.GetConfigBool(ConfigKey(), "ai.enabled", true)
EndFunction

String Function GetLLMVariant() Global
    return SkyrimNetApi.GetConfigString(ConfigKey(), "llm.variant", "dialogue")
EndFunction

; On = the sender fact is still generated and cached, but nothing is spoken via
; DirectNarration, leaving the unvoiced INFO as the only thing that can make
; noise. Off is the shipping behaviour.
Bool Function IsProbeMode() Global
    return SkyrimNetApi.GetConfigBool(ConfigKey(), "dbvo.probe_mode", false)
EndFunction

; =============================================================================
; Debug / test trigger - from the console, `set CDN_DebugDelivery to 1` queues a
; courier delivery of CDN_TestLetter (a non-voiced test note) so you can hear the
; generated line. Checked only while Debug Mode is on. Be somewhere a courier can
; reach you (a town or exterior).
; =============================================================================

Function CheckDebugTrigger()
    GlobalVariable gv = GetDebugDeliveryGV()
    if gv != None && gv.GetValue() >= 1.0
        Int which = gv.GetValue() as Int
        gv.SetValue(0.0)
        DebugForceDelivery(which)
    endIf
EndFunction

; The five test letters are deliberately different in TONE, because that is the
; only thing the prompt has to work from: the courier never reads the letter, so
; the sender description is inferred purely from what kind of person would send
; this text. Delivering the same note repeatedly tells you nothing about whether
; the inference works - it only tells you the pipeline runs.
;
;   set CDN_DebugDelivery to 1   secretive midnight-meeting note  -> furtive, won't give a name
;   set CDN_DebugDelivery to 2   summons from a Jarl's steward    -> court official, formal
;   set CDN_DebugDelivery to 3   debt collector's threat          -> hard-eyed, unpleasant
;   set CDN_DebugDelivery to 4   warm letter from a mother        -> older woman, kindly
;   set CDN_DebugDelivery to 5   merchant's statement of account  -> tradesman, businesslike
;   set CDN_DebugDelivery to 6   a jarl's personal summons        -> RECOGNISED, may name him
;
; Letter 6 is the FAMILIARITY test, and it is built to isolate one thing. Its
; title ("A Private Word") deliberately carries NO name, so rule 1a's title route
; cannot fire - the only way the courier can say "Balgruuf" is by recognising him
; on sight, which is the RECOGNISED rung. If he names the jarl, the top of the
; ladder works; if he only describes a well-dressed Nord, it does not.
; Letter 5 (Bersi Honey-Hand) is the PLACED comparison and letters 1-4 resolve to
; no sender at all, so the three rungs are covered across the set.
;
; Anything outside 1-6 falls back to letter 1.
Int Function TestLetterFormID(Int which)
    if which == 2
        return 0x00000807      ; CDN_TestLetter_Court
    elseIf which == 3
        return 0x00000808      ; CDN_TestLetter_Debt
    elseIf which == 4
        return 0x00000809      ; CDN_TestLetter_Family
    elseIf which == 5
        return 0x0000080A      ; CDN_TestLetter_Trade
    elseIf which == 6
        return 0x0000080E      ; CDN_TestLetter_Jarl
    endIf
    return TEST_LETTER_FORMID  ; 0x801 CDN_TestLetter
EndFunction

Function DebugForceDelivery(Int which = 1)
    WICourierScript wi = GetWICourier()
    if wi == None
        Debug.Notification("[CDN] DebugForceDelivery: WICourier not resolved")
        return
    endIf
    Form testLetter = Game.GetFormFromFile(TestLetterFormID(which), "CDN_CourierDescriptions.esp")
    if testLetter == None
        Debug.Notification("[CDN] DebugForceDelivery: test letter " + which + " not found")
        return
    endIf
    wi.addItemToContainer(testLetter, 1)
    Debug.Notification("[CDN] Test delivery " + which + " queued - the courier will find you.")
    Debug.Trace("[CDN] DebugForceDelivery: queued " + testLetter.GetName() + " (slot " + which + ") via WICourier", 0)
EndFunction

GlobalVariable Function GetDebugDeliveryGV()
    return Game.GetFormFromFile(DEBUG_DELIVERY_GV_FORMID, "CDN_CourierDescriptions.esp") as GlobalVariable
EndFunction

; ⚠️ The Papyrus TRACE IS NOT GATED ON DEBUG MODE, deliberately. Debug Mode
; controls the on-screen notifications ONLY.
;
; DBG fires on state transitions, not per poll - an idle poll logs nothing, and a
; whole delivery is well under twenty lines - so always-on tracing costs a user
; nothing and is the only thing that makes a bug report diagnosable. It was gated
; before, and the cost showed up immediately: the first user report of a broken
; delivery (2026-08-13) arrived with a Papyrus log containing not one [CDN] line,
; because a normal player has no reason to have turned Debug Mode on. There was
; nothing to read, and the round trip to ask for another capture cost a day.
;
; Debug.Trace only reaches a file when the user has [Papyrus] bEnableLogging /
; bEnableTrace set, so this writes nothing at all for most players. That makes
; the "is it noisy" question moot: the people who see these lines are the ones
; who deliberately turned Papyrus logging on.
Function DBG(String msg)
    Debug.Trace("[CDN] " + msg, 0)
    if GetDebugMode()
        Debug.Notification("[CDN] " + msg)
    endIf
EndFunction
