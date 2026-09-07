[CmdletBinding()]
param(
    [string]$OutputDirectory = (Join-Path $PSScriptRoot "generated"),
    [switch]$VerifyOnly
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$Utf8NoBom = [System.Text.UTF8Encoding]::new($false)
$ManifestName = "manifest.json"
$GeneratorVersion = "normalizer-canonical-fixture-v1"
$ExpectedFiles = @(
    "mesh-positive.glb",
    "point-positive.glb",
    "instance-positive.glb",
    "external-uri-negative.glb",
    "unknown-required-extension-negative.glb",
    "bad-length-negative.glb"
)

function Assert-Condition {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

function New-GlbBytes {
    param([string]$Json, [int]$BinByteLength, [switch]$BadLength)

    $jsonBytes = [System.Collections.Generic.List[byte]]::new()
    $jsonBytes.AddRange($Utf8NoBom.GetBytes($Json))
    while (($jsonBytes.Count % 4) -ne 0) { $jsonBytes.Add([byte]0x20) }
    Assert-Condition ($BinByteLength -gt 0 -and ($BinByteLength % 4) -eq 0) `
        "Normalizer fixture BIN length must be positive and aligned."
    [byte[]]$binBytes = [byte[]]::new($BinByteLength)
    $declaredLength = [uint32](12 + 8 + $jsonBytes.Count + 8 + $binBytes.Length)
    if ($BadLength) { $declaredLength = [uint32]($declaredLength + 4) }

    $stream = [System.IO.MemoryStream]::new()
    $writer = [System.IO.BinaryWriter]::new($stream, $Utf8NoBom, $true)
    try {
        $writer.Write([byte[]](0x67, 0x6c, 0x54, 0x46))
        $writer.Write([uint32]2)
        $writer.Write($declaredLength)
        $writer.Write([uint32]$jsonBytes.Count)
        $writer.Write([uint32]0x4e4f534a)
        $writer.Write($jsonBytes.ToArray())
        $writer.Write([uint32]$binBytes.Length)
        $writer.Write([uint32]0x004e4942)
        $writer.Write($binBytes)
        return $stream.ToArray()
    } finally {
        $writer.Dispose()
        $stream.Dispose()
    }
}

function Write-Bytes {
    param([string]$Root, [string]$Name, [byte[]]$Bytes)
    [System.IO.File]::WriteAllBytes((Join-Path $Root $Name), $Bytes)
}

function New-Record {
    param([string]$Root, [string]$Name)
    $path = Join-Path $Root $Name
    return [ordered]@{
        path = $Name
        size = (Get-Item -LiteralPath $path).Length
        sha256 = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
    }
}

$mesh = '{"accessors":[{"bufferView":0,"componentType":5126,"count":3,"max":[0,0,0],"min":[0,0,0],"type":"VEC3"}],"asset":{"version":"2.0"},"bufferViews":[{"buffer":0,"byteLength":36}],"buffers":[{"byteLength":36}],"meshes":[{"primitives":[{"attributes":{"POSITION":0},"mode":4}]}],"nodes":[{"mesh":0}],"scene":0,"scenes":[{"nodes":[0]}]}'
$point = '{"accessors":[{"bufferView":0,"componentType":5126,"count":1,"max":[0,0,0],"min":[0,0,0],"type":"VEC3"}],"asset":{"version":"2.0"},"bufferViews":[{"buffer":0,"byteLength":12}],"buffers":[{"byteLength":12}],"meshes":[{"primitives":[{"attributes":{"POSITION":0},"mode":0}]}],"nodes":[{"mesh":0}],"scene":0,"scenes":[{"nodes":[0]}]}'
$instance = '{"accessors":[{"bufferView":0,"componentType":5126,"count":3,"max":[0,0,0],"min":[0,0,0],"type":"VEC3"}],"asset":{"version":"2.0"},"bufferViews":[{"buffer":0,"byteLength":36}],"buffers":[{"byteLength":36}],"extensionsRequired":["EXT_mesh_gpu_instancing"],"extensionsUsed":["EXT_mesh_gpu_instancing"],"meshes":[{"primitives":[{"attributes":{"POSITION":0},"mode":4}]}],"nodes":[{"extensions":{"EXT_mesh_gpu_instancing":{"attributes":{"TRANSLATION":0}}},"mesh":0}],"scene":0,"scenes":[{"nodes":[0]}]}'
$external = $mesh.Replace('"buffers":[{"byteLength":36}]', '"buffers":[{"byteLength":36,"uri":"outside.bin"}]')
$unknown = $mesh.Replace('"meshes"', '"extensionsRequired":["VENDOR_unknown_required"],"extensionsUsed":["VENDOR_unknown_required"],"meshes"')

$temporaryDirectory = $null
if ($VerifyOnly) {
    $temporaryDirectory = Join-Path ([System.IO.Path]::GetTempPath()) `
        ("normalizer-fixtures-" + [Guid]::NewGuid().ToString("N"))
    $workingDirectory = $temporaryDirectory
} else {
    $workingDirectory = $OutputDirectory
}
New-Item -ItemType Directory -Path $workingDirectory -Force | Out-Null

try {
    Write-Bytes $workingDirectory "mesh-positive.glb" (New-GlbBytes $mesh 36)
    Write-Bytes $workingDirectory "point-positive.glb" (New-GlbBytes $point 12)
    Write-Bytes $workingDirectory "instance-positive.glb" (New-GlbBytes $instance 36)
    Write-Bytes $workingDirectory "external-uri-negative.glb" (New-GlbBytes $external 36)
    Write-Bytes $workingDirectory "unknown-required-extension-negative.glb" (New-GlbBytes $unknown 36)
    Write-Bytes $workingDirectory "bad-length-negative.glb" (New-GlbBytes $mesh 36 -BadLength)

    $records = @($ExpectedFiles | ForEach-Object { New-Record $workingDirectory $_ })
    $manifest = [ordered]@{
        schemaVersion = 1
        generatorVersion = $GeneratorVersion
        files = $records
    }
    $manifestJson = ($manifest | ConvertTo-Json -Depth 6).Replace("`r`n", "`n").TrimEnd() + "`n"
    if ($VerifyOnly) {
        $committedPath = Join-Path $OutputDirectory $ManifestName
        Assert-Condition (Test-Path -LiteralPath $committedPath) `
            "Committed normalizer fixture manifest is missing."
        $committed = (Get-Content -LiteralPath $committedPath -Raw).Replace("`r`n", "`n")
        Assert-Condition ($committed -eq $manifestJson) `
            "Normalizer fixture bytes differ from the committed manifest."
        Write-Host "Verified $($records.Count) Normalizer V1 canonical fixtures."
    } else {
        [System.IO.File]::WriteAllText((Join-Path $workingDirectory $ManifestName),
            $manifestJson, $Utf8NoBom)
        Write-Host "Generated $($records.Count) Normalizer V1 canonical fixtures."
    }
} finally {
    if ($VerifyOnly -and $null -ne $temporaryDirectory `
            -and (Test-Path -LiteralPath $temporaryDirectory)) {
        $resolved = (Resolve-Path -LiteralPath $temporaryDirectory).Path
        $systemTemp = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath())
        Assert-Condition ($resolved.StartsWith($systemTemp,
            [System.StringComparison]::OrdinalIgnoreCase)) `
            "Refusing to remove a non-temporary fixture directory."
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
}
