Scriptname CDN_BookText Hidden

; Returns the body text of a book/letter as authored (may include markup such as
; <font ...> and <Alias=...> tokens). Backed by the CDNBookText SKSE plugin.
; Returns "" if the book has no readable text or is None.
String Function GetBookText(Book akBook) Global Native

; Arms a runtime override of a dialogue INFO's response text, so an unvoiced INFO
; can carry a line generated this session. SkyrimNet's DBVO voicing reads that
; text verbatim, which is how the courier's generated line gets a real dialogue
; slot (before "Looks like that's it") instead of trailing the conversation the
; way DirectNarration does.
;
; Implemented as a HOOK on the engine's response lookup, not a write: a
; TESTopicInfo owns no response list, it rebuilds one per call, so a plain write
; lands in a throwaway. The override therefore applies from the moment it is set,
; needs no populated topic, and can be armed well before the conversation starts.
; Pass an empty string to disarm. Returns false only if the plugin is not loaded.
Bool Function SetTopicInfoText(String asPlugin, Int aiFormID, String asText) Global Native

; Reads the INFO's response text back through the SAME path the engine uses, so
; it verifies the hook rather than a private copy. Needs the parent topic because
; an INFO is not reachable by FormID. Returns "" before the topic is populated,
; which is expected until the conversation starts.
String Function GetTopicInfoText(String asTopicPlugin, Int aiTopicFormID, String asInfoPlugin, Int aiInfoFormID) Global Native

; Best-effort: works out who sent this letter and returns their actor REFERENCE
; FormID, or 0 when the sender cannot be identified unambiguously.
;
; Needed because SkyrimNetApi.FindActorByName only searches within 4000 units of
; the player, and a letter's sender is almost never nearby - which is the whole
; case this exists for. The FormID is a REFERENCE, which is what SkyrimNet's
; formid_to_uuid / render_character_profile decorators expect.
;
; Reads the name from the title ("Letter from Ysolda", "Ysolda's Letter") and
; failing that from the letter's signature line, then resolves it against a
; session-cached index of UNIQUE NPCs. Any name matching more than one actor
; returns 0 - a wrong identification is worse than none, since the courier would
; then confidently describe the wrong person.
;
; The index is built on first call: do that from ArmDelivery, which runs minutes
; before the courier arrives, NOT from the armed poll or the closing backstop.
Int Function FindSenderRef(String asItemTitle, Book akBook) Global Native

; The sender's name AS WRITTEN ON THE ITEM'S LABEL, or "" if the title does not
; name one. This is the ONLY name the courier may speak without knowing the
; person - he can read the outside of a folded note without reading the letter.
;
; Independent of FindSenderRef on purpose: a title name is speakable whether or
; not it resolves to an actor. Place names are filtered out, so "A warning from
; Ivarstead" returns "".
String Function SenderNameFromTitle(String asItemTitle) Global Native
