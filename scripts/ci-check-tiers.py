#!/usr/bin/env python3
# Copyright 2026 Metaversal Corporation
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# ci-check-tiers.py -- guard against drift between the hand-maintained CI tier
# matrices in .github/workflows/build-platform.yml and the dependency manifest
# deps/dependencies.json. The CI tiers are a manual topological LAYERING of the
# graph (each dep built after its dependencies); this asserts that layering is
# still valid and complete. Exits non-zero (listing every problem) on drift.
#
# Checks:
#   1. Every manifest dep appears in exactly one tier, and every tier dep is in
#      the manifest (no missing / extra / duplicate).
#   2. Every dep's DIRECT dependencies sit in a strictly EARLIER tier (so the
#      artifact exists before the dependent builds).
#
# Stdlib only; parses the YAML by structure (no PyYAML dependency on runners).

import json
import os
import re
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.normpath(os.path.join(_HERE, ".."))
_MANIFEST = os.path.join(_ROOT, "deps", "dependencies.json")
_WORKFLOW = os.path.join(_ROOT, ".github", "workflows", "build-platform.yml")

_TIER_JOBS = ["tier0", "tier1", "tier2", "tier3"]


def _load_manifest():
   with open(_MANIFEST, "r", encoding="utf-8") as f:
      data = json.load(f)
   return list(data.get("versions", {}).keys()), data.get("dependencies", {})


def _tier_sections(text):
   # Job headers sit at exactly two-space indent: "  tier0:", "  sneeze:", ...
   headers = list(re.finditer(r"(?m)^  ([A-Za-z0-9_]+):\s*$", text))
   sections = {}
   for i, m in enumerate(headers):
      name = m.group(1)
      start = m.end()
      end = headers[i + 1].start() if i + 1 < len(headers) else len(text)
      sections[name] = text[start:end]
   return sections


def _deps_in_section(body):
   deps = []
   # tier0 style: dep: [a, b, c]
   m = re.search(r"(?m)^\s*dep:\s*\[([^\]]*)\]", body)
   if m:
      deps.extend(d.strip() for d in m.group(1).split(",") if d.strip())
   # tier1+ style: "- dep: name" (matrix include entries)
   deps.extend(re.findall(r"(?m)^\s*-\s*dep:\s*([A-Za-z0-9._-]+)\s*$", body))
   return deps


def main():
   names, edges = _load_manifest()
   with open(_WORKFLOW, "r", encoding="utf-8") as f:
      sections = _tier_sections(f.read())

   tier_of = {}
   problems = []

   for idx, job in enumerate(_TIER_JOBS):
      if job not in sections:
         problems.append("workflow is missing job '%s'" % job)
         continue
      for dep in _deps_in_section(sections[job]):
         if dep in tier_of:
            problems.append("dep '%s' appears in multiple tiers (%s and %s)" % (dep, _TIER_JOBS[tier_of[dep]], job))
         else:
            tier_of[dep] = idx

   manifest_set = set(names)
   ci_set = set(tier_of.keys())

   for dep in sorted(manifest_set - ci_set):
      problems.append("manifest dep '%s' is in NO CI tier (add it to a tier matrix)" % dep)
   for dep in sorted(ci_set - manifest_set):
      problems.append("CI tier dep '%s' is not in the manifest" % dep)

   # Layering: a dep's direct dependencies must build in an earlier tier.
   for dep, tier in tier_of.items():
      for req in edges.get(dep, []):
         if req not in tier_of:
            continue  # already reported as missing above
         if tier_of[req] >= tier:
            problems.append(
               "'%s' (%s) depends on '%s' (%s): a dependency must be in an EARLIER tier"
               % (dep, _TIER_JOBS[tier], req, _TIER_JOBS[tier_of[req]]))

   if problems:
      sys.stderr.write("CI tier / manifest drift detected:\n")
      for p in problems:
         sys.stderr.write("  - %s\n" % p)
      return 1

   print("CI tiers consistent with manifest: %d deps across %d tiers." % (len(ci_set), len(_TIER_JOBS)))
   return 0


if __name__ == "__main__":
   sys.exit(main())
