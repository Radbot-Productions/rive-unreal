# Rive Unreal

An Unreal runtime library for [Rive](https://rive.app). We're hoping to gather feedback about the API and feature-set as we expand platform support.

Please see the documentation at https://rive.app/docs/game-runtimes/unreal/unreal

Radbot build notes live in `CODEX.md`. Use
`.\Plugins\Rive\Scripts\BuildRadbotRive.ps1` from the project root to rebuild
the Radbot runtime libraries and then build the Unreal editor target.

The Radbot script validates the ignored Win64 native library set against
`Scripts/RiveWin64Libraries.manifest.json`. For a source-only checkout, either
let the script build the Rive runtime submodule or pass
`-RiveLibrariesSourceDir` / `RADBOT_RIVE_WIN64_LIBRARIES` to hydrate the full
known-good library set before building Unreal.

