---
name: sync-build
description: >-
  Sync the hand-maintained MSVC project with the CMake build system. Use when
  the user says sync build, sync cmake, sync project, update cmake, or wants to
  push changes that include new/removed source files.
---

# SyncBuild — MSVC ↔ CMake Sync (Sneeze)

Synchronize the hand-maintained Visual Studio project (`msvc/Sneeze.vcxproj`) with the
cross-platform CMake build (`src/CMakeLists.txt`). The `.vcxproj` is the source of truth
for source file lists on Windows.

## When to use

- Before committing changes that added or removed source files via Visual Studio
- When the user says "sync build", "sync cmake", "sync project", or "update cmake"
- After pulling changes that modified `CMakeLists.txt` (reverse sync — update `.vcxproj`)

## Procedure

### Step 1 — Generate a reference .vcxproj from CMake

Run CMake to produce a reference project file from the current `CMakeLists.txt`:

```
cmake -S <repo>/src -B <repo>/builds/windows-x64/build -G "Visual Studio 17 2022" -A x64
```

This generates `<repo>/builds/windows-x64/build/Sneeze.vcxproj` reflecting what CMake
thinks the project should contain.

### Step 2 — Compare

Read both project files:
- **Hand-maintained:** `<repo>/msvc/Sneeze.vcxproj`
- **CMake-generated:** `<repo>/builds/windows-x64/build/Sneeze.vcxproj`

Extract and compare:
- `<ClCompile Include="...">` entries (source files)
- `<ClInclude Include="...">` entries (header files)
- `<AdditionalIncludeDirectories>` (include paths)
- `<PreprocessorDefinitions>` (defines)

Normalize paths before comparison — the hand-maintained file uses relative paths with
MSBuild macros (`$(SneezeRoot)`, `$(SneezeDepsRoot)`), while the generated file uses
absolute paths. Convert both to a canonical form (forward-slash, relative to repo root)
for diffing.

### Step 3 — Categorize differences

For each difference found:

- **Files in hand-maintained but not in CMake-generated:** These are new files added in VS.
  Update `src/CMakeLists.txt` to include them in the correct module variable based on path
  prefix and existing grouping. Consider platform guards.

- **Files in CMake-generated but not in hand-maintained:** These may have been added to
  CMake on another platform. Warn the user — do not auto-remove from CMake. If the file
  is Windows-relevant, suggest adding it to the `.vcxproj`.

- **Setting differences (includes, defines):** Report them clearly. Apply if unambiguous.

- **Expected differences:** Ignore CMake-specific entries (ZERO_CHECK, cmake_pch paths,
  custom build rules, CompileKernels resources). These exist only in the generated file.

### Step 4 — Apply changes to CMakeLists.txt

Edit `src/CMakeLists.txt` to reflect new/removed files. Follow existing conventions:
- 3-space indentation
- Files listed one per line
- Forward-slash path separators
- Grouped by module

### Step 5 — Report

Summarize:
- Files added to CMakeLists.txt
- Files that need manual attention (warnings)
- Setting differences detected
- Confirmation that both files are now in sync

## Path conventions

The hand-maintained `.vcxproj` uses these MSBuild property macros:

| Macro | Resolves to | Example |
|-------|-------------|---------|
| `$(SneezeRoot)` | Sneeze repo root | `E:\Dev\OMB\Sneeze` |
| `$(SneezeDepsRoot)` | Sneeze deps root | `E:\Dev\OMB\Sneeze\deps\builds\windows-x64` |
| `$(CfgLower)` | Lowercase config name | `debug` or `release` |

Source files use relative paths from `msvc/`: `../src/sneeze/Engine.cpp`

## Adding a new Sneeze dependency

Adding a new third-party lib to Sneeze requires edits in **eight** places across
two repos. The CMake side propagates transitively (PUBLIC `target_link_libraries`
reaches every executable that links Sneeze), but the hand-maintained
`.vcxproj` files do not — they each need explicit include-path and lib entries.
Missing any one of these produces a symptom that's easy to misdiagnose:

- Missing #1–#4 → configure/build failure on Linux/macOS CI
- Missing #5 → C++ compile error "cannot open <Dep\Dep.h>" in Sneeze
- Missing #6 (include) → C++ compile error in `Sneeze.vcxproj` (Debug or Release)
- Missing #7 → LNK2019 in `SneezeTest.exe`
- Missing #8 → LNK2019 in Artemis (`Sneeze.lib(...)` references an unresolved
  symbol even though `Sneeze.lib` and the dep both compiled cleanly)

Checklist — apply in order:

| # | File | What to add |
|---|------|-------------|
| 1 | `deps/<dep>.cmake` | `ExternalProject_Add` recipe. Mirror the closest existing dep (RMAP for full-sharing deps, freetype for simple deps). |
| 2 | `deps/CMakeLists.txt` | Add dep name to `SNEEZE_DEPS` list. Add an `add_dependencies (<dep> ...)` block if it depends on other Sneeze deps. |
| 3 | `scripts/build-deps.sh` | Add dep name to `DEPS_ORDERED` (respect topological order: deps before dependents). |
| 4 | `scripts/build-windows.ps1` | Same — add to `$DepsOrdered` array. Keep the two arrays in lockstep. |
| 5 | `src/CMakeLists.txt` | `find_package (<Dep> CONFIG REQUIRED PATHS ...)`, add to `_tgt` dualization loop, `target_link_libraries (Sneeze PUBLIC <Dep>::<Dep>)`. PUBLIC is required so consumers get transitive propagation. |
| 6 | `msvc/Sneeze.vcxproj` | `AdditionalIncludeDirectories` entries for Debug **and** Release configs. Sneeze is a static lib so nothing to link here. |
| 7 | `msvc/SneezeTest.vcxproj` | Both `AdditionalIncludeDirectories` **and** `AdditionalDependencies` entries for Debug and Release configs. |
| 8 | `Artemis/msvc/Artemis.vcxproj` (separate repo) | Same as #7 — include path + lib entry, both configs. This one is easily forgotten because it's in a different repo. |

Optional #9: `src/sneeze/Pch.h` — add `#include <Dep/Dep.h>` if Sneeze's own code
uses the dep's headers (versus just linking against its lib). If the dep header
collides with a Windows preprocessor macro (as MAP did with `RELATIVE` from
`wingdi.h`), wrap the include with `#pragma push_macro / #undef / pop_macro`.

### Verify

After all eight edits, run three builds to catch drift at each layer:

1. `./scripts/build-linux.sh --only <dep>` — dep builds standalone
2. `./scripts/build-linux.sh --all` — Sneeze links against the dep
3. `.\scripts\build-windows.ps1 -All` then build Artemis from its `.sln` — hand-maintained msvc projects link cleanly

If any layer errors out with LNK2019 or "cannot open", cross-reference the
error against the symptom table above to identify which of the 8 steps was
skipped.

## Important notes

- The `.vcxproj` is the source of truth for file lists on Windows. `CMakeLists.txt` is
  synced FROM it, not the other way around (except when pulling changes from other platforms).
- Sneeze is a static library (`ConfigurationType = StaticLibrary`).
- The host application's repo has its own independent `sync-build` skill with the same pattern.
- `.vcxproj.user` files are gitignored (personal debug settings).
