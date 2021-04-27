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
#include "Vectors.h"
#include "Window.h"

struct FramebufferDesc {
  GLuint m_nDepthBufferId;
  GLuint m_nRenderTextureId;
  GLuint m_nRenderFramebufferId;
  GLuint m_nResolveTextureId;
  GLuint m_nResolveFramebufferId;
};
struct FramebufferDesc leftEyeDesc;
struct FramebufferDesc rightEyeDesc;

struct VR_IVRSystem_FnTable* vr_system;
struct VR_IVRCompositor_FnTable* vr_compositor;

TrackedDevicePose_t m_rTrackedDevicePose[64 /* k_unMaxTrackedDeviceCount */];

uint32_t m_nRenderWidth;
uint32_t m_nRenderHeight;

float m_fNearClip = 0.01f;
float m_fFarClip = 100000.0f;

mat4s m_mat4ProjectionLeft;
mat4s m_mat4ProjectionRight;
mat4s m_mat4eyePosLeft;
mat4s m_mat4eyePosRight;

void CreateFrameBuffer(int nWidth,
                       int nHeight,
                       struct FramebufferDesc* framebufferDesc) {
  glGenFramebuffers(1, &framebufferDesc->m_nRenderFramebufferId);
  glBindFramebuffer(GL_FRAMEBUFFER, framebufferDesc->m_nRenderFramebufferId);

  glGenRenderbuffers(1, &framebufferDesc->m_nDepthBufferId);
  glBindRenderbuffer(GL_RENDERBUFFER, framebufferDesc->m_nDepthBufferId);
  glRenderbufferStorageMultisample(GL_RENDERBUFFER, 4, GL_DEPTH_COMPONENT,
                                   nWidth, nHeight);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                            GL_RENDERBUFFER, framebufferDesc->m_nDepthBufferId);

  glGenTextures(1, &framebufferDesc->m_nRenderTextureId);
  glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, framebufferDesc->m_nRenderTextureId);
  glTexImage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, 4, GL_RGBA8, nWidth,
                          nHeight, true);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                         GL_TEXTURE_2D_MULTISAMPLE,
                         framebufferDesc->m_nRenderTextureId, 0);

  glGenFramebuffers(1, &framebufferDesc->m_nResolveFramebufferId);
  glBindFramebuffer(GL_FRAMEBUFFER, framebufferDesc->m_nResolveFramebufferId);

  glGenTextures(1, &framebufferDesc->m_nResolveTextureId);
  glBindTexture(GL_TEXTURE_2D, framebufferDesc->m_nResolveTextureId);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, nWidth, nHeight, 0, GL_RGBA,
               GL_UNSIGNED_BYTE, NULL);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         framebufferDesc->m_nResolveTextureId, 0);

  // check FBO status
  GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  if (status != GL_FRAMEBUFFER_COMPLETE) {
    Logger_Abort("glCheckFramebufferStatus != GL_FRAMEBUFFER_COMPLETE");
    return;
  }

  glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

mat4s m_rmat4DevicePose[64 /* k_unMaxTrackedDeviceCount */];
mat4s m_mat4HMDPose;

mat4s ConvertSteamVRMatrixToMatrix4(const HmdMatrix34_t matPose) {
  mat4s matrixObj = {matPose.m[0][0], matPose.m[1][0], matPose.m[2][0], 0.0,
                     matPose.m[0][1], matPose.m[1][1], matPose.m[2][1], 0.0,
                     matPose.m[0][2], matPose.m[1][2], matPose.m[2][2], 0.0,
                     matPose.m[0][3], matPose.m[1][3], matPose.m[2][3], 1.0f};
  return matrixObj;
}

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

static mat4s GetHMDMatrixProjectionEye(Hmd_Eye nEye) {
  HmdMatrix44_t mat =
      vr_system->GetProjectionMatrix(nEye, m_fNearClip, m_fFarClip);

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
  mat4s matMVP;
  if (nEye == EVREye_Eye_Left) {
    matMVP = glms_mat4_mul(
        glms_mat4_mul(m_mat4ProjectionLeft, m_mat4eyePosLeft), m_mat4HMDPose);
  } else if (nEye == EVREye_Eye_Right) {
    matMVP = glms_mat4_mul(
        glms_mat4_mul(m_mat4ProjectionRight, m_mat4eyePosRight), m_mat4HMDPose);
  }

  return matMVP;
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

GLuint m_glSceneVertBuffer;
GLuint m_unSceneVAO;
GLuint m_unCompanionWindowVAO;
GLuint m_glCompanionWindowIDVertBuffer;
GLuint m_glCompanionWindowIDIndexBuffer;
unsigned int m_uiCompanionWindowIndexSize;

struct Vector2 {
  float x;
  float y;
};

struct VertexDataWindow {
  struct Vector2 position;
  struct Vector2 texCoord;
};

static void SetupCompanionWindow() {
  struct VertexDataWindow vVerts[8] = {
      // left eye verts
      {{-1, -1}, {0, 1}},
      {{0, -1}, {1, 1}},
      {{-1, 1}, {0, 0}},
      {{0, 1}, {1, 0}},

      // right eye verts
      {{0, -1}, {0, 1}},
      {{1, -1}, {1, 1}},
      {{0, 1}, {0, 0}},
      {{1, 1}, {1, 0}},
  };

  GLushort vIndices[] = {0, 1, 3, 0, 3, 2, 4, 5, 7, 4, 7, 6};
  m_uiCompanionWindowIndexSize = _countof(vIndices);

  glGenVertexArrays(1, &m_unCompanionWindowVAO);
  glBindVertexArray(m_unCompanionWindowVAO);

  glGenBuffers(1, &m_glCompanionWindowIDVertBuffer);
  glBindBuffer(GL_ARRAY_BUFFER, m_glCompanionWindowIDVertBuffer);
  glBufferData(GL_ARRAY_BUFFER, 8 * sizeof(struct VertexDataWindow), &vVerts[0],
               GL_STATIC_DRAW);

  glGenBuffers(1, &m_glCompanionWindowIDIndexBuffer);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_glCompanionWindowIDIndexBuffer);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER,
               m_uiCompanionWindowIndexSize * sizeof(GLushort), &vIndices[0],
               GL_STATIC_DRAW);

  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE,
                        sizeof(struct VertexDataWindow),
                        (void*)offsetof(struct VertexDataWindow, position));

  glEnableVertexAttribArray(1);
  glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE,
                        sizeof(struct VertexDataWindow),
                        (void*)offsetof(struct VertexDataWindow, texCoord));

  glBindVertexArray(0);

  glDisableVertexAttribArray(0);
  glDisableVertexAttribArray(1);

  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
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
  vr_system = VR_GetGenericInterface(systemInterfaceName, &error);
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
  vr_compositor = VR_GetGenericInterface(systemInterfaceName, &error);
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
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
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
