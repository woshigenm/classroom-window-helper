# lpe_documents.ps1 -- unit tests for trigger.ps1's known-folder handling.
#
# The two blocks are extracted verbatim from lpe\trigger.ps1 (markers
# "# >>> known-folder" / "# <<< known-folder" and "# >>> restore" /
# "# <<< restore") and run in a child PowerShell with stubs, so this exercises
# the text that ships instead of a hand-copied model of it.
#
# One substitution is unavoidable: [Environment]::GetFolderPath reads the real
# per-user value, so that single line is replaced with $env:TEST_ORIGINAL.
# Everything else - snapshot location, validation, candidate order, the
# Test-Path gates, exit codes and the restore messages - is the shipped code.
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
$abort   = Get-Block '# >>> abort-check' '# <<< abort-check'

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

# %LOCALAPPDATA%\ClassroomBypass-LPE\documents.orig, with %LOCALAPPDATA% pointed
# at the sandbox.
function Snap([string]$dir) { return (Join-Path $dir 'la\ClassroomBypass-LPE\documents.orig') }

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
$source = '@SOURCE@'
$snapshot = '@SNAPSHOT@'
$documents = '@DOCUMENTS@'
function Set-Stage([string]$text) {
    [IO.File]::AppendAllText('@STAGES@', "$text`r`n", (New-Object Text.UTF8Encoding($false)))
}
@BLOCK@
exit 0
'@

# The shipped block is a `return` inside try/finally, at script scope.  This
# template reproduces that shape exactly - same nesting, same "after the block"
# and "after the try" lines - because the whole point is that return still runs
# the finally that restores Documents.  If PowerShell ever changes that, this
# case fails instead of silently leaving Documents redirected.
$abortTemplate = @'
$ErrorActionPreference = 'Stop'
$env:LOCALAPPDATA = '@LAPPDATA@'
$AbortPid = '@PID@'
function Set-Stage([string]$text) {
    [IO.File]::AppendAllText('@STAGES@', "$text`r`n", (New-Object Text.UTF8Encoding($false)))
}
try {
    do {
@BLOCK@
        Set-Stage 'after-block'
        break
    } while ($true)
} finally {
    Set-Stage 'finally ran'
}
Set-Stage 'tail'
exit 0
'@

function Run-Case([string]$name, [string]$template, [string]$block, $values,
                  [string[]]$expectStages, [int]$expectCode,
                  [scriptblock]$setup = $null) {
    $dir = New-Dir (Join-Path $work $name)
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
    return [pscustomobject]@{ Dir = $dir; Stages = $stages; Snapshot = (Snap $dir) }
}

function Case-Dir([string]$name) { return (Join-Path $work $name) }

# ---- known-folder block -----------------------------------------------------

$dir = Case-Dir 'healthy'
$case = Run-Case 'healthy' $knownTemplate $known @{
    LAPPDATA = "$dir\la"; UP = "$dir\up"; OD = ''
    ORIGINAL = (New-Dir "$dir\docs")
} @('documents original=', 'source=original') 0
Check ((Get-Snapshot $case.Snapshot) -eq "$dir\docs") 'healthy    snapshot holds the original'

$dir = Case-Dir 'stale-recorded'
$recorded = New-Dir "$dir\onedrive\Documents"
$case = Run-Case 'stale-recorded' $knownTemplate $known @{
    LAPPDATA = "$dir\la"; UP = "$dir\up"; OD = ''
    ORIGINAL = "$dir\la\ClassroomBypass-LPE\run1"
} @('recorded original', 'source=snapshot') 0 {
    param($d) Set-Snapshot (Snap $d) $recorded
}
Check ((Get-Snapshot $case.Snapshot) -eq $recorded) 'recorded   snapshot keeps the recorded path'

$dir = Case-Dir 'stale-recorded-dead'
$case = Run-Case 'stale-recorded-dead' $knownTemplate $known @{
    LAPPDATA = "$dir\la"; UP = "$dir\up"; OD = ''
    ORIGINAL = "$dir\la\ClassroomBypass-LPE\run1"
} @('recorded original is not a directory') 4 {
    param($d) Set-Snapshot (Snap $d) "$d\gone"
}
Check ((Get-Snapshot $case.Snapshot) -eq "$dir\gone") 'dead       snapshot not overwritten by a guess'

$dir = Case-Dir 'stale-onedrive'
$oneDriveDocs = New-Dir "$dir\od\Documents"
$profileDocs  = New-Dir "$dir\up\Documents"
$case = Run-Case 'stale-onedrive' $knownTemplate $known @{
    LAPPDATA = "$dir\la"; UP = "$dir\up"; OD = "$dir\od"
    ORIGINAL = "$dir\la\ClassroomBypass-LPE\run1"
} @('candidates:', 'under LPE root -> using ', 'source=fallback') 0
Check ((Get-Snapshot $case.Snapshot) -eq $oneDriveDocs) 'onedrive   picked over %USERPROFILE%'
Check ($profileDocs -ne $oneDriveDocs) 'onedrive   the two candidates really differ'

$dir = Case-Dir 'stale-nothing'
$case = Run-Case 'stale-nothing' $knownTemplate $known @{
    LAPPDATA = "$dir\la"; UP = (New-Dir "$dir\up"); OD = (New-Dir "$dir\od")
    ORIGINAL = "$dir\la\ClassroomBypass-LPE\run1"
} @('no candidate to restore') 4
Check ($null -eq (Get-Snapshot $case.Snapshot)) 'nothing    no snapshot written when refusing'

# A corrupt snapshot is not a value: it is ignored, and only then may the run
# fall back to guessing.
$dir = Case-Dir 'snapshot-garbage'
$oneDriveDocs = New-Dir "$dir\od\Documents"
$case = Run-Case 'snapshot-garbage' $knownTemplate $known @{
    LAPPDATA = "$dir\la"; UP = "$dir\up"; OD = "$dir\od"
    ORIGINAL = "$dir\la\ClassroomBypass-LPE\run1"
} @('more than one line', 'candidates:') 0 {
    param($d) Set-Snapshot (Snap $d) "$($d)\one`r`n$($d)\two"
}
Check ((Get-Snapshot $case.Snapshot) -eq $oneDriveDocs) 'garbage    ignored, candidate used instead'

$dir = Case-Dir 'unreadable-recorded'
$docs = New-Dir "$dir\docs"
$case = Run-Case 'unreadable-recorded' $knownTemplate $known @{
    LAPPDATA = "$dir\la"; UP = "$dir\up"; OD = ''
    ORIGINAL = ''
} @('using recorded original', 'source=snapshot') 0 {
    param($d) Set-Snapshot (Snap $d) $docs
}
Check ((Get-Snapshot $case.Snapshot) -eq $docs) 'unreadable snapshot is authoritative'

$dir = Case-Dir 'unreadable-nothing'
$null = Run-Case 'unreadable-nothing' $knownTemplate $known @{
    LAPPDATA = "$dir\la"; UP = "$dir\up"; OD = ''; ORIGINAL = ''
} @('could not read the Documents path') 4

# Same refusal as the stale branch: a recorded but unreachable original is not
# a reason to try a candidate.
$dir = Case-Dir 'unreadable-recorded-dead'
$null = Run-Case 'unreadable-recorded-dead' $knownTemplate $known @{
    LAPPDATA = "$dir\la"; UP = "$dir\up"; OD = ''; ORIGINAL = ''
} @('unreadable and the recorded original is not a directory') 4 {
    param($d) Set-Snapshot (Snap $d) "$d\gone"
}

$dir = Case-Dir 'not-a-directory'
$file = Join-Path (New-Dir $dir) 'notadir.txt'
[IO.File]::WriteAllText($file, 'x')
$null = Run-Case 'not-a-directory' $knownTemplate $known @{
    LAPPDATA = "$dir\la"; UP = "$dir\up"; OD = ''; ORIGINAL = $file
} @('path is not a directory') 4

# User Shell Folders\Personal written as REG_SZ keeps its %VAR% unexpanded;
# SHGetKnownFolderPath then hands back the literal, which never resolves.  The
# run must refuse and say which registry value is wrong, not just "not a
# directory".
$dir = Case-Dir 'unexpanded'
$null = Run-Case 'unexpanded' $knownTemplate $known @{
    LAPPDATA = "$dir\la"; UP = "$dir\up"; OD = ''
    ORIGINAL = '%USERPROFILE%\Documents'
} @('contains an unexpanded variable', 'must be REG_EXPAND_SZ') 4

# ---- restore block ----------------------------------------------------------

$dir = Case-Dir 'restore-ok'
$stub = New-DocumentsStub $dir
$snap = Join-Path $dir 'documents.orig'
$docs = New-Dir "$dir\docs"
Set-Snapshot $snap $docs
$null = Run-Case 'restore-ok' $restoreTemplate $restore @{
    ORIGINAL = $docs; SOURCE = 'original'; SNAPSHOT = $snap; DOCUMENTS = $stub
} @('documents restored', 'source=original') 0
$calls = [IO.File]::ReadAllText((Join-Path $dir 'calls.txt'), [Text.Encoding]::Default)
Check ($calls.Trim() -eq "set $docs") 'restore    called set with the original path'
Check (-not (Test-Path -LiteralPath $snap)) 'restore    snapshot removed after success'

# Same call, different provenance: a fallback candidate restores a *working*
# Documents, not the path the user configured.  The only thing that tells the
# two apart afterwards is the source marker, so it gets a case of its own.
$dir = Case-Dir 'restore-fallback'
$stub = New-DocumentsStub $dir
$snap = Join-Path $dir 'documents.orig'
$docs = New-Dir "$dir\up\Documents"
Set-Snapshot $snap $docs
$case = Run-Case 'restore-fallback' $restoreTemplate $restore @{
    ORIGINAL = $docs; SOURCE = 'fallback'; SNAPSHOT = $snap; DOCUMENTS = $stub
} @('documents restored', 'source=fallback') 0
Check ($case.Stages -notmatch 'source=(original|snapshot)') 'fallback   never logged as a real restore'

$dir = Case-Dir 'restore-skipped'
$stub = New-DocumentsStub $dir
$snap = Join-Path $dir 'documents.orig'
Set-Snapshot $snap "$dir\gone"
$null = Run-Case 'restore-skipped' $restoreTemplate $restore @{
    ORIGINAL = "$dir\gone"; SOURCE = 'original'; SNAPSHOT = $snap; DOCUMENTS = $stub
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
    ORIGINAL = $docs; SOURCE = 'original'; SNAPSHOT = $snap; DOCUMENTS = $stub
} @('restore failed \(exit 1\)') 0
Check (Test-Path -LiteralPath (Join-Path $dir 'calls.txt')) 'failed     set was attempted'
Check (Test-Path -LiteralPath $snap) 'failed     snapshot kept for the next run'

# ---- abort check block ------------------------------------------------------
#
# "Skip" turns into a file that the launcher writes; the wait loop only has to
# notice it and let the script unwind.  Two things can go wrong silently: the
# loop keeps waiting (user sees nothing happen) or the return skips the finally
# (Documents stays on the LPE root).  Both are asserted here.

$dir = Case-Dir 'abort-hit'
$abortFile = Join-Path $dir 'la\ClassroomBypass-LPE\abort.4242'
New-Dir (Split-Path -Parent $abortFile) | Out-Null
Set-Content -LiteralPath $abortFile -Value 'abort 1' -NoNewline
$case = Run-Case 'abort-hit' $abortTemplate $abort @{
    LAPPDATA = "$dir\la"; PID = '4242'
} @('abort observed', 'finally ran') 0
Check ($case.Stages -notmatch 'after-block') 'abort      stops waiting at once'
Check ($case.Stages -notmatch 'tail') 'abort      exits the script, does not fall through'

$dir = Case-Dir 'abort-cold'
$case = Run-Case 'abort-cold' $abortTemplate $abort @{
    LAPPDATA = "$dir\la"; PID = '4242'
} @('after-block', 'finally ran', 'tail') 0
Check ($case.Stages -notmatch 'abort observed') 'cold       no file, no abort'

# Without an injected pid there is no path to test, and the guard has to keep
# that from turning into "...\abort." (which a stale file could match).
$dir = Case-Dir 'abort-nopid'
$strayFile = Join-Path $dir 'la\ClassroomBypass-LPE\abort.'
New-Dir (Split-Path -Parent $strayFile) | Out-Null
Set-Content -LiteralPath $strayFile -Value 'abort 1' -NoNewline
$case = Run-Case 'abort-nopid' $abortTemplate $abort @{
    LAPPDATA = "$dir\la"; PID = ''
} @('tail') 0
Check ($case.Stages -notmatch 'abort observed') 'nopid      no pid, no abort even with a stray file'

# ---- materialized payload: the -Command / IEX leg ---------------------------
#
# the launcher no longer ships the script inside -EncodedCommand (trigger.ps1
# outgrew the 32767-character limit).  It writes preamble + script to
# %LOCALAPPDATA%\ClassroomBypass-LPE\trigger.<pid>.<tick>.ps1 and runs
#
#   -Command "Invoke-Expression (Get-Content -LiteralPath '<path>' -Raw)"
#
# Three things can break that silently and none of them is covered above: the
# single-quote escaping of the path, the UTF-8 BOM (PowerShell 5.1 decodes a
# BOM-less file with the ANSI code page and mangles a non-ASCII preamble), and
# whether IEX keeps the -File semantics the launcher's exit-code logic assumes.
# -ExecutionPolicy Bypass is deliberately NOT passed: -Command must not need it.

$iexDir = New-Dir (Join-Path $work "q'uote dir")
$iex = Join-Path $iexDir 'trigger.999.1.ps1'
$body = @'
$msg = -join ([char]0x4E2D, [char]0x6587)
Write-Output ("preamble len=" + $msg.Length)
try {
    do {
        Start-Sleep -Milliseconds 20
        break
    } while ($false)
    return
} finally {
    Write-Output 'finally ran'
}
Write-Output 'after-return'
'@
[IO.File]::WriteAllText($iex, $body, (New-Object Text.UTF8Encoding($true)))
$q = $iex.Replace("'", "''")
$out = (& powershell -NoProfile -Command "Invoke-Expression (Get-Content -LiteralPath '$q' -Raw)" 2>&1 | Out-String)
Check ($LASTEXITCODE -eq 0) 'iex        exit 0 without -ExecutionPolicy'
Check ($out -match 'preamble len=2') 'iex        BOM honoured, non-ASCII preamble intact'
Check ($out -match 'finally ran') 'iex        return inside try still runs finally'
Check ($out -notmatch 'after-return') 'iex        and does not fall through'

# The launcher reads the exit code to pick its grace period (0 = the child may
# still be in UAC, 25 s; non-zero = final, 2.5 s), so exit N has to survive IEX.
$iexExit = Join-Path $iexDir 'trigger.999.2.ps1'
[IO.File]::WriteAllText($iexExit, "exit 7`r`n", (New-Object Text.UTF8Encoding($true)))
$q2 = $iexExit.Replace("'", "''")
& powershell -NoProfile -Command "Invoke-Expression (Get-Content -LiteralPath '$q2' -Raw)" | Out-Null
Check ($LASTEXITCODE -eq 7) 'iex        exit N becomes the process exit code'

Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue

Write-Host ''
if ($script:failed -gt 0) {
    Write-Host "$($script:failed) check(s) FAILED" -ForegroundColor Red
    exit 1
}
Write-Host 'lpe documents: all checks PASS' -ForegroundColor Green
exit 0
