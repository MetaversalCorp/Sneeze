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
# ci-tier-matrix.py -- emit GitHub Actions strategy.matrix JSON for each
# longest-path layer of deps/dependencies.json. Single source of truth: the
# manifest. Replaces hand-maintained tier lists + ci-check-tiers.py.
#
#   python3 scripts/ci-tier-matrix.py              # human summary to stdout
#   python3 scripts/ci-tier-matrix.py --github-output >> "$GITHUB_OUTPUT"
#
# --github-output writes (for each layer i = 0 .. max):
#   tier{i}={"include":[{"dep":"name"},...]}
#   has_tier{i}=true|false
#   ntiers=<count>
# Fails if the graph needs more layers than --max-tier (default 4 => tiers 0..4).

import argparse
import json
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.normpath(os.path.join(_HERE, ".."))
_DEFAULT_MANIFEST = os.path.join(_ROOT, "deps", "dependencies.json")


def _load(path):
   with open(path, "r", encoding="utf-8") as f:
      data = json.load(f)
   data.setdefault("versions", {})
   data.setdefault("dependencies", {})
   return data


def _layers(data):
   versions = data["versions"]
   edges = data["dependencies"]
   layer = {}

   def depth(name, stack):
      if name in layer:
         return layer[name]
      if name in stack:
         sys.exit("ci-tier-matrix: cycle involving %s" % name)
      deps = edges.get(name, [])
      for d in deps:
         if d not in versions:
            sys.exit("ci-tier-matrix: '%s' depends on unknown dep '%s'" % (name, d))
      stack = stack + [name]
      layer[name] = 0 if not deps else 1 + max(depth(d, stack) for d in deps)
      return layer[name]

   for name in versions:
      depth(name, [])

   by = {}
   for name, L in layer.items():
      by.setdefault(L, []).append(name)
   for L in by:
      by[L].sort()
   return by


def _matrix_json(names):
   return json.dumps({"include": [{"dep": n} for n in names]}, separators=(",", ":"))


def main():
   ap = argparse.ArgumentParser(description=__doc__)
   ap.add_argument("--manifest", default=os.environ.get("SNEEZE_DEP_MANIFEST", _DEFAULT_MANIFEST))
   ap.add_argument("--github-output", action="store_true",
                   help="Write tier{N}=... and has_tier{N}=... lines for GITHUB_OUTPUT")
   ap.add_argument("--max-tier", type=int, default=4,
                   help="Highest tier index allowed (default 4 => tiers 0..4)")
   args = ap.parse_args()

   data = _load(args.manifest)
   by = _layers(data)
   if not by:
      sys.exit("ci-tier-matrix: no deps in manifest")

   max_L = max(by.keys())
   if max_L > args.max_tier:
      sys.exit(
         "ci-tier-matrix: graph needs layer %d but --max-tier=%d; "
         "add a tier job to build-platform.yml or raise --max-tier"
         % (max_L, args.max_tier))

   # Human summary always on stdout (Actions logs).
   for L in range(0, args.max_tier + 1):
      names = by.get(L, [])
      print("tier%d (%d): %s" % (L, len(names), ", ".join(names) if names else "(empty)"))

   if args.github_output:
      out = os.environ.get("GITHUB_OUTPUT")
      lines = []
      lines.append("ntiers=%d" % (max_L + 1))
      for L in range(0, args.max_tier + 1):
         names = by.get(L, [])
         lines.append("tier%d=%s" % (L, _matrix_json(names)))
         lines.append("has_tier%d=%s" % (L, "true" if names else "false"))
      text = "\n".join(lines) + "\n"
      if out:
         with open(out, "a", encoding="utf-8") as f:
            f.write(text)
      else:
         # Still emit to stdout so `>> $GITHUB_OUTPUT` redirection works.
         sys.stdout.write(text)

   return 0


if __name__ == "__main__":
   sys.exit(main())