#ifndef CC_VR_H
#define CC_VR_H

#include <GL/glew.h>
#include <cglm/struct.h>
#include <openvr_capi.h>
#include <stdbool.h>
#include <stdint.h>

#include "Input.h"
#include "Vectors.h"

extern mat4s g_rmat4DevicePose[64 /* k_unMaxTrackedDeviceCount */];

/* view matrix for vr headset */
extern mat4s g_mat4HMDPose;

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

extern struct Controller g_controllerLeft;
extern struct Controller g_controllerRight;

/* sets up connection to steamvr, call once on app init */
void VR_Setup();

/* calls VR_ShutdownInternal */
void VR_Shutdown();

/* call before rendering frame */
void VR_BeginFrame();

/* will call `RenderScene` 2 times for left and right eye, call in place of
 * previous scene-render logic */
void VR_RenderStereoTargets(void (*RenderScene)(Hmd_Eye nEye,
                                                double delta,
                                                float t),
                            double delta,
                            float t);

/* submits both eye textures to steamvr, call after frame rendered */
void VR_EndFrame();

/* updates all device poses, call after frame rendered */
void VR_UpdateHMDMatrixPose();

/* gets view matrix for specified eye */
struct Matrix VR_GetViewMatrix();

/* gets projection matrix for specified eye */
struct Matrix VR_GetProjectionMatrix(Hmd_Eye nEye);

void VR_RenderControllers(Hmd_Eye nEye);

cc_bool VR_IsPressed(KeyBind binding);

/* returns x/y decimal for walk-axis */
Vec2 VR_GetWalk2Axis();

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

#endif
