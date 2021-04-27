
#include <GL/glew.h>
#include <cglm/struct.h>
#include <openvr_capi.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "Game.h"
#include "Logger.h"

// --------------------- openvr exports ---------------------------
// LiquidVR
// VRControlPanel
// VRHeadsetView
// VRPaths
// VRVirtualDisplay
// VR_GetStringForHmdError

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
// ----------------------------------------------------------------

struct FramebufferDesc {
  GLuint m_nDepthBufferId;
  GLuint m_nRenderTextureId;
  GLuint m_nRenderFramebufferId;
};
struct FramebufferDesc leftEyeDesc;
struct FramebufferDesc rightEyeDesc;

struct VR_IVRSystem_FnTable* vr_system;
struct VR_IVRCompositor_FnTable* vr_compositor;

TrackedDevicePose_t m_rTrackedDevicePose[64 /* k_unMaxTrackedDeviceCount */];

uint32_t m_nRenderWidth;
uint32_t m_nRenderHeight;

float m_fNearClip = 0.01f;

mat4s m_mat4ProjectionLeft;
mat4s m_mat4ProjectionRight;
mat4s m_mat4eyePosLeft;
mat4s m_mat4eyePosRight;

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

mat4s m_rmat4DevicePose[64 /* k_unMaxTrackedDeviceCount */];
mat4s m_mat4HMDPose;

static mat4s ConvertSteamVRMatrixToMatrix4(const HmdMatrix34_t matPose) {
  mat4s matrixObj = {matPose.m[0][0], matPose.m[1][0], matPose.m[2][0], 0.0,
                     matPose.m[0][1], matPose.m[1][1], matPose.m[2][1], 0.0,
                     matPose.m[0][2], matPose.m[1][2], matPose.m[2][2], 0.0,
                     matPose.m[0][3], matPose.m[1][3], matPose.m[2][3], 1.0f};
  return matrixObj;
}

static mat4s GetHMDMatrixProjectionEye(Hmd_Eye nEye) {
  HmdMatrix44_t mat = vr_system->GetProjectionMatrix(nEye, m_fNearClip,
                                                     (float)Game_ViewDistance);

  mat4s matrixObj = {mat.m[0][0], mat.m[1][0], mat.m[2][0], mat.m[3][0],
                     mat.m[0][1], mat.m[1][1], mat.m[2][1], mat.m[3][1],
                     mat.m[0][2], mat.m[1][2], mat.m[2][2], mat.m[3][2],
                     mat.m[0][3], mat.m[1][3], mat.m[2][3], mat.m[3][3]};

  return matrixObj;
}

static mat4s GetHMDMatrixPoseEye(Hmd_Eye nEye) {
  HmdMatrix34_t matEyeRight = vr_system->GetEyeToHeadTransform(nEye);
  mat4s matrixObj = {
      matEyeRight.m[0][0], matEyeRight.m[1][0], matEyeRight.m[2][0], 0.0,
      matEyeRight.m[0][1], matEyeRight.m[1][1], matEyeRight.m[2][1], 0.0,
      matEyeRight.m[0][2], matEyeRight.m[1][2], matEyeRight.m[2][2], 0.0,
      matEyeRight.m[0][3], matEyeRight.m[1][3], matEyeRight.m[2][3], 1.0f};

  return glms_mat4_inv(matrixObj);
}

static mat4s GetCurrentViewProjectionMatrix(Hmd_Eye nEye) {
  // GetProjectionMatrix * GetEyeToHeadTransform *
  // TrackedDevicePose.mDeviceToAbsoluteTracking
  if (nEye == EVREye_Eye_Left) {
    return glms_mat4_mul(glms_mat4_mul(m_mat4ProjectionLeft, m_mat4eyePosLeft),
                         m_mat4HMDPose);
  } else if (nEye == EVREye_Eye_Right) {
    return glms_mat4_mul(
        glms_mat4_mul(m_mat4ProjectionRight, m_mat4eyePosRight), m_mat4HMDPose);
  } else {
    return glms_mat4_identity();
  }
}

static void SetupCameras() {
  m_mat4ProjectionLeft = GetHMDMatrixProjectionEye(EVREye_Eye_Left);
  m_mat4ProjectionRight = GetHMDMatrixProjectionEye(EVREye_Eye_Right);
  m_mat4eyePosLeft = GetHMDMatrixPoseEye(EVREye_Eye_Left);
  m_mat4eyePosRight = GetHMDMatrixPoseEye(EVREye_Eye_Right);
}

static void SetupStereoRenderTargets() {
  vr_system->GetRecommendedRenderTargetSize(&m_nRenderWidth, &m_nRenderHeight);
  CreateFrameBuffer(m_nRenderWidth, m_nRenderHeight, &leftEyeDesc);
  CreateFrameBuffer(m_nRenderWidth, m_nRenderHeight, &rightEyeDesc);
}
