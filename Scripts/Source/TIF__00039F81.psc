Scriptname TIF__00039F81 extends TopicInfo Hidden

; =============================================================================
; Override of the vanilla WICourier closing-line result fragment.
;
; Vanilla Fragment_0: (GetOwningQuest() as WICourierScript).GiveItemsToPlayer()
;   - hands the queued items to the player (the delivery moment).
;
; This override FIRST has the courier speak a generated "who sent it" line for
; any items he has no vanilla voiceline for (CDN_Courier_QuestScript handles the
; skip-list + speech via SkyrimNetApi.DirectNarration), and THEN hands the items
; over as normal. The items are still in the courier's container when the line is
; spoken, so it lands in the per-item slot, before "Looks like that's it."
;
; Ships as a loose Scripts/TIF__00039F81.pex (overrides the vanilla packed one).
; =============================================================================

Function Fragment_0(ObjectReference akSpeakerRef)
    CDN_Courier_QuestScript.TriggerDeliveryLine(akSpeakerRef as Actor)
    (GetOwningQuest() as WICourierScript).GiveItemsToPlayer()
EndFunction
