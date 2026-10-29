; Compile stub for SkyrimNet's SkyrimNetApi. At runtime the real SkyrimNetApi
; .pex from the SkyrimNet mod is used; this only resolves signatures at compile.
Scriptname SkyrimNetApi

int function SendCustomPromptToLLM(String promptName, String variant, String contextJson, Quest callbackQuest, String callbackScriptName, String callbackFunctionName) Global Native

int function DirectNarration(String content, Actor originatorActor = None, Actor targetActor = None) Global Native

; ⛔ PrepareNPCDialogue / IsNPCDialogueReady are GONE from beta 29-rc1 -
; unregistered along with TriggerPlayerTTS / IsPlayerTTSFinished when SkyrimNet
; replaced the DBVO bridge with its own DialogueTopicHold. Declaring them here
; still compiles; the call then fails at runtime as a missing function, because
; the shipped SkyrimNetApi.pex no longer carries the name either. Do not put
; them back without checking the installed SkyrimNetApi.psc first.

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
