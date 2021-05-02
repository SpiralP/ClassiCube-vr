
#include <GL/glew.h>
#include <cglm/struct.h>
#include <openvr_capi.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "Event.h"
#include "Game.h"
#include "Input.h"
#include "Logger.h"
#include "VR.h"

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

VRActionHandle_t g_actionPlaceBlock;
VRActionHandle_t g_actionDeleteBlock;
VRActionHandle_t g_actionPickBlock;

VRActionHandle_t g_actionWalk2Axis;
VRActionHandle_t g_actionTurn2Axis;
VRActionHandle_t g_actionJump;

VRActionHandle_t g_actionHapticHandLeft;
VRActionHandle_t g_actionHapticHandRight;
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

  inputError = g_pInput->GetActionHandle("/actions/main/in/haptic_hand_left",
                                         &g_actionHapticHandLeft);
  if (inputError != EVRInputError_VRInputError_None) {
    Logger_Abort2(inputError, "GetActionHandle haptic_hand_left");
    return;
  }

  inputError = g_pInput->GetActionHandle("/actions/main/in/haptic_hand_right",
                                         &g_actionHapticHandRight);
  if (inputError != EVRInputError_VRInputError_None) {
    Logger_Abort2(inputError, "GetActionHandle haptic_hand_right");
    return;
  }

  inputError = g_pInput->GetActionHandle("/actions/main/in/place_block",
                                         &g_actionPlaceBlock);
  if (inputError != EVRInputError_VRInputError_None) {
    Logger_Abort2(inputError, "GetActionHandle place_block");
    return;
  }

  inputError = g_pInput->GetActionHandle("/actions/main/in/delete_block",
                                         &g_actionDeleteBlock);
  if (inputError != EVRInputError_VRInputError_None) {
    Logger_Abort2(inputError, "GetActionHandle delete_block");
    return;
  }

  inputError = g_pInput->GetActionHandle("/actions/main/in/pick_block",
                                         &g_actionPickBlock);
  if (inputError != EVRInputError_VRInputError_None) {
    Logger_Abort2(inputError, "GetActionHandle pick_block");
    return;
  }

  inputError = g_pInput->GetActionHandle("/actions/main/in/walk_2_axis",
                                         &g_actionWalk2Axis);
  if (inputError != EVRInputError_VRInputError_None) {
    Logger_Abort2(inputError, "GetActionHandle walk_2_axis");
    return;
  }

  inputError = g_pInput->GetActionHandle("/actions/main/in/turn_2_axis",
                                         &g_actionTurn2Axis);
  if (inputError != EVRInputError_VRInputError_None) {
    Logger_Abort2(inputError, "GetActionHandle turn_2_axis");
    return;
  }

  inputError =
      g_pInput->GetActionHandle("/actions/main/in/jump", &g_actionJump);
  if (inputError != EVRInputError_VRInputError_None) {
    Logger_Abort2(inputError, "GetActionHandle jump");
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

static Vec2 VR_GetTurn2Axis() {
  Vec2 v = {0};

  struct InputAnalogActionData_t pActionData;
  EVRInputError inputError;

  inputError = g_pInput->GetAnalogActionData(g_actionTurn2Axis, &pActionData,
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

struct KeyBindToAction {
  KeyBind keyBind;
  VRActionHandle_t* action;
};

struct KeyBindToAction bindings[] = {
    {KEYBIND_DELETE_BLOCK, &g_actionDeleteBlock},
    {KEYBIND_PICK_BLOCK, &g_actionPickBlock},
    {KEYBIND_PLACE_BLOCK, &g_actionPlaceBlock},
    {KEYBIND_JUMP, &g_actionJump},
};

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

  Vec2 turn = VR_GetTurn2Axis();
  Event_RaiseRawMove(&PointerEvents.RawMoved, turn.X * 20.0f, turn.Y * 20.0f);

  for (size_t i = 0; i < Array_Elems(bindings); i++) {
    struct KeyBindToAction* keyBindToAction = &bindings[i];

    struct InputDigitalActionData_t pActionData;
    inputError = g_pInput->GetDigitalActionData(
        *keyBindToAction->action, &pActionData, sizeof(pActionData),
        k_ulInvalidInputValueHandle);
    if (inputError != EVRInputError_VRInputError_None) {
      Logger_Abort2(inputError, "GetDigitalActionData");
      return;
    }

    if (pActionData.bActive && pActionData.bChanged) {
      Input_Set(KeyBinds[keyBindToAction->keyBind], pActionData.bState);
    }
  }
}
