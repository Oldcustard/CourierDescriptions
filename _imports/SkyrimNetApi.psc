; Compile stub for SkyrimNet's SkyrimNetApi. At runtime the real SkyrimNetApi
; .pex from the SkyrimNet mod is used; this only resolves signatures at compile.
Scriptname SkyrimNetApi

int function SendCustomPromptToLLM(String promptName, String variant, String contextJson, Quest callbackQuest, String callbackScriptName, String callbackFunctionName) Global Native

int function DirectNarration(String content, Actor originatorActor = None, Actor targetActor = None) Global Native

; Pre-generate TTS for silent NPC responses in the selected topic. Returns 0 on
; success, 1 on error. Paired with IsNPCDialogueReady() for polling.
int function PrepareNPCDialogue(String playerDialogueText) Global Native
bool function IsNPCDialogueReady() Global Native

; Speech pacing. UNUSED, AND DO NOT BUILD ON THESE for silent-INFO dialogue:
; neither one observes it. Measured across a whole delivery, GetSpeechQueueSize()
; returned 0 on every call while the generated line was audibly playing (never
; anything but 0 anywhere in a 4 MB log), and GetTimeSinceLastAudioEnded() counted
; straight through the playback without resetting. That audio goes through
; SkyrimNet's SilentDialogueManager, not the queue these report. An adaptive hold
; was built on them and had to be removed - see CLAUDE.md's pacing section.
int function GetSpeechQueueSize() Global Native
int function GetTimeSinceLastAudioEnded() Global Native

bool function GetConfigBool(String configName, String path, bool defaultValue) Global Native
String function GetConfigString(String configName, String path, String defaultValue) Global Native
int function GetConfigInt(String configName, String path, int defaultValue) Global Native
