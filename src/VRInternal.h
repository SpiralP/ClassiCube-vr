
#include <GL/glew.h>
#include <cglm/struct.h>
#include <openvr_capi.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "Game.h"
#include "Logger.h"

// ---------------------- global variables ------------------------
struct FramebufferDesc {
  GLuint m_nDepthBufferId;
  GLuint m_nRenderTextureId;
  GLuint m_nRenderFramebufferId;
};
struct FramebufferDesc g_descLeftEye;
struct FramebufferDesc g_descRightEye;

struct VR_IVRSystem_FnTable* g_pSystem;
struct VR_IVRCompositor_FnTable* g_pCompositor;

TrackedDevicePose_t g_rTrackedDevicePose[64 /* k_unMaxTrackedDeviceCount */];

uint32_t g_nRenderWidth;
uint32_t g_nRenderHeight;

mat4s g_mat4ProjectionLeft;
mat4s g_mat4ProjectionRight;
mat4s g_mat4eyePosLeft;
mat4s g_mat4eyePosRight;

// --------------------- openvr exports ---------------------------
intptr_t VR_InitInternal(EVRInitError* peError, EVRApplicationType eType);
void VR_ShutdownInternal();
bool VR_IsHmdPresent();
intptr_t VR_GetGenericInterface(const char* pchInterfaceVersion,
                                EVRInitError* peError);
bool VR_IsRuntimeInstalled();
const char* VR_GetVRInitErrorAsSymbol(EVRInitError error);
const char* VR_GetVRInitErrorAsEnglishDescription(EVRInitError error);

uint32_t VR_GetInitToken();
bool VR_GetRuntimePath(char* pchPathBuffer,
                       uint32_t unBufferSize,
                       uint32_t* punRequiredBufferSize);
uint32_t VR_InitInternal2(EVRInitError* peError,
                          EVRApplicationType eApplicationType,
                          const char* pStartupInfo);
bool VR_IsInterfaceVersionValid(const char* pchInterfaceVersion);
const char* VR_GetStringForHmdError(EVRInitError error);

// -------------------- matrix helpers ----------------------------
static mat4s Mat4sFromHmdMatrix34(const HmdMatrix34_t m) {
  mat4s m2 = {
      m.m[0][0], m.m[1][0], m.m[2][0], 0.0,   //
      m.m[0][1], m.m[1][1], m.m[2][1], 0.0,   //
      m.m[0][2], m.m[1][2], m.m[2][2], 0.0,   //
      m.m[0][3], m.m[1][3], m.m[2][3], 1.0f,  //
  };
  return m2;
}

static mat4s Mat4sFromHmdMatrix44(const HmdMatrix44_t m) {
  mat4s m2 = {
      m.m[0][0], m.m[1][0], m.m[2][0], m.m[3][0],  //
      m.m[0][1], m.m[1][1], m.m[2][1], m.m[3][1],  //
      m.m[0][2], m.m[1][2], m.m[2][2], m.m[3][2],  //
      m.m[0][3], m.m[1][3], m.m[2][3], m.m[3][3],  //
  };
  return m2;
}

static struct Matrix MatrixFromMat4s(mat4s m) {
  struct Matrix m2 = {
      m.m00, m.m01, m.m02, m.m03,  //
      m.m10, m.m11, m.m12, m.m13,  //
      m.m20, m.m21, m.m22, m.m23,  //
      m.m30, m.m31, m.m32, m.m33,  //
  };

  return m2;
}

// ----------------------------------------------------------------

static void CreateFrameBuffer(int nWidth,
                              int nHeight,
                              struct FramebufferDesc* desc) {
  glGenFramebuffers(1, &desc->m_nRenderFramebufferId);
  glBindFramebuffer(GL_FRAMEBUFFER, desc->m_nRenderFramebufferId);

  glGenRenderbuffers(1, &desc->m_nDepthBufferId);
  glBindRenderbuffer(GL_RENDERBUFFER, desc->m_nDepthBufferId);
  glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT, nWidth, nHeight);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                            GL_RENDERBUFFER, desc->m_nDepthBufferId);

  glGenTextures(1, &desc->m_nRenderTextureId);
  glBindTexture(GL_TEXTURE_2D, desc->m_nRenderTextureId);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, nWidth, nHeight, 0, GL_RGBA,
               GL_UNSIGNED_BYTE, NULL);

  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         desc->m_nRenderTextureId, 0);

  // check FBO status
  GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  if (status != GL_FRAMEBUFFER_COMPLETE) {
    Logger_Abort2(status,
                  "glCheckFramebufferStatus != GL_FRAMEBUFFER_COMPLETE");
    return;
  }

  glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

static mat4s GetProjectionMatrix(Hmd_Eye nEye) {
  float fNearZ = 0.1f;
  return Mat4sFromHmdMatrix44(
      g_pSystem->GetProjectionMatrix(nEye, fNearZ, (float)Game_ViewDistance));
}

static mat4s GetEyeToHeadTransform(Hmd_Eye nEye) {
  return glms_mat4_inv(
      Mat4sFromHmdMatrix34(g_pSystem->GetEyeToHeadTransform(nEye)));
}

static void SetupCameras() {
  g_mat4ProjectionLeft = GetProjectionMatrix(EVREye_Eye_Left);
  g_mat4ProjectionRight = GetProjectionMatrix(EVREye_Eye_Right);
  g_mat4eyePosLeft = GetEyeToHeadTransform(EVREye_Eye_Left);
  g_mat4eyePosRight = GetEyeToHeadTransform(EVREye_Eye_Right);
}

static void SetupStereoRenderTargets() {
  g_pSystem->GetRecommendedRenderTargetSize(&g_nRenderWidth, &g_nRenderHeight);
  CreateFrameBuffer(g_nRenderWidth, g_nRenderHeight, &g_descLeftEye);
  CreateFrameBuffer(g_nRenderWidth, g_nRenderHeight, &g_descRightEye);
}
