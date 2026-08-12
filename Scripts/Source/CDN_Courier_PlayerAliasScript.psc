Scriptname CDN_Courier_PlayerAliasScript Extends ReferenceAlias

; =============================================================================
; DEPRECATED - kept for reference only, NOT attached to any alias.
;
; Earlier design detected deliveries via the player alias OnItemAdded. That fires
; at the handover (GiveItemsToPlayer, inside the closing line), which lands the
; spoken line around the closing - not cleanly in the per-item slot.
;
; The shipped design instead overrides the closing line's result fragment
; (TIF__00039F81) to speak just BEFORE the handover. See CDN_Courier_QuestScript
; and TIF__00039F81. This file is not compiled into the build.
; =============================================================================
