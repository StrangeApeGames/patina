# Installs the latest Patina release (the binary, plus the skill and MCP server for Claude Code and Codex):
#   irm https://raw.githubusercontent.com/parodyband/patina/main/install.ps1 | iex
# Afterwards `patina update` keeps it current; its MCP server tells your agent when to run it.
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'  # the progress bar makes Invoke-WebRequest very slow on PowerShell 5
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$asset = 'patina-windows-x64.exe'
$base = 'https://github.com/parodyband/patina/releases/latest/download'
$tmp = Join-Path ([IO.Path]::GetTempPath()) ('patina-' + [guid]::NewGuid())
New-Item -ItemType Directory $tmp | Out-Null
try {
  $exe = Join-Path $tmp $asset
  $sums = Join-Path $tmp 'SHA256SUMS.txt'
  Invoke-WebRequest "$base/$asset" -OutFile $exe -UseBasicParsing
  Invoke-WebRequest "$base/SHA256SUMS.txt" -OutFile $sums -UseBasicParsing
  $line = Get-Content $sums | Where-Object { $_ -match ('\s' + [regex]::Escape($asset) + '\s*$') } | Select-Object -First 1
  $got = (Get-FileHash $exe -Algorithm SHA256).Hash.ToLower()
  if (-not $line -or $got -ne ($line -split '\s+')[0].ToLower()) { throw "checksum mismatch for $asset" }
  & $exe install
  if ($LASTEXITCODE -ne 0) { throw 'patina install failed' }
} finally {
  Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}
