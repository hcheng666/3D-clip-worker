[CmdletBinding()]
param(
    [string]$CaseManifestPath = (Join-Path $PSScriptRoot "fixture-cases.json"),
    [string]$OutputDirectory = (Join-Path $PSScriptRoot "generated"),
    [switch]$VerifyOnly
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$ExpectedSchemaVersion = 1
$ExpectedGeneratorVersion = "fixture-generator-v1"
$GeneratedManifestName = "manifest.json"
$ArchiveEntryName = "expanded/zeros.bin"
$ArchiveTimestamp = [DateTimeOffset]::new(1980, 1, 1, 0, 0, 0, [TimeSpan]::Zero)
$Utf8NoBom = [System.Text.UTF8Encoding]::new($false)
$GlbMagic = [byte[]](0x67, 0x6C, 0x54, 0x46)
$B3dmMagic = [byte[]](0x62, 0x33, 0x64, 0x6D)
$BinaryFormatVersion = [uint32]1
$GltfBinaryVersion = [uint32]2
$B3dmHeaderLength = 28
$GlbHeaderLength = 12
$InvalidLengthDelta = [uint32]128

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
    param([string]$Value)

    Assert-Condition (-not [string]::IsNullOrWhiteSpace($Value)) "Fixture output path must not be empty."
    Assert-Condition (-not [System.IO.Path]::IsPathRooted($Value)) "Fixture output path must be relative: $Value"
    Assert-Condition (-not $Value.Contains("\")) "Fixture output path must use forward slashes: $Value"
    $segments = $Value.Split("/", [System.StringSplitOptions]::RemoveEmptyEntries)
    Assert-Condition ($segments.Count -gt 0) "Fixture output path must contain a segment: $Value"
    Assert-Condition (-not ($segments -contains ".")) "Fixture output path must be normalized: $Value"
    Assert-Condition (-not ($segments -contains "..")) "Fixture output path must not traverse: $Value"
}

function Write-DeterministicText {
    param(
        [string]$Root,
        [string]$RelativePath,
        [string]$Content
    )

    $targetPath = Join-Path $Root $RelativePath
    $targetDirectory = Split-Path -Parent $targetPath
    New-Item -ItemType Directory -Path $targetDirectory -Force | Out-Null
    $normalized = $Content.Replace("`r`n", "`n").TrimEnd() + "`n"
    [System.IO.File]::WriteAllText($targetPath, $normalized, $Utf8NoBom)
}

function Write-DeterministicBytes {
    param(
        [string]$Root,
        [string]$RelativePath,
        [byte[]]$Bytes
    )

    $targetPath = Join-Path $Root $RelativePath
    $targetDirectory = Split-Path -Parent $targetPath
    New-Item -ItemType Directory -Path $targetDirectory -Force | Out-Null
    [System.IO.File]::WriteAllBytes($targetPath, $Bytes)
}

function New-MalformedGlb {
    $stream = [System.IO.MemoryStream]::new()
    $writer = [System.IO.BinaryWriter]::new($stream, $Utf8NoBom, $true)
    try {
        $writer.Write($GlbMagic)
        $writer.Write($GltfBinaryVersion)
        $writer.Write([uint32]($GlbHeaderLength + $InvalidLengthDelta))
        return $stream.ToArray()
    } finally {
        $writer.Dispose()
        $stream.Dispose()
    }
}

function New-MalformedB3dm {
    [byte[]]$embeddedGlb = @(New-MalformedGlb)
    $actualLength = [uint32]($B3dmHeaderLength + $embeddedGlb.Length)
    $stream = [System.IO.MemoryStream]::new()
    $writer = [System.IO.BinaryWriter]::new($stream, $Utf8NoBom, $true)
    try {
        $writer.Write($B3dmMagic)
        $writer.Write($BinaryFormatVersion)
        $writer.Write([uint32]($actualLength + $InvalidLengthDelta))
        foreach ($tableLength in 1..4) {
            $writer.Write([uint32]0)
        }
        $writer.Write([byte[]]$embeddedGlb)
        return $stream.ToArray()
    } finally {
        $writer.Dispose()
        $stream.Dispose()
    }
}

function New-ArchiveBomb {
    param([int]$ExpandedBytes)

    $archiveStream = [System.IO.MemoryStream]::new()
    $archive = [System.IO.Compression.ZipArchive]::new(
        $archiveStream,
        [System.IO.Compression.ZipArchiveMode]::Create,
        $true,
        $Utf8NoBom)
    try {
        $entry = $archive.CreateEntry($ArchiveEntryName, [System.IO.Compression.CompressionLevel]::Optimal)
        $entry.LastWriteTime = $ArchiveTimestamp
        $entryStream = $entry.Open()
        try {
            $remaining = $ExpandedBytes
            $zeroBlock = [byte[]]::new(16384)
            while ($remaining -gt 0) {
                $writeLength = [Math]::Min($remaining, $zeroBlock.Length)
                $entryStream.Write($zeroBlock, 0, $writeLength)
                $remaining -= $writeLength
            }
        } finally {
            $entryStream.Dispose()
        }
    } finally {
        $archive.Dispose()
    }

    try {
        return $archiveStream.ToArray()
    } finally {
        $archiveStream.Dispose()
    }
}

function New-MetadataGeometryBytes {
    $stream = [System.IO.MemoryStream]::new()
    $writer = [System.IO.BinaryWriter]::new($stream, $Utf8NoBom, $true)
    try {
        foreach ($coordinate in [single[]](-1, -1, 0, 1, -1, 0, 0, 1, 0)) {
            $writer.Write($coordinate)
        }
        $writer.Write([byte[]](0, 1, 1, 0))
        return $stream.ToArray()
    } finally {
        $writer.Dispose()
        $stream.Dispose()
    }
}

function New-MetadataPropertyBytes {
    $stream = [System.IO.MemoryStream]::new()
    $writer = [System.IO.BinaryWriter]::new($stream, $Utf8NoBom, $true)
    try {
        foreach ($offset in [uint32[]](0, 6, 12)) {
            $writer.Write($offset)
        }
        $writer.Write($Utf8NoBom.GetBytes("publicsecret"))
        return $stream.ToArray()
    } finally {
        $writer.Dispose()
        $stream.Dispose()
    }
}

function Get-RelativeFiles {
    param([string]$Root)

    return @(Get-ChildItem -LiteralPath $Root -Recurse -File | ForEach-Object {
        [System.IO.Path]::GetRelativePath($Root, $_.FullName).Replace("\", "/")
    } | Sort-Object)
}

function Get-FileDigestRecord {
    param(
        [string]$Root,
        [string]$RelativePath
    )

    $path = Join-Path $Root $RelativePath
    return [ordered]@{
        path = $RelativePath
        size = (Get-Item -LiteralPath $path).Length
        sha256 = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
    }
}

$resolvedCaseManifest = (Resolve-Path -LiteralPath $CaseManifestPath).Path
$caseManifest = Get-Content -LiteralPath $resolvedCaseManifest -Raw | ConvertFrom-Json
Assert-Condition ($caseManifest.schemaVersion -eq $ExpectedSchemaVersion) `
    "Unsupported fixture case schema version: $($caseManifest.schemaVersion)"
Assert-Condition ($caseManifest.generatorVersion -eq $ExpectedGeneratorVersion) `
    "Fixture generator version does not match the case manifest."

$caseIds = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::Ordinal)
$declaredOutputs = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::Ordinal)
foreach ($case in $caseManifest.cases) {
    Assert-Condition ($caseIds.Add([string]$case.id)) "Duplicate fixture case id: $($case.id)"
    Assert-Condition ($case.outputs.Count -gt 0) "Fixture case has no outputs: $($case.id)"
    foreach ($output in $case.outputs) {
        Assert-SafeRelativePath ([string]$output)
        Assert-Condition ($declaredOutputs.Add([string]$output)) "Duplicate fixture output: $output"
    }
}

$archiveBombCase = @($caseManifest.cases | Where-Object { $_.id -eq "archive-bomb" })
Assert-Condition ($archiveBombCase.Count -eq 1) "Exactly one archive-bomb case is required."

$workingDirectory = $OutputDirectory
$temporaryDirectory = $null
if ($VerifyOnly) {
    $temporaryDirectory = Join-Path ([System.IO.Path]::GetTempPath()) `
        ("clip-worker-fixtures-" + [Guid]::NewGuid().ToString("N"))
    $workingDirectory = $temporaryDirectory
}
New-Item -ItemType Directory -Path $workingDirectory -Force | Out-Null

try {
    Write-DeterministicText $workingDirectory "geometry/boundary-crossing.geojson" @'
{
  "type": "FeatureCollection",
  "features": [
    {"type":"Feature","properties":{"role":"scope"},"geometry":{"type":"Polygon","coordinates":[[[-1,-1],[1,-1],[1,1],[-1,1],[-1,-1]]]}},
    {"type":"Feature","properties":{"role":"source-triangle","expected":"CLIPPED"},"geometry":{"type":"Polygon","coordinates":[[[-2,0],[2,0],[0,2],[-2,0]]]}}
  ]
}
'@

    Write-DeterministicText $workingDirectory "geometry/polygon-hole.geojson" @'
{
  "type": "FeatureCollection",
  "features": [
    {"type":"Feature","properties":{"role":"scope"},"geometry":{"type":"Polygon","coordinates":[[[-3,-3],[3,-3],[3,3],[-3,3],[-3,-3]],[[-1,-1],[-1,1],[1,1],[1,-1],[-1,-1]]]}},
    {"type":"Feature","properties":{"role":"source-triangle","expected":"SPLIT_AROUND_HOLE"},"geometry":{"type":"Polygon","coordinates":[[[-2,0],[2,0],[0,2],[-2,0]]]}},
    {"type":"Feature","properties":{"role":"point-in-hole","expected":"EXCLUDED"},"geometry":{"type":"Point","coordinates":[0,0]}}
  ]
}
'@

    Write-DeterministicText $workingDirectory "geometry/antimeridian-adjacent.geojson" @'
{
  "type": "FeatureCollection",
  "features": [
    {"type":"Feature","properties":{"role":"scope","representation":"split-multipolygon"},"geometry":{"type":"MultiPolygon","coordinates":[[[[179.8,-1],[180,-1],[180,1],[179.8,1],[179.8,-1]]],[[[-180,-1],[-179.8,-1],[-179.8,1],[-180,1],[-180,-1]]]]}},
    {"type":"Feature","properties":{"role":"east-point","expected":"INCLUDED"},"geometry":{"type":"Point","coordinates":[179.9,0]}},
    {"type":"Feature","properties":{"role":"west-point","expected":"INCLUDED"},"geometry":{"type":"Point","coordinates":[-179.9,0]}},
    {"type":"Feature","properties":{"role":"greenwich-point","expected":"EXCLUDED"},"geometry":{"type":"Point","coordinates":[0,0]}}
  ]
}
'@

    [byte[]]$malformedGlb = @(New-MalformedGlb)
    [byte[]]$malformedB3dm = @(New-MalformedB3dm)
    Assert-Condition ($malformedGlb.Length -eq $GlbHeaderLength) `
        "Malformed GLB must contain exactly one complete header."
    Assert-Condition ($malformedB3dm.Length -eq ($B3dmHeaderLength + $GlbHeaderLength)) `
        "Malformed B3DM must contain complete B3DM and GLB headers."
    Assert-Condition ([BitConverter]::ToUInt32($malformedGlb, 8) -gt $malformedGlb.Length) `
        "Malformed GLB must declare a byte length greater than its actual length."
    Assert-Condition ([BitConverter]::ToUInt32($malformedB3dm, 8) -gt $malformedB3dm.Length) `
        "Malformed B3DM must declare a byte length greater than its actual length."
    Write-DeterministicBytes $workingDirectory "malformed/glb-declared-length-too-large.glb" $malformedGlb
    Write-DeterministicBytes $workingDirectory "malformed/b3dm-declared-length-too-large.b3dm" $malformedB3dm

    Write-DeterministicText $workingDirectory "security/uri-traversal/tileset-parent-reference.json" @'
{
  "asset": {"version": "1.1"},
  "geometricError": 1,
  "root": {
    "boundingVolume": {"region": [0, 0, 0.01, 0.01, 0, 10]},
    "geometricError": 0,
    "content": {"uri": "../outside.b3dm"}
  }
}
'@

    Write-DeterministicText $workingDirectory "security/uri-traversal/gltf-percent-encoded-parent.gltf" @'
{
  "asset": {"version": "2.0"},
  "buffers": [{"uri": "%2e%2e/secret.bin", "byteLength": 4}]
}
'@

    $archiveBytes = New-ArchiveBomb ([int]$archiveBombCase[0].expandedBytes)
    $compressionRatio = [double]$archiveBombCase[0].expandedBytes / [double]$archiveBytes.Length
    Assert-Condition ($compressionRatio -ge [double]$archiveBombCase[0].minimumCompressionRatio) `
        "Generated archive does not meet the declared compression-ratio threshold."
    Write-DeterministicBytes $workingDirectory "security/archive-bomb.zip" $archiveBytes

    Write-DeterministicText $workingDirectory "security/unknown-required-extension.gltf" @'
{
  "asset": {"version": "2.0"},
  "extensionsUsed": ["VENDOR_unknown_required"],
  "extensionsRequired": ["VENDOR_unknown_required"],
  "extensions": {"VENDOR_unknown_required": {"payload": "must-not-be-ignored"}},
  "scene": 0,
  "scenes": [{"nodes": []}]
}
'@

    Write-DeterministicText $workingDirectory "metadata/metadata-leakage.gltf" @'
{
  "asset": {"version": "2.0"},
  "extensionsUsed": ["EXT_mesh_features", "EXT_structural_metadata"],
  "extensionsRequired": ["EXT_mesh_features", "EXT_structural_metadata"],
  "scene": 0,
  "scenes": [{"nodes": [0]}],
  "nodes": [{"mesh": 0}],
  "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "_FEATURE_ID_0": 1}, "extensions": {"EXT_mesh_features": {"featureIds": [{"featureCount": 2, "attribute": 0, "propertyTable": 0}]}}}]}],
  "buffers": [{"uri": "geometry.bin", "byteLength": 40}, {"uri": "metadata.bin", "byteLength": 24}],
  "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36, "target": 34962}, {"buffer": 0, "byteOffset": 36, "byteLength": 3, "target": 34962}, {"buffer": 1, "byteOffset": 0, "byteLength": 12}, {"buffer": 1, "byteOffset": 12, "byteLength": 12}],
  "accessors": [{"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [-1, -1, 0], "max": [1, 1, 0]}, {"bufferView": 1, "componentType": 5121, "count": 3, "type": "SCALAR"}],
  "extensions": {"EXT_structural_metadata": {"schema": {"id": "metadata-leakage-v1", "classes": {"building": {"properties": {"name": {"type": "STRING"}}}}}, "propertyTables": [{"class": "building", "count": 2, "properties": {"name": {"stringOffsets": 2, "values": 3}}}]}}
}
'@
    Write-DeterministicBytes $workingDirectory "metadata/geometry.bin" (New-MetadataGeometryBytes)
    [byte[]]$metadataPropertyBytes = @(New-MetadataPropertyBytes)
    Assert-Condition ($Utf8NoBom.GetString($metadataPropertyBytes).Contains("secret")) `
        "Metadata leakage fixture must contain its forbidden UTF-8 marker."
    Write-DeterministicBytes $workingDirectory "metadata/metadata.bin" $metadataPropertyBytes

    Write-DeterministicText $workingDirectory "authorization/empty-authorization.geojson" @'
{
  "type": "FeatureCollection",
  "features": [
    {"type":"Feature","properties":{"role":"scope"},"geometry":{"type":"Polygon","coordinates":[[[-1,-1],[1,-1],[1,1],[-1,1],[-1,-1]]]}},
    {"type":"Feature","properties":{"role":"source-triangle","expected":"REMOVED"},"geometry":{"type":"Polygon","coordinates":[[[10,10],[11,10],[10,11],[10,10]]]}}
  ]
}
'@

    Write-DeterministicText $workingDirectory "authorization/expected-empty-completion.json" @'
{
  "result": "EMPTY",
  "reason": "NO_AUTHORIZED_GEOMETRY"
}
'@

    $emptyCompletion = Get-Content `
        -LiteralPath (Join-Path $workingDirectory "authorization/expected-empty-completion.json") `
        -Raw | ConvertFrom-Json
    foreach ($forbiddenProperty in @("outputEtag", "outputSha256", "outputSize", "outputObjectKey")) {
        Assert-Condition (-not ($emptyCompletion.PSObject.Properties.Name -contains $forbiddenProperty)) `
            "EMPTY completion must not expose output property: $forbiddenProperty"
    }

    $actualOutputs = @(Get-RelativeFiles $workingDirectory | Where-Object { $_ -ne $GeneratedManifestName })
    $missingOutputs = @($declaredOutputs | Where-Object { $_ -notin $actualOutputs })
    $unexpectedOutputs = @($actualOutputs | Where-Object { -not $declaredOutputs.Contains($_) })
    Assert-Condition ($missingOutputs.Count -eq 0) `
        "Generator did not create declared outputs: $($missingOutputs -join ', ')"
    Assert-Condition ($unexpectedOutputs.Count -eq 0) `
        "Generator created undeclared outputs: $($unexpectedOutputs -join ', ')"

    $fileRecords = @($actualOutputs | ForEach-Object {
        Get-FileDigestRecord $workingDirectory $_
    })
    $generatedManifest = [ordered]@{
        schemaVersion = $ExpectedSchemaVersion
        generatorVersion = $ExpectedGeneratorVersion
        caseManifestSha256 = (Get-FileHash -LiteralPath $resolvedCaseManifest -Algorithm SHA256).Hash.ToLowerInvariant()
        files = $fileRecords
    }

    if ($VerifyOnly) {
        $committedManifestPath = Join-Path $OutputDirectory $GeneratedManifestName
        Assert-Condition (Test-Path -LiteralPath $committedManifestPath) `
            "Committed generated manifest is missing: $committedManifestPath"
        $committedManifest = Get-Content -LiteralPath $committedManifestPath -Raw | ConvertFrom-Json
        $expectedJson = $committedManifest | ConvertTo-Json -Depth 8 -Compress
        $actualJson = $generatedManifest | ConvertTo-Json -Depth 8 -Compress
        Assert-Condition ($expectedJson -eq $actualJson) `
            "Generated fixture bytes differ from the committed SHA-256 manifest."
        Write-Host "Verified $($caseIds.Count) fixture cases and $($fileRecords.Count) generated files."
    } else {
        $manifestJson = $generatedManifest | ConvertTo-Json -Depth 8
        Write-DeterministicText $workingDirectory $GeneratedManifestName $manifestJson
        Write-Host "Generated $($caseIds.Count) fixture cases and $($fileRecords.Count) files at $workingDirectory."
    }
} finally {
    if ($VerifyOnly -and $null -ne $temporaryDirectory -and (Test-Path -LiteralPath $temporaryDirectory)) {
        $resolvedTemporaryDirectory = (Resolve-Path -LiteralPath $temporaryDirectory).Path
        $resolvedSystemTemp = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath())
        Assert-Condition ($resolvedTemporaryDirectory.StartsWith($resolvedSystemTemp, [System.StringComparison]::OrdinalIgnoreCase)) `
            "Refusing to clean a fixture verification directory outside the system temp directory."
        Remove-Item -LiteralPath $resolvedTemporaryDirectory -Recurse -Force
    }
}
