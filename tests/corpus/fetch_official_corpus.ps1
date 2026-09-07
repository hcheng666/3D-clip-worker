[CmdletBinding()]
param(
    [string]$ManifestPath = (Join-Path $PSScriptRoot "official-corpus-manifest.json"),
    [string[]]$EntryId = @(),
    [switch]$ValidateOnly
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$ExpectedSchemaVersion = 1
$RevisionPattern = "^[0-9a-f]{40}$"
$SecureRepositoryPattern = "^https://github\.com/[^/]+/[^/]+\.git$"
$CacheDirectoryName = ".cache"
$DownloadDirectoryName = "downloaded"
$PayloadDirectoryName = "payload"
$LicenseDirectoryName = "licenses"

function Assert-Condition {
    param(
        [bool]$Condition,
        [string]$Message
    )

    if (-not $Condition) {
        throw $Message
    }
}

function Assert-SafeRelativePath {
    param(
        [string]$Value,
        [string]$FieldName
    )

    Assert-Condition (-not [string]::IsNullOrWhiteSpace($Value)) "$FieldName must not be empty."
    Assert-Condition (-not [System.IO.Path]::IsPathRooted($Value)) "$FieldName must be relative: $Value"
    Assert-Condition (-not $Value.Contains("\")) "$FieldName must use forward slashes: $Value"

    $segments = $Value.Split("/", [System.StringSplitOptions]::RemoveEmptyEntries)
    Assert-Condition ($segments.Count -gt 0) "$FieldName must contain at least one path segment."
    Assert-Condition (-not ($segments -contains "..")) "$FieldName must not traverse parent directories: $Value"
    Assert-Condition (-not ($segments -contains ".")) "$FieldName must be normalized: $Value"
}

function Invoke-Git {
    param(
        [string[]]$Arguments,
        [string]$FailureMessage
    )

    & git @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw $FailureMessage
    }
}

$resolvedManifestPath = (Resolve-Path -LiteralPath $ManifestPath).Path
$manifest = Get-Content -LiteralPath $resolvedManifestPath -Raw | ConvertFrom-Json

Assert-Condition ($manifest.schemaVersion -eq $ExpectedSchemaVersion) `
    "Unsupported corpus manifest schema version: $($manifest.schemaVersion)"
Assert-Condition ($manifest.storagePolicy -eq "MANIFEST_ONLY") `
    "Only MANIFEST_ONLY corpus storage is permitted."
Assert-Condition ($manifest.sources.Count -gt 0) "The corpus manifest has no sources."

$sourceIds = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::Ordinal)
$entryIds = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::Ordinal)
$coverage = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::Ordinal)
$entrySource = @{}

foreach ($source in $manifest.sources) {
    Assert-Condition ($sourceIds.Add([string]$source.id)) "Duplicate source id: $($source.id)"
    Assert-Condition ($source.repository -match $SecureRepositoryPattern) `
        "Source repository must be an HTTPS GitHub Git URL: $($source.repository)"
    Assert-Condition ($source.revision -match $RevisionPattern) `
        "Source revision must be a lowercase full Git commit: $($source.id)"
    Assert-SafeRelativePath ([string]$source.licensePath) "source.licensePath"
    Assert-Condition ($source.entries.Count -gt 0) "Source has no selected entries: $($source.id)"

    foreach ($entry in $source.entries) {
        Assert-Condition ($entryIds.Add([string]$entry.id)) "Duplicate corpus entry id: $($entry.id)"
        Assert-SafeRelativePath ([string]$entry.path) "entry.path"
        Assert-Condition (-not [string]::IsNullOrWhiteSpace([string]$entry.license)) `
            "Entry license is required: $($entry.id)"

        if ($entry.PSObject.Properties.Name -contains "licensePath") {
            Assert-SafeRelativePath ([string]$entry.licensePath) "entry.licensePath"
        }

        Assert-Condition ($entry.coverage.Count -gt 0) "Entry coverage is required: $($entry.id)"
        foreach ($capability in $entry.coverage) {
            [void]$coverage.Add([string]$capability)
        }
        $entrySource[[string]$entry.id] = [string]$source.id
    }
}

$selectedEntryIds = [System.Collections.Generic.HashSet[string]]::new(
    [System.StringComparer]::Ordinal)
foreach ($requestedEntryId in $EntryId) {
    Assert-Condition ($entrySource.ContainsKey([string]$requestedEntryId)) `
        "Requested corpus entry is unknown or derived-only: $requestedEntryId"
    [void]$selectedEntryIds.Add([string]$requestedEntryId)
}

foreach ($derivedEntry in $manifest.derivedEntries) {
    Assert-Condition ($entryIds.Add([string]$derivedEntry.id)) `
        "Duplicate derived corpus entry id: $($derivedEntry.id)"
    Assert-Condition ($entrySource.ContainsKey([string]$derivedEntry.sourceEntryId)) `
        "Derived entry references an unknown source entry: $($derivedEntry.id)"
    Assert-SafeRelativePath ([string]$derivedEntry.generator) "derivedEntry.generator"
    Assert-SafeRelativePath ([string]$derivedEntry.outputPath) "derivedEntry.outputPath"
    Assert-Condition ($derivedEntry.coverage.Count -gt 0) `
        "Derived entry coverage is required: $($derivedEntry.id)"
    foreach ($capability in $derivedEntry.coverage) {
        [void]$coverage.Add([string]$capability)
    }
}

$missingCoverage = @($manifest.requiredCoverage | Where-Object { -not $coverage.Contains([string]$_) })
Assert-Condition ($missingCoverage.Count -eq 0) `
    "Corpus manifest is missing required coverage: $($missingCoverage -join ', ')"

Write-Host "Validated $($sourceIds.Count) sources, $($entryIds.Count) entries, and $($coverage.Count) coverage keys."
if ($ValidateOnly) {
    return
}

$corpusDirectory = Split-Path -Parent $resolvedManifestPath
$cacheDirectory = Join-Path $corpusDirectory $CacheDirectoryName
$downloadDirectory = Join-Path $corpusDirectory $DownloadDirectoryName
New-Item -ItemType Directory -Path $cacheDirectory -Force | Out-Null
New-Item -ItemType Directory -Path $downloadDirectory -Force | Out-Null

foreach ($source in $manifest.sources) {
    $selectedEntries = @($source.entries | Where-Object {
        $selectedEntryIds.Count -eq 0 -or $selectedEntryIds.Contains([string]$_.id)
    })
    if ($selectedEntries.Count -eq 0) {
        continue
    }
    $repositoryDirectory = Join-Path $cacheDirectory ([string]$source.id)
    if (-not (Test-Path -LiteralPath $repositoryDirectory)) {
        Invoke-Git @("clone", "--filter=blob:none", "--no-checkout", [string]$source.repository, $repositoryDirectory) `
            "Failed to clone source: $($source.id)"
    }

    Assert-Condition (Test-Path -LiteralPath (Join-Path $repositoryDirectory ".git")) `
        "Corpus cache is not a Git repository: $repositoryDirectory"

    # Read the canonical config value; process-local insteadOf rules may rewrite
    # the transport URL but must not weaken the manifest's remote identity.
    $configuredRemote = (& git -C $repositoryDirectory config --get remote.origin.url).Trim()
    Assert-Condition ($LASTEXITCODE -eq 0) "Failed to inspect source remote: $($source.id)"
    Assert-Condition ($configuredRemote -eq [string]$source.repository) `
        "Cached source remote does not match manifest: $($source.id)"

    $sparsePaths = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::Ordinal)
    [void]$sparsePaths.Add([string]$source.licensePath)
    foreach ($entry in $selectedEntries) {
        [void]$sparsePaths.Add([string]$entry.path)
        if ($entry.PSObject.Properties.Name -contains "licensePath") {
            [void]$sparsePaths.Add([string]$entry.licensePath)
        }
    }

    Invoke-Git @("-C", $repositoryDirectory, "sparse-checkout", "init", "--no-cone") `
        "Failed to initialize sparse checkout: $($source.id)"
    @($sparsePaths) | & git -C $repositoryDirectory sparse-checkout set --no-cone --stdin
    Assert-Condition ($LASTEXITCODE -eq 0) "Failed to select sparse paths: $($source.id)"
    Invoke-Git @("-C", $repositoryDirectory, "fetch", "--depth", "1", "origin", [string]$source.revision) `
        "Failed to fetch pinned revision: $($source.id)"
    Invoke-Git @("-C", $repositoryDirectory, "checkout", "--detach", "FETCH_HEAD") `
        "Failed to checkout pinned revision: $($source.id)"

    $actualRevision = (& git -C $repositoryDirectory rev-parse HEAD).Trim()
    Assert-Condition ($LASTEXITCODE -eq 0) "Failed to read checked-out revision: $($source.id)"
    Assert-Condition ($actualRevision -eq [string]$source.revision) `
        "Checked-out revision does not match manifest: $($source.id)"

    foreach ($entry in $selectedEntries) {
        $entryDirectory = Join-Path $downloadDirectory ([string]$entry.id)
        Assert-Condition (-not (Test-Path -LiteralPath $entryDirectory)) `
            "Materialized entry already exists; remove it explicitly before refetching: $($entry.id)"

        $sourcePayloadPath = Join-Path $repositoryDirectory ([string]$entry.path)
        Assert-Condition (Test-Path -LiteralPath $sourcePayloadPath) `
            "Pinned source does not contain selected entry path: $($entry.path)"

        New-Item -ItemType Directory -Path $entryDirectory | Out-Null
        Copy-Item -LiteralPath $sourcePayloadPath -Destination (Join-Path $entryDirectory $PayloadDirectoryName) -Recurse

        $entryLicensePath = [string]$source.licensePath
        if ($entry.PSObject.Properties.Name -contains "licensePath") {
            $entryLicensePath = [string]$entry.licensePath
        }
        $sourceLicensePath = Join-Path $repositoryDirectory $entryLicensePath
        Assert-Condition (Test-Path -LiteralPath $sourceLicensePath) `
            "Pinned source does not contain license path: $entryLicensePath"

        $licenseDirectory = Join-Path $entryDirectory $LicenseDirectoryName
        New-Item -ItemType Directory -Path $licenseDirectory | Out-Null
        Copy-Item -LiteralPath $sourceLicensePath -Destination $licenseDirectory

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
        $sourceRecord | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $entryDirectory "SOURCE.json") -Encoding UTF8
        Write-Host "Materialized $($entry.id) at $entryDirectory"
    }
}
