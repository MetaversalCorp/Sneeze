// Copyright 2026 Metaversal Corporation
//
// Android XR / Galaxy XR: OpenXR session + XR_ANDROID_face_tracking +
// XR_ANDROIDSYS_body_tracking. Non-Android builds get no-op stubs.
//
// Patterns adapted from OpenNexus3DStudio/native/android-xr-face-bridge
// (openxr_face_engine.cpp). Permissions (FACE_TRACKING, BODY_TRACKING) are
// requested by the Android host APK before BeginAndroidSession.

#include "xr/XrRuntimeImpl.h"

#include <cstring>

#ifndef __ANDROID__

namespace SNEEZE { namespace DEP {

bool XrAndroid_BeginSession (XR_RUNTIME::Impl* impl, void*, void*, void*)
{
   if (impl && impl->m_pEngine)
      impl->m_pEngine->Log (IENGINE::kLOGLEVEL_Info, "XR_RUNTIME",
         "BeginAndroidSession: not an Android build — use InjectFaceFixture / InjectBodyFixture.");
   return false;
}

void XrAndroid_EndSession (XR_RUNTIME::Impl* impl)
{
   if (!impl)
      return;
   impl->pAndroidSession = nullptr;
   impl->caps.bSessionActive = false;
   impl->caps.bFaceTrackerActive = false;
   impl->caps.bBodyTrackerActive = false;
}

bool XrAndroid_PumpTracking (XR_RUNTIME::Impl*)
{
   return false;
}

}} // namespace SNEEZE::DEP

#else // __ANDROID__

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <android/log.h>
#include <android/native_window.h>

#include <chrono>
#include <dlfcn.h>
#include <mutex>
#include <vector>

#ifndef XR_ANDROID_FACE_TRACKING_EXTENSION_NAME
#define XR_ANDROID_FACE_TRACKING_EXTENSION_NAME "XR_ANDROID_face_tracking"
#endif
#ifndef XR_ANDROIDSYS_BODY_TRACKING_EXTENSION_NAME
#define XR_ANDROIDSYS_BODY_TRACKING_EXTENSION_NAME "XR_ANDROIDSYS_body_tracking"
#endif
#ifndef XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME
#define XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME "XR_KHR_android_create_instance"
#endif
#ifndef XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME
#define XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME "XR_KHR_opengl_es_enable"
#endif

// Vendored ANDROIDSYS body types if the linked OpenXR headers lack them
// (Khronos 1.1.58 may omit vendor previews; OpenNexus jetpack headers include them).
#ifndef XR_ANDROIDSYS_body_tracking
XR_DEFINE_HANDLE(XrBodyTrackerANDROIDSYS)
#define XR_ANDROIDSYS_BODY_TRACKING_EXTENSION_NAME "XR_ANDROIDSYS_body_tracking"
#define XR_BODY_UPPER_BODY_JOINT_COUNT_ANDROIDSYS 14
typedef enum XrBodyJointSetANDROIDSYS {
    XR_BODY_JOINT_SET_UPPER_BODY_ANDROIDSYS = 0,
    XR_BODY_JOINT_SET_MAX_ENUM_ANDROIDSYS = 0x7FFFFFFF
} XrBodyJointSetANDROIDSYS;
typedef struct XrBodyJointLocationsANDROIDSYS {
    XrStructureType         type;
    void *                  next;
    XrTime                  lastUpdateTime;
    XrUuid                  restSkeletonGenerationUuid;
    uint32_t                jointCount;
    XrSpaceLocationData*    joints;
} XrBodyJointLocationsANDROIDSYS;
typedef struct XrBodyTrackerCreateInfoANDROIDSYS {
    XrStructureType             type;
    const void *                next;
    XrBodyJointSetANDROIDSYS    jointSet;
} XrBodyTrackerCreateInfoANDROIDSYS;
typedef struct XrBodyJointsLocateInfoANDROIDSYS {
    XrStructureType    type;
    const void *       next;
    XrTime             time;
    XrSpace            space;
} XrBodyJointsLocateInfoANDROIDSYS;
#ifndef XR_TYPE_BODY_TRACKER_CREATE_INFO_ANDROIDSYS
// Structure type values from jetpack-xr-natives / Android XR preview headers.
// If the runtime rejects these, PumpTracking will clear body validity.
#define XR_TYPE_BODY_TRACKER_CREATE_INFO_ANDROIDSYS ((XrStructureType)1000460000)
#define XR_TYPE_BODY_JOINTS_LOCATE_INFO_ANDROIDSYS ((XrStructureType)1000460001)
#define XR_TYPE_BODY_JOINT_LOCATIONS_ANDROIDSYS ((XrStructureType)1000460002)
#endif
#endif

#define LOG_TAG "Sneeze-XR-Android"
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define ALOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace SNEEZE { namespace DEP {
namespace {

struct AndroidSession
{
   XrInstance instance = XR_NULL_HANDLE;
   XrSession  session = XR_NULL_HANDLE;
   XrSpace    localSpace = XR_NULL_HANDLE;
   XrFaceTrackerANDROID faceTracker = XR_NULL_HANDLE;
   XrBodyTrackerANDROIDSYS bodyTracker = XR_NULL_HANDLE;
   bool ownsInstance = false;

   PFN_xrCreateFaceTrackerANDROID pfnCreateFace = nullptr;
   PFN_xrDestroyFaceTrackerANDROID pfnDestroyFace = nullptr;
   PFN_xrGetFaceStateANDROID pfnGetFace = nullptr;

   using PFN_CreateBody = XrResult (XRAPI_PTR*)(XrSession, const XrBodyTrackerCreateInfoANDROIDSYS*, XrBodyTrackerANDROIDSYS*);
   using PFN_DestroyBody = XrResult (XRAPI_PTR*)(XrBodyTrackerANDROIDSYS);
   using PFN_LocateBody = XrResult (XRAPI_PTR*)(XrBodyTrackerANDROIDSYS, const XrBodyJointsLocateInfoANDROIDSYS*, XrBodyJointLocationsANDROIDSYS*);
   PFN_CreateBody pfnCreateBody = nullptr;
   PFN_DestroyBody pfnDestroyBody = nullptr;
   PFN_LocateBody pfnLocateBody = nullptr;
};

bool LoadProc (XrInstance inst, const char* name, PFN_xrVoidFunction* out)
{
   return XR_SUCCEEDED (xrGetInstanceProcAddr (inst, name, out)) && *out != nullptr;
}

} // namespace

bool XrAndroid_BeginSession (XR_RUNTIME::Impl* impl, void* pJavaVM, void* pActivity, void* /*pNativeWindow*/)
{
   if (!impl || !pJavaVM || !pActivity)
      return false;

   XrAndroid_EndSession (impl);

   auto* as = new AndroidSession ();

   // Prefer existing engine instance when present.
   as->instance = impl->hInstance;
   as->ownsInstance = false;

   if (as->instance == XR_NULL_HANDLE)
   {
      XrLoaderInitInfoAndroidKHR loaderInfo{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
      loaderInfo.applicationVM = pJavaVM;
      loaderInfo.applicationContext = pActivity;
      PFN_xrInitializeLoaderKHR pfnInit = nullptr;
      if (!LoadProc (XR_NULL_HANDLE, "xrInitializeLoaderKHR",
                     reinterpret_cast<PFN_xrVoidFunction*> (&pfnInit)) || !pfnInit)
      {
         ALOGE ("xrInitializeLoaderKHR missing");
         delete as;
         return false;
      }
      if (XR_FAILED (pfnInit (reinterpret_cast<XrLoaderInitInfoBaseHeaderKHR*> (&loaderInfo))))
      {
         ALOGE ("xrInitializeLoaderKHR failed");
         delete as;
         return false;
      }

      const char* exts[] = {
         XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
         XR_ANDROID_FACE_TRACKING_EXTENSION_NAME,
         XR_ANDROIDSYS_BODY_TRACKING_EXTENSION_NAME,
         XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME
      };

      XrInstanceCreateInfoAndroidKHR androidCi{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
      androidCi.applicationVM = pJavaVM;
      androidCi.applicationActivity = pActivity;

      XrApplicationInfo appInfo {};
      std::strncpy (appInfo.applicationName, "Sneeze", XR_MAX_APPLICATION_NAME_SIZE);
      appInfo.applicationVersion = 1;
      std::strncpy (appInfo.engineName, "MBE", XR_MAX_ENGINE_NAME_SIZE);
      appInfo.engineVersion = 1;
      appInfo.apiVersion = XR_MAKE_VERSION (1, 0, 34);

      XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
      ci.next = &androidCi;
      ci.applicationInfo = appInfo;
      ci.enabledExtensionCount = 4;
      ci.enabledExtensionNames = exts;

      if (XR_FAILED (xrCreateInstance (&ci, &as->instance)))
      {
         ALOGE ("xrCreateInstance failed");
         delete as;
         return false;
      }
      as->ownsInstance = true;
      impl->hInstance = as->instance;
      impl->bHasRuntime = true;
   }

   XrSystemGetInfo sysInfo{XR_TYPE_SYSTEM_GET_INFO};
   sysInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
   XrSystemId systemId = XR_NULL_SYSTEM_ID;
   if (XR_FAILED (xrGetSystem (as->instance, &sysInfo, &systemId)))
   {
      ALOGE ("xrGetSystem failed");
      if (as->ownsInstance)
         xrDestroyInstance (as->instance);
      delete as;
      return false;
   }

   // Graphics binding is required by most Android XR runtimes; OpenNexus uses
   // an EGL PBuffer. Hosts should create GLES context before calling this.
   // Tracking-only: attempt session without binding; if it fails, log and return.
   XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
   sci.systemId = systemId;
   XrResult sr = xrCreateSession (as->instance, &sci, &as->session);
   if (XR_FAILED (sr))
   {
      ALOGW ("xrCreateSession without graphics binding failed (%d) — host must supply GLES binding (see OpenNexus openxr_gfx_egl).",
             static_cast<int> (sr));
      if (as->ownsInstance)
         xrDestroyInstance (as->instance);
      delete as;
      return false;
   }

   XrReferenceSpaceCreateInfo spaceCi{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
   spaceCi.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
   spaceCi.poseInReferenceSpace.orientation.w = 1.f;
   if (XR_FAILED (xrCreateReferenceSpace (as->session, &spaceCi, &as->localSpace)))
      ALOGW ("xrCreateReferenceSpace LOCAL failed");

   XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
   begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
   xrBeginSession (as->session, &begin);

   if (LoadProc (as->instance, "xrCreateFaceTrackerANDROID",
                 reinterpret_cast<PFN_xrVoidFunction*> (&as->pfnCreateFace)) &&
       LoadProc (as->instance, "xrDestroyFaceTrackerANDROID",
                 reinterpret_cast<PFN_xrVoidFunction*> (&as->pfnDestroyFace)) &&
       LoadProc (as->instance, "xrGetFaceStateANDROID",
                 reinterpret_cast<PFN_xrVoidFunction*> (&as->pfnGetFace)))
   {
      XrFaceTrackerCreateInfoANDROID fci{XR_TYPE_FACE_TRACKER_CREATE_INFO_ANDROID};
      if (XR_SUCCEEDED (as->pfnCreateFace (as->session, &fci, &as->faceTracker)))
      {
         impl->caps.bFaceTrackerActive = true;
         ALOGI ("Face tracker created (XR_ANDROID_face_tracking)");
      }
      else
         ALOGW ("xrCreateFaceTrackerANDROID failed — check android.permission.FACE_TRACKING");
   }

   if (LoadProc (as->instance, "xrCreateBodyTrackerANDROIDSYS",
                 reinterpret_cast<PFN_xrVoidFunction*> (&as->pfnCreateBody)) &&
       LoadProc (as->instance, "xrDestroyBodyTrackerANDROIDSYS",
                 reinterpret_cast<PFN_xrVoidFunction*> (&as->pfnDestroyBody)) &&
       LoadProc (as->instance, "xrLocateBodyJointsANDROIDSYS",
                 reinterpret_cast<PFN_xrVoidFunction*> (&as->pfnLocateBody)))
   {
      XrBodyTrackerCreateInfoANDROIDSYS bci{XR_TYPE_BODY_TRACKER_CREATE_INFO_ANDROIDSYS};
      bci.jointSet = XR_BODY_JOINT_SET_UPPER_BODY_ANDROIDSYS;
      if (XR_SUCCEEDED (as->pfnCreateBody (as->session, &bci, &as->bodyTracker)))
      {
         impl->caps.bBodyTrackerActive = true;
         ALOGI ("Body tracker created (XR_ANDROIDSYS_body_tracking)");
      }
      else
         ALOGW ("xrCreateBodyTrackerANDROIDSYS failed — check BODY_TRACKING permission");
   }

   impl->caps.bSessionActive = true;
   impl->pAndroidSession = as;
   if (impl->m_pEngine)
      impl->m_pEngine->Log (IENGINE::kLOGLEVEL_Info, "XR_RUNTIME",
         "Android XR session started (face=" +
         std::string (impl->caps.bFaceTrackerActive ? "on" : "off") +
         " body=" + std::string (impl->caps.bBodyTrackerActive ? "on" : "off") + ")");
   return true;
}

void XrAndroid_EndSession (XR_RUNTIME::Impl* impl)
{
   if (!impl || !impl->pAndroidSession)
      return;
   auto* as = static_cast<AndroidSession*> (impl->pAndroidSession);
   if (as->faceTracker && as->pfnDestroyFace)
      as->pfnDestroyFace (as->faceTracker);
   if (as->bodyTracker && as->pfnDestroyBody)
      as->pfnDestroyBody (as->bodyTracker);
   if (as->localSpace)
      xrDestroySpace (as->localSpace);
   if (as->session)
   {
      xrEndSession (as->session);
      xrDestroySession (as->session);
   }
   if (as->ownsInstance && as->instance)
   {
      xrDestroyInstance (as->instance);
      if (impl->hInstance == as->instance)
         impl->hInstance = XR_NULL_HANDLE;
   }
   delete as;
   impl->pAndroidSession = nullptr;
   impl->caps.bSessionActive = false;
   impl->caps.bFaceTrackerActive = false;
   impl->caps.bBodyTrackerActive = false;
}

bool XrAndroid_PumpTracking (XR_RUNTIME::Impl* impl)
{
   if (!impl || !impl->pAndroidSession)
      return false;
   auto* as = static_cast<AndroidSession*> (impl->pAndroidSession);

   XR_FACE_STATE face {};
   XR_BODY_STATE body {};

   if (as->faceTracker && as->pfnGetFace)
   {
      XrFaceStateGetInfoANDROID gi{XR_TYPE_FACE_STATE_GET_INFO_ANDROID};
      float params[kXR_FACE_PARAMETER_COUNT] {};
      XrFaceStateANDROID state{XR_TYPE_FACE_STATE_ANDROID};
      state.parameters = params;
      // parametersCapacityInput naming varies; set both common fields via next if needed.
      // Khronos: parametersCapacityInput / parametersCountOutput.
      state.parametersCapacityInput = kXR_FACE_PARAMETER_COUNT;
      if (XR_SUCCEEDED (as->pfnGetFace (as->faceTracker, &gi, &state)))
      {
         face.bValid = state.isValid == XR_TRUE;
         face.bTracking = state.faceTrackingState == XR_FACE_TRACKING_STATE_TRACKING_ANDROID;
         face.nSampleTimeNs = static_cast<int64_t> (state.sampleTime);
         const uint32_t nOut = state.parametersCountOutput < kXR_FACE_PARAMETER_COUNT
                                  ? state.parametersCountOutput
                                  : static_cast<uint32_t> (kXR_FACE_PARAMETER_COUNT);
         for (uint32_t i = 0; i < nOut; ++i)
            face.aParameters[i] = params[i];
      }
   }

   if (as->bodyTracker && as->pfnLocateBody && as->localSpace)
   {
      XrSpaceLocationData joints[XR_BODY_UPPER_BODY_JOINT_COUNT_ANDROIDSYS] {};
      XrBodyJointsLocateInfoANDROIDSYS li{XR_TYPE_BODY_JOINTS_LOCATE_INFO_ANDROIDSYS};
      li.time = 0; // runtime may accept 0 as "now" — prefer predicted display time when available
      li.space = as->localSpace;
      XrBodyJointLocationsANDROIDSYS locs{XR_TYPE_BODY_JOINT_LOCATIONS_ANDROIDSYS};
      locs.jointCount = XR_BODY_UPPER_BODY_JOINT_COUNT_ANDROIDSYS;
      locs.joints = joints;
      if (XR_SUCCEEDED (as->pfnLocateBody (as->bodyTracker, &li, &locs)))
      {
         body.bValid = true;
         body.nJointCount = static_cast<int> (locs.jointCount);
         body.nSampleTimeNs = static_cast<int64_t> (locs.lastUpdateTime);
         const int n = body.nJointCount < kXR_BODY_UPPER_JOINT_COUNT
                          ? body.nJointCount
                          : kXR_BODY_UPPER_JOINT_COUNT;
         for (int i = 0; i < n; ++i)
         {
            XR_BODY_JOINT& j = body.aJoints[static_cast<size_t> (i)];
            const XrSpaceLocationData& d = joints[i];
            j.bValid = (d.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0;
            j.fPosX = d.pose.position.x;
            j.fPosY = d.pose.position.y;
            j.fPosZ = d.pose.position.z;
            j.fOriX = d.pose.orientation.x;
            j.fOriY = d.pose.orientation.y;
            j.fOriZ = d.pose.orientation.z;
            j.fOriW = d.pose.orientation.w;
         }
      }
   }

   {
      std::lock_guard<std::mutex> lock (impl->mutex);
      if (!impl->bFaceFixture && (face.bValid || face.bTracking))
         impl->faceCached = face;
      if (!impl->bBodyFixture && body.bValid)
         impl->bodyCached = body;
   }
   return face.bValid || body.bValid;
}

}} // namespace SNEEZE::DEP

#endif // __ANDROID__
