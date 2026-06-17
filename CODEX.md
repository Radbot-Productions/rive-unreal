# Radbot Rive Plugin Notes

This plugin is a source-vendored Rive Unreal plugin with Radbot fixes layered on top of the upstream plugin used by Skewered.

Runtime source:

- The Rive C++ runtime source lives at `Plugins/Rive/submodules/rive-runtime`.
- The submodule tracks `https://github.com/Radbot-Productions/rive-runtime.git` on `codex/rive-hug-layout-fix`.
- Do not replace the full plugin tree from `Radbot-Productions/rive-unreal` without checking the version first. Skewered currently carries newer plugin source than the validated test fork, so merge fixes intentionally.

Build/install command:

```powershell
.\Plugins\Rive\Scripts\BuildRadbotRive.ps1
```

That command initializes the runtime submodule, builds Win64 Rive runtime static libraries, copies them into the plugin's ignored `Source/ThirdParty/RiveLibrary/Libraries/Win64` directory, and then invokes `Scripts/BuildEditor.ps1`.

The Win64 library directory must be treated as a single versioned set even
though it is ignored by git. A mixed set can compile but break Rive hug layout
at runtime. The build script validates the ignored library directory against
`Scripts/RiveWin64Libraries.manifest.json` before invoking Unreal.

Useful variants:

```powershell
.\Plugins\Rive\Scripts\BuildRadbotRive.ps1 -ReleaseOnly -SkipUnrealBuild
.\Plugins\Rive\Scripts\BuildRadbotRive.ps1 -SkipRuntimeBuild
.\Plugins\Rive\Scripts\BuildRadbotRive.ps1 -CleanRuntime
.\Plugins\Rive\Scripts\BuildRadbotRive.ps1 -SkipRuntimeBuild -RiveLibrariesSourceDir F:\path\to\known-good\Win64
```

For local machines that keep a reusable prebuilt library cache, set
`RADBOT_RIVE_WIN64_LIBRARIES` to the known-good `Win64` directory and run:

```powershell
.\Plugins\Rive\Scripts\BuildRadbotRive.ps1 -SkipRuntimeBuild
```

Use `-AllowUnverifiedRiveLibraries` only while deliberately testing a new
native Rive build; do not use it for the known hug-layout build.

Only use `-SyncHeadersAndShaders` when intentionally refreshing tracked Rive runtime headers and generated shader files from the submodule. It can dirty many tracked files.

Do not commit generated or binary payloads:

- `Plugins/Rive/Binaries`
- `Plugins/Rive/Intermediate`
- `Plugins/Rive/Build`
- `Plugins/Rive/DerivedDataCache`
- `Plugins/Rive/Source/ThirdParty/RiveLibrary/Libraries`

The root `.gitignore` already excludes these paths.
