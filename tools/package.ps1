<#
.SYNOPSIS
    Assembles a Courier Descriptions release archive.

.DESCRIPTION
    Builds the shipping layout from tracked sources plus the two build inputs
    that are deliberately NOT in git (the compiled Papyrus scripts and the SKSE
    DLL), then zips it.

    Everything it packs is listed at the end, so what shipped is never a guess.
    Any missing input is a hard error - a release cannot be built half-empty.

.PARAMETER ScriptsDir
    Folder holding the three compiled scripts: CDN_BookText.pex,
    CDN_Courier_QuestScript.pex, TIF__00039F81.pex.

.PARAMETER Dll
    Path to the built CDNBookText.dll.

.PARAMETER Version
    Version string for the archive name. Should match
    External/oldcustard.courier-descriptions/manifest.json and
    External/oldcustard.courier-descriptions/settings/CourierDescriptions.yaml.

.PARAMETER OutDir
    Where to write the archive. Defaults to the repository root.

.EXAMPLE
    tools\package.ps1 -ScriptsDir C:\build\scripts `
                      -Dll BookTextPlugin\build\CDNBookText\SKSE\Plugins\Release\CDNBookText.dll `
                      -Version 1.0.0
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string] $ScriptsDir,
    [Parameter(Mandatory = $true)] [string] $Dll,
    [Parameter(Mandatory = $true)] [string] $Version,
    [string] $OutDir
)

$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent $PSScriptRoot
if (-not $OutDir) { $OutDir = $repo }

$stage = Join-Path $repo 'dist_staging'
$zip   = Join-Path $OutDir ("Courier Descriptions-{0}.zip" -f $Version)

# The voice types to ship a placeholder under, and the four dialogue lines that
# need one. The FormID in the path is the INFO's own, masked to 00 - which is
# why every line needs its own file.
#
# The engine derives the folder from the SPEAKING actor's voice type, so a
# courier replacer that assigns its own voice type sends the lookup somewhere we
# do not ship to, and the line falls back to Fuz Ro D-oh's 10 s cap. One folder
# per known replacer covers that; an unused folder costs nothing.
#
# The topic name is truncated to 16 characters, NOT the 15 houseCARL computes -
# confirmed from the engine's own resolved path for our INFO in the 2026-08-12
# logs, with both spellings present in the folder:
#   Sound\Voice\CDN_CourierDescriptions.esp\MaleYoungEager\WICourier_WICourierDeliver_00000803_1.fuz
$voiceTypes  = @(
    'MaleYoungEager',       # vanilla WICourierNPC (039F83)
    'PrettyCourierVoice',   # Lilly - Female Courier Replacer (Nexus 4595)

    # The Courier Crew (Nexus 61400). Repoints the persistent courier ref
    # 039FB7 at WICourierWorld, templated (Traits) from a 6-entry LCharCourier
    # rolled per delivery - so ONE ref delivers under five different voice
    # types. MaleYoungEager covers two of the six entries; these four cover the
    # rest.
    'FemaleCoward',         # WICourierNPC3 (000801:CourierCrew.esp)
    'FemaleCondescending',  # WICourierNPC4 (000809:CourierCrew.esp)
    'MaleEvenToned',        # WICourierNPC5 (000D63:CourierCrew.esp)
    'MaleKhajiit'           # WICourierNPC6 (000810:CourierCrew.esp)
)
$infoFormIds = @('00000803', '0000080B', '0000080C', '0000080D')
$topicStem   = 'WICourier_WICourierDeliver'

$pexNames = @('CDN_BookText.pex', 'CDN_Courier_QuestScript.pex', 'TIF__00039F81.pex')

# --- the SkyrimNet content layer (beta 25+) -------------------------------
# Content ships as an EXTERNAL content plugin, one folder under
# SKSE/Plugins/SkyrimNet/external/. The folder name MUST equal the manifest's
# "id" or SkyrimNet rejects the whole folder at startup, so $contentId is
# load-bearing in both places.
#
# The WebUI settings schema ships INSIDE the layer as settings/{Name}.yaml, and
# {Name} is the config name the quest script reads (Plugin_CourierDescriptions,
# ConfigKey() in CDN_Courier_QuestScript.psc) - rename one, rename both. It used
# to ship as config/plugins/Courier Descriptions/manifest.yaml; settings/ names
# allow no spaces, and that legacy path is deliberately no longer shipped.
$contentId  = 'oldcustard.courier-descriptions'
$contentDir = Join-Path $repo ("External\{0}" -f $contentId)
$settingsName = 'CourierDescriptions'

# --- verify every input up front ------------------------------------------
$silence = Join-Path $repo '_build\silence_13s.fuz'
$inputs  = [ordered]@{
    'plugin'    = Join-Path $repo 'Plugin\CDN_CourierDescriptions.esp'
    'seq'       = Join-Path $repo 'Plugin\CDN_CourierDescriptions.seq'
    'settings'  = Join-Path $contentDir ("settings\{0}.yaml" -f $settingsName)
    'cmanifest' = Join-Path $contentDir 'manifest.json'
    'prompt'    = Join-Path $contentDir 'prompts\courier_sender_line.prompt'
    'install'   = Join-Path $repo 'INSTALL.txt'
    'silence'   = $silence
    'dll'       = $Dll
}
foreach ($n in $pexNames) { $inputs["pex:$n"] = Join-Path $ScriptsDir $n }

$missing = @()
foreach ($k in $inputs.Keys) {
    if (-not (Test-Path -LiteralPath $inputs[$k])) { $missing += ("{0}  ->  {1}" -f $k, $inputs[$k]) }
}
if ($missing.Count -gt 0) {
    throw ("cannot package - {0} input(s) missing:`n  {1}" -f $missing.Count, ($missing -join "`n  "))
}

# both versions should match what we are naming the archive
if ($settingsName -notmatch '^[A-Za-z0-9_-]+$') {
    throw ("settings name '{0}' has characters a settings/ file name cannot use (letters, digits, - and _ only)" -f $settingsName)
}
$manifestVersion = (Select-String -Path $inputs['settings'] -Pattern '^\s*version:\s*"([^"]+)"' |
                    Select-Object -First 1).Matches.Groups[1].Value
if ($manifestVersion -ne $Version) {
    Write-Warning ("settings/{2}.yaml says version {0} but the archive is being named {1}" -f $manifestVersion, $Version, $settingsName)
}

$contentManifest = Get-Content -LiteralPath $inputs['cmanifest'] -Raw | ConvertFrom-Json
if ($contentManifest.id -ne $contentId) {
    throw ("content manifest id '{0}' does not match folder name '{1}' - SkyrimNet would reject the folder" -f $contentManifest.id, $contentId)
}
if ($contentManifest.version -notmatch '^\d+\.\d+\.\d+$') {
    throw ("content manifest version '{0}' is not strict semver (X.Y.Z)" -f $contentManifest.version)
}
if ($contentManifest.version -ne $Version) {
    throw ("content manifest.json says version {0} but the archive is being named {1} - bump both together" -f $contentManifest.version, $Version)
}

# --- stage -----------------------------------------------------------------
if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }

$voiceDirs = @($voiceTypes | ForEach-Object {
    Join-Path $stage ("Sound\Voice\CDN_CourierDescriptions.esp\{0}" -f $_)
})
foreach ($d in @(
    (Join-Path $stage 'SEQ'),
    (Join-Path $stage 'Scripts'),
    (Join-Path $stage 'SKSE\Plugins'),
    (Join-Path $stage "SKSE\Plugins\SkyrimNet\external\$contentId\prompts"),
    (Join-Path $stage "SKSE\Plugins\SkyrimNet\external\$contentId\settings")
) + $voiceDirs) {
    New-Item -ItemType Directory -Path $d -Force | Out-Null
}

Copy-Item -LiteralPath $inputs['plugin']    -Destination (Join-Path $stage 'CDN_CourierDescriptions.esp')
Copy-Item -LiteralPath $inputs['seq']       -Destination (Join-Path $stage 'SEQ\CDN_CourierDescriptions.seq')
Copy-Item -LiteralPath $inputs['install']   -Destination (Join-Path $stage 'INSTALL.txt')
Copy-Item -LiteralPath $inputs['dll']       -Destination (Join-Path $stage 'SKSE\Plugins\CDNBookText.dll')
Copy-Item -LiteralPath $inputs['cmanifest'] -Destination (Join-Path $stage "SKSE\Plugins\SkyrimNet\external\$contentId\manifest.json")
Copy-Item -LiteralPath $inputs['prompt']    -Destination (Join-Path $stage "SKSE\Plugins\SkyrimNet\external\$contentId\prompts\courier_sender_line.prompt")
Copy-Item -LiteralPath $inputs['settings']  -Destination (Join-Path $stage ("SKSE\Plugins\SkyrimNet\external\$contentId\settings\{0}.yaml" -f $settingsName))
foreach ($n in $pexNames) {
    Copy-Item -LiteralPath $inputs["pex:$n"] -Destination (Join-Path $stage ('Scripts\' + $n))
}

# one silent placeholder per dialogue line, per voice type
foreach ($voiceDir in $voiceDirs) {
    foreach ($id in $infoFormIds) {
        Copy-Item -LiteralPath $silence -Destination (Join-Path $voiceDir ("{0}_{1}_1.fuz" -f $topicStem, $id))
    }
}

# --- archive ---------------------------------------------------------------
if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip -CompressionLevel Optimal

# --- report exactly what shipped -------------------------------------------
$files = Get-ChildItem -LiteralPath $stage -Recurse -File
Write-Host ""
Write-Host ("packaged {0} files -> {1}" -f $files.Count, $zip)
Write-Host ("archive size: {0:N0} bytes" -f (Get-Item -LiteralPath $zip).Length)
Write-Host ""
$files |
    Sort-Object FullName |
    ForEach-Object { "{0,10:N0}  {1}" -f $_.Length, $_.FullName.Substring($stage.Length + 1) }
