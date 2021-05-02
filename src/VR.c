#include "VR.h"

#include <GL/glew.h>
#include <cglm/struct.h>
#include <openvr_capi.h>
#include <stdint.h>
#include <windows.h>

#include "Constants.h"
#include "Entity.h"
#include "Funcs.h"
#include "Game.h"
#include "Graphics.h"
#include "Input.h"
#include "Logger.h"
#include "Platform.h"
#include "String.h"
#include "VRInternal.h"
#include "Vectors.h"
#include "Window.h"

mat4s g_rmat4DevicePose[64 /* k_unMaxTrackedDeviceCount */];
mat4s g_mat4HMDPose;

void VR_UpdateHMDMatrixPose() {
  EVRCompositorError error = g_pCompositor->WaitGetPoses(
      g_rTrackedDevicePose, k_unMaxTrackedDeviceCount, NULL, 0);
  if (error != EVRCompositorError_VRCompositorError_None &&
      error != EVRCompositorError_VRCompositorError_DoNotHaveFocus) {
    Logger_Abort2(error, "WaitGetPoses");
    return;
  }

  for (unsigned int nDevice = 0; nDevice < k_unMaxTrackedDeviceCount;
       ++nDevice) {
    if (g_rTrackedDevicePose[nDevice].bPoseIsValid) {
      g_rmat4DevicePose[nDevice] = Mat4sFromHmdMatrix34(
          g_rTrackedDevicePose[nDevice].mDeviceToAbsoluteTracking);
    }
  }

  if (g_rTrackedDevicePose[k_unTrackedDeviceIndex_Hmd].bPoseIsValid) {
    g_mat4HMDPose =
        glms_mat4_inv(g_rmat4DevicePose[k_unTrackedDeviceIndex_Hmd]);

    // mat4s a = g_rmat4DevicePose[k_unTrackedDeviceIndex_Hmd];
    // printf("%0.2f %0.2f %0.2f\n", a.m30, a.m31, a.m32);
  }
}

void APIENTRY DebugCallback(GLenum source,
                            GLenum type,
                            GLuint id,
                            GLenum severity,
                            GLsizei length,
                            const char* message,
                            const void* userParam) {
  printf("GL Error: %s\n", message);
}

void VR_Setup() {
  char errorMessage[256];

  glewExperimental = GL_TRUE;
  GLenum nGlewError = glewInit();
  if (nGlewError != GLEW_OK) {
    snprintf(errorMessage, sizeof(errorMessage), "glewInit: %s",
             glewGetErrorString(nGlewError));
    Logger_Abort2(nGlewError, errorMessage);
    return;
  }
  glGetError();  // to clear the error caused deep in GLEW

  glDebugMessageCallback((GLDEBUGPROC)DebugCallback, NULL);
  glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DONT_CARE, 0, NULL,
                        GL_TRUE);
  glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);

  EVRInitError error;
  VR_InitInternal(&error, EVRApplicationType_VRApplication_Scene);
  if (error != EVRInitError_VRInitError_None) {
    snprintf(errorMessage, sizeof(errorMessage), "VR_InitInternal: %s",
             VR_GetStringForHmdError(error));
    Logger_Abort2(error, errorMessage);
    return;
  }

  if (!VR_IsInterfaceVersionValid(IVRSystem_Version)) {
    VR_ShutdownInternal();
    Logger_Abort("!VR_IsInterfaceVersionValid System");
    return;
  }

  // get System interface
  char interfaceName[256];
  snprintf(interfaceName, sizeof(interfaceName), "FnTable:%s",
           IVRSystem_Version);
  g_pSystem = (struct VR_IVRSystem_FnTable*)VR_GetGenericInterface(
      interfaceName, &error);
  if (error != EVRInitError_VRInitError_None) {
    snprintf(errorMessage, sizeof(errorMessage),
             "VR_GetGenericInterface System: %s",
             VR_GetStringForHmdError(error));
    Logger_Abort2(error, errorMessage);
    return;
  }

  // get Compositor interface
  snprintf(interfaceName, sizeof(interfaceName), "FnTable:%s",
           IVRCompositor_Version);
  g_pCompositor = (struct VR_IVRCompositor_FnTable*)VR_GetGenericInterface(
      interfaceName, &error);
  if (error != EVRInitError_VRInitError_None) {
    snprintf(errorMessage, sizeof(errorMessage),
             "VR_GetGenericInterface Compositor: %s",
             VR_GetStringForHmdError(error));
    Logger_Abort2(error, errorMessage);
    return;
  }

  // get Input interface
  snprintf(interfaceName, sizeof(interfaceName), "FnTable:%s",
           IVRInput_Version);
  g_pInput = (struct VR_IVRInput_FnTable*)VR_GetGenericInterface(interfaceName,
                                                                 &error);
  if (error != EVRInitError_VRInitError_None) {
    snprintf(errorMessage, sizeof(errorMessage),
             "VR_GetGenericInterface Input: %s",
             VR_GetStringForHmdError(error));
    Logger_Abort2(error, errorMessage);
    return;
  }

  // get RenderModels interface
  snprintf(interfaceName, sizeof(interfaceName), "FnTable:%s",
           IVRRenderModels_Version);
  g_pRenderModels = (struct VR_IVRRenderModels_FnTable*)VR_GetGenericInterface(
      interfaceName, &error);
  if (error != EVRInitError_VRInitError_None) {
    snprintf(errorMessage, sizeof(errorMessage),
             "VR_GetGenericInterface RenderModels: %s",
             VR_GetStringForHmdError(error));
    Logger_Abort2(error, errorMessage);
    return;
  }

  // set window title
  cc_string title;
  char titleBuffer[STRING_SIZE];
  String_InitArray(title, titleBuffer);

  char headsetName[256];
  ETrackedPropertyError propError;
  g_pSystem->GetStringTrackedDeviceProperty(
      k_unTrackedDeviceIndex_Hmd,
      ETrackedDeviceProperty_Prop_TrackingSystemName_String, headsetName,
      sizeof(headsetName), &propError);
  if (propError != ETrackedPropertyError_TrackedProp_Success) {
    Logger_Abort2(
        propError,
        "GetStringTrackedDeviceProperty Prop_TrackingSystemName_String");
    return;
  }
  String_Format3(&title, "%c (%s) - %c", GAME_APP_TITLE, &Game_Username,
                 &headsetName);
  Window_SetTitle(&title);

  SetupInput();
  SetupCameras();
  SetupStereoRenderTargets();
}

void RenderCompanionWindow() {
  Gfx_Begin2D(Game.Width, Game.Height);
  glViewport(0, 0, Game.Width, Game.Height);
  Gfx_SetTexturing(true);
  struct Texture tex;

  // render left eye (first half of index array )
  tex.ID = g_descLeftEye.m_nRenderTextureId;
  tex.X = 0;
  tex.Y = 0;
  tex.Width = Game.Width / 2;
  tex.Height = Game.Height;
  tex.uv.U1 = 0;
  tex.uv.U2 = 1;
  tex.uv.V1 = 1;
  tex.uv.V2 = 0;
  Texture_Render(&tex);

  // render right eye (second half of index array )
  tex.ID = g_descRightEye.m_nRenderTextureId;
  tex.X = Game.Width / 2;
  tex.Y = 0;
  tex.Width = Game.Width / 2;
  tex.Height = Game.Height;
  tex.uv.U1 = 0;
  tex.uv.U2 = 1;
  tex.uv.V1 = 1;
  tex.uv.V2 = 0;
  Texture_Render(&tex);

  Gfx_SetTexturing(false);
  Gfx_End2D();
}

static void RenderController(Hmd_Eye nEye, struct Controller* c) {
  struct Matrix modelView =
      MatrixFromMat4s(glms_mat4_mul(g_mat4HMDPose, c->pose));
  Gfx_LoadMatrix(MATRIX_VIEW, &modelView);

  glBindVertexArray(c->glVertArray);
  glBindBuffer(GL_ARRAY_BUFFER, c->glVertBuffer);
  Gfx_BindIb(c->glIndexBuffer);

  glColor3f(0.0f, 0.0f, 0.0f);
  glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
  glDrawElements(GL_TRIANGLES, c->model->unTriangleCount * 3, GL_UNSIGNED_SHORT,
                 0);
  glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

  Gfx_BindIb(Gfx_defaultIb);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glBindVertexArray(0);

  glBegin(GL_LINES);
  glVertex3f(0, 0, 0);
  glVertex3f(0, 0, -LocalPlayer_Instance.ReachDistance);
  glEnd();

  Gfx_LoadMatrix(MATRIX_VIEW, &Gfx.View);
}

void VR_RenderControllers(Hmd_Eye nEye) {
  if (g_controllerLeft.initialized)
    RenderController(nEye, &g_controllerLeft);
  if (g_controllerRight.initialized)
    RenderController(nEye, &g_controllerRight);
}

void VR_RenderStereoTargets(void (*RenderScene)(Hmd_Eye nEye,
                                                double delta,
                                                float t),
                            double delta,
                            float t) {
  // Left Eye
  glBindFramebuffer(GL_FRAMEBUFFER, g_descLeftEye.m_nRenderFramebufferId);
  glViewport(0, 0, g_nRenderWidth, g_nRenderHeight);
  RenderScene(EVREye_Eye_Left, delta, t);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);

  // Right Eye
  glBindFramebuffer(GL_FRAMEBUFFER, g_descRightEye.m_nRenderFramebufferId);
  glViewport(0, 0, g_nRenderWidth, g_nRenderHeight);
  RenderScene(EVREye_Eye_Right, delta, t);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void VR_BeginFrame() {
  UpdateInput();
}

void VR_EndFrame() {
  // end RenderStereoTargets();

  RenderCompanionWindow();

  // after RenderCompanionWindow();
  EVRCompositorError error;
  Texture_t leftEyeTexture = {
      (void*)(uintptr_t)g_descLeftEye.m_nRenderTextureId,
      ETextureType_TextureType_OpenGL, EColorSpace_ColorSpace_Gamma};
  error = g_pCompositor->Submit(EVREye_Eye_Left, &leftEyeTexture, NULL,
                                EVRSubmitFlags_Submit_Default);
  if (error != EVRCompositorError_VRCompositorError_None &&
      error != EVRCompositorError_VRCompositorError_DoNotHaveFocus) {
    Logger_Abort2(error, "Submit EVREye_Eye_Left");
    return;
  }

  Texture_t rightEyeTexture = {
      (void*)(uintptr_t)g_descRightEye.m_nRenderTextureId,
      ETextureType_TextureType_OpenGL, EColorSpace_ColorSpace_Gamma};
  error = g_pCompositor->Submit(EVREye_Eye_Right, &rightEyeTexture, NULL,
                                EVRSubmitFlags_Submit_Default);
  if (error != EVRCompositorError_VRCompositorError_None &&
      error != EVRCompositorError_VRCompositorError_DoNotHaveFocus) {
    Logger_Abort2(error, "Submit EVREye_Eye_Right");
    return;
  }
}

struct Matrix VR_GetViewMatrix() {
  return MatrixFromMat4s(g_mat4HMDPose);
}

struct Matrix VR_GetProjectionMatrix(Hmd_Eye nEye) {
  mat4s m;

  if (nEye == EVREye_Eye_Left) {
    m = glms_mat4_mul(g_mat4ProjectionLeft, g_mat4eyePosLeft);
  } else if (nEye == EVREye_Eye_Right) {
    m = glms_mat4_mul(g_mat4ProjectionRight, g_mat4eyePosRight);
  } else {
    m = glms_mat4_identity();
  }

  return MatrixFromMat4s(m);
}

void VR_Shutdown() {
  VR_ShutdownInternal();
}

cc_bool VR_IsPressed(KeyBind binding) {
  struct InputDigitalActionData_t pActionData;
  EVRInputError inputError;
  VRActionHandle_t action;

  cc_bool found = false;
  for (size_t i = 0; i < Array_Elems(bindings); i++) {
    struct KeyBindToAction* keyBindToAction = &bindings[i];
    if (keyBindToAction->keyBind == binding) {
      action = *keyBindToAction->action;
      found = true;
    }
  }
  if (!found) {
    return false;
  }

  inputError = g_pInput->GetDigitalActionData(
      action, &pActionData, sizeof(pActionData), k_ulInvalidInputValueHandle);
  if (inputError != EVRInputError_VRInputError_None) {
    Logger_Abort2(inputError, "GetDigitalActionData");
    return false;
  }

  return pActionData.bActive && pActionData.bState;
}

Vec2 VR_GetWalk2Axis() {
  Vec2 v = {0};

  struct InputAnalogActionData_t pActionData;
  EVRInputError inputError;

  inputError = g_pInput->GetAnalogActionData(g_actionWalk2Axis, &pActionData,
                                             sizeof(pActionData),
                                             k_ulInvalidInputValueHandle);
  if (inputError != EVRInputError_VRInputError_None) {
    Logger_Abort2(inputError, "GetDigitalActionData");
    return v;
  }

  if (pActionData.bActive) {
    v.X = pActionData.x;
    v.Y = pActionData.y;
  }

  return v;
}
