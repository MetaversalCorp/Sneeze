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
# depgraph.py -- read-only reporting + emitter over deps/dependencies.json (the
# single source of truth). Used by the build scripts (order + pin) and humans
# (why/closure/validate). Python 3 stdlib only; never modifies anything.
#
#   depgraph.py order              topo order, deps before dependents (one/line)
#   depgraph.py pin <dep>          "<folder>\t<ref>\t<url>" for one dep
#   depgraph.py closure <dep>      transitive dependencies of <dep> (one/line)
#   depgraph.py dependents <dep>   transitive dependents of <dep>, topo-ordered
#                                  (the deps that must rebuild when <dep> moves)
#   depgraph.py why <dep>          direct dependents + direct dependencies
#   depgraph.py validate           check cycles / dangling edges (exit non-zero)
#
# Manifest path: ../../deps/dependencies.json next to this file, or override
# with the SNEEZE_DEP_MANIFEST environment variable.

import json
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
_DEFAULT_MANIFEST = os.path.normpath(os.path.join(_HERE, "..", "..", "deps", "dependencies.json"))


def _manifest_path():
   return os.environ.get("SNEEZE_DEP_MANIFEST", _DEFAULT_MANIFEST)


def _load():
   path = _manifest_path()
   try:
      with open(path, "r", encoding="utf-8") as f:
         data = json.load(f)
   except OSError as e:
      sys.exit("depgraph: cannot read manifest %s: %s" % (path, e))
   except ValueError as e:
      sys.exit("depgraph: invalid JSON in %s: %s" % (path, e))
   data.setdefault("versions", {})
   data.setdefault("dependencies", {})
   return data


def _edges(data, name):
   return data["dependencies"].get(name, [])


def _topo(data):
   versions = data["versions"]
   order = []
   visited = set()

   def visit(node, stack):
      if node in visited:
         return
      if node in stack:
         cycle = " -> ".join(list(stack) + [node])
         sys.exit("depgraph: dependency cycle: %s" % cycle)
      stack = stack + [node]
      for dep in _edges(data, node):
         if dep not in versions:
            sys.exit("depgraph: '%s' depends on unknown dep '%s'" % (node, dep))
         visit(dep, stack)
      visited.add(node)
      order.append(node)

   # Iterate in manifest declaration order for a stable, readable result.
   for name in versions.keys():
      visit(name, [])
   return order


def _closure(data, name):
   result = []
   work = list(_edges(data, name))
   while work:
      cur = work.pop(0)
      if cur not in result:
         result.append(cur)
         work.extend(_edges(data, cur))
   return result


def _dependents(data, name):
   # Transitive set of deps that directly or indirectly depend on `name`.
   seen = set()
   frontier = [name]
   while frontier:
      cur = frontier.pop(0)
      for node in data["versions"]:
         if cur in _edges(data, node) and node not in seen:
            seen.add(node)
            frontier.append(node)
   return seen


def cmd_order(data, args):
   for name in _topo(data):
      print(name)
   return 0


def cmd_dependents(data, args):
   if len(args) != 1:
      sys.exit("usage: depgraph.py dependents <dep>")
   name = args[0]
   if name not in data["versions"]:
      sys.exit("depgraph: '%s' is not in the manifest" % name)
   deps = _dependents(data, name)
   # Emit in global topo order so callers rebuild deps before their dependents.
   for node in _topo(data):
      if node in deps:
         print(node)
   return 0


def cmd_pin(data, args):
   if len(args) != 1:
      sys.exit("usage: depgraph.py pin <dep>")
   name = args[0]
   v = data["versions"].get(name)
   if v is None:
      sys.exit("depgraph: '%s' is not in the manifest" % name)
   print("%s\t%s\t%s" % (v.get("folder", name), v.get("ref", ""), v.get("url", "")))
   return 0


def cmd_closure(data, args):
   if len(args) != 1:
      sys.exit("usage: depgraph.py closure <dep>")
   name = args[0]
   if name not in data["versions"]:
      sys.exit("depgraph: '%s' is not in the manifest" % name)
   for dep in _closure(data, name):
      print(dep)
   return 0


def cmd_why(data, args):
   if len(args) != 1:
      sys.exit("usage: depgraph.py why <dep>")
   name = args[0]
   if name not in data["versions"]:
      sys.exit("depgraph: '%s' is not in the manifest" % name)
   direct = _edges(data, name)
   dependents = [n for n in data["versions"] if name in _edges(data, n)]
   print("%s (ref %s)" % (name, data["versions"][name].get("ref", "")))
   print("  depends on:    %s" % (", ".join(direct) if direct else "(nothing)"))
   print("  depended on by: %s" % (", ".join(dependents) if dependents else "(nothing)"))
   return 0


def cmd_validate(data, args):
   versions = data["versions"]
   problems = []
   for name, edges in data["dependencies"].items():
      if name not in versions:
         problems.append("edge source '%s' has no versions entry" % name)
      for dep in edges:
         if dep not in versions:
            problems.append("'%s' depends on unknown dep '%s'" % (name, dep))
   # _topo aborts on a cycle; run it to surface that too.
   _topo(data)
   if problems:
      for p in problems:
         sys.stderr.write("depgraph: %s\n" % p)
      return 1
   print("manifest OK: %d deps, %d edge sets" % (len(versions), len(data["dependencies"])))
   return 0


_COMMANDS = {
   "order": cmd_order,
   "pin": cmd_pin,
   "closure": cmd_closure,
   "dependents": cmd_dependents,
   "why": cmd_why,
   "validate": cmd_validate,
}


def main(argv):
   if len(argv) < 2 or argv[1] not in _COMMANDS:
      sys.stderr.write(__doc__ or "")
      sys.stderr.write("\ncommands: %s\n" % ", ".join(_COMMANDS))
      return 2
   data = _load()
   return _COMMANDS[argv[1]](data, argv[2:])


if __name__ == "__main__":
   sys.exit(main(sys.argv))
