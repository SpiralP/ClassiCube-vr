#include "VR.h"

#include <GL/glew.h>
#include <cglm/struct.h>
#include <openvr_capi.h>
#include <stdint.h>
#include <stdio.h>

#include "Constants.h"
#include "Game.h"
#include "Logger.h"
#include "Platform.h"
#include "String.h"
#include "VRInternal.h"
#include "Vectors.h"
#include "Window.h"

void VR_UpdateHMDMatrixPose() {
  EVRCompositorError error = vr_compositor->WaitGetPoses(
      m_rTrackedDevicePose, k_unMaxTrackedDeviceCount, NULL, 0);
  if (error != EVRCompositorError_VRCompositorError_None &&
      error != EVRCompositorError_VRCompositorError_DoNotHaveFocus) {
    Platform_Log1("WaitGetPoses %i", &error);
    Logger_Abort("WaitGetPoses");
    return;
  }

  for (unsigned int nDevice = 0; nDevice < k_unMaxTrackedDeviceCount;
       ++nDevice) {
    if (m_rTrackedDevicePose[nDevice].bPoseIsValid) {
      m_rmat4DevicePose[nDevice] = ConvertSteamVRMatrixToMatrix4(
          m_rTrackedDevicePose[nDevice].mDeviceToAbsoluteTracking);
    }
  }

  if (m_rTrackedDevicePose[k_unTrackedDeviceIndex_Hmd].bPoseIsValid) {
    m_mat4HMDPose = m_rmat4DevicePose[k_unTrackedDeviceIndex_Hmd];
    m_mat4HMDPose = glms_mat4_inv(m_mat4HMDPose);
  }
}

void VR_Setup() {
  glewExperimental = GL_TRUE;
  GLenum nGlewError = glewInit();
  if (nGlewError != GLEW_OK) {
    Platform_Log1("Error initializing GLEW! %c",
                  glewGetErrorString(nGlewError));
    Logger_Abort("glewInit != GLEW_OK");
    return;
  }
  glGetError();  // to clear the error caused deep in GLEW

  EVRInitError error = EVRInitError_VRInitError_None;
  VR_InitInternal(&error, EVRApplicationType_VRApplication_Scene);
  if (error != EVRInitError_VRInitError_None) {
    Logger_Abort("VR_InitInternal");
    return;
  }

  if (!VR_IsInterfaceVersionValid(IVRSystem_Version)) {
    VR_ShutdownInternal();
    Logger_Abort("!VR_IsInterfaceVersionValid");
    return;
  }

  error = EVRInitError_VRInitError_None;

  char systemInterfaceName[256] = {0};

  sprintf(systemInterfaceName, "FnTable:%s", IVRSystem_Version);
  vr_system = (struct VR_IVRSystem_FnTable*)VR_GetGenericInterface(
      systemInterfaceName, &error);
  if (error != EVRInitError_VRInitError_None) {
    Logger_Abort("VR_GetGenericInterface System");
  }

  cc_string title;
  char titleBuffer[STRING_SIZE];
  String_InitArray(title, titleBuffer);

  char name[256 + 1] = {0};
  uint32_t nameLen = vr_system->GetStringTrackedDeviceProperty(
      k_unTrackedDeviceIndex_Hmd,
      ETrackedDeviceProperty_Prop_TrackingSystemName_String, name, 256, NULL);
  char serial[256 + 1] = {0};
  uint32_t serialLen = vr_system->GetStringTrackedDeviceProperty(
      k_unTrackedDeviceIndex_Hmd,
      ETrackedDeviceProperty_Prop_SerialNumber_String, serial, 256, NULL);
  String_Format4(&title, "%c (%s) - %c (%c)", GAME_APP_TITLE, &Game_Username,
                 &name, &serial);
  Window_SetTitle(&title);

  sprintf(systemInterfaceName, "FnTable:%s", IVRCompositor_Version);
  vr_compositor = (struct VR_IVRCompositor_FnTable*)VR_GetGenericInterface(
      systemInterfaceName, &error);
  if (error != EVRInitError_VRInitError_None) {
    Logger_Abort("VR_GetGenericInterface Compositor");
    return;
  }

  // SetupTexturemaps();
  // SetupScene();
  SetupCameras();
  SetupStereoRenderTargets();
  SetupCompanionWindow();
}

void RenderCompanionWindow() {
  glDisable(GL_DEPTH_TEST);
  glViewport(0, 0, Game.Width, Game.Height);

  glBindVertexArray(m_unCompanionWindowVAO);
  // glUseProgram(m_unCompanionWindowProgramID);

  // render left eye (first half of index array )
  glBindTexture(GL_TEXTURE_2D, leftEyeDesc.m_nResolveTextureId);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glDrawElements(GL_TRIANGLES, m_uiCompanionWindowIndexSize / 2,
                 GL_UNSIGNED_SHORT, 0);

  // render right eye (second half of index array )
  glBindTexture(GL_TEXTURE_2D, rightEyeDesc.m_nResolveTextureId);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glDrawElements(GL_TRIANGLES, m_uiCompanionWindowIndexSize / 2,
                 GL_UNSIGNED_SHORT,
                 (const void*)(uintptr_t)(m_uiCompanionWindowIndexSize));

  glBindVertexArray(0);
  // glUseProgram(0);
}

void VR_RenderStereoTargets(void (*RenderScene)(Hmd_Eye nEye,
                                                double delta,
                                                float t),
                            double delta,
                            float t) {
  glEnable(GL_MULTISAMPLE);

  // Left Eye
  glBindFramebuffer(GL_FRAMEBUFFER, leftEyeDesc.m_nRenderFramebufferId);
  glViewport(0, 0, m_nRenderWidth, m_nRenderHeight);
  RenderScene(EVREye_Eye_Left, delta, t);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);

  glDisable(GL_MULTISAMPLE);

  glBindFramebuffer(GL_READ_FRAMEBUFFER, leftEyeDesc.m_nRenderFramebufferId);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, leftEyeDesc.m_nResolveFramebufferId);

  glBlitFramebuffer(0, 0, m_nRenderWidth, m_nRenderHeight, 0, 0, m_nRenderWidth,
                    m_nRenderHeight, GL_COLOR_BUFFER_BIT, GL_LINEAR);

  glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);

  glEnable(GL_MULTISAMPLE);

  // Right Eye
  glBindFramebuffer(GL_FRAMEBUFFER, rightEyeDesc.m_nRenderFramebufferId);
  glViewport(0, 0, m_nRenderWidth, m_nRenderHeight);
  RenderScene(EVREye_Eye_Right, delta, t);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);

  glDisable(GL_MULTISAMPLE);

  glBindFramebuffer(GL_READ_FRAMEBUFFER, rightEyeDesc.m_nRenderFramebufferId);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, rightEyeDesc.m_nResolveFramebufferId);

  glBlitFramebuffer(0, 0, m_nRenderWidth, m_nRenderHeight, 0, 0, m_nRenderWidth,
                    m_nRenderHeight, GL_COLOR_BUFFER_BIT, GL_LINEAR);

  glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
}

void VR_BeginFrame() {
  //
}

void VR_EndFrame() {
  // end RenderStereoTargets();

  RenderCompanionWindow();

  // after RenderCompanionWindow();
  EVRCompositorError error;
  Texture_t leftEyeTexture = {(void*)(uintptr_t)leftEyeDesc.m_nResolveTextureId,
                              ETextureType_TextureType_OpenGL,
                              EColorSpace_ColorSpace_Gamma};
  error = vr_compositor->Submit(EVREye_Eye_Left, &leftEyeTexture, NULL,
                                EVRSubmitFlags_Submit_Default);
  if (error != EVRCompositorError_VRCompositorError_None &&
      error != EVRCompositorError_VRCompositorError_DoNotHaveFocus) {
    Platform_Log1("Submit EVREye_Eye_Left %i", &error);
    Logger_Abort("Submit EVREye_Eye_Left");
    return;
  }

  Texture_t rightEyeTexture = {
      (void*)(uintptr_t)rightEyeDesc.m_nResolveTextureId,
      ETextureType_TextureType_OpenGL, EColorSpace_ColorSpace_Gamma};
  error = vr_compositor->Submit(EVREye_Eye_Right, &rightEyeTexture, NULL,
                                EVRSubmitFlags_Submit_Default);
  if (error != EVRCompositorError_VRCompositorError_None &&
      error != EVRCompositorError_VRCompositorError_DoNotHaveFocus) {
    Platform_Log1("Submit EVREye_Eye_Right %i", &error);
    Logger_Abort("Submit EVREye_Eye_Right");
    return;
  }
}

struct Matrix VR_GetProjection(Hmd_Eye nEye) {
  mat4s m = GetCurrentViewProjectionMatrix(nEye);
  // Platform_Log1("WaitGetPoses %i", &error);
  // glm_mat4_print(&m, stdout);

  struct Matrix m2 = {
      m.m00, m.m01, m.m02, m.m03,  //
      m.m10, m.m11, m.m12, m.m13,  //
      m.m20, m.m21, m.m22, m.m23,  //
      m.m30, m.m31, m.m32, m.m33,  //
  };

  return m2;
}
