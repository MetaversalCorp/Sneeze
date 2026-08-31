// Copyright 2026 Metaversal Corporation
#include <XrTracking.h>

#include <cstdio>
#include <cstring>
#include <string>

static int nPassed = 0;
static int nFailed = 0;

static void Check (bool b, const char* name)
{
   if (b) { std::printf ("  PASS: %s\n", name); ++nPassed; }
   else   { std::printf ("  FAIL: %s\n", name); ++nFailed; }
}

int RunXrTrackingTests (int /*nArgc*/, char** /*aArgv*/)
{
   nPassed = 0;
   nFailed = 0;

   std::printf ("\n[XrTracking] face key table\n");
   Check (SNEEZE::XrFaceParameterWebXrKeyCount () == 68, "68 face keys");
   Check (std::strcmp (SNEEZE::XrFaceParameterWebXrKeys ()[24], "jaw_drop") == 0, "index 24 jaw_drop");
   Check (std::strcmp (SNEEZE::XrFaceParameterWebXrKeys ()[12], "eyes_closed_left") == 0, "index 12 eyes_closed_left");

   std::printf ("\n[XrTracking] body name table\n");
   Check (SNEEZE::XrBodyUpperJointHumanoidNameCount () == 14, "14 body joints");
   Check (std::strcmp (SNEEZE::XrBodyUpperJointHumanoidNames ()[5], "head") == 0, "joint 5 head");

   SNEEZE::XR_FACE_STATE face {};
   face.bValid = true;
   face.aParameters[24] = 0.5f;
   const std::string w = SNEEZE::XrFaceStateToWebXrWeightsJson (face, false);
   Check (w.find ("jaw_drop") != std::string::npos, "weights json has jaw_drop");
   Check (w.find ("eyes_closed") == std::string::npos, "omits zero weights");

   SNEEZE::XR_BODY_STATE body {};
   body.bValid = true;
   body.nJointCount = 14;
   body.aJoints[5].bValid = true;
   body.aJoints[5].fPosY = 1.6f;
   const std::string payload = SNEEZE::XrTrackingPayloadJson (face, body, 12345, "test");
   Check (payload.find ("\"t\":12345") != std::string::npos, "payload timestamp");
   Check (payload.find ("openxrParameters") != std::string::npos, "payload openxrParameters");
   Check (payload.find ("\"name\":\"head\"") != std::string::npos, "payload head joint");

   SNEEZE::XR_FACE_STATE parsedFace {};
   SNEEZE::XR_BODY_STATE parsedBody {};
   Check (SNEEZE::XrTrackingPayloadParse (payload, parsedFace, parsedBody), "parse roundtrip");
   Check (parsedFace.bValid && parsedFace.aParameters[24] > 0.4f, "parse jaw_drop");

   std::printf ("\n[XrTracking] %d passed, %d failed\n", nPassed, nFailed);
   return nFailed == 0 ? 0 : 1;
}
