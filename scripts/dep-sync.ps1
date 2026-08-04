# Copyright 2026 Metaversal Corporation
#
# Shared helper for build-windows.ps1. Dependency order and pins now come from
# the manifest (deps/dependencies.json) via tools/DepGraph/depgraph.py, and the
# read-only checkout gate lives in deps/verify.cmake -- so the old
# $DepsOrdered-vs-CMake cross-check and pin regexes are gone. This file keeps
# only stamp invalidation: drop a dep's .done stamp when its recipe or the deps
# CMakeLists changed, so the next -Deps rebuilds it.

function Update-DepStamps {
   param (
      [Parameter(Mandatory)] [string]   $DepsSourceDir,
      [Parameter(Mandatory)] [string[]] $Deps,
      [Parameter(Mandatory)] [string]   $StampDir
   )

   New-Item -ItemType Directory -Force -Path $StampDir | Out-Null

   $cmakeLists  = Get-Item (Join-Path $DepsSourceDir 'CMakeLists.txt')
   $invalidated = New-Object 'System.Collections.Generic.List[string]'

   foreach ($dep in $Deps) {
      $cmakeFile = Join-Path $DepsSourceDir "$dep.cmake"
      $stampFile = Join-Path $StampDir "$dep.done"

      if (-not (Test-Path $stampFile)) {
         continue
      }

      $stampTime = (Get-Item $stampFile).LastWriteTimeUtc
      $reason    = $null

      if ((Test-Path $cmakeFile) -and (Get-Item $cmakeFile).LastWriteTimeUtc -gt $stampTime) {
         $reason = 'recipe'
      }
      elseif ($cmakeLists.LastWriteTimeUtc -gt $stampTime) {
         $reason = 'CMakeLists'
      }

      if ($reason) {
         Remove-Item -Force -ErrorAction SilentlyContinue $stampFile
         $invalidated.Add("$dep ($reason)")
      }
   }

   if ($invalidated.Count -gt 0) {
      Write-Host "  Dep stamps cleared (newer CMake): $($invalidated -join ', ')"
   }
}
