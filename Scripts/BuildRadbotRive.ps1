param(
    [string]$ProjectRoot = "",
    [string]$Configuration = "Development",
    [string]$Platform = "Win64",
    [switch]$SkipSubmoduleUpdate,
    [switch]$SkipRuntimeBuild,
    [switch]$SkipUnrealBuild,
    [switch]$CleanRuntime,
    [switch]$BootstrapWin64,
    [switch]$ReleaseOnly,
    [switch]$DebugOnly,
    [switch]$SyncHeadersAndShaders,
    [string]$RiveLibrariesSourceDir = "",
    [switch]$AllowUnverifiedRiveLibraries,
    [switch]$RequireExactRiveLibraryHashes,
    [switch]$StrictRiveLibrarySet,
    [switch]$WriteRiveLibraryManifest,
    [switch]$SkipRiveLibraryValidation,
    [switch]$SkipPluginBinaryClean
)

$ErrorActionPreference = "Stop"

function Find-ProjectRoot {
    param([string]$StartPath)

    $current = (Resolve-Path -LiteralPath $StartPath).Path
    if (-not (Get-Item -LiteralPath $current).PSIsContainer) {
        $current = Split-Path -Parent $current
    }

    while ($true) {
        $project = Get-ChildItem -LiteralPath $current -Filter *.uproject -File -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($project) {
            return $current
        }

        $parent = Split-Path -Parent $current
        if ([string]::IsNullOrWhiteSpace($parent) -or $parent -eq $current) {
            throw "Could not find a .uproject above '$StartPath'."
        }

        $current = $parent
    }
}

function Invoke-Checked {
    param(
        [Parameter(Mandatory = $true)]
        [string]$FilePath,

        [string[]]$Arguments = @(),

        [string]$WorkingDirectory = ""
    )

    $command = Get-Command -Name $FilePath -ErrorAction SilentlyContinue
    if (-not $command -and -not (Test-Path -LiteralPath $FilePath)) {
        throw "Command not found: $FilePath"
    }

    $oldLocation = Get-Location
    try {
        if (-not [string]::IsNullOrWhiteSpace($WorkingDirectory)) {
            Set-Location -LiteralPath $WorkingDirectory
        }

        Write-Host "Running: $FilePath $($Arguments -join ' ')"
        & $FilePath @Arguments
        if ($LASTEXITCODE -ne 0) {
            throw "Command failed with exit code $LASTEXITCODE`: $FilePath"
        }
    }
    finally {
        Set-Location $oldLocation
    }
}

function Assert-PathUnderDirectory {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [string]$Directory
    )

    $fullPath = [System.IO.Path]::GetFullPath($Path)
    $fullDirectory = [System.IO.Path]::GetFullPath($Directory).TrimEnd('\') + '\'
    if (-not $fullPath.StartsWith($fullDirectory, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to modify '$fullPath' because it is not under '$fullDirectory'."
    }
}

function Clear-RiveLibraries {
    param(
        [Parameter(Mandatory = $true)]
        [string]$LibrariesDirectory,

        [Parameter(Mandatory = $true)]
        [string]$PluginRoot
    )

    Assert-PathUnderDirectory -Path $LibrariesDirectory -Directory $PluginRoot
    New-Item -ItemType Directory -Force -Path $LibrariesDirectory | Out-Null

    Get-ChildItem -LiteralPath $LibrariesDirectory -Filter *.lib -File -ErrorAction SilentlyContinue | ForEach-Object {
        Remove-Item -LiteralPath $_.FullName -Force
    }
}

function Copy-RiveLibraries {
    param(
        [Parameter(Mandatory = $true)]
        [string]$SourceDirectory,

        [Parameter(Mandatory = $true)]
        [string]$DestinationDirectory,

        [Parameter(Mandatory = $true)]
        [bool]$IsRelease
    )

    if (-not (Test-Path -LiteralPath $SourceDirectory)) {
        throw "Rive build output directory does not exist: $SourceDirectory"
    }

    New-Item -ItemType Directory -Force -Path $DestinationDirectory | Out-Null

    $libs = Get-ChildItem -LiteralPath $SourceDirectory -Filter *.lib -File
    if ($libs.Count -eq 0) {
        throw "No .lib files found in '$SourceDirectory'."
    }

    foreach ($lib in $libs) {
        $destinationName = $lib.Name
        if ((-not $destinationName.StartsWith("rive")) -and ($destinationName -notlike "*luau*")) {
            $destinationName = "rive_$destinationName"
        }

        if (-not $IsRelease) {
            $baseName = [System.IO.Path]::GetFileNameWithoutExtension($destinationName)
            $extension = [System.IO.Path]::GetExtension($destinationName)
            $destinationName = "$($baseName)_d$extension"
        }

        Copy-Item -LiteralPath $lib.FullName -Destination (Join-Path $DestinationDirectory $destinationName) -Force
        Write-Host "Copied $($lib.Name) -> $destinationName"
    }
}

function Copy-RiveLibrariesClean {
    param(
        [Parameter(Mandatory = $true)]
        [string]$SourceDirectory,

        [Parameter(Mandatory = $true)]
        [string]$DestinationDirectory,

        [Parameter(Mandatory = $true)]
        [string]$PluginRoot
    )

    if (-not (Test-Path -LiteralPath $SourceDirectory)) {
        throw "Rive library source directory does not exist: $SourceDirectory"
    }

    $resolvedSource = (Resolve-Path -LiteralPath $SourceDirectory).Path
    $resolvedDestination = [System.IO.Path]::GetFullPath($DestinationDirectory)
    if ($resolvedSource.TrimEnd('\') -eq $resolvedDestination.TrimEnd('\')) {
        throw "Rive library source and destination are the same directory: $resolvedSource"
    }

    $sourceLibraries = Get-ChildItem -LiteralPath $SourceDirectory -Filter *.lib -File
    if ($sourceLibraries.Count -eq 0) {
        throw "No .lib files found in Rive library source directory '$SourceDirectory'."
    }

    Clear-RiveLibraries -LibrariesDirectory $DestinationDirectory -PluginRoot $PluginRoot

    foreach ($library in $sourceLibraries) {
        Copy-Item -LiteralPath $library.FullName -Destination (Join-Path $DestinationDirectory $library.Name) -Force
        Write-Host "Hydrated $($library.Name)"
    }
}

function Test-RiveLibraries {
    param(
        [Parameter(Mandatory = $true)]
        [string]$LibrariesDirectory,

        [Parameter(Mandatory = $true)]
        [string]$ManifestPath,

        [Parameter(Mandatory = $true)]
        [bool]$CheckHashes,

        [Parameter(Mandatory = $true)]
        [bool]$StrictLibrarySet
    )

    if (-not (Test-Path -LiteralPath $ManifestPath)) {
        throw "Rive library manifest not found: $ManifestPath"
    }

    if (-not (Test-Path -LiteralPath $LibrariesDirectory)) {
        throw "Rive library directory does not exist: $LibrariesDirectory"
    }

    $manifest = Get-Content -LiteralPath $ManifestPath -Raw | ConvertFrom-Json
    $expected = @{}
    foreach ($entry in $manifest.libraries) {
        $expected[$entry.name] = $entry
    }

    $actualFiles = Get-ChildItem -LiteralPath $LibrariesDirectory -Filter *.lib -File | Sort-Object Name
    $actual = @{}
    foreach ($file in $actualFiles) {
        $actual[$file.Name] = $file
    }

    $errors = New-Object System.Collections.Generic.List[string]

    foreach ($expectedName in ($expected.Keys | Sort-Object)) {
        if (-not $actual.ContainsKey($expectedName)) {
            $errors.Add("missing $expectedName")
            continue
        }

        $file = $actual[$expectedName]
        if ([int64]$file.Length -ne [int64]$expected[$expectedName].size) {
            $errors.Add("size mismatch $expectedName expected=$($expected[$expectedName].size) actual=$($file.Length)")
        }

        if ($CheckHashes) {
            $hash = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash
            if ($hash -ne $expected[$expectedName].sha256) {
                $errors.Add("hash mismatch $expectedName")
            }
        }
    }

    foreach ($actualName in ($actual.Keys | Sort-Object)) {
        if (-not $expected.ContainsKey($actualName)) {
            if ($StrictLibrarySet) {
                $errors.Add("unexpected $actualName")
            }
            else {
                Write-Warning "Ignoring extra Rive library not listed in manifest: $actualName"
            }
        }
    }

    if ($errors.Count -gt 0) {
        $message = "Rive Win64 libraries do not match $ManifestPath`n - " + ($errors -join "`n - ")
        throw $message
    }

    Write-Host "Verified $($expected.Count) Rive Win64 libraries in $LibrariesDirectory"
}

function Write-RiveLibrariesManifest {
    param(
        [Parameter(Mandatory = $true)]
        [string]$LibrariesDirectory,

        [Parameter(Mandatory = $true)]
        [string]$ManifestPath
    )

    if (-not (Test-Path -LiteralPath $LibrariesDirectory)) {
        throw "Rive library directory does not exist: $LibrariesDirectory"
    }

    $libraries = Get-ChildItem -LiteralPath $LibrariesDirectory -Filter *.lib -File | Sort-Object Name
    if ($libraries.Count -eq 0) {
        throw "No .lib files found in '$LibrariesDirectory'."
    }

    $manifest = [ordered]@{
        description = "Radbot Rive Win64 static library fingerprint generated from Plugins/Rive/submodules/rive-runtime. Libraries are intentionally not committed; run BuildRadbotRive.ps1 -BootstrapWin64 to recreate them."
        libraries = @(
            foreach ($library in $libraries) {
                [ordered]@{
                    name = $library.Name
                    size = [int64]$library.Length
                    sha256 = (Get-FileHash -LiteralPath $library.FullName -Algorithm SHA256).Hash
                }
            }
        )
    }

    $manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $ManifestPath -Encoding UTF8
    Write-Host "Wrote Rive Win64 library manifest: $ManifestPath"
}

function Clear-RivePluginBuildProducts {
    param(
        [Parameter(Mandatory = $true)]
        [string]$PluginRoot
    )

    foreach ($relativePath in @("Binaries", "Intermediate\Build")) {
        $path = Join-Path $PluginRoot $relativePath
        if (Test-Path -LiteralPath $path) {
            Assert-PathUnderDirectory -Path $path -Directory $PluginRoot
            try {
                Remove-Item -LiteralPath $path -Recurse -Force
                Write-Host "Removed stale Rive build product: $path"
            }
            catch {
                Write-Warning "Could not remove stale Rive build product '$path'. Close Unreal Editor or any process locking Rive DLLs before the Unreal build phase. $($_.Exception.Message)"
            }
        }
    }
}

function Copy-DirectoryContentsClean {
    param(
        [Parameter(Mandatory = $true)]
        [string]$SourceDirectory,

        [Parameter(Mandatory = $true)]
        [string]$DestinationDirectory
    )

    if (-not (Test-Path -LiteralPath $SourceDirectory)) {
        throw "Source directory does not exist: $SourceDirectory"
    }

    if (Test-Path -LiteralPath $DestinationDirectory) {
        Remove-Item -LiteralPath $DestinationDirectory -Recurse -Force
    }

    New-Item -ItemType Directory -Force -Path $DestinationDirectory | Out-Null
    Copy-Item -Path (Join-Path $SourceDirectory "*") -Destination $DestinationDirectory -Recurse -Force
}

function Sync-RiveHeadersAndShaders {
    param(
        [Parameter(Mandatory = $true)]
        [string]$RuntimeRoot,

        [Parameter(Mandatory = $true)]
        [string]$PluginRoot
    )

    $includesDestination = Join-Path $PluginRoot "Source\ThirdParty\RiveLibrary\Includes"

    Assert-PathUnderDirectory -Path $includesDestination -Directory $PluginRoot
    if (Test-Path -LiteralPath $includesDestination) {
        Remove-Item -LiteralPath $includesDestination -Recurse -Force
    }

    New-Item -ItemType Directory -Force -Path $includesDestination | Out-Null
    Copy-Item -Path (Join-Path $RuntimeRoot "include\*") -Destination $includesDestination -Recurse -Force
    Copy-Item -Path (Join-Path $RuntimeRoot "renderer\include\*") -Destination $includesDestination -Recurse -Force
    Copy-Item -Path (Join-Path $RuntimeRoot "decoders\include\*") -Destination $includesDestination -Recurse -Force

    $generatedIncludes = Join-Path $RuntimeRoot "out\windows\release\include"
    if (Test-Path -LiteralPath $generatedIncludes) {
        $generatedSource = Join-Path $generatedIncludes "generated"
        if (Test-Path -LiteralPath $generatedSource) {
            $generatedDestination = Join-Path $includesDestination "rive\generated"
            New-Item -ItemType Directory -Force -Path $generatedDestination | Out-Null
            Copy-Item -Path (Join-Path $generatedSource "*") -Destination $generatedDestination -Recurse -Force
        }

        $libPngSource = Join-Path $generatedIncludes "libpng"
        if (Test-Path -LiteralPath $libPngSource) {
            $libPngDestination = Join-Path $includesDestination "libpng"
            New-Item -ItemType Directory -Force -Path $libPngDestination | Out-Null
            Copy-Item -Path (Join-Path $libPngSource "*") -Destination $libPngDestination -Recurse -Force
        }
    }

    $shaderSource = Join-Path $RuntimeRoot "renderer\src\shaders"
    if (Test-Path -LiteralPath $shaderSource) {
        $shaderDestination = Join-Path $includesDestination "rive\shaders"
        New-Item -ItemType Directory -Force -Path $shaderDestination | Out-Null
        foreach ($shaderName in @("constants.glsl", "flush_uniforms.glsl", "image_draw_uniforms.glsl")) {
            $shaderPath = Join-Path $shaderSource $shaderName
            if (Test-Path -LiteralPath $shaderPath) {
                Copy-Item -LiteralPath $shaderPath -Destination (Join-Path $shaderDestination $shaderName) -Force
            }
        }
    }

    $mathTypesPath = Join-Path $includesDestination "rive\math\math_types.hpp"
    if (Test-Path -LiteralPath $mathTypesPath) {
        $content = Get-Content -LiteralPath $mathTypesPath -Raw
        if ($content -notmatch "#ifdef PI\s*`r?`n#undef PI") {
            $content = $content -replace "namespace math\s*\{", "#ifdef PI`r`n#undef PI`r`n#endif`r`n`r`nnamespace math`r`n{"
            Set-Content -LiteralPath $mathTypesPath -Value $content -Encoding UTF8
        }
    }
}

function Resolve-RiveShellDirectory {
    $shCommand = Get-Command -Name "sh.exe" -ErrorAction SilentlyContinue
    if ($shCommand) {
        return Split-Path -Parent $shCommand.Source
    }

    $candidatePaths = @(
        "C:\Program Files\Git\bin\sh.exe",
        "C:\Program Files\Git\usr\bin\sh.exe",
        "C:\Program Files (x86)\Git\bin\sh.exe",
        "C:\Program Files (x86)\Git\usr\bin\sh.exe"
    )

    foreach ($candidatePath in $candidatePaths) {
        if (Test-Path -LiteralPath $candidatePath) {
            return Split-Path -Parent $candidatePath
        }
    }

    throw "Could not find sh.exe. Install Git for Windows or add Git's bin directory to PATH."
}

function Resolve-RiveMakeDirectory {
    $makeCommand = Get-Command -Name "make.exe" -ErrorAction SilentlyContinue
    if ($makeCommand) {
        return Split-Path -Parent $makeCommand.Source
    }

    $candidatePaths = @(
        "C:\msys64\usr\bin\make.exe",
        "C:\msys64\mingw64\bin\mingw32-make.exe",
        "C:\Program Files\Git\usr\bin\make.exe",
        "C:\Program Files (x86)\Git\usr\bin\make.exe"
    )

    foreach ($candidatePath in $candidatePaths) {
        if (Test-Path -LiteralPath $candidatePath) {
            return Split-Path -Parent $candidatePath
        }
    }

    throw "Could not find make.exe. Install MSYS2 or add make.exe to PATH."
}

function Resolve-RivePythonDirectory {
    $candidatePaths = @(
        "C:\msys64\ucrt64\bin\python3.exe",
        "C:\msys64\usr\bin\python3.exe",
        "C:\Python312\python.exe",
        "C:\Python314\python.exe"
    )

    foreach ($candidatePath in $candidatePaths) {
        if (Test-Path -LiteralPath $candidatePath) {
            return Split-Path -Parent $candidatePath
        }
    }

    $pythonCommand = Get-Command -Name "python3.exe" -ErrorAction SilentlyContinue
    if ($pythonCommand -and $pythonCommand.Source -notlike "*WindowsApps*") {
        return Split-Path -Parent $pythonCommand.Source
    }

    throw "Could not find a usable python3.exe. Install Python or MSYS2 Python and add it to PATH."
}

function Resolve-RiveMsvcToolsetVersion {
    $msvcRoot = [Environment]::GetEnvironmentVariable("MSVC_ROOT", "Process")
    if ([string]::IsNullOrWhiteSpace($msvcRoot)) {
        $msvcRoot = "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC"
    }

    if (-not (Test-Path -LiteralPath $msvcRoot)) {
        throw "MSVC toolset root not found: $msvcRoot"
    }

    $versions = Get-ChildItem -LiteralPath $msvcRoot -Directory | Select-Object -ExpandProperty Name
    if ($versions.Count -eq 0) {
        throw "No MSVC toolset versions found under '$msvcRoot'."
    }

    $preferred = $versions | Where-Object { $_ -like "14.38*" } | Sort-Object -Descending | Select-Object -First 1
    if ($preferred) {
        return $preferred
    }

    return ($versions | Sort-Object -Descending | Select-Object -First 1)
}

if ($ReleaseOnly -and $DebugOnly) {
    throw "Use either -ReleaseOnly or -DebugOnly, not both."
}

if ($BootstrapWin64) {
    $Platform = "Win64"
    $CleanRuntime = $true
    $SyncHeadersAndShaders = $true
    $SkipUnrealBuild = $true
    $RequireExactRiveLibraryHashes = $true
    $StrictRiveLibrarySet = $true
    $WriteRiveLibraryManifest = $true
}

$pluginRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..")).Path
if ([string]::IsNullOrWhiteSpace($ProjectRoot)) {
    $ProjectRoot = Find-ProjectRoot -StartPath $pluginRoot
}
else {
    $ProjectRoot = (Resolve-Path -LiteralPath $ProjectRoot).Path
}

$runtimeRoot = Join-Path $pluginRoot "submodules\rive-runtime"
$librariesRoot = Join-Path $pluginRoot "Source\ThirdParty\RiveLibrary\Libraries\Win64"
$librariesManifest = Join-Path $PSScriptRoot "RiveWin64Libraries.manifest.json"
$didHydrateRiveLibraries = $false

if (-not $SkipSubmoduleUpdate) {
    Invoke-Checked -FilePath "git" -Arguments @("-C", $pluginRoot, "submodule", "update", "--init", "--recursive", "--", "submodules/rive-runtime")
}

if (-not (Test-Path -LiteralPath (Join-Path $runtimeRoot "build\build_rive.ps1"))) {
    throw "Rive runtime submodule is not initialized at '$runtimeRoot'."
}

if (-not $SkipRuntimeBuild) {
    Clear-RiveLibraries -LibrariesDirectory $librariesRoot -PluginRoot $pluginRoot

    $targets = @(
        "rive",
        "rive_decoders",
        "rive_harfbuzz",
        "rive_pls_renderer",
        "rive_sheenbidi",
        "miniaudio",
        "rive_yoga",
        "libpng",
        "libjpeg",
        "libwebp",
        "zlib",
        "luau_vm"
    )

    $buildKinds = @()
    if (-not $DebugOnly) {
        $buildKinds += @{ Name = "release"; IsRelease = $true; Args = @("release") }
    }
    if (-not $ReleaseOnly) {
        $buildKinds += @{ Name = "debug"; IsRelease = $false; Args = @() }
    }

    foreach ($buildKind in $buildKinds) {
        $outDir = Join-Path $runtimeRoot "out\windows\$($buildKind.Name)"
        $env:RIVE_OUT = "..\out\windows\$($buildKind.Name)"
        $shellDirectory = Resolve-RiveShellDirectory
        $makeDirectory = Resolve-RiveMakeDirectory
        $pythonDirectory = Resolve-RivePythonDirectory
        $env:MSYS2_ARG_CONV_EXCL = "/I;/T;/Fh;/D;/E;/Zi"
        Remove-Item Env:\MSYS_NO_PATHCONV -ErrorAction SilentlyContinue
        $env:PATH = "$shellDirectory;$makeDirectory;$pythonDirectory;$($env:PATH);$(Join-Path $runtimeRoot "build")"

        $buildArgs = @()
        if ($CleanRuntime) {
            $buildArgs += "clean"
        }
        $buildArgs += @("--with_rive_audio=external", "--for_unreal", "--cpp20")
        $buildArgs += "--toolsversion=$(Resolve-RiveMsvcToolsetVersion)"
        $buildArgs += $buildKind.Args
        $buildArgs += "--with_rive_scripting"
        $buildArgs += "--"
        $buildArgs += $targets

        Invoke-Checked -FilePath "powershell.exe" -Arguments (@("-ExecutionPolicy", "Bypass", "-File", (Join-Path $runtimeRoot "build\build_rive.ps1")) + $buildArgs) -WorkingDirectory (Join-Path $runtimeRoot "renderer")
        Copy-RiveLibraries -SourceDirectory $outDir -DestinationDirectory $librariesRoot -IsRelease $buildKind.IsRelease
    }

    $didHydrateRiveLibraries = $true
}
elseif (-not [string]::IsNullOrWhiteSpace($RiveLibrariesSourceDir)) {
    $resolvedSource = (Resolve-Path -LiteralPath $RiveLibrariesSourceDir).Path
    Copy-RiveLibrariesClean -SourceDirectory $resolvedSource -DestinationDirectory $librariesRoot -PluginRoot $pluginRoot
    $didHydrateRiveLibraries = $true
}
elseif (-not [string]::IsNullOrWhiteSpace($env:RADBOT_RIVE_WIN64_LIBRARIES)) {
    $resolvedSource = (Resolve-Path -LiteralPath $env:RADBOT_RIVE_WIN64_LIBRARIES).Path
    Copy-RiveLibrariesClean -SourceDirectory $resolvedSource -DestinationDirectory $librariesRoot -PluginRoot $pluginRoot
    $didHydrateRiveLibraries = $true
}
else {
    Write-Host "Using existing Rive Win64 libraries in $librariesRoot"
}

if ($SyncHeadersAndShaders) {
    Sync-RiveHeadersAndShaders -RuntimeRoot $runtimeRoot -PluginRoot $pluginRoot
}

if ($WriteRiveLibraryManifest) {
    Write-RiveLibrariesManifest -LibrariesDirectory $librariesRoot -ManifestPath $librariesManifest
}

if (-not $SkipRiveLibraryValidation) {
    Test-RiveLibraries `
        -LibrariesDirectory $librariesRoot `
        -ManifestPath $librariesManifest `
        -CheckHashes:($RequireExactRiveLibraryHashes -and -not $AllowUnverifiedRiveLibraries) `
        -StrictLibrarySet:$StrictRiveLibrarySet
}

if ($didHydrateRiveLibraries -and -not $SkipPluginBinaryClean) {
    Clear-RivePluginBuildProducts -PluginRoot $pluginRoot
}

if (-not $SkipUnrealBuild) {
    $buildEditor = Join-Path $ProjectRoot "Scripts\BuildEditor.ps1"
    $projectPath = Get-ChildItem -LiteralPath $ProjectRoot -Filter *.uproject -File | Select-Object -First 1
    if (-not $projectPath) {
        throw "No .uproject found in '$ProjectRoot'."
    }

    Invoke-Checked -FilePath "powershell.exe" -Arguments @(
        "-ExecutionPolicy",
        "Bypass",
        "-File",
        $buildEditor,
        "-ProjectPath",
        $projectPath.FullName,
        "-Configuration",
        $Configuration,
        "-Platform",
        $Platform
    )
}

Write-Host "Radbot Rive build flow completed."
