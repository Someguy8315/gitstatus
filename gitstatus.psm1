# PowerShell bindings for gitstatusd. Works with Windows PowerShell 5.1 and PowerShell 7+.
#
# Usage (e.g., in $PROFILE):
#
#   Import-Module C:\path\to\gitstatus\gitstatus.psm1
#   Start-GitStatus
#   function prompt {
#     $s = Get-GitStatus
#     $git = if ($s) { " [$($s.LocalBranch)$(if ($s.HasDirty) { '*' })]" } else { '' }
#     "PS $($executionContext.SessionState.Path.CurrentLocation)$git> "
#   }
#
# See docs/windows.md for building gitstatusd.exe. The daemon is looked up in this order:
# -Daemon parameter, $env:GITSTATUS_DAEMON, usrbin\gitstatusd.exe next to this file.

Set-StrictMode -Version 3.0

$script:Daemons = @{}
$script:MsgSep = [byte]0x1e
$script:FieldSep = [char]0x1f
$script:Utf8 = New-Object System.Text.UTF8Encoding($false)

# Response fields after the request ID and the "is a repo" flag, in protocol order.
$script:Fields = @(
  'Workdir', 'Commit', 'LocalBranch', 'RemoteBranch', 'RemoteName', 'RemoteUrl', 'Action',
  'IndexSize', 'NumStaged', 'NumUnstaged', 'NumConflicted', 'NumUntracked', 'CommitsAhead',
  'CommitsBehind', 'StashesCount', 'Tag', 'NumUnstagedDeleted', 'NumStagedNew',
  'NumStagedDeleted', 'PushRemoteName', 'PushRemoteUrl', 'PushCommitsAhead',
  'PushCommitsBehind', 'NumSkipWorktree', 'NumAssumeUnchanged', 'CommitEncoding',
  'CommitSummary')
$script:IntFields = @(
  'IndexSize', 'NumStaged', 'NumUnstaged', 'NumConflicted', 'NumUntracked', 'CommitsAhead',
  'CommitsBehind', 'StashesCount', 'NumUnstagedDeleted', 'NumStagedNew', 'NumStagedDeleted',
  'PushCommitsAhead', 'PushCommitsBehind', 'NumSkipWorktree', 'NumAssumeUnchanged')

function Get-DefaultDaemon {
  if ($env:GITSTATUS_DAEMON) { return $env:GITSTATUS_DAEMON }
  $exe = if ([System.IO.Path]::DirectorySeparatorChar -eq '\') { 'gitstatusd.exe' } else { 'gitstatusd' }
  Join-Path (Join-Path $PSScriptRoot 'usrbin') $exe
}

function Send-Request($d, [string]$id, [string]$dir, [bool]$noDiff) {
  $msg = "$id$script:FieldSep$dir"
  if ($noDiff) { $msg += "${script:FieldSep}1" }
  [byte[]]$bytes = $script:Utf8.GetBytes($msg + [char]$script:MsgSep)
  $d.Stdin.Write($bytes, 0, $bytes.Length)
  $d.Stdin.Flush()
}

# Returns the fields of the next response, or $null if none arrived within the deadline.
function Read-Response($d, [DateTime]$deadline) {
  while ($true) {
    $sep = $d.Buffer.IndexOf($script:MsgSep)
    if ($sep -ge 0) {
      $msg = $script:Utf8.GetString($d.Buffer.GetRange(0, $sep).ToArray())
      $d.Buffer.RemoveRange(0, $sep + 1)
      return , $msg.Split($script:FieldSep)
    }
    # A read that timed out stays pending and is picked up by the next call.
    if (-not $d.Pending) { $d.Pending = $d.Stdout.ReadAsync($d.ReadBuf, 0, $d.ReadBuf.Length) }
    $ms = [int][Math]::Max(0, ($deadline - [DateTime]::UtcNow).TotalMilliseconds)
    if (-not $d.Pending.Wait($ms)) { return $null }
    $n = $d.Pending.Result
    $d.Pending = $null
    if ($n -eq 0) { throw 'gitstatusd exited unexpectedly' }
    $d.Buffer.AddRange((New-Object 'System.ArraySegment[byte]' -ArgumentList $d.ReadBuf, 0, $n))
  }
}

function Start-GitStatus {
  <#
  .SYNOPSIS
  Starts a gitstatusd daemon. Get-GitStatus talks to it until Stop-GitStatus.
  #>
  [CmdletBinding()]
  param(
    [string]$Name = 'default',
    [string]$Daemon = (Get-DefaultDaemon),
    [int]$Threads = [Math]::Min(32, 2 * [Environment]::ProcessorCount),
    # -1 means unlimited. Counts above the limit are reported as the limit.
    [int]$MaxNumStaged = 1,
    [int]$MaxNumUnstaged = 1,
    [int]$MaxNumConflicted = 1,
    [int]$MaxNumUntracked = 1,
    # Don't count unstaged/untracked files in repositories with a larger index.
    [int]$DirtyMaxIndexSize = -1,
    [switch]$RecurseUntrackedDirs,
    [ValidateSet('DEBUG', 'INFO', 'WARN', 'ERROR', 'FATAL')]
    [string]$LogLevel = 'FATAL',
    [int]$TimeoutSeconds = 10
  )
  Stop-GitStatus -Name $Name
  if (-not (Test-Path -LiteralPath $Daemon -PathType Leaf)) {
    throw "gitstatusd not found: $Daemon"
  }

  $daemonArgs = @(
    '-p', $PID, '-t', $Threads, '-v', $LogLevel,
    '-s', $MaxNumStaged, '-u', $MaxNumUnstaged, '-c', $MaxNumConflicted,
    '-d', $MaxNumUntracked, '-m', $DirtyMaxIndexSize)
  if ($RecurseUntrackedDirs) { $daemonArgs += '-e' }

  $psi = New-Object System.Diagnostics.ProcessStartInfo
  $psi.FileName = (Resolve-Path -LiteralPath $Daemon).ProviderPath
  $psi.Arguments = $daemonArgs -join ' '
  $psi.UseShellExecute = $false
  $psi.CreateNoWindow = $true
  $psi.RedirectStandardInput = $true
  $psi.RedirectStandardOutput = $true
  $psi.RedirectStandardError = $true
  $proc = [System.Diagnostics.Process]::Start($psi)
  # Drain stderr so that logging can never block the daemon.
  $proc.BeginErrorReadLine()

  $d = [pscustomobject]@{
    Process = $proc
    Stdin   = $proc.StandardInput.BaseStream
    Stdout  = $proc.StandardOutput.BaseStream
    Buffer  = New-Object 'System.Collections.Generic.List[byte]'
    ReadBuf = New-Object byte[] 65536
    Pending = $null
    NextId  = 0
  }
  $script:Daemons[$Name] = $d

  # A relative directory is never a repository, so this checks that the daemon is responsive.
  try {
    Send-Request $d 'hello' '' $false
    $resp = Read-Response $d ([DateTime]::UtcNow.AddSeconds($TimeoutSeconds))
  } catch {
    $resp = $null
  }
  if (-not $resp -or $resp[0] -ne 'hello' -or $resp[1] -ne '0') {
    Stop-GitStatus -Name $Name
    throw "gitstatusd failed to start: $Daemon"
  }
}

function Get-GitStatus {
  <#
  .SYNOPSIS
  Returns the status of the git repository containing -Path (the current directory by
  default), or $null if it's not in a repository or the daemon didn't reply in time.
  #>
  [CmdletBinding()]
  param(
    [string]$Path,
    [string]$Name = 'default',
    # Skip computing staged/unstaged/untracked counts.
    [switch]$NoDiff,
    [int]$TimeoutMilliseconds = 5000
  )
  $d = $script:Daemons[$Name]
  if (-not $d) { throw "gitstatus '$Name' is not started; call Start-GitStatus" }

  if ($Path) {
    $loc = Get-Item -LiteralPath $Path -ErrorAction SilentlyContinue
    if (-not $loc -or $loc.PSProvider.Name -ne 'FileSystem') { return $null }
    $dir = $loc.FullName
  } else {
    $loc = $ExecutionContext.SessionState.Path.CurrentLocation
    if ($loc.Provider.Name -ne 'FileSystem') { return $null }
    $dir = $loc.ProviderPath
  }

  $id = [string](++$d.NextId)
  Send-Request $d $id $dir $NoDiff.IsPresent
  $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMilliseconds)
  # Skip replies to earlier requests that timed out.
  do {
    $resp = Read-Response $d $deadline
    if ($null -eq $resp) { return $null }
  } while ($resp[0] -ne $id)
  if ($resp[1] -ne '1') { return $null }

  $status = [ordered]@{}
  for ($i = 0; $i -lt $script:Fields.Count; ++$i) {
    $field = $script:Fields[$i]
    $value = if ($i + 2 -lt $resp.Count) { $resp[$i + 2] } else { '' }
    if ($script:IntFields -contains $field) { $value = [int]$value }
    $status[$field] = $value
  }
  if ([System.IO.Path]::DirectorySeparatorChar -eq '\') {
    $status.Workdir = $status.Workdir.Replace('/', '\')
  }
  $status.HasDirty = ($status.NumStaged + $status.NumUnstaged + $status.NumConflicted +
                      $status.NumUntracked) -gt 0
  [pscustomobject]$status
}

function Stop-GitStatus {
  <#
  .SYNOPSIS
  Stops the daemon started by Start-GitStatus.
  #>
  [CmdletBinding()]
  param([string]$Name = 'default')
  $d = $script:Daemons[$Name]
  if (-not $d) { return }
  $script:Daemons.Remove($Name)
  # gitstatusd exits on EOF.
  try { $d.Stdin.Close() } catch { }
  if (-not $d.Process.WaitForExit(2000)) {
    try { $d.Process.Kill() } catch { }
  }
  $d.Process.Dispose()
}

$MyInvocation.MyCommand.ScriptBlock.Module.OnRemove = {
  foreach ($name in @($script:Daemons.Keys)) { Stop-GitStatus -Name $name }
}

Export-ModuleMember -Function Start-GitStatus, Get-GitStatus, Stop-GitStatus
