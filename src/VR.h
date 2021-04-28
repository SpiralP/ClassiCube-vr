#ifndef CC_VR_H
#define CC_VR_H

#include <cglm/struct.h>
#include <openvr_capi.h>
#include <stdbool.h>
#include <stdint.h>

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

#endif
