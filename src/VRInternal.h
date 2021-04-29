
#include <GL/glew.h>
#include <cglm/struct.h>
#include <openvr_capi.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "Game.h"
#include "Logger.h"

// static void check() {
//   GLenum e = glGetError();
//   if (e) {
//     Logger_Abort2(e, "glGetError");
//     return;
//   }
// }

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
struct VR_IVRInput_FnTable* g_pInput;
struct VR_IVRRenderModels_FnTable* g_pRenderModels;

TrackedDevicePose_t g_rTrackedDevicePose[64 /* k_unMaxTrackedDeviceCount */];

uint32_t g_nRenderWidth;
uint32_t g_nRenderHeight;

mat4s g_mat4ProjectionLeft;
mat4s g_mat4ProjectionRight;
mat4s g_mat4eyePosLeft;
mat4s g_mat4eyePosRight;

VRActionSetHandle_t g_actionSetMain;

struct CGLM_ALIGN_MAT Controller {
  VRActionHandle_t actionHandle;
  char modelName[256];
  /* mDeviceToAbsoluteTracking */
  mat4s pose;
  RenderModel_t* model;
  GLuint glVertBuffer;
  GLuint glVertArray;
  GLuint glIndexBuffer;
  bool initialized;
};

struct Controller g_controllerLeft = {0};
struct Controller g_controllerRight = {0};

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

static mat4s Mat4sFromMatrix(struct Matrix m) {
  mat4s m2 = {
      m.row1.X, m.row1.Y, m.row1.Z, m.row1.W,  //
      m.row2.X, m.row2.Y, m.row2.Z, m.row2.W,  //
      m.row3.X, m.row3.Y, m.row3.Z, m.row3.W,  //
      m.row4.X, m.row4.Y, m.row4.Z, m.row4.W,  //
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

static void SetupInput() {
  char cwd[MAX_PATH];
  if (!GetCurrentDirectoryA(sizeof(cwd), cwd)) {
    Logger_Abort("GetCurrentDirectoryA");
    return;
  }

  char manifestPath[256];
  snprintf(manifestPath, sizeof(manifestPath), "%s\\%s", cwd,
           "openvr_actions.json");
  EVRInputError inputError = g_pInput->SetActionManifestPath(manifestPath);
  if (inputError != EVRInputError_VRInputError_None) {
    Logger_Abort2(inputError, "SetActionManifestPath");
    return;
  }

  inputError = g_pInput->GetActionSetHandle("/actions/main", &g_actionSetMain);
  if (inputError != EVRInputError_VRInputError_None) {
    Logger_Abort2(inputError, "GetActionSetHandle");
    return;
  }

  inputError = g_pInput->GetActionHandle("/actions/main/in/hand_left",
                                         &g_controllerLeft.actionHandle);
  if (inputError != EVRInputError_VRInputError_None) {
    Logger_Abort2(inputError, "GetActionHandle hand_left");
    return;
  }

  inputError = g_pInput->GetActionHandle("/actions/main/in/hand_right",
                                         &g_controllerRight.actionHandle);
  if (inputError != EVRInputError_VRInputError_None) {
    Logger_Abort2(inputError, "GetActionHandle hand_right");
    return;
  }
}

static void InitController(struct Controller* c) {
  // create and bind a VAO to hold state for this model
  glGenVertexArrays(1, &c->glVertArray);
  glBindVertexArray(c->glVertArray);

  // Populate a vertex buffer
  glGenBuffers(1, &c->glVertBuffer);
  glBindBuffer(GL_ARRAY_BUFFER, c->glVertBuffer);
  glBufferData(GL_ARRAY_BUFFER,
               sizeof(RenderModel_Vertex_t) * c->model->unVertexCount,
               c->model->rVertexData, GL_STATIC_DRAW);

  // Identify the components in the vertex buffer
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(RenderModel_Vertex_t),
                        (void*)offsetof(RenderModel_Vertex_t, vPosition));
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(RenderModel_Vertex_t),
                        (void*)offsetof(RenderModel_Vertex_t, vNormal));
  glEnableVertexAttribArray(2);
  glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(RenderModel_Vertex_t),
                        (void*)offsetof(RenderModel_Vertex_t, rfTextureCoord));

  // Create and populate the index buffer
  glGenBuffers(1, &c->glIndexBuffer);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, c->glIndexBuffer);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER,
               sizeof(uint16_t) * c->model->unTriangleCount * 3,
               c->model->rIndexData, GL_STATIC_DRAW);

  c->initialized = true;
}

static void UpdateController(struct Controller* controller) {
  InputPoseActionData_t poseData;
  EVRInputError inputError = g_pInput->GetPoseActionDataForNextFrame(
      controller->actionHandle,
      ETrackingUniverseOrigin_TrackingUniverseStanding, &poseData,
      sizeof(poseData), k_ulInvalidInputValueHandle);
  if (inputError != EVRInputError_VRInputError_None) {
    Logger_Abort2(inputError, "GetPoseActionDataForNextFrame");
    return;
  }

  if (poseData.bActive && poseData.pose.bPoseIsValid) {
    controller->pose =
        Mat4sFromHmdMatrix34(poseData.pose.mDeviceToAbsoluteTracking);

    InputOriginInfo_t originInfo;
    inputError = g_pInput->GetOriginTrackedDeviceInfo(
        poseData.activeOrigin, &originInfo, sizeof(originInfo));
    if (inputError != EVRInputError_VRInputError_None) {
      Logger_Abort2(inputError, "GetOriginTrackedDeviceInfo");
      return;
    }

    // printf("%i\n", originInfo.trackedDeviceIndex);
    if (originInfo.trackedDeviceIndex != k_unTrackedDeviceIndexInvalid) {
      char modelName[256];
      ETrackedPropertyError propError;
      g_pSystem->GetStringTrackedDeviceProperty(
          originInfo.trackedDeviceIndex,
          ETrackedDeviceProperty_Prop_RenderModelName_String, modelName,
          sizeof(modelName), &propError);
      if (propError != ETrackedPropertyError_TrackedProp_Success) {
        Logger_Abort2(propError,
                      "GetStringTrackedDeviceProperty "
                      "Prop_RenderModelName_String");
        return;
      }

      if (strcmp(modelName, controller->modelName) != 0) {
        strncpy(controller->modelName, modelName,
                sizeof(((struct Controller*)0)->modelName));

        RenderModel_t* pModel;
        EVRRenderModelError modelError;
        while (true) {
          modelError =
              g_pRenderModels->LoadRenderModel_Async(modelName, &pModel);
          if (modelError != EVRRenderModelError_VRRenderModelError_Loading) {
            break;
          }

          Sleep(1);
        }

        if (modelError != EVRRenderModelError_VRRenderModelError_None) {
          Logger_Abort2(modelError, "LoadRenderModel_Async");
          return;
        }
        controller->model = pModel;

        InitController(controller);

        Platform_Log1("%c OK", modelName);
      }
    }
  }
}

static void UpdateInput() {
  VRActiveActionSet_t actionSet = {0};
  actionSet.ulActionSet = g_actionSetMain;
  EVRInputError inputError =
      g_pInput->UpdateActionState(&actionSet, sizeof(actionSet), 1);
  if (inputError != EVRInputError_VRInputError_None) {
    Logger_Abort2(inputError, "UpdateActionState");
    return;
  }

  UpdateController(&g_controllerLeft);
  UpdateController(&g_controllerRight);
}
