#!/usr/bin/env python3
# Copyright 2026 OMBI
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

# ---------------------------------------------------------------------------
# compact_msf.py - migrate an existing Sneeze MSF JSON document (already in the
# native node schema) to the compact standard:
#
#   * Head collapses to a single human id: { "Self": "<L>-<index>" }
#     (R root, C celestial, T terrestrial, P physical). Class / Index / Parent /
#     Event are dropped (Self encodes class+index; parentage comes from nesting).
#   * Default sub-objects are dropped: all-zero Type, empty Resource, zero
#     Orbit, zero Properties, zero Bound, identity Transform components.
#   * Leaf nodes print on one line; only nodes with children expand.
#
# Payload numbers (Orbit elements, Properties such as the denormal-packed
# fColor and the huge fMass, real rotations, bound radii) are emitted EXACTLY -
# the only snapping is the epsilon test that decides whether a field equals its
# default and can be dropped.
#
# The top-level envelope (container / services / modules) is preserved as-is.
# Runs in place by default; pass -o to write elsewhere.
# ---------------------------------------------------------------------------

import argparse
import json
import sys

EPS = 1e-10

CLASS_LETTER = { 70: "R", 71: "C", 72: "T", 73: "P" }


def self_to_id (jHead):
    # Prefer the composed Self (string already-migrated, or raw integer);
    # fall back to explicit Class/Index.
    if "Self" in jHead:
        vSelf = jHead["Self"]
        if isinstance (vSelf, str):
            return vSelf
        twSelf = int (vSelf)
        nClass = twSelf >> 48
        nIndex = twSelf & 0x0000FFFFFFFFFFFF
        return "%s-%d" % (CLASS_LETTER.get (nClass, "?"), nIndex)

    nClass = int (jHead.get ("Class", 73))
    nIndex = int (jHead.get ("Index", 0))
    return "%s-%d" % (CLASS_LETTER.get (nClass, "?"), nIndex)


def near (aValue, aDefault):
    return all (abs (float (v) - float (d)) <= EPS for v, d in zip (aValue, aDefault))


class Stats:
    def __init__ (self):
        self.nNodes = 0


def compact_node (node, stats):
    stats.nNodes += 1

    out = { "Head": { "Self": self_to_id (node.get ("Head", {})) } }

    if node.get ("Name"):
        out["Name"] = node["Name"]

    # Type: keep only non-zero fields (bType celestial kind, bSubtype 255 attach).
    jType = node.get ("Type", {}) or {}
    tOut = {}
    for k in ("bType", "bSubtype", "bFiction"):
        if int (jType.get (k, 0)) != 0:
            tOut[k] = int (jType[k])
    if tOut:
        out["Type"] = tOut

    # Resource: keep only fields that carry a value.
    jRes = node.get ("Resource", {}) or {}
    rOut = {}
    if int (jRes.get ("qwResource", 0)) != 0:
        rOut["qwResource"] = int (jRes["qwResource"])
    if jRes.get ("sName"):
        rOut["sName"] = jRes["sName"]
    if jRes.get ("sReference"):
        rOut["sReference"] = jRes["sReference"]
    if rOut:
        out["Resource"] = rOut

    # Transform: drop components equal to the decoder defaults.
    jXf = node.get ("Transform", {}) or {}
    xOut = {}
    if "Position" in jXf and not near (jXf["Position"], [0.0, 0.0, 0.0]):
        xOut["Position"] = jXf["Position"]
    if "Rotation" in jXf and not near (jXf["Rotation"], [0.0, 0.0, 0.0, 1.0]):
        xOut["Rotation"] = jXf["Rotation"]
    if "Scale" in jXf and not near (jXf["Scale"], [1.0, 1.0, 1.0]):
        xOut["Scale"] = jXf["Scale"]
    if xOut:
        out["Transform"] = xOut

    # Orbit: keep only the non-zero elements.
    jOrb = node.get ("Orbit", {}) or {}
    oOut = {}
    for k in ("dA", "dB", "tmOrigin", "tmPeriod"):
        if k in jOrb and jOrb[k] != 0:
            oOut[k] = jOrb[k]
    if oOut:
        out["Orbit"] = oOut

    # Bound: drop a zero box.
    jBnd = node.get ("Bound", {}) or {}
    if "Max" in jBnd and not near (jBnd["Max"], [0.0, 0.0, 0.0]):
        out["Bound"] = { "Max": jBnd["Max"] }

    # Properties: keep every field that is exactly non-zero, EXACT values. Use a
    # strict != 0 test (NOT the epsilon): fColor is a denormal-packed colour
    # (~1e-38) that is semantically meaningful but smaller than EPS, and fMass
    # is huge - never round or epsilon-drop these.
    jProp = node.get ("Properties", {}) or {}
    pOut = {}
    for k in ("fMass", "fGravity", "fColor", "fBrightness", "fReflectivity"):
        if k in jProp and float (jProp[k]) != 0.0:
            pOut[k] = jProp[k]
    if pOut:
        out["Properties"] = pOut

    if int (node.get ("Owner", 0)) != 0:
        out["Owner"] = int (node["Owner"])

    aChild = node.get ("aChildren", []) or []
    jChildren = [compact_node (c, stats) for c in aChild]
    if jChildren:
        out["aChildren"] = jChildren

    return out


# ---------------------------------------------------------------------------
# Compact emitter (numbers emitted exactly; no value snapping).
# ---------------------------------------------------------------------------

def fmt_num (v):
    if isinstance (v, bool):
        return "true" if v else "false"
    if isinstance (v, int):
        return str (v)
    if v == int (v) and abs (v) < 1e15:
        return str (int (v))
    return repr (v)


def inline (v):
    if isinstance (v, dict):
        return "{ " + ", ".join ('%s: %s' % (json.dumps (k), inline (x)) for k, x in v.items ()) + " }"
    if isinstance (v, list):
        return "[" + ", ".join (inline (x) for x in v) + "]"
    if isinstance (v, str):
        return json.dumps (v, ensure_ascii = False)
    return fmt_num (v)


def emit_node (node, indent):
    pad  = " " * indent
    pad2 = " " * (indent + 3)

    aField = [(k, v) for k, v in node.items () if k != "aChildren"]
    aChild = node.get ("aChildren")

    if not aChild:
        return "{ " + ", ".join ('%s: %s' % (json.dumps (k), inline (v)) for k, v in aField) + " }"

    sHead = ", ".join ('%s: %s' % (json.dumps (k), inline (v)) for k, v in aField)

    aLine = ['{ ' + sHead + ', "aChildren":']
    aLine.append ('%s[' % pad2)
    aLine.append (",\n".join ('%s%s' % (" " * (indent + 6), emit_node (c, indent + 6)) for c in aChild))
    aLine.append ('%s]' % pad2)
    aLine.append ('%s}' % pad)
    return "\n".join (aLine)


def migrate (sIn, sOut):
    with open (sIn, "r", encoding = "utf-8-sig") as f:
        jMsf = json.load (f)

    jData = jMsf.get ("Data")
    if jData is None:
        sys.exit ("error: %s has no 'Data' node" % sIn)

    stats = Stats ()
    jOut = compact_node (jData, stats)

    aOut = ["{"]
    if "Container" in jMsf:
        aOut.append ('   "Container": %s,' % json.dumps (jMsf["Container"]))
    aOut.append ('   "Services": %s,' % json.dumps (jMsf.get ("Services", [])))
    aOut.append ('   "Modules":')
    aOut.append ('   [')
    aModule = jMsf.get ("Modules", [])
    for i, m in enumerate (aModule):
        aOut.append ('      {')
        aKeys = list (m.keys ())
        for j, k in enumerate (aKeys):
            sComma = "" if j == len (aKeys) - 1 else ","
            aOut.append ('         %s: %s%s' % (json.dumps (k), json.dumps (m[k]), sComma))
        aOut.append ('      }%s' % ("" if i == len (aModule) - 1 else ","))
    aOut.append ('   ],')
    aOut.append ('   "Data": %s' % emit_node (jOut, 3))
    aOut.append ("}")

    with open (sOut, "w", encoding = "utf-8") as f:
        f.write ("\n".join (aOut) + "\n")

    print ("%s -> %s  (%d nodes)" % (sIn, sOut, stats.nNodes))


def main ():
    ap = argparse.ArgumentParser (description = "Compact an existing Sneeze MSF JSON to the current standard.")
    ap.add_argument ("inputs", nargs = "+", help = "MSF JSON file(s) to migrate")
    ap.add_argument ("-o", "--output", help = "destination (only valid with a single input; default is in place)")
    opt = ap.parse_args ()

    if opt.output and len (opt.inputs) != 1:
        sys.exit ("error: -o requires exactly one input file")

    for sIn in opt.inputs:
        migrate (sIn, opt.output or sIn)


if __name__ == "__main__":
    main ()
