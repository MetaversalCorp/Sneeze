"""
Generate the complete Solar System WASM module data from the JavaScript project.

Reads ALL data files from E:\\Dev\\SolarSystem\\app\\data\\, builds the full object
tree (1245 objects), assigns predefined object indices, computes MSF wire-format
values, and outputs Rust source files with one Submit call per JS RMCOBJECT.

Each JS "new RMCOBJECT({...})" becomes exactly one RMCOBJECT in the WASM module.
No collapsing. Object indices are predefined (depth-first assignment).

Usage:  python gen_solar_system.py
"""

import re, math, os, sys

JS_DIR  = r"E:\Dev\SolarSystem\app\data"
OUT_DIR = os.path.join (os.path.dirname (os.path.abspath (__file__)), "src")

AU_M             = 149597870700.0
TICKS_PER_SEC    = 64
SECS_PER_DAY     = 86400.0
TICKS_PER_DAY    = SECS_PER_DAY * TICKS_PER_SEC
DAYS_PER_CENTURY = 36525.0
DEG              = math.pi / 180.0

# JS RMCOBJECT.TYPE -> C++ MAP_OBJECT_TYPE_SUBTYPE_CELESTIAL (uint8_t)
SUBTYPE_MAP = {
   "STARSYSTEM":    9,
   "STAR":          10,
   "PLANETSYSTEM":  11,
   "PLANET":        12,
   "MOONSYSTEM":    125,
   "MOON":          13,
   "DEBRISSYSTEM":  135,
   "DEBRIS":        14,
   "SURFACE":       17,
}

# ---------------------------------------------------------------------------
# JS parser — extracts every "new RMCOBJECT({...})" from a file
# ---------------------------------------------------------------------------

def parse_js_objects (sPath):
   with open (sPath, "r", encoding="utf-8") as f:
      sText = f.read ()

   aResult = []
   for m in re.finditer (r"new\s+RMCOBJECT\s*\(\s*\{(.*?)\}\s*\)", sText, re.DOTALL):
      sBlock = m.group (1)
      d = {}

      for line in sBlock.split ("\n"):
         line = line.strip ()
         if not line or line.startswith ("//"):
            continue
         mKV = re.match (r"(\w+)\s*:\s*(.+?)\s*,?\s*(?://.*)?$", line)
         if not mKV:
            continue

         sKey = mKV.group (1)
         sVal = mKV.group (2).rstrip (",").strip ()

         # COLOR constructor
         mColor = re.match (r"new\s+COLOR\s*\(\s*(\S+)\s*,\s*(\S+)\s*,\s*(\S+)\s*\)", sVal)
         if mColor:
            r = int (mColor.group (1), 0)
            g = int (mColor.group (2), 0)
            b = int (mColor.group (3), 0)
            d[sKey] = (r, g, b)
            continue

         # EPOCH constructor
         mEpoch = re.match (r"new\s+EPOCH\s*\(\s*['\"](.+?)['\"]\s*\)", sVal)
         if mEpoch:
            d[sKey] = mEpoch.group (1)
            continue

         # RMCOBJECT.TYPE.xxx
         if sVal.startswith ("RMCOBJECT.TYPE."):
            d[sKey] = sVal.split (".")[-1]
            continue

         # Quoted strings
         if (sVal.startswith ("'") and sVal.endswith ("'")) or \
            (sVal.startswith ('"') and sVal.endswith ('"')):
            d[sKey] = sVal[1:-1]
            continue

         # Booleans
         if sVal in ("true", "false"):
            d[sKey] = sVal == "true"
            continue

         # Numeric — handle underscore separators (JS: 384_400)
         sNum = sVal.replace ("_", "")
         try:
            if "." in sNum or "e" in sNum.lower ():
               d[sKey] = float (sNum)
            else:
               d[sKey] = int (sNum)
         except ValueError:
            d[sKey] = sVal

      if d:
         aResult.append (d)

   return aResult


# ---------------------------------------------------------------------------
# Orbital math
# ---------------------------------------------------------------------------

def quat_from_euler_zxz (dLonAscNode_deg, dInclination_deg, dArgPerihelion_deg):
   a = dLonAscNode_deg * DEG
   b = dInclination_deg * DEG
   c = dArgPerihelion_deg * DEG

   ca, sa = math.cos (a / 2), math.sin (a / 2)
   cb, sb = math.cos (b / 2), math.sin (b / 2)
   cc, sc = math.cos (c / 2), math.sin (c / 2)

   # ZXZ Euler -> quaternion, then ecliptic Z-up -> Y-up swap
   w = ca * cb * cc - sa * cb * sc
   x = ca * sb * cc + sa * sb * sc
   y = sa * sb * cc - ca * sb * sc
   z = sa * cb * cc + ca * cb * sc

   return (x, z, -y, w)   # Y-up: (x, z, -y, w)


def precession_vector (dLonAscNodeDot, dInclinationDot, dArgPerihelionDot,
                       dLonAscNode_deg, dInclination_deg, dArgPerihelion_deg):
   if dLonAscNodeDot == 0 and dInclinationDot == 0 and dArgPerihelionDot == 0:
      return (0.0, 0.0, 0.0)

   dt_ticks = DAYS_PER_CENTURY * TICKS_PER_DAY

   qx0, qy0, qz0, qw0 = quat_from_euler_zxz (dLonAscNode_deg, dInclination_deg, dArgPerihelion_deg)

   qx1, qy1, qz1, qw1 = quat_from_euler_zxz (
      dLonAscNode_deg + dLonAscNodeDot,
      dInclination_deg + dInclinationDot,
      dArgPerihelion_deg + dArgPerihelionDot)

   # Quaternion difference: dq = q1 * q0^-1
   dw =  qw1 * qw0 + qx1 * qx0 + qy1 * qy0 + qz1 * qz0
   dx = -qw1 * qx0 + qx1 * qw0 - qy1 * qz0 + qz1 * qy0
   dy = -qw1 * qy0 + qy1 * qw0 - qz1 * qx0 + qx1 * qz0
   dz = -qw1 * qz0 + qz1 * qw0 - qx1 * qy0 + qy1 * qx0

   if dw < 0:
      dx, dy, dz, dw = -dx, -dy, -dz, -dw

   angle = 2.0 * math.acos (min (1.0, max (-1.0, dw)))
   s = math.sqrt (dx * dx + dy * dy + dz * dz)

   if s < 1e-30 or angle < 1e-30:
      return (0.0, 0.0, 0.0)

   rate = angle / dt_ticks
   return (dx / s * rate, dy / s * rate, dz / s * rate)


def compute_msf_planet (d):
   """Planet systems: dLonPerihelion / dMeanLongitude format with secular rates."""
   dSemiMajor_AU = d.get ("dSemiMajorAU", 0)
   dEcc          = d.get ("dEccentricity", 0)
   dInc          = d.get ("dInclination", 0)
   dLonAscNode   = d.get ("dLonAscNode", 0)
   dLonPeri      = d.get ("dLonPerihelion", 0)
   dMeanLong     = d.get ("dMeanLongitude", 0)

   dArgPeri  = dLonPeri - dLonAscNode
   dMeanAnom = dMeanLong - dLonPeri

   dA = dSemiMajor_AU * AU_M
   dB = dA * math.sqrt (max (0, 1.0 - dEcc * dEcc)) if dEcc < 1.0 else dA

   dMeanLongDot    = d.get ("dMeanLongitudeDot", 0)
   dLonPeriDot     = d.get ("dLonPerihelionDot", 0)
   dMeanAnomalyDot = dMeanLongDot - dLonPeriDot

   if abs (dMeanAnomalyDot) < 1e-12:
      tmPeriod = 0
   else:
      dPeriod_cy = 360.0 / dMeanAnomalyDot
      tmPeriod   = int (round (dPeriod_cy * DAYS_PER_CENTURY * TICKS_PER_DAY))

   if tmPeriod != 0:
      dFrac = (dMeanAnom % 360.0) / 360.0
      if dFrac < 0:
         dFrac += 1.0
      tmOrigin = int (round (dFrac * tmPeriod))
   else:
      tmOrigin = 0

   qx, qy, qz, qw = quat_from_euler_zxz (dLonAscNode, dInc, dArgPeri)

   dLonAscNodeDot = d.get ("dLonAscNodeDot", 0)
   dIncDot        = d.get ("dInclinationDot", 0)
   dArgPeriDot    = dLonPeriDot - dLonAscNodeDot
   precX, precY, precZ = precession_vector (dLonAscNodeDot, dIncDot, dArgPeriDot,
                                             dLonAscNode, dInc, dArgPeri)
   return (dA, dB, tmPeriod, tmOrigin, qx, qy, qz, qw, precX, precY, precZ)


def compute_msf_moon (d):
   """Moon/debris systems: dArgPerihelion / dMeanAnomaly format."""
   dSemiMajor_AU = d.get ("dSemiMajorAU", 0)
   dEcc          = d.get ("dEccentricity", 0)
   dInc          = d.get ("dInclination", 0)
   dLonAscNode   = d.get ("dLonAscNode", 0)
   dArgPeri      = d.get ("dArgPerihelion", 0)
   dMeanAnom     = d.get ("dMeanAnomaly", 0)

   dA = dSemiMajor_AU * AU_M
   dB = dA * math.sqrt (max (0, 1.0 - dEcc * dEcc)) if dEcc < 1.0 else dA

   dPeriodDays  = d.get ("dPeriodDays", 0)
   dMeanLongDot = d.get ("dMeanLongitudeDot", 0)

   if dMeanLongDot != 0:
      dLonAscNodeDot = d.get ("dLonAscNodeDot", 0)
      dArgPeriDot    = d.get ("dArgPerihelionDot", 0)
      dLonPeriDot    = dArgPeriDot + dLonAscNodeDot
      dMADot         = dMeanLongDot - dLonPeriDot
      if abs (dMADot) > 1e-12:
         dPeriod_cy = 360.0 / dMADot
         tmPeriod   = int (round (dPeriod_cy * DAYS_PER_CENTURY * TICKS_PER_DAY))
      else:
         tmPeriod = 0
   elif dPeriodDays > 0:
      tmPeriod = int (round (dPeriodDays * TICKS_PER_DAY))
   else:
      tmPeriod = 0

   if tmPeriod != 0 and dMeanAnom != 0:
      dFrac = (dMeanAnom % 360.0) / 360.0
      if dFrac < 0:
         dFrac += 1.0
      tmOrigin = int (round (dFrac * tmPeriod))
   else:
      tmOrigin = 0

   qx, qy, qz, qw = quat_from_euler_zxz (dLonAscNode, dInc, dArgPeri)

   dLonAscNodeDot = d.get ("dLonAscNodeDot", 0)
   dIncDot        = d.get ("dInclinationDot", 0)
   dArgPeriDot    = d.get ("dArgPerihelionDot", 0)
   precX, precY, precZ = precession_vector (dLonAscNodeDot, dIncDot, dArgPeriDot,
                                             dLonAscNode, dInc, dArgPeri)
   return (dA, dB, tmPeriod, tmOrigin, qx, qy, qz, qw, precX, precY, precZ)


def compute_msf (d):
   """Route to the correct MSF computation based on available fields."""
   if "dLonPerihelion" in d or "dMeanLongitude" in d:
      return compute_msf_planet (d)
   elif d.get ("dSemiMajorAU", 0) > 0:
      return compute_msf_moon (d)
   return (0.0, 0.0, 0, 0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0)


# ---------------------------------------------------------------------------
# Tilt computation — replicated from rmcobject.js ComputeTilt()
# ---------------------------------------------------------------------------

OBLIQUITY_J2000 = 23.4392911

def matrix_to_quat (m00, m01, m02, m10, m11, m12, m20, m21, m22):
   dTrace = m00 + m11 + m22

   if dTrace > 0:
      dS = 2.0 * math.sqrt (dTrace + 1.0)
      dW = 0.25 * dS
      dX = (m21 - m12) / dS
      dY = (m02 - m20) / dS
      dZ = (m10 - m01) / dS
   elif m00 > m11 and m00 > m22:
      dS = 2.0 * math.sqrt (1.0 + m00 - m11 - m22)
      dW = (m21 - m12) / dS
      dX = 0.25 * dS
      dY = (m01 + m10) / dS
      dZ = (m02 + m20) / dS
   elif m11 > m22:
      dS = 2.0 * math.sqrt (1.0 + m11 - m00 - m22)
      dW = (m02 - m20) / dS
      dX = (m01 + m10) / dS
      dY = 0.25 * dS
      dZ = (m12 + m21) / dS
   else:
      dS = 2.0 * math.sqrt (1.0 + m22 - m00 - m11)
      dW = (m10 - m01) / dS
      dX = (m02 + m20) / dS
      dY = (m12 + m21) / dS
      dZ = 0.25 * dS

   return (dX, dY, dZ, dW)


def frame_to_ecliptic_quat (dRA_deg, dDec_deg):
   dAlpha = dRA_deg * DEG
   dDelta = dDec_deg * DEG
   dEps   = OBLIQUITY_J2000 * DEG

   dCosA = math.cos (dAlpha)
   dSinA = math.sin (dAlpha)
   dCosD = math.cos (dDelta)
   dSinD = math.sin (dDelta)
   dCosE = math.cos (dEps)
   dSinE = math.sin (dEps)

   m00 =  dSinA
   m10 = -dCosA * dCosE
   m20 =  dCosA * dSinE

   m01 =  dSinD * dCosA
   m11 =  dSinD * dSinA * dCosE - dCosD * dSinE
   m21 = -dSinD * dSinA * dSinE - dCosD * dCosE

   m02 =  dCosD * dCosA
   m12 =  dCosD * dSinA * dCosE + dSinD * dSinE
   m22 = -dCosD * dSinA * dSinE + dSinD * dCosE

   return matrix_to_quat (m00, m01, m02, m10, m11, m12, m20, m21, m22)


def compute_tilt (d):
   """Compute tilt quaternion + tilt precession for a body object.
   Returns (qx,qy,qz,qw, precX,precY,precZ) in Y-up, rad/tick."""

   dPoleRA  = d.get ("dPoleRA", None)
   dPoleDec = d.get ("dPoleDec", None)

   qx, qy, qz, qw = 0.0, 0.0, 0.0, 1.0
   precX, precY, precZ = 0.0, 0.0, 0.0

   TICKS_PER_CY = DAYS_PER_CENTURY * TICKS_PER_DAY

   if dPoleRA is not None and dPoleDec is not None:
      qx, qy, qz, qw = frame_to_ecliptic_quat (dPoleRA, dPoleDec)
      tmpQy = qy
      qy = qz
      qz = -tmpQy

   dTiltPrecRate = d.get ("dTiltPrecRate", None)
   dPoleRADot    = d.get ("dPoleRADot", None)
   dPoleDecDot   = d.get ("dPoleDecDot", None)

   if dTiltPrecRate is not None:
      dRate = dTiltPrecRate * DEG
      precX = 0.0
      precY = dRate / TICKS_PER_CY
      precZ = 0.0
   elif dPoleRADot is not None and dPoleDecDot is not None \
        and dPoleRA is not None and dPoleDec is not None:
      dRA     = dPoleRA * DEG
      dDec    = dPoleDec * DEG
      dRADot  = dPoleRADot * DEG
      dDecDot = dPoleDecDot * DEG

      dCosRA  = math.cos (dRA)
      dSinRA  = math.sin (dRA)
      dCosDec = math.cos (dDec)
      dSinDec = math.sin (dDec)

      px = dCosDec * dCosRA
      py = dCosDec * dSinRA
      pz = dSinDec

      vx = -dCosDec * dSinRA * dRADot  -  dSinDec * dCosRA * dDecDot
      vy =  dCosDec * dCosRA * dRADot  -  dSinDec * dSinRA * dDecDot
      vz =  dCosDec * dDecDot

      ox = py * vz  -  pz * vy
      oy = pz * vx  -  px * vz
      oz = px * vy  -  py * vx

      dEps    = OBLIQUITY_J2000 * DEG
      dCosEps = math.cos (dEps)
      dSinEps = math.sin (dEps)

      eclX =  ox
      eclY =  oy * dCosEps  +  oz * dSinEps
      eclZ = -oy * dSinEps  +  oz * dCosEps

      precX = eclX / TICKS_PER_CY
      precY = eclZ / TICKS_PER_CY
      precZ = -eclY / TICKS_PER_CY

   return (qx, qy, qz, qw, precX, precY, precZ)


# ---------------------------------------------------------------------------
# Spin computation — replicated from rmcobject.js ComputeSpin()
# ---------------------------------------------------------------------------

def compute_spin (d):
   """Compute spin parameters for a surface object.
   Returns (dW0Rad, tmSpinPeriod)."""

   dW0       = d.get ("dW0", 0.0) or 0.0
   dSpinRate = d.get ("dSpinRate", None)

   dW0Rad       = dW0 * DEG
   tmSpinPeriod = 0

   if dSpinRate is not None and dSpinRate != 0:
      tmSpinPeriod = int (round (abs (360.0 / dSpinRate) * TICKS_PER_DAY))
      if dSpinRate < 0:
         tmSpinPeriod = -tmSpinPeriod

   return (dW0Rad, tmSpinPeriod)


def color_to_u32 (pColor):
   if isinstance (pColor, tuple) and len (pColor) == 3:
      r, g, b = pColor
      return (r << 16) | (g << 8) | b
   return 0x00888888


def fmt_f32 (v):
   if v == 0 or v == 0.0:
      return "0.0"
   s = f"{v:.6e}"
   return s


def fmt_f64 (v):
   if v == 0.0:
      return "0.0"
   return repr (v)


# ---------------------------------------------------------------------------
# Object tree
# ---------------------------------------------------------------------------

class OBJ:
   __slots__ = ("sId", "sIdParent", "sType", "d", "nIndex", "nParentIndex", "aChildren")

   def __init__ (self, sId, sIdParent, sType, d):
      self.sId          = sId
      self.sIdParent    = sIdParent
      self.sType        = sType
      self.d            = d
      self.nIndex       = 0
      self.nParentIndex = 0
      self.aChildren    = []


def build_tree (aAllObjects):
   dById = {}
   for o in aAllObjects:
      dById[o.sId] = o

   pRoot = OBJ ("__root__", None, "ROOT", {"sName": "Solar System"})

   for o in aAllObjects:
      sParent = o.sIdParent
      if not sParent:
         pRoot.aChildren.append (o)
      elif sParent in dById:
         dById[sParent].aChildren.append (o)
      else:
         print (f"  WARNING: orphan object {o.sId!r} (parent {sParent!r} not found), attaching to root")
         pRoot.aChildren.append (o)

   return pRoot


def assign_indices (pNode, nNext):
   pNode.nIndex = nNext
   nNext += 1
   for pChild in pNode.aChildren:
      pChild.nParentIndex = pNode.nIndex
      nNext = assign_indices (pChild, nNext)
   return nNext


# ---------------------------------------------------------------------------
# Code generation — one Rust function call per JS RMCOBJECT
# ---------------------------------------------------------------------------

def escape_rust_string (s):
   return s.replace ("\\", "\\\\").replace ('"', '\\"')


def gen_system_call (o):
   d       = o.d
   sName   = escape_rust_string (d.get ("sName", o.sId))
   nColor  = color_to_u32 (d.get ("pColor", None))
   fMass   = float (d.get ("dMass", 0.0))
   dRadius = float (d.get ("dRadius", 0.0)) * 1000.0
   dSysRad = float (d.get ("dSystemRadiusKm", 0.0)) * 1000.0
   dBound  = dSysRad if dSysRad > 0 else dRadius
   nSub    = SUBTYPE_MAP.get (o.sType, 0)

   dA, dB, tmP, tmO, qx, qy, qz, qw, pX, pY, pZ = compute_msf (d)

   return (
      f'   crate::Submit_System ({o.nParentIndex}, {o.nIndex}, "{sName}", {nSub}, '
      f'{fmt_f64 (dA)}, {fmt_f64 (dB)}, {tmP}_i64, {tmO}_i64, '
      f'{fmt_f64 (qx)}, {fmt_f64 (qy)}, {fmt_f64 (qz)}, {fmt_f64 (qw)}, '
      f'{fmt_f64 (pX)}, {fmt_f64 (pY)}, {fmt_f64 (pZ)}, '
      f'{fmt_f64 (dBound)}, {fmt_f32 (fMass)}, 0x{nColor:08X});\n'
   )


def gen_body_call (o):
   d       = o.d
   sName   = escape_rust_string (d.get ("sName", o.sId))
   nColor  = color_to_u32 (d.get ("pColor", None))
   fMass   = float (d.get ("dMass", 0.0))
   dRadius = float (d.get ("dRadius", 0.0)) * 1000.0
   nSub    = SUBTYPE_MAP.get (o.sType, 0)

   tqx, tqy, tqz, tqw, tpX, tpY, tpZ = compute_tilt (d)

   return (
      f'   crate::Submit_Body ({o.nParentIndex}, {o.nIndex}, "{sName}", {nSub}, '
      f'{fmt_f64 (dRadius)}, {fmt_f32 (fMass)}, 0x{nColor:08X}, '
      f'{fmt_f64 (tqx)}, {fmt_f64 (tqy)}, {fmt_f64 (tqz)}, {fmt_f64 (tqw)}, '
      f'{fmt_f64 (tpX)}, {fmt_f64 (tpY)}, {fmt_f64 (tpZ)});\n'
   )


def gen_surface_call (o):
   d        = o.d
   sName    = escape_rust_string (d.get ("sName", o.sId))
   sTexture = d.get ("sTexture", "")
   if sTexture.startswith ("img/"):
      sTexture = sTexture[4:]

   dW0Rad, tmSpinPeriod = compute_spin (d)

   return (
      f'   crate::Submit_Surface ({o.nParentIndex}, {o.nIndex}, "{sName}", "{sTexture}", '
      f'{fmt_f64 (dW0Rad)}, {tmSpinPeriod}_i64);\n'
   )


def gen_call (o):
   if o.sType in ("STARSYSTEM", "PLANETSYSTEM", "MOONSYSTEM", "DEBRISSYSTEM"):
      return gen_system_call (o)
   elif o.sType in ("STAR", "PLANET", "MOON", "DEBRIS"):
      return gen_body_call (o)
   elif o.sType == "SURFACE":
      return gen_surface_call (o)
   else:
      print (f"  WARNING: unknown type {o.sType!r} for {o.sId!r}")
      return f"   // UNKNOWN: {o.sId} ({o.sType})\n"


def collect_dfs (pNode, aResult):
   aResult.append (pNode)
   for pChild in pNode.aChildren:
      collect_dfs (pChild, aResult)


def write_file (sFilename, sFnName, aObjects):
   sPath = os.path.join (OUT_DIR, sFilename)
   lines = []
   lines.append (f"#![allow (clippy::excessive_precision)]")
   lines.append (f"")
   lines.append (f"pub fn {sFnName} () -> u32")
   lines.append (f"{{")

   for o in aObjects:
      lines.append (gen_call (o))

   lines.append (f"   {len (aObjects)}")
   lines.append (f"}}")
   lines.append (f"")

   with open (sPath, "w", encoding="utf-8") as f:
      f.write ("\n".join (lines))

   print (f"  {sFilename}: {len (aObjects)} objects")
   return len (aObjects)


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main ():
   if not os.path.isdir (JS_DIR):
      print (f"ERROR: JS data directory not found: {JS_DIR}")
      sys.exit (1)

   # ---- Parse all JS files ----
   aAllJS = []
   aFiles = sorted (f for f in os.listdir (JS_DIR) if f.endswith (".js"))

   for sFile in aFiles:
      sPath = os.path.join (JS_DIR, sFile)
      aObjs = parse_js_objects (sPath)
      aAllJS.extend (aObjs)

   print (f"Parsed {len (aAllJS)} JS objects from {len (aFiles)} files in {JS_DIR}")

   # ---- Build OBJ wrappers ----
   aAllObjects = []
   for d in aAllJS:
      sId       = d.get ("sId", f"__anon_{len (aAllObjects)}")
      sIdParent = d.get ("sId_Parent", None)
      sType     = d.get ("bType", "NONE")
      if isinstance (sType, int):
         sType = "NONE"
      aAllObjects.append (OBJ (sId, sIdParent, sType, d))

   # ---- Build tree and assign indices ----
   pRoot = build_tree (aAllObjects)
   nTotal = assign_indices (pRoot, 1)
   print (f"Assigned {nTotal - 1} indices (1 = root, 2..{nTotal - 1} = objects)")

   # ---- Verify object counts ----
   aAll = []
   collect_dfs (pRoot, aAll)
   nNonRoot = len (aAll) - 1   # exclude root
   print (f"Tree has {nNonRoot} non-root objects")

   # ---- Find the star system (root's first child) ----
   pStarSystem = None
   for c in pRoot.aChildren:
      if c.sType == "STARSYSTEM":
         pStarSystem = c
         break

   if not pStarSystem:
      print ("ERROR: No star system found in tree")
      sys.exit (1)

   # ---- Classify star system children ----
   pSun           = None
   aPlanetSystems = []
   aDebrisSystems = []

   for c in pStarSystem.aChildren:
      if c.sType == "STAR":
         pSun = c
      elif c.sType == "PLANETSYSTEM":
         aPlanetSystems.append (c)
      elif c.sType == "DEBRISSYSTEM":
         aDebrisSystems.append (c)

   # ---- Generate files ----
   nGenerated = 0

   # star.rs: star system + sun subtree
   aStarObjs = [pStarSystem]
   if pSun:
      collect_dfs (pSun, aStarObjs)
   nGenerated += write_file ("star.rs", "Submit", aStarObjs)

   # planets.rs: planet systems + planets + planet surfaces (exclude moon systems)
   aPlanetObjs = []
   for ps in aPlanetSystems:
      aPlanetObjs.append (ps)
      for c in ps.aChildren:
         if c.sType == "PLANET":
            collect_dfs (c, aPlanetObjs)
   nGenerated += write_file ("planets.rs", "Submit", aPlanetObjs)

   # Moon files — one per parent planet
   MOON_PARENTS = {
      "earth_system":   "moons_earth",
      "mars_system":    "moons_mars",
      "jupiter_system": "moons_jupiter",
      "saturn_system":  "moons_saturn",
      "uranus_system":  "moons_uranus",
      "neptune_system": "moons_neptune",
      "pluto_system":   "moons_pluto",
   }

   for ps in aPlanetSystems:
      sKey = ps.sId
      if sKey in MOON_PARENTS:
         aMoonObjs = []
         for c in ps.aChildren:
            if c.sType == "MOONSYSTEM":
               collect_dfs (c, aMoonObjs)
         if aMoonObjs:
            sFile = MOON_PARENTS[sKey] + ".rs"
            nGenerated += write_file (sFile, "Submit", aMoonObjs)

   # debris.rs: debris systems + debris bodies + debris surfaces
   if aDebrisSystems:
      aDebrisObjs = []
      for ds in aDebrisSystems:
         collect_dfs (ds, aDebrisObjs)
      nGenerated += write_file ("debris.rs", "Submit", aDebrisObjs)

   print (f"\nTotal generated: {nGenerated} objects across output files")

   if nGenerated != nNonRoot:
      print (f"WARNING: generated {nGenerated} != tree count {nNonRoot}")
   else:
      print (f"OK: matches tree count")


if __name__ == "__main__":
   main ()
