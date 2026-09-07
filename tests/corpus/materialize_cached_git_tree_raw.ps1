[CmdletBinding()]
param(
    [string]$ManifestPath = (Join-Path $PSScriptRoot "official-corpus-manifest.json"),
    [Parameter(Mandatory = $true)]
    [string[]]$EntryId
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$ExpectedSchemaVersion = 1
$RepositoryPattern = "^https://github\.com/([^/]+)/([^/]+)\.git$"
$CacheDirectoryName = ".cache"
$DownloadDirectoryName = "downloaded"
$PayloadDirectoryName = "payload"
$LicenseDirectoryName = "licenses"
$SourceRecordName = "SOURCE.json"

function Assert-Condition {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) {
        throw $Message
    }
}

function Assert-SafeRelativePath {
    param([string]$Value, [string]$FieldName)
    Assert-Condition (-not [string]::IsNullOrWhiteSpace($Value)) "$FieldName must not be empty."
    Assert-Condition (-not [System.IO.Path]::IsPathRooted($Value)) "$FieldName must be relative: $Value"
    Assert-Condition (-not $Value.Contains("\")) "$FieldName must use forward slashes: $Value"
    $segments = $Value.Split("/", [System.StringSplitOptions]::RemoveEmptyEntries)
    Assert-Condition ($segments.Count -gt 0) "$FieldName must contain a path segment."
    Assert-Condition (-not ($segments -contains "..")) "$FieldName must not traverse parent directories: $Value"
    Assert-Condition (-not ($segments -contains ".")) "$FieldName must be normalized: $Value"
}

function ConvertTo-RawPath {
    param([string]$Value)
    return (($Value.Split("/") | ForEach-Object {
        [System.Uri]::EscapeDataString($_)
    }) -join "/")
}

function Get-RepositoryIdentity {
    param([string]$Repository)
    $match = [System.Text.RegularExpressions.Regex]::Match($Repository, $RepositoryPattern)
    Assert-Condition $match.Success "Only HTTPS GitHub repositories are supported by the raw fallback: $Repository"
    return [ordered]@{
        Owner = $match.Groups[1].Value
        Name = $match.Groups[2].Value
    }
}

function Get-TreeFiles {
    param([string]$RepositoryDirectory, [string]$Revision, [string]$SourcePath)
    $files = @(& git -C $RepositoryDirectory ls-tree -r --name-only $Revision -- $SourcePath)
    Assert-Condition ($LASTEXITCODE -eq 0) "Failed to enumerate pinned Git tree: $SourcePath"
    Assert-Condition ($files.Count -gt 0) "Pinned Git tree does not contain: $SourcePath"
    return $files
}

function Invoke-RawDownload {
    param(
        [System.Net.Http.HttpClient]$Client,
        [string]$Owner,
        [string]$RepositoryName,
        [string]$Revision,
        [string]$SourcePath,
        [string]$DestinationPath
    )
    $rawPath = ConvertTo-RawPath $SourcePath
    $uri = "https://raw.githubusercontent.com/$Owner/$RepositoryName/$Revision/$rawPath"
    $destinationDirectory = Split-Path -Parent $DestinationPath
    New-Item -ItemType Directory -Path $destinationDirectory -Force | Out-Null
    $response = $Client.GetAsync(
        $uri,
        [System.Net.Http.HttpCompletionOption]::ResponseHeadersRead).GetAwaiter().GetResult()
    try {
        Assert-Condition $response.IsSuccessStatusCode `
            "Raw download failed with HTTP $([int]$response.StatusCode): $SourcePath"
        $input = $response.Content.ReadAsStreamAsync().GetAwaiter().GetResult()
        try {
            $output = [System.IO.File]::Create($DestinationPath)
            try {
                $input.CopyTo($output)
            } finally {
                $output.Dispose()
            }
        } finally {
            $input.Dispose()
        }
    } finally {
        $response.Dispose()
    }
}

$resolvedManifestPath = (Resolve-Path -LiteralPath $ManifestPath).Path
$manifest = Get-Content -LiteralPath $resolvedManifestPath -Raw | ConvertFrom-Json
Assert-Condition ($manifest.schemaVersion -eq $ExpectedSchemaVersion) `
    "Unsupported corpus manifest schema version: $($manifest.schemaVersion)"

$entriesById = @{}
$sourcesById = @{}
foreach ($source in $manifest.sources) {
    $sourcesById[[string]$source.id] = $source
    foreach ($entry in $source.entries) {
        Assert-Condition (-not $entriesById.ContainsKey([string]$entry.id)) `
            "Duplicate corpus entry id: $($entry.id)"
        $entriesById[[string]$entry.id] = [ordered]@{
            Source = $source
            Entry = $entry
        }
    }
}

$corpusDirectory = Split-Path -Parent $resolvedManifestPath
$cacheDirectory = Join-Path $corpusDirectory $CacheDirectoryName
$downloadDirectory = Join-Path $corpusDirectory $DownloadDirectoryName
New-Item -ItemType Directory -Path $downloadDirectory -Force | Out-Null
$requestedEntryIds = @($EntryId | ForEach-Object {
    $_.Split(",", [System.StringSplitOptions]::RemoveEmptyEntries)
} | ForEach-Object { $_.Trim() })
Assert-Condition ($requestedEntryIds.Count -gt 0) "At least one corpus entry id is required."

$client = [System.Net.Http.HttpClient]::new()
$client.Timeout = [TimeSpan]::FromMinutes(5)
try {
    foreach ($requestedEntryId in $requestedEntryIds) {
        Assert-Condition ($entriesById.ContainsKey($requestedEntryId)) `
            "Requested corpus entry is unknown or derived-only: $requestedEntryId"
        $selection = $entriesById[$requestedEntryId]
        $source = $selection.Source
        $entry = $selection.Entry
        $identity = Get-RepositoryIdentity ([string]$source.repository)
        Assert-SafeRelativePath ([string]$entry.path) "entry.path"

        $repositoryDirectory = Join-Path $cacheDirectory ([string]$source.id)
        Assert-Condition (Test-Path -LiteralPath (Join-Path $repositoryDirectory ".git")) `
            "Pinned Git tree cache is unavailable: $repositoryDirectory"
        $configuredRemote = (& git -C $repositoryDirectory config --get remote.origin.url).Trim()
        Assert-Condition ($LASTEXITCODE -eq 0) "Failed to inspect cached source remote: $($source.id)"
        Assert-Condition ($configuredRemote -eq [string]$source.repository) `
            "Cached source remote does not match manifest: $($source.id)"
        & git -C $repositoryDirectory cat-file -e "$([string]$source.revision)^{commit}"
        Assert-Condition ($LASTEXITCODE -eq 0) "Pinned commit is absent from the cached tree: $($source.id)"

        $entryDirectory = Join-Path $downloadDirectory $requestedEntryId
        Assert-Condition (-not (Test-Path -LiteralPath $entryDirectory)) `
            "Materialized entry already exists; remove it explicitly before refetching: $requestedEntryId"
        $temporaryDirectory = Join-Path $downloadDirectory `
            (".raw-" + $requestedEntryId + "-" + [System.Guid]::NewGuid().ToString("N"))
        New-Item -ItemType Directory -Path $temporaryDirectory | Out-Null
        try {
            $payloadDirectory = Join-Path $temporaryDirectory $PayloadDirectoryName
            New-Item -ItemType Directory -Path $payloadDirectory | Out-Null
            $sourcePath = [string]$entry.path
            $treeFiles = Get-TreeFiles $repositoryDirectory ([string]$source.revision) $sourcePath
            foreach ($treeFile in $treeFiles) {
                Assert-SafeRelativePath $treeFile "tree.path"
                $relativePath = if ($treeFile -eq $sourcePath) {
                    [System.IO.Path]::GetFileName($treeFile)
                } else {
                    $treeFile.Substring($sourcePath.Length).TrimStart("/")
                }
                Assert-SafeRelativePath $relativePath "tree.relativePath"
                $destinationPath = Join-Path $payloadDirectory $relativePath
                Invoke-RawDownload $client $identity.Owner $identity.Name `
                    ([string]$source.revision) $treeFile $destinationPath
            }

            $licensePath = [string]$source.licensePath
            if ($entry.PSObject.Properties.Name -contains "licensePath") {
                $licensePath = [string]$entry.licensePath
            }
            Assert-SafeRelativePath $licensePath "licensePath"
            $licenseDirectory = Join-Path $temporaryDirectory $LicenseDirectoryName
            $licenseDestination = Join-Path $licenseDirectory ([System.IO.Path]::GetFileName($licensePath))
            Invoke-RawDownload $client $identity.Owner $identity.Name `
                ([string]$source.revision) $licensePath $licenseDestination

            $sourceRecord = [ordered]@{
                entryId = [string]$entry.id
                sourceId = [string]$source.id
                repository = [string]$source.repository
                revision = [string]$source.revision
                sourcePath = [string]$entry.path
                license = [string]$entry.license
                attribution = if ($entry.PSObject.Properties.Name -contains "attribution") {
                    [string]$entry.attribution
                } else {
                    $null
                }
            }
            $sourceRecord | ConvertTo-Json -Depth 4 | Set-Content `
                -LiteralPath (Join-Path $temporaryDirectory $SourceRecordName) -Encoding UTF8
            Move-Item -LiteralPath $temporaryDirectory -Destination $entryDirectory
            Write-Host "Materialized $requestedEntryId from pinned cached tree at $entryDirectory"
        } catch {
            if (Test-Path -LiteralPath $temporaryDirectory) {
                Remove-Item -LiteralPath $temporaryDirectory -Recurse -Force
            }
            throw
        }
    }
} finally {
    $client.Dispose()
}
