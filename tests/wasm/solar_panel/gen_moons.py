"""
Generate Rust moon data files from the JavaScript Solar System project.

Reads moonsystem_*.js (orbital), moon_*.js (physical), and surface_moon_*.js (texture)
files from E:\Dev\SolarSystem\app\data\, joins them by sId linkage, computes MSF wire
format values (dA, dB, tmPeriod, tmOrigin, quaternion, precession), and writes one
.rs file per parent planet into src/.

Usage:  python gen_moons.py
"""

import re, math, os

JS_DIR  = r"E:\Dev\SolarSystem\app\data"
OUT_DIR = os.path.join (os.path.dirname (__file__), "src")

AU_M                = 149597870700.0
TICKS_PER_SEC       = 64
SECS_PER_DAY        = 86400.0
TICKS_PER_DAY       = SECS_PER_DAY * TICKS_PER_SEC
DAYS_PER_CENTURY    = 36525.0
DEG                 = math.pi / 180.0

PARENTS = ["earth", "mars", "jupiter", "saturn", "uranus", "neptune", "pluto"]

# ---------------------------------------------------------------------------
# JS parser — extract RMCOBJECT({...}) blocks as dicts
# ---------------------------------------------------------------------------

def parse_js_objects (sPath):
    """Parse new RMCOBJECT({...}) blocks from a JS file into a list of dicts."""
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
            if mKV:
                sKey = mKV.group (1)
                sVal = mKV.group (2).rstrip (",").strip ()

                # Parse new COLOR(r, g, b)
                mColor = re.match (r"new\s+COLOR\s*\(\s*(\S+)\s*,\s*(\S+)\s*,\s*(\S+)\s*\)", sVal)
                if mColor:
                    r = int (mColor.group (1), 0)
                    g = int (mColor.group (2), 0)
                    b = int (mColor.group (3), 0)
                    d[sKey] = (r, g, b)
                    continue

                # String
                if sVal.startswith ("'") or sVal.startswith ('"'):
                    d[sKey] = sVal.strip ("'\"")
                    continue

                # RMCOBJECT.TYPE.X — store as string
                if sVal.startswith ("RMCOBJECT.TYPE."):
                    d[sKey] = sVal.split (".")[-1]
                    continue

                # Number
                try:
                    if "." in sVal or "e" in sVal.lower ():
                        d[sKey] = float (sVal)
                    else:
                        d[sKey] = int (sVal)
                except ValueError:
                    d[sKey] = sVal

        if d:
            aResult.append (d)

    return aResult


# ---------------------------------------------------------------------------
# Orbital math — same pipeline as the planet MSF computation
# ---------------------------------------------------------------------------

def quat_from_euler_zxz (dLonAscNode_deg, dInclination_deg, dArgPerihelion_deg):
    """
    Compute orbital quaternion from ecliptic Euler angles.
    Rotation sequence: Z(lonAscNode) * X(inclination) * Z(argPerihelion)
    Then convert ecliptic Z-up to graphics Y-up: swap Y<->Z, negate new Z.
    """
    a = dLonAscNode_deg * DEG
    b = dInclination_deg * DEG
    c = dArgPerihelion_deg * DEG

    ca, sa = math.cos (a / 2), math.sin (a / 2)
    cb, sb = math.cos (b / 2), math.sin (b / 2)
    cc, sc = math.cos (c / 2), math.sin (c / 2)

    # ZXZ quaternion composition (ecliptic frame, Z-up)
    w = ca * cb * cc - sa * cb * sc
    x = ca * sb * cc + sa * sb * sc
    y = sa * sb * cc - ca * sb * sc
    z = sa * cb * cc + ca * cb * sc

    # Ecliptic Z-up -> graphics Y-up: (x, y, z, w) -> (x, z, -y, w)
    qx, qy, qz, qw = x, z, -y, w

    return qx, qy, qz, qw


def precession_vector (dLonAscNodeDot, dInclinationDot, dArgPerihelionDot,
                       dLonAscNode_deg, dInclination_deg, dArgPerihelion_deg):
    """
    Compute precession as a change in quaternion per tick.
    Rates are in °/century. Returns (precX, precY, precZ) in the Y-up frame.
    """
    if dLonAscNodeDot == 0 and dInclinationDot == 0 and dArgPerihelionDot == 0:
        return 0.0, 0.0, 0.0

    dt_ticks = DAYS_PER_CENTURY * TICKS_PER_DAY

    qx0, qy0, qz0, qw0 = quat_from_euler_zxz (dLonAscNode_deg, dInclination_deg, dArgPerihelion_deg)

    dLonPerihelionDot = dArgPerihelionDot + dLonAscNodeDot

    lon1 = dLonAscNode_deg + dLonAscNodeDot
    inc1 = dInclination_deg + dInclinationDot
    argp1 = (dLonAscNode_deg + dLonAscNodeDot + dArgPerihelion_deg + dLonPerihelionDot) - lon1
    # Actually, the longitude of perihelion is ω̃ = Ω + ω
    # After one century: Ω₁ = Ω + dLonAscNodeDot, ω̃₁ = ω̃ + dLonPerihelionDot
    # So ω₁ = ω̃₁ - Ω₁ = (ω̃ + dLonPerihelionDot) - (Ω + dLonAscNodeDot)
    #        = ω + dArgPerihelionDot
    argp1 = dArgPerihelion_deg + dArgPerihelionDot

    qx1, qy1, qz1, qw1 = quat_from_euler_zxz (lon1, inc1, argp1)

    # q_delta = q1 * q0^-1
    # q0^-1 = conjugate for unit quaternion = (-qx0, -qy0, -qz0, qw0)
    dw = qw1 * qw0 + qx1 * qx0 + qy1 * qy0 + qz1 * qz0
    dx = -qw1 * qx0 + qx1 * qw0 - qy1 * qz0 + qz1 * qy0
    dy = -qw1 * qy0 + qy1 * qw0 - qz1 * qx0 + qx1 * qz0
    dz = -qw1 * qz0 + qz1 * qw0 - qx1 * qy0 + qy1 * qx0

    if dw < 0:
        dx, dy, dz, dw = -dx, -dy, -dz, -dw

    angle = 2.0 * math.acos (min (1.0, max (-1.0, dw)))
    s = math.sqrt (dx * dx + dy * dy + dz * dz)

    if s < 1e-30 or angle < 1e-30:
        return 0.0, 0.0, 0.0

    rate = angle / dt_ticks
    return dx / s * rate, dy / s * rate, dz / s * rate


def compute_msf (d):
    """Compute MSF wire format values from a moonsystem dict."""
    dSemiMajor_AU = d.get ("dSemiMajorAU", 0)
    dEcc          = d.get ("dEccentricity", 0)
    dInc          = d.get ("dInclination", 0)
    dLonAscNode   = d.get ("dLonAscNode", 0)
    dArgPeri      = d.get ("dArgPerihelion", 0)
    dMeanAnom     = d.get ("dMeanAnomaly", 0)

    dA = dSemiMajor_AU * AU_M
    dB = dA * math.sqrt (1.0 - dEcc * dEcc) if dEcc < 1.0 else dA

    # Period
    dPeriodDays = d.get ("dPeriodDays", 0)
    dMeanLongDot = d.get ("dMeanLongitudeDot", 0)

    if dMeanLongDot != 0:
        # Rate-based (Earth's Moon)
        dLonAscNodeDot  = d.get ("dLonAscNodeDot", 0)
        dArgPeriDot     = d.get ("dArgPerihelionDot", 0)
        dLonPeriDot     = dArgPeriDot + dLonAscNodeDot
        dMeanAnomalyDot = dMeanLongDot - dLonPeriDot  # °/century
        dPeriod_cy      = 360.0 / dMeanAnomalyDot     # centuries
        tmPeriod        = int (round (dPeriod_cy * DAYS_PER_CENTURY * TICKS_PER_DAY))
    elif dPeriodDays > 0:
        tmPeriod = int (round (dPeriodDays * TICKS_PER_DAY))
    else:
        tmPeriod = 0

    # Origin (tmOrigin) from mean anomaly
    if tmPeriod > 0 and dMeanAnom != 0:
        dFrac    = dMeanAnom / 360.0
        tmOrigin = int (round (dFrac * tmPeriod))
    else:
        tmOrigin = 0

    # Quaternion
    qx, qy, qz, qw = quat_from_euler_zxz (dLonAscNode, dInc, dArgPeri)

    # Precession
    dLonAscNodeDot  = d.get ("dLonAscNodeDot", 0)
    dIncDot         = d.get ("dInclinationDot", 0)
    dArgPeriDot     = d.get ("dArgPerihelionDot", 0)
    precX, precY, precZ = precession_vector (dLonAscNodeDot, dIncDot, dArgPeriDot,
                                              dLonAscNode, dInc, dArgPeri)

    return {
        "dA": dA, "dB": dB, "tmPeriod": tmPeriod, "tmOrigin": tmOrigin,
        "qx": qx, "qy": qy, "qz": qz, "qw": qw,
        "precX": precX, "precY": precY, "precZ": precZ,
    }


def color_to_u32 (pColor):
    """Convert (r, g, b) tuple to 0x00RRGGBB."""
    if isinstance (pColor, tuple):
        r, g, b = pColor
        return (r << 16) | (g << 8) | b
    return 0x00888888


# ---------------------------------------------------------------------------
# Generate Rust source
# ---------------------------------------------------------------------------

def generate_rs (sParent, aMoons):
    """Generate a Rust source file for one parent planet's moons."""
    sFilename = f"moons_{sParent}.rs"
    sPath     = os.path.join (OUT_DIR, sFilename)
    nCount    = len (aMoons)

    lines = []
    lines.append (f"use crate::data::BODY_DATA;")
    lines.append (f"")
    lines.append (f"pub static MOONS_{sParent.upper ()}: [BODY_DATA; {nCount}] =")
    lines.append (f"[")

    for moon in aMoons:
        sName    = moon["sName"]
        dRadius  = moon.get ("dRadius", 1.0)
        fMass    = moon.get ("dMass", 0.0)
        nColor   = color_to_u32 (moon.get ("pColor", (0x88, 0x88, 0x88)))
        sTexture = moon.get ("sTexture", "generic_moon-0.png")
        msf      = moon["msf"]

        # Format mass as scientific notation
        if fMass == 0:
            sMass = "0.0"
        else:
            sMass = f"{fMass:.3e}"

        lines.append (f"   // {sName}")
        lines.append (f"   BODY_DATA")
        lines.append (f"   {{")
        lines.append (f'      sName: "{sName}", bSubtype: crate::data::SUBTYPE_MOONSYSTEM,')
        lines.append (f"      dRadius: {dRadius * 1000.0:.1f}, fMass: {sMass}, nColor: 0x{nColor:08X},")
        lines.append (f'      sTexture: "{sTexture}",')
        lines.append (f"      dA: {msf['dA']}, dB: {msf['dB']},")
        lines.append (f"      tmPeriod: {msf['tmPeriod']}, tmOrigin: {msf['tmOrigin']},")
        lines.append (f"      qx: {msf['qx']}, qy: {msf['qy']},")
        lines.append (f"      qz: {msf['qz']}, qw: {msf['qw']},")
        lines.append (f"      precX: {msf['precX']}, precY: {msf['precY']}, precZ: {msf['precZ']},")
        lines.append (f"   }},")
        lines.append (f"")

    lines.append (f"];")
    lines.append (f"")

    with open (sPath, "w", encoding="utf-8") as f:
        f.write ("\n".join (lines))

    print (f"  {sFilename}: {nCount} moons")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main ():
    nTotal = 0

    for sParent in PARENTS:
        # Parse all three data layers
        sOrbit   = os.path.join (JS_DIR, f"moonsystem_{sParent}.js")
        sPhys    = os.path.join (JS_DIR, f"moon_{sParent}.js")
        sSurface = os.path.join (JS_DIR, f"surface_moon_{sParent}.js")

        if not os.path.exists (sOrbit):
            print (f"  SKIP: {sParent} (no moonsystem file)")
            continue

        aOrbit   = parse_js_objects (sOrbit)
        aPhys    = parse_js_objects (sPhys) if os.path.exists (sPhys) else []
        aSurface = parse_js_objects (sSurface) if os.path.exists (sSurface) else []

        # Index physical and surface by sId_Parent -> dict
        dPhys    = {}
        for d in aPhys:
            sIdParent = d.get ("sId_Parent", "")
            if sIdParent not in dPhys:
                dPhys[sIdParent] = d

        dSurface = {}
        for d in aSurface:
            sIdParent = d.get ("sId_Parent", "")
            if sIdParent not in dSurface:
                dSurface[sIdParent] = d

        # Join and compute MSF
        aMoons = []
        for orb in aOrbit:
            sId   = orb.get ("sId", "")
            sName = orb.get ("sName", sId).replace (" System", "")

            # Find matching physical entry (moon's sId_Parent == moonsystem's sId)
            phys = dPhys.get (sId, {})

            # Find matching surface entry (surface's sId_Parent == moon's sId)
            sMoonId = phys.get ("sId", sId.replace ("_system", ""))
            surf = dSurface.get (sMoonId, {})

            # Merge data
            moon = {
                "sName":   sName,
                "dRadius": phys.get ("dRadius", orb.get ("dRadius", 1.0)),
                "dMass":   phys.get ("dMass", orb.get ("dMass", 0.0)),
                "pColor":  phys.get ("pColor", orb.get ("pColor", (0x88, 0x88, 0x88))),
            }

            # Texture: strip 'img/' prefix if present
            sTexture = surf.get ("sTexture", "")
            if sTexture.startswith ("img/"):
                sTexture = sTexture[4:]
            if not sTexture:
                # Generate a generic texture based on nSourceIx
                nIx = orb.get ("nSourceIx", 0)
                sTexture = f"generic_moon-{nIx % 5}.png"
            moon["sTexture"] = sTexture

            # Compute MSF
            moon["msf"] = compute_msf (orb)

            aMoons.append (moon)

        if aMoons:
            generate_rs (sParent, aMoons)
            nTotal += len (aMoons)

    print (f"\nTotal: {nTotal} moons generated across {len (PARENTS)} parent planets")


if __name__ == "__main__":
    main ()
