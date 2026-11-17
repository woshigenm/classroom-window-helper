# lpe_documents.ps1 -- unit tests for trigger.ps1's known-folder handling.
#
# The two blocks are extracted verbatim from lpe\trigger.ps1 (markers
# "# >>> known-folder" / "# <<< known-folder" and "# >>> restore" /
# "# <<< restore") and run in a child PowerShell with stubs, so this exercises
# the text that ships instead of a hand-copied model of it.
#
# One substitution is unavoidable: [Environment]::GetFolderPath reads the real
# per-user value, so that single line is replaced with $env:TEST_ORIGINAL.
# Everything else - snapshot protocol, candidate order, the Test-Path gates,
# exit codes and the two different restore messages - is the shipped code.
#
# Exit code 0 only if every case passed.

$ErrorActionPreference = 'Stop'

$root  = Split-Path -Parent $PSScriptRoot
$src   = Join-Path $root 'lpe\trigger.ps1'
$lines = [IO.File]::ReadAllLines($src)

function Get-Block([string]$open, [string]$close) {
    $a = -1; $b = -1
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $t = $lines[$i].Trim()
        if ($t -eq $open) { $a = $i }
        elseif ($t -eq $close -and $a -ge 0) { $b = $i; break }
    }
    if ($a -lt 0 -or $b -lt 0) { throw "markers $open / $close not found in $src" }
    return ($lines[($a + 1)..($b - 1)] -join "`r`n")
}

$known = Get-Block '# >>> known-folder' '# <<< known-folder'
# \r? matters: the block is joined with CRLF, and (?m)$ only matches before \n.
$known = $known -replace '(?m)^\$original = \[Environment\]::GetFolderPath\(.*\)\r?$',
                         '$original = $$env:TEST_ORIGINAL'
if ($known -notmatch '\$env:TEST_ORIGINAL') {
    throw 'GetFolderPath line not found - update the substitution in this test'
}
$restore = Get-Block '# >>> restore' '# <<< restore'

$work = Join-Path $env:TEMP ('lpe_docs_' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $work -Force | Out-Null

$script:failed = 0
function Check([bool]$ok, [string]$what) {
    if ($ok) {
        Write-Host "  ok   $what"
    } else {
        Write-Host "  FAIL $what" -ForegroundColor Red
        $script:failed++
    }
}

function New-Dir([string]$path) {
    if (-not (Test-Path -LiteralPath $path)) {
        New-Item -ItemType Directory -Path $path -Force | Out-Null
    }
    return $path
}

function Set-Snapshot([string]$path, [string]$value) {
    New-Dir (Split-Path -Parent $path) | Out-Null
    [IO.File]::WriteAllText($path, $value, (New-Object Text.UTF8Encoding($false)))
}

function Get-Snapshot([string]$path) {
    if (-not (Test-Path -LiteralPath $path)) { return $null }
    return [IO.File]::ReadAllText($path, [Text.Encoding]::UTF8)
}

# A stand-in for documents_path.exe: records its arguments and returns the exit
# code in rc.txt, so the failing-restore path can be exercised too.
function New-DocumentsStub([string]$dir) {
    New-Dir $dir | Out-Null
    $cmd = @(
        '@echo off'
        '>>"%~dp0calls.txt" echo %*'
        'set /p RC=<"%~dp0rc.txt"'
        'exit /b %RC%'
    ) -join "`r`n"
    [IO.File]::WriteAllText((Join-Path $dir 'documents.cmd'), $cmd,
                            (New-Object Text.UTF8Encoding($false)))
    [IO.File]::WriteAllText((Join-Path $dir 'rc.txt'), '0',
                            (New-Object Text.UTF8Encoding($false)))
    return (Join-Path $dir 'documents.cmd')
}

function Esc([string]$s) { return $s.Replace("'", "''") }

$knownTemplate = @'
$ErrorActionPreference = 'Stop'
$Repo = '@REPO@'
$env:LOCALAPPDATA = '@LAPPDATA@'
$env:USERPROFILE = '@UP@'
$env:OneDrive = '@OD@'
$env:OneDriveConsumer = ''
$env:TEST_ORIGINAL = '@ORIGINAL@'
function Set-Stage([string]$text) {
    [IO.File]::AppendAllText('@STAGES@', "$text`r`n", (New-Object Text.UTF8Encoding($false)))
}
@BLOCK@
exit 0
'@

$restoreTemplate = @'
$ErrorActionPreference = 'Stop'
$original = '@ORIGINAL@'
$snapshot = '@SNAPSHOT@'
$documents = '@DOCUMENTS@'
function Set-Stage([string]$text) {
    [IO.File]::AppendAllText('@STAGES@', "$text`r`n", (New-Object Text.UTF8Encoding($false)))
}
@BLOCK@
exit 0
'@

function Run-Case([string]$name, [string]$template, [string]$block, $values,
                  [string[]]$expectStages, [int]$expectCode,
                  [scriptblock]$setup = $null) {
    $dir = New-Dir (Join-Path $work $name)
    # bin\lpe always exists in production, so the snapshot write cannot fail
    # for a missing directory there either.
    New-Dir (Join-Path $dir 'repo') | Out-Null
    if ($setup) { & $setup $dir }
    $case = Join-Path $dir 'case.ps1'
    $text = $template.Replace('@STAGES@', (Esc (Join-Path $dir 'stages.txt')))
    foreach ($k in $values.Keys) { $text = $text.Replace("@$k@", (Esc ([string]$values[$k]))) }
    $text = $text.Replace('@BLOCK@', $block)
    [IO.File]::WriteAllText($case, $text, (New-Object Text.UTF8Encoding($true)))

    & powershell -NoProfile -ExecutionPolicy Bypass -File $case | Out-Null
    $code = $LASTEXITCODE

    $stages = ''
    $stageFile = Join-Path $dir 'stages.txt'
    if (Test-Path -LiteralPath $stageFile) {
        $stages = [IO.File]::ReadAllText($stageFile, [Text.Encoding]::UTF8)
    }

    $label = $name.PadRight(22)
    Check ($code -eq $expectCode) "$label exit $code (want $expectCode)"
    foreach ($pattern in $expectStages) {
        Check ($stages -match $pattern) "$label log /$pattern/"
    }
    return [pscustomobject]@{
        Dir      = $dir
        Stages   = $stages
        Snapshot = (Join-Path $dir 'repo\documents.orig')
    }
}

# A case directory is <work>\<name>, so a caller can pre-write files into it.
function Case-Dir([string]$name) { return (Join-Path $work $name) }

# ---- known-folder block -----------------------------------------------------

$dir = Case-Dir 'healthy'
$case = Run-Case 'healthy' $knownTemplate $known @{
    REPO = "$dir\repo"; LAPPDATA = "$dir\la"; UP = "$dir\up"; OD = ''
    ORIGINAL = (New-Dir "$dir\docs")
} @('documents original=') 0
Check ((Get-Snapshot $case.Snapshot) -eq "$dir\docs") 'healthy    snapshot holds the original'

$dir = Case-Dir 'stale-recorded'
$recorded = New-Dir "$dir\onedrive\Documents"
$case = Run-Case 'stale-recorded' $knownTemplate $known @{
    REPO = "$dir\repo"; LAPPDATA = "$dir\la"; UP = "$dir\up"; OD = ''
    ORIGINAL = "$dir\la\ClassroomBypass-LPE\run1"
} @('recorded original') 0 {
    param($d) Set-Snapshot "$d\repo\documents.orig" $recorded
}
Check ((Get-Snapshot $case.Snapshot) -eq $recorded) 'recorded   snapshot keeps the recorded path'

$dir = Case-Dir 'stale-recorded-dead'
$case = Run-Case 'stale-recorded-dead' $knownTemplate $known @{
    REPO = "$dir\repo"; LAPPDATA = "$dir\la"; UP = "$dir\up"; OD = ''
    ORIGINAL = "$dir\la\ClassroomBypass-LPE\run1"
} @('recorded original is not a directory') 4 {
    param($d) Set-Snapshot "$d\repo\documents.orig" "$d\gone"
}
Check ((Get-Snapshot $case.Snapshot) -eq "$dir\gone") 'dead       snapshot not overwritten by a guess'

$dir = Case-Dir 'stale-onedrive'
$oneDriveDocs = New-Dir "$dir\od\Documents"
$profileDocs  = New-Dir "$dir\up\Documents"
$case = Run-Case 'stale-onedrive' $knownTemplate $known @{
    REPO = "$dir\repo"; LAPPDATA = "$dir\la"; UP = "$dir\up"; OD = "$dir\od"
    ORIGINAL = "$dir\la\ClassroomBypass-LPE\run1"
} @('candidates:', 'under LPE root -> using ') 0
Check ((Get-Snapshot $case.Snapshot) -eq $oneDriveDocs) 'onedrive   picked over %USERPROFILE%'
Check ($profileDocs -ne $oneDriveDocs) 'onedrive   the two candidates really differ'

$dir = Case-Dir 'stale-nothing'
$case = Run-Case 'stale-nothing' $knownTemplate $known @{
    REPO = "$dir\repo"; LAPPDATA = "$dir\la"
    UP = (New-Dir "$dir\up"); OD = (New-Dir "$dir\od")
    ORIGINAL = "$dir\la\ClassroomBypass-LPE\run1"
} @('no candidate to restore') 4
Check ($null -eq (Get-Snapshot $case.Snapshot)) 'nothing    no snapshot written when refusing'

$dir = Case-Dir 'unreadable-recorded'
$docs = New-Dir "$dir\docs"
$case = Run-Case 'unreadable-recorded' $knownTemplate $known @{
    REPO = "$dir\repo"; LAPPDATA = "$dir\la"; UP = "$dir\up"; OD = ''
    ORIGINAL = ''
} @('using recorded original') 0 {
    param($d) Set-Snapshot "$d\repo\documents.orig" $docs
}
Check ((Get-Snapshot $case.Snapshot) -eq $docs) 'unreadable snapshot is authoritative'

$dir = Case-Dir 'unreadable-nothing'
$null = Run-Case 'unreadable-nothing' $knownTemplate $known @{
    REPO = "$dir\repo"; LAPPDATA = "$dir\la"; UP = "$dir\up"; OD = ''; ORIGINAL = ''
} @('could not read the Documents path') 4

$dir = Case-Dir 'not-a-directory'
$file = Join-Path (New-Dir $dir) 'notadir.txt'
[IO.File]::WriteAllText($file, 'x')
$null = Run-Case 'not-a-directory' $knownTemplate $known @{
    REPO = "$dir\repo"; LAPPDATA = "$dir\la"; UP = "$dir\up"; OD = ''; ORIGINAL = $file
} @('path is not a directory') 4

# ---- restore block ----------------------------------------------------------

$dir = Case-Dir 'restore-ok'
$stub = New-DocumentsStub $dir
$snap = Join-Path $dir 'documents.orig'
$docs = New-Dir "$dir\docs"
Set-Snapshot $snap $docs
$null = Run-Case 'restore-ok' $restoreTemplate $restore @{
    ORIGINAL = $docs; SNAPSHOT = $snap; DOCUMENTS = $stub
} @('documents restored') 0
$calls = [IO.File]::ReadAllText((Join-Path $dir 'calls.txt'), [Text.Encoding]::Default)
Check ($calls.Trim() -eq "set $docs") 'restore    called set with the original path'
Check (-not (Test-Path -LiteralPath $snap)) 'restore    snapshot removed after success'

$dir = Case-Dir 'restore-skipped'
$stub = New-DocumentsStub $dir
$snap = Join-Path $dir 'documents.orig'
Set-Snapshot $snap "$dir\gone"
$null = Run-Case 'restore-skipped' $restoreTemplate $restore @{
    ORIGINAL = "$dir\gone"; SNAPSHOT = $snap; DOCUMENTS = $stub
} @('restore skipped') 0
Check (-not (Test-Path -LiteralPath (Join-Path $dir 'calls.txt'))) 'skipped    set was not called at all'
Check (Test-Path -LiteralPath $snap) 'skipped    snapshot kept for the next run'

$dir = Case-Dir 'restore-failed'
$stub = New-DocumentsStub $dir
$snap = Join-Path $dir 'documents.orig'
$docs = New-Dir "$dir\docs"
Set-Snapshot $snap $docs
[IO.File]::WriteAllText((Join-Path $dir 'rc.txt'), '1')
$null = Run-Case 'restore-failed' $restoreTemplate $restore @{
    ORIGINAL = $docs; SNAPSHOT = $snap; DOCUMENTS = $stub
} @('restore failed \(exit 1\)') 0
Check (Test-Path -LiteralPath (Join-Path $dir 'calls.txt')) 'failed     set was attempted'
Check (Test-Path -LiteralPath $snap) 'failed     snapshot kept for the next run'

Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue

Write-Host ''
if ($script:failed -gt 0) {
    Write-Host "$($script:failed) check(s) FAILED" -ForegroundColor Red
    exit 1
}
Write-Host 'lpe documents: all checks PASS' -ForegroundColor Green
exit 0
