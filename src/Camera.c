#include "Camera.h"
#include "ExtMath.h"
#include "Game.h"
#include "Window.h"
#include "Graphics.h"
#include "Funcs.h"
#include "Gui.h"
#include "Entity.h"
#include "Input.h"
#include "Event.h"
#include "Options.h"
#include "Picking.h"
#include "VR.h"

struct _CameraData Camera;
static struct RayTracer cameraClipPos;
static Vec2 cam_rotOffset;
static cc_bool cam_isForwardThird;
// entire mouse rotation in degrees
Vec2 mouseRot;

#define CAMERA_SENSI_FACTOR (0.0002f / 3.0f * MATH_RAD2DEG)

static void Camera_OnRawMovement(float deltaX, float deltaY) {
	float sensitivity = CAMERA_SENSI_FACTOR * Camera.Sensitivity;
	deltaX = deltaX * sensitivity;
	deltaY = deltaY * sensitivity;
	if (Camera.Invert) deltaY = -deltaY;

	// TODO precision after a long time
	mouseRot.X += deltaX;
	mouseRot.Y += deltaY;
}

/*########################################################################################################################*
*--------------------------------------------------Perspective camera-----------------------------------------------------*
*#########################################################################################################################*/
static void PerspectiveCamera_GetProjection(struct Matrix* proj, Hmd_Eye nEye) {
	// TODO maybe allow fov changes?
	*proj = VR_GetProjectionMatrix(nEye);
}

static void PerspectiveCamera_GetView(struct Matrix* mat) {
	struct Entity* p = &LocalPlayer_Instance.Base;
	Vec3 pos = Camera.CurrentPos;
	Vec2 rot = mouseRot;
	rot.X *= MATH_DEG2RAD; rot.Y *= MATH_DEG2RAD;

	// don't want pitch
	rot.Y = 0;
	Matrix_LookRot(mat, pos, rot);

	struct Matrix vrView = VR_GetViewMatrix();
	Matrix_MulBy(mat, &vrView);
}

static void PerspectiveCamera_GetPickedBlock(struct RayTracer* t) {
	struct Entity* p = &LocalPlayer_Instance.Base;

	vec3s translatePos = {p->Position.X, p->Position.Y, p->Position.Z};
	mat4s translate = glms_translate_make(translatePos);
	mat4s view = translate;

	vec3s axis = {0, -1.0f, 0};
	mat4s rotate = glms_rotate_make(mouseRot.X * MATH_DEG2RAD, axis);
	view = glms_mat4_mul(view, rotate);


	if (g_controllerRight.initialized) {
		view = glms_mat4_mul(view, g_controllerRight.pose);
	} else {
		// use headset
		view = glms_mat4_mul(view, g_rmat4DevicePose[k_unTrackedDeviceIndex_Hmd]);
	}

	vec3s pos = glms_vec3(view.col[3]);
	vec3s dir = glms_vec3_negate(glms_vec3_normalize(glms_vec3(view.col[2])));

	Vec3 ccPos = {pos.x, pos.y, pos.z};
	Vec3 ccDir = {dir.x, dir.y, dir.z};
	Picking_CalcPickedBlock(&ccPos, &ccDir, LocalPlayer_Instance.ReachDistance, t);
}

static void PerspectiveCamera_UpdateMouseRotation(double delta) {
	struct LocalPlayer* p = &LocalPlayer_Instance;
	struct Entity* e      = &p->Base;

	struct LocationUpdate update;
	float yaw, pitch;

	vec3s cur = glms_euler_angles(g_mat4HMDPose);
	glm_make_deg(&cur.x);
	glm_make_deg(&cur.y);
	glm_make_deg(&cur.z);

	if (Key_IsAltPressed() && Camera.Active->isThirdPerson) {
		cam_rotOffset.X += mouseRot.X; cam_rotOffset.Y += mouseRot.Y;
		return;
	}
	
	yaw   = mouseRot.X + cur.y;
	// don't use pitch
	pitch = cur.x;
	
	LocationUpdate_MakeOri(&update, yaw, pitch);

	/* Need to make sure we don't cross the vertical axes, because that gets weird. */
	if (update.Pitch >= 90.0f && update.Pitch <= 270.0f) {
		update.Pitch = p->Interp.Next.Pitch < 180.0f ? 90.0f : 270.0f;
	}
	e->VTABLE->SetLocation(e, &update, false);
}

static void PerspectiveCamera_UpdateMouse(double delta) {
	if (!Gui.InputGrab && WindowInfo.Focused) Window_UpdateRawMouse();

	VR_UpdateHMDMatrixPose();
	PerspectiveCamera_UpdateMouseRotation(delta);
}

static void PerspectiveCamera_CalcViewBobbing(float t, float velTiltScale) {
	struct LocalPlayer* p = &LocalPlayer_Instance;
	struct Entity* e = &p->Base;

	struct Matrix tiltY, velX;
	float vel;
	if (!Game_ViewBobbing) { Camera.TiltM = Matrix_Identity; return; }

	Matrix_RotateZ(&Camera.TiltM, -p->Tilt.TiltX                  * e->Anim.BobStrength);
	Matrix_RotateX(&tiltY,        Math_AbsF(p->Tilt.TiltY) * 3.0f * e->Anim.BobStrength);
	Matrix_MulBy(&Camera.TiltM, &tiltY);

	Camera.BobbingHor = (e->Anim.BobbingHor * 0.3f) * e->Anim.BobStrength;
	Camera.BobbingVer = (e->Anim.BobbingVer * 0.6f) * e->Anim.BobStrength;

	vel = Math_Lerp(p->OldVelocity.Y + 0.08f, e->Velocity.Y + 0.08f, t);
	Matrix_RotateX(&velX, -vel * 0.05f * p->Tilt.VelTiltStrength / velTiltScale);
	Matrix_MulBy(&Camera.TiltM, &velX);
}


/*########################################################################################################################*
*---------------------------------------------------First person camera---------------------------------------------------*
*#########################################################################################################################*/
static Vec2 FirstPersonCamera_GetOrientation(void) {
	Vec2 v = mouseRot;
	v.X *= MATH_DEG2RAD; v.Y *= MATH_DEG2RAD;

	vec3s vr = glms_euler_angles(g_mat4HMDPose);
	v.X += vr.y;
	v.Y += vr.x;

	return v;
}

static Vec3 FirstPersonCamera_GetPosition(float t) {
	struct Entity* p = &LocalPlayer_Instance.Base;
	Vec3 camPos   = p->Position;
	return camPos;
}

static cc_bool FirstPersonCamera_Zoom(float amount) { return false; }
static struct Camera cam_FirstPerson = {
	false,
	PerspectiveCamera_GetProjection,  PerspectiveCamera_GetView,
	FirstPersonCamera_GetOrientation, FirstPersonCamera_GetPosition,
	PerspectiveCamera_UpdateMouse,    Camera_OnRawMovement,
	Window_EnableRawMouse,            Window_DisableRawMouse,
	PerspectiveCamera_GetPickedBlock, FirstPersonCamera_Zoom,
};


/*########################################################################################################################*
*---------------------------------------------------Third person camera---------------------------------------------------*
*#########################################################################################################################*/
#define DEF_ZOOM 3.0f
static float dist_third = DEF_ZOOM, dist_forward = DEF_ZOOM;

static Vec2 ThirdPersonCamera_GetOrientation(void) {
	Vec2 v = mouseRot;
	v.X *= MATH_DEG2RAD; v.Y *= MATH_DEG2RAD;
	if (cam_isForwardThird) { v.X += MATH_PI; v.Y = -v.Y; }

	v.X += cam_rotOffset.X * MATH_DEG2RAD; 
	v.Y += cam_rotOffset.Y * MATH_DEG2RAD;

	vec3s vr = glms_euler_angles(g_mat4HMDPose);
	v.X += vr.y;
	v.Y += vr.x;

	return v;
}

static float ThirdPersonCamera_GetZoom(void) {
	float dist = cam_isForwardThird ? dist_forward : dist_third;
	/* Don't allow zooming out when -fly */
	if (dist > DEF_ZOOM && !LocalPlayer_CheckCanZoom()) dist = DEF_ZOOM;
	return dist;
}

static Vec3 ThirdPersonCamera_GetPosition(float t) {
	struct Entity* p = &LocalPlayer_Instance.Base;
	float dist = ThirdPersonCamera_GetZoom();
	Vec3 target, dir;
	Vec2 rot;

	target = p->Position;
	target.Y += Camera.BobbingVer;

	rot = Camera.Active->GetOrientation();
	dir = Vec3_GetDirVector(rot.X, rot.Y);
	Vec3_Negate(&dir, &dir);

	Picking_ClipCameraPos(&target, &dir, dist, &cameraClipPos);
	return cameraClipPos.Intersect;
}

static cc_bool ThirdPersonCamera_Zoom(float amount) {
	float* dist   = cam_isForwardThird ? &dist_forward : &dist_third;
	float newDist = *dist - amount;

	*dist = max(newDist, 2.0f); 
	return true;
}

static struct Camera cam_ThirdPerson = {
	true,
	PerspectiveCamera_GetProjection,  PerspectiveCamera_GetView,
	ThirdPersonCamera_GetOrientation, ThirdPersonCamera_GetPosition,
	PerspectiveCamera_UpdateMouse,    Camera_OnRawMovement,
	Window_EnableRawMouse,            Window_DisableRawMouse,
	PerspectiveCamera_GetPickedBlock, ThirdPersonCamera_Zoom,
};
static struct Camera cam_ForwardThird = {
	true,
	PerspectiveCamera_GetProjection,  PerspectiveCamera_GetView,
	ThirdPersonCamera_GetOrientation, ThirdPersonCamera_GetPosition,
	PerspectiveCamera_UpdateMouse,    Camera_OnRawMovement,
	Window_EnableRawMouse,            Window_DisableRawMouse,
	PerspectiveCamera_GetPickedBlock, ThirdPersonCamera_Zoom,
};


/*########################################################################################################################*
*-----------------------------------------------------General camera------------------------------------------------------*
*#########################################################################################################################*/
static void OnRawMovement(void* obj, float deltaX, float deltaY) {
	Camera.Active->OnRawMovement(deltaX, deltaY);
}

static void OnHacksChanged(void* obj) {
	struct HacksComp* h = &LocalPlayer_Instance.Hacks;
	/* Leave third person if not allowed anymore */
	if (!h->CanUseThirdPerson || !h->Enabled) Camera_CycleActive();
}

void Camera_CycleActive(void) {
	struct LocalPlayer* p = &LocalPlayer_Instance;
	if (Game_ClassicMode) return;
	Camera.Active = Camera.Active->next;

	if (!p->Hacks.CanUseThirdPerson || !p->Hacks.Enabled) {
		Camera.Active = &cam_FirstPerson;
	}
	cam_isForwardThird = Camera.Active == &cam_ForwardThird;

	/* reset rotation offset when changing cameras */
	cam_rotOffset.X = 0.0f; cam_rotOffset.Y = 0.0f;
	Camera_UpdateProjection(EVREye_Eye_Left);
}

static struct Camera* cams_head;
static struct Camera* cams_tail;
void Camera_Register(struct Camera* cam) {
	LinkedList_Append(cam, cams_head, cams_tail);
	/* want a circular linked list */
	cam->next = cams_head;
}

static cc_bool cam_focussed;
void Camera_CheckFocus(void) {
	cc_bool focus = Gui.InputGrab == NULL;
	if (focus == cam_focussed) return;
	cam_focussed = focus;

	if (focus) {
		Camera.Active->AcquireFocus();
	} else {
		Camera.Active->LoseFocus();
	}
}

void Camera_SetFov(int fov) {
	if (Camera.Fov == fov) return;
	Camera.Fov = fov;
	Camera_UpdateProjection(EVREye_Eye_Left);
}

void Camera_UpdateProjection(Hmd_Eye nEye) {
	Camera.Active->GetProjection(&Gfx.Projection, nEye);
	Gfx_LoadMatrix(MATRIX_PROJECTION, &Gfx.Projection);
	Event_RaiseVoid(&GfxEvents.ProjectionChanged);
}

static void OnInit(void) {
	Camera_Register(&cam_FirstPerson);
	Camera_Register(&cam_ThirdPerson);
	Camera_Register(&cam_ForwardThird);

	Camera.Active = &cam_FirstPerson;
	Event_Register_(&PointerEvents.RawMoved,      NULL, OnRawMovement);
	Event_Register_(&UserEvents.HackPermsChanged, NULL, OnHacksChanged);

#ifdef CC_BUILD_WIN
	Camera.Sensitivity = Options_GetInt(OPT_SENSITIVITY, 1, 200, 40);
#else
	Camera.Sensitivity = Options_GetInt(OPT_SENSITIVITY, 1, 200, 30);
#endif
	Camera.Clipping    = Options_GetBool(OPT_CAMERA_CLIPPING, true);
	Camera.Invert      = Options_GetBool(OPT_INVERT_MOUSE, false);
	Camera.Mass        = Options_GetFloat(OPT_CAMERA_MASS, 1, 100, 20);
	Camera.Smooth      = Options_GetBool(OPT_CAMERA_SMOOTH, false);

	Camera.DefaultFov  = Options_GetInt(OPT_FIELD_OF_VIEW, 1, 179, 70);
	Camera.Fov         = Camera.DefaultFov;
	Camera.ZoomFov     = Camera.DefaultFov;
	Camera_UpdateProjection(EVREye_Eye_Left);
}

struct IGameComponent Camera_Component = {
	OnInit /* Init  */
};
