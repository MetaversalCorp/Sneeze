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
# convert_dfw.py - turn a foreign "spatial2" export (e.g. the DFW airport
# fabric from patchedreality) into a Sneeze MSF JSON document that the generic
# map.wasm module can inject via Scene.Node_Map. The node tree is emitted at
# "Data.Scene", where map.wasm expects it; the rest of "Data" is free for
# other use.
#
# The foreign format nests nodes under "root"; each node carries:
#   id           "<class>:<index>"   (terrestrial / physical / celestial / root)
#   name         display string
#   objectType   "<class>:<sub>..."  (e.g. physical:default, physical:default:attachment)
#   position     {x,y,z}             metres, local to parent
#   rotation     {x,y,z,w}           quaternion
#   scale        {x,y,z}
#   bound        {x,y,z}             full box extents, metres
#   resourceReference  glb path or external fabric URL (attachments) or null
#   children     [ ... ]
#
# The Sneeze node schema (see HostFunctions.cpp RmcObject_FromJson) is:
#   Head { Parent, Self, Event }     composed uint64 handles
#   Name "string"
#   Type { bType, bSubtype, bFiction }
#   Resource { qwResource, sName, sReference }
#   Transform { Position[3], Rotation[4], Scale[3] }
#   Bound { Max[3] }
#   aChildren [ ... ]
# ---------------------------------------------------------------------------

import argparse
import json
import os
import sys

# MAP_OBJECT_CLASS values (MapObject.h)
CLASS = {
    "root":        70,
    "celestial":   71,
    "terrestrial": 72,
    "physical":    73,
}

# Short id prefix per class, e.g. terrestrial:3505 -> "T-3505".
CLASS_LETTER = {
    "root":        "R",
    "celestial":   "C",
    "terrestrial": "T",
    "physical":    "P",
}

ATTACHMENT_SUBTYPE = 255


def compose (sClass, nIndex):
    eClass = CLASS.get (sClass)
    if eClass is None:
        raise ValueError ("unknown object class '%s'" % sClass)
    return (eClass << 48) | (nIndex & 0x0000FFFFFFFFFFFF)


def split_id (sId):
    # "terrestrial:3505" -> ("terrestrial", 3505)
    sClass, _, sIndex = sId.partition (":")
    return sClass, int (sIndex)


class Stats:
    def __init__ (self):
        self.nNodes = 0
        self.umpClass = {}
        self.nDepthMax = 0
        self.nAttach = 0
        self.aDuplicate = []
        self.setSeen = set ()


def convert_node (jSrc, twParent, opt, stats, nDepth):
    sClass, nIndex = split_id (jSrc["id"])
    twSelf = compose (sClass, nIndex)

    stats.nNodes += 1
    stats.umpClass[sClass] = stats.umpClass.get (sClass, 0) + 1
    stats.nDepthMax = max (stats.nDepthMax, nDepth)
    if twSelf in stats.setSeen:
        stats.aDuplicate.append (jSrc["id"])
    stats.setSeen.add (twSelf)

    sObjectType = jSrc.get ("objectType", "") or ""
    bIsAttach   = sObjectType.endswith (":attachment")

    sRef = jSrc.get ("resourceReference") or ""
    if sRef and opt.asset_base and not sRef.startswith (("http://", "https://")):
        sRef = opt.asset_base.rstrip ("/") + "/" + sRef.lstrip ("/")

    bSubtype = 0
    if bIsAttach:
        stats.nAttach += 1
        if not opt.inline_attachments:
            bSubtype = ATTACHMENT_SUBTYPE

    pos = jSrc.get ("position", {}) or {}
    rot = jSrc.get ("rotation", {}) or {}
    scl = jSrc.get ("scale",    {}) or {}
    bnd = jSrc.get ("bound",    {}) or {}

    # Head: Self is the human "class:index" id. Parent is omitted (the engine
    # derives parentage from nesting and never reads it); Event is always 0.
    jNode = {
        "Head": {
            "Self": "%s-%d" % (CLASS_LETTER[sClass], nIndex),
        },
        "Name": jSrc.get ("name", "") or "",
    }

    # Type: emit only when something is non-default (the parser defaults to 0).
    if bSubtype or jSrc.get ("bFiction"):
        jNode["Type"] = {
            "bType":    0,
            "bSubtype": bSubtype,
            "bFiction": jSrc.get ("bFiction", 0),
        }

    # Resource: emit only the fields that carry a value.
    jResource = {}
    if jSrc.get ("resourceName"):
        jResource["sName"] = jSrc["resourceName"]
    if sRef:
        jResource["sReference"] = sRef
    if jResource:
        jNode["Resource"] = jResource

    # Transform: emit only the components that differ from the decoder's
    # defaults (Position [0,0,0], Rotation identity, Scale unit). A node whose
    # transform is fully default drops the Transform block entirely.
    aPosition = [pos.get ("x", 0.0), pos.get ("y", 0.0), pos.get ("z", 0.0)]
    aRotation = [rot.get ("x", 0.0), rot.get ("y", 0.0), rot.get ("z", 0.0), rot.get ("w", 1.0)]
    aScale    = [scl.get ("x", 1.0), scl.get ("y", 1.0), scl.get ("z", 1.0)]

    jTransform = {}
    if not is_default (aPosition, [0.0, 0.0, 0.0]):
        jTransform["Position"] = aPosition
    if not is_default (aRotation, [0.0, 0.0, 0.0, 1.0]):
        jTransform["Rotation"] = aRotation
    if not is_default (aScale, [1.0, 1.0, 1.0]):
        jTransform["Scale"] = aScale
    if jTransform:
        jNode["Transform"] = jTransform

    jNode["Bound"] = {
        "Max": [bnd.get ("x", 0.0), bnd.get ("y", 0.0), bnd.get ("z", 0.0)],
    }

    aChild = jSrc.get ("children", []) or []
    if bIsAttach and not opt.inline_attachments:
        # Treat as a real attachment point: drop inline children; the engine
        # will spawn a child fabric from sReference.
        aChild = []

    jChildren = [convert_node (c, twSelf, opt, stats, nDepth + 1) for c in aChild]
    if jChildren:
        jNode["aChildren"] = jChildren

    return jNode


# ---------------------------------------------------------------------------
# Compact emitter: leaf nodes collapse onto a single line, vectors stay inline,
# and only nodes with children expand across multiple lines. Floats within
# SNAP_EPS of 0, 1 or -1 are snapped (clears export noise like 0.9999999999999998
# without touching meaningful sub-micro values).
# ---------------------------------------------------------------------------

SNAP_EPS = 1e-10


def is_default (aValue, aDefault):
    return all (abs (v - d) <= SNAP_EPS for v, d in zip (aValue, aDefault))


def fmt_num (v):
    if isinstance (v, bool):
        return "true" if v else "false"
    if isinstance (v, int):
        return str (v)
    for t in (0.0, 1.0, -1.0):
        if abs (v - t) <= SNAP_EPS:
            v = t
            break
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


def main ():
    ap = argparse.ArgumentParser (description = "Convert a spatial2 export to a Sneeze MSF JSON document.")
    ap.add_argument ("input", help = "source export JSON (e.g. dfw.json)")
    ap.add_argument ("-o", "--output", help = "destination MSF JSON (default: <input>.msf.json)")
    ap.add_argument ("--container", default = "dfw", help = "container name (default: dfw)")
    ap.add_argument ("--module-url", default = "https://cdn.rp1.com/test/map.wasm", help = "generic map module URL")
    ap.add_argument ("--module-hash", default = "", help = "sha256- comment hash for the module")
    ap.add_argument ("--asset-base", default = "", help = "prefix prepended to relative resourceReference paths")
    ap.add_argument ("--keep-attachments", dest = "inline_attachments", action = "store_false",
                     help = "mark :attachment nodes as bSubtype=255 and drop their inline children (default keeps children inline)")
    ap.set_defaults (inline_attachments = True)
    opt = ap.parse_args ()

    sOut = opt.output or (os.path.splitext (opt.input)[0] + ".msf.json")

    with open (opt.input, "r", encoding = "utf-8-sig") as f:
        jSrc = json.load (f)

    jRoot = jSrc.get ("root")
    if jRoot is None:
        sys.exit ("error: no 'root' node in %s" % opt.input)

    stats = Stats ()
    jData = convert_node (jRoot, 0, opt, stats, 0)

    aOut = [
        "{",
        '   "Container": %s,' % json.dumps (opt.container),
        '   "Services": [],',
        '   "Modules":',
        '   [',
        '      {',
        '         "sUrl": %s,' % json.dumps (opt.module_url),
        '         "comment-sHash": %s' % json.dumps (opt.module_hash or "sha256-REPLACE_ME"),
        '      }',
        '   ],',
        '   "Data":',
        '   {',
        '      "Scene": %s' % emit_node (jData, 6),
        '   }',
        "}",
    ]

    with open (sOut, "w", encoding = "utf-8") as f:
        f.write ("\n".join (aOut) + "\n")

    print ("wrote %s" % sOut)
    print ("  nodes:       %d" % stats.nNodes)
    print ("  by class:    %s" % ", ".join ("%s=%d" % (k, v) for k, v in sorted (stats.umpClass.items ())))
    print ("  max depth:   %d" % stats.nDepthMax)
    print ("  attachments: %d (%s)" % (stats.nAttach, "inlined" if opt.inline_attachments else "kept as fabric spawn points"))
    if stats.aDuplicate:
        print ("  WARNING duplicate composed handles: %s" % ", ".join (stats.aDuplicate))


if __name__ == "__main__":
    main ()
