#include "../stdafx.h"
#define BASE_IMPL
#pragma comment(lib, "Dwrite")
#include "BaseCompositor.h"
#include "BaseInput.h"
#include "BaseOverlay.h"
#include "BaseSystem.h"
#include "Compositor/compositor.h"
#include "Drivers/Backend.h"
#include "Misc/Config.h"
#include "Misc/ScopeGuard.h"
#include "Misc/SwapchainManager.h"
#include "convert.h"
#include "generated/static_bases.gen.h"
#if __has_include(<glm/glm.hpp>)
#include <glm/glm.hpp>
#else
#include "../../libs/glm/glm/glm.hpp"
#endif
#include <string>
#include <algorithm>
#include "../OpenOVR/Misc/xrutil.h"
#include <d3d11.h>
#include <dwrite.h>
#include "../DrvOpenXR/pub/DrvOpenXR.h"
#include "../DrvOpenXR/XrBackend.h"
#include "../DrvOpenXR/XrDriverPrivate.h"
#include <corecrt_math_defines.h>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstring>
#include <thread>

using glm::mat4;
using glm::vec3;

using namespace vr;

extern XrInstance xr_instance;
extern SessionWrapper xr_session;
extern XrSystemId xr_system;
extern XrSessionGlobals* xr_gbl;

std::mutex stateMutex; // to protect swapchain / images during rendering

static constexpr int kKeyboardTexWidth = 1024;
static constexpr int kKeyboardTexHeight = 512;
static constexpr float kKeyboardWidthMeters = 0.82f;
static constexpr float kKeyboardHeightMeters = 0.41f;
static constexpr float kKeyboardForwardDistanceMeters = 0.72f;
static constexpr float kKeyboardVerticalOffsetMeters = -0.04f;

static constexpr int kPointerTexWidth = 16;
static constexpr int kPointerTexHeight = 16;
static constexpr float kPointerWidthMeters = 0.0035f;

BaseOverlay::OverlayState BaseOverlay::overlayState;
static BaseOverlay::OverlayState& overlayState = BaseOverlay::overlayState;
static vr::VROverlayHandle_t keyboardOverlayHandle = vr::k_ulOverlayHandleInvalid;

// Forward declarations for keyboard helpers
static void EnsureKeyboardLayout();
static void PollKeyboardControllers();
static void CreatePointerSwapchain(XrSession session, BaseOverlay::OverlayState& state);

void BaseOverlay::InitializeOverlay()
{
	if (!overlayState.initialized) {
		UpdateKeyboardOverlay(overlayState.currentTextBuffer);
		overlayState.initialized = true;
	}
}

void BaseOverlay::SubmitOverlay()
{
	if (overlayState.initialized) {
		const XrCompositionLayerBaseHeader* layers[] = { reinterpret_cast<const XrCompositionLayerBaseHeader*>(&overlayState.overlayLayer) };

		XrFrameEndInfo frameEndInfo{ XR_TYPE_FRAME_END_INFO };
		frameEndInfo.displayTime = xr_gbl->GetBestTime();
		frameEndInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
		frameEndInfo.layerCount = 1;
		frameEndInfo.layers = layers;

		xrEndFrame(xr_session.get(), &frameEndInfo);
	}
}

bool BaseOverlay::IsOverlayInitialized()
{
	return overlayState.initialized;
}

// Class to represent an overlay
class BaseOverlay::OverlayData {
public:
	const string key;
	string name;
	HmdColor_t colour;

	float widthMeters = 1; // default 1 meter

	float autoCurveDistanceRangeMin, autoCurveDistanceRangeMax; // WTF does this do?
	EColorSpace colourSpace = ColorSpace_Auto;
	bool visible = false; // TODO check against SteamVR
	VRTextureBounds_t textureBounds = { 0, 0, 1, 1 };
	VROverlayInputMethod inputMethod = VROverlayInputMethod_None; // TODO fire events
	HmdVector2_t mouseScale = { 1.0f, 1.0f };
	bool highQuality = false;
	uint64_t flags = 0;
	float texelAspect = 1;
	std::queue<VREvent_t> eventQueue;
	std::mutex eventQueueMutex;

	// Rendering
	Texture_t texture = {};
	XrCompositionLayerQuad layerQuad = { XR_TYPE_COMPOSITION_LAYER_QUAD };
	std::unique_ptr<Compositor> compositor;

	// Transform
	VROverlayTransformType transformType = VROverlayTransform_Absolute;
	union {
		struct {
			HmdMatrix34_t offset;
			TrackedDeviceIndex_t device;
		} deviceRelative;
	} transformData;

	MfMatrix4f overlayTransform{
		1.0f, 0.0f, 0.0f, 0.0f,
		0.0f, 1.0f, 0.0f, 0.0f,
		0.0f, 0.0f, 1.0f, -1.01f,
		0.0f, 0.0f, 0.0f, 1.0f
	};

	OverlayData(string key, string name)
	    : key(key), name(name)
	{
	}
};

// TODO don't pass around handles, as it will cause
// crashes when we should merely return VROverlayError_InvalidHandle
#define OVL (*((OverlayData**)pOverlayHandle))
#define USEH()                                                                        \
	OverlayData* overlay = (OverlayData*)ulOverlayHandle;                             \
	if (!overlay || !validOverlays.count(overlay) || !overlays.count(overlay->key)) { \
		return VROverlayError_InvalidHandle;                                          \
	}

#define USEHB()                                           \
	OverlayData* overlay = (OverlayData*)ulOverlayHandle; \
	if (!overlay || !overlays.count(overlay->key)) {      \
		return false;                                     \
	}

BaseOverlay::~BaseOverlay()
{
	for (const auto& kv : overlays) {
		if (kv.second) {
			delete kv.second;
		}
	}
}

int BaseOverlay::_BuildLayers(XrCompositionLayerBaseHeader* sceneLayer, const XrCompositionLayerBaseHeader* const*& result)
{
	layerHeaders.clear();

	if (sceneLayer)
		layerHeaders.push_back(sceneLayer);

	bool keyboardActive = false;
	{
		std::lock_guard<std::mutex> lockState(overlayState.stateMutex);
		keyboardActive = overlayState.keyboardVisible && overlayState.keyboardSwapchain != XR_NULL_HANDLE;
	}

	// Poll pointer/controller input on the main frame thread to avoid OpenXR/input races from the render thread.
	if (keyboardActive) {
		PollKeyboardControllers();
		RenderKeyboardText();
	}

	// If the keyboard is visible and we have a valid keyboard swapchain, submit it as an extra layer.
	{
		std::lock_guard<std::mutex> lockState(overlayState.stateMutex);
		if (overlayState.keyboardVisible && overlayState.keyboardSwapchain != XR_NULL_HANDLE) {
			layerHeaders.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&overlayState.overlayLayer));
			for (int i = 0; i < 2; ++i) {
				if (overlayState.pointerVisible[i] && overlayState.pointerLayers[i].subImage.swapchain != XR_NULL_HANDLE) {
					layerHeaders.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&overlayState.pointerLayers[i]));
				}
			}
		}
	}

	result = layerHeaders.data();
	return static_cast<int>(layerHeaders.size());
}

bool BaseOverlay::_HandleOverlayInput(EVREye side, TrackedDeviceIndex_t index, VRControllerState_t state)
{
	if (!usingInput)
		return true;

	if (!keyboard)
		return true;

#ifndef OC_XR_PORT
	keyboard->HandleOverlayInput(side, state, (float)ovr_GetTimeInSeconds());
#endif
	return false;
}

EVROverlayError BaseOverlay::FindOverlay(const char* pchOverlayKey, VROverlayHandle_t* pOverlayHandle)
{
	if (overlays.count(pchOverlayKey)) {
		OVL = overlays[pchOverlayKey];
		return VROverlayError_None;
	}

	// TODO is this the correct return value
	return VROverlayError_InvalidParameter;
}
EVROverlayError BaseOverlay::CreateOverlay(const char* pchOverlayKey, const char* pchOverlayName, VROverlayHandle_t* pOverlayHandle)
{
	if (overlays.count(pchOverlayKey)) {
		return VROverlayError_KeyInUse;
	}

	OverlayData* data = new OverlayData(pchOverlayKey, pchOverlayName);
	OVL = data;

	overlays[pchOverlayKey] = data;
	validOverlays.insert(data);

	data->layerQuad.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
	data->layerQuad.next = NULL;
	data->layerQuad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
	data->layerQuad.space = xr_space_from_ref_space_type(GetUnsafeBaseSystem()->currentSpace);
	data->layerQuad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
	data->layerQuad.pose = { { 0.f, 0.f, 0.f, 1.f },
		{ 0.0f, 0.0f, -0.65f } };

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::DestroyOverlay(VROverlayHandle_t ulOverlayHandle)
{
	USEH();

	if (highQualityOverlay == ulOverlayHandle)
		highQualityOverlay = vr::k_ulOverlayHandleInvalid;

	overlays.erase(overlay->key);
	validOverlays.erase(overlay);
	delete overlay;

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::SetHighQualityOverlay(VROverlayHandle_t ulOverlayHandle)
{
	USEH();

	highQualityOverlay = ulOverlayHandle;

	return VROverlayError_None;
}
VROverlayHandle_t BaseOverlay::GetHighQualityOverlay()
{
	if (!highQualityOverlay)
		return k_ulOverlayHandleInvalid;

	return highQualityOverlay;
}
uint32_t BaseOverlay::GetOverlayKey(VROverlayHandle_t ulOverlayHandle, char* pchValue, uint32_t unBufferSize, EVROverlayError* pError)
{
	OverlayData* overlay = (OverlayData*)ulOverlayHandle;
	if (!overlays.count(overlay->key)) {
		if (pError)
			*pError = VROverlayError_InvalidHandle;
		if (unBufferSize != 0)
			pchValue = 0;
		return 0;
	}

	const char* key = overlay->key.c_str();
	strncpy_s(pchValue, unBufferSize, key, unBufferSize);

	if (strlen(key) >= unBufferSize && unBufferSize != 0) {
		pchValue[unBufferSize - 1] = 0;
	}

	if (pError)
		*pError = VROverlayError_None;

	// Is this supposed to include the NULL or not?
	// TODO test, this could cause some very nasty bugs
	return static_cast<uint32_t>(strlen(pchValue) + 1);
}
uint32_t BaseOverlay::GetOverlayName(VROverlayHandle_t ulOverlayHandle, VR_OUT_STRING() char* pchValue, uint32_t unBufferSize, EVROverlayError* pError)
{
	if (pError)
		*pError = VROverlayError_None;

	OverlayData* overlay = (OverlayData*)ulOverlayHandle;
	if (!overlays.count(overlay->key)) {
		if (pError)
			*pError = VROverlayError_InvalidHandle;
		if (unBufferSize != 0)
			pchValue[0] = 0;
		return 0;
	}

	const char* name = overlay->name.c_str();
	strncpy_s(pchValue, unBufferSize, name, unBufferSize);

	if (strlen(name) >= unBufferSize && unBufferSize != 0) {
		pchValue[unBufferSize - 1] = 0;
	}

	// Is this supposed to include the NULL or not?
	// TODO test, this could cause some very nasty bugs
	return static_cast<uint32_t>(strlen(pchValue) + 1);
}
EVROverlayError BaseOverlay::SetOverlayName(VROverlayHandle_t ulOverlayHandle, const char* pchName)
{
	USEH();

	overlay->name = pchName;

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::GetOverlayImageData(VROverlayHandle_t ulOverlayHandle, void* pvBuffer, uint32_t unBufferSize, uint32_t* punWidth, uint32_t* punHeight)
{
	STUBBED();
}
const char* BaseOverlay::GetOverlayErrorNameFromEnum(EVROverlayError error)
{
#define ERR_CASE(name)          \
	case VROverlayError_##name: \
		return #name;
	switch (error) {
		ERR_CASE(None);
		ERR_CASE(UnknownOverlay);
		ERR_CASE(InvalidHandle);
		ERR_CASE(PermissionDenied);
		ERR_CASE(OverlayLimitExceeded);
		ERR_CASE(WrongVisibilityType);
		ERR_CASE(KeyTooLong);
		ERR_CASE(NameTooLong);
		ERR_CASE(KeyInUse);
		ERR_CASE(WrongTransformType);
		ERR_CASE(InvalidTrackedDevice);
		ERR_CASE(InvalidParameter);
		ERR_CASE(ThumbnailCantBeDestroyed);
		ERR_CASE(ArrayTooSmall);
		ERR_CASE(RequestFailed);
		ERR_CASE(InvalidTexture);
		ERR_CASE(UnableToLoadFile);
		ERR_CASE(KeyboardAlreadyInUse);
		ERR_CASE(NoNeighbor);
		ERR_CASE(TooManyMaskPrimitives);
		ERR_CASE(BadMaskPrimitive);
	}
#undef ERR_CASE

	string msg = "Unknown overlay error code: " + to_string(error);
	OOVR_LOG(msg.c_str());

	STUBBED();
}
EVROverlayError BaseOverlay::SetOverlayRenderingPid(VROverlayHandle_t ulOverlayHandle, uint32_t unPID)
{
	STUBBED();
}
uint32_t BaseOverlay::GetOverlayRenderingPid(VROverlayHandle_t ulOverlayHandle)
{
	STUBBED();
}
EVROverlayError BaseOverlay::SetOverlayFlag(VROverlayHandle_t ulOverlayHandle, VROverlayFlags eOverlayFlag, bool bEnabled)
{
	USEH();

	if (bEnabled) {
		overlay->flags |= 1uLL << eOverlayFlag;
	} else {
		overlay->flags &= ~(1uLL << eOverlayFlag);
	}

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::GetOverlayFlag(VROverlayHandle_t ulOverlayHandle, VROverlayFlags eOverlayFlag, bool* pbEnabled)
{
	USEH();

	*pbEnabled = (overlay->flags & (1uLL << eOverlayFlag)) != 0uLL;

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::SetOverlayColor(VROverlayHandle_t ulOverlayHandle, float fRed, float fGreen, float fBlue)
{
	USEH();

	overlay->colour.r = fRed;
	overlay->colour.g = fGreen;
	overlay->colour.b = fBlue;

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::GetOverlayColor(VROverlayHandle_t ulOverlayHandle, float* pfRed, float* pfGreen, float* pfBlue)
{
	USEH();

	*pfRed = overlay->colour.r;
	*pfGreen = overlay->colour.g;
	*pfBlue = overlay->colour.b;

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::SetOverlayAlpha(VROverlayHandle_t ulOverlayHandle, float fAlpha)
{
	USEH();

	overlay->colour.a = fAlpha;

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::GetOverlayAlpha(VROverlayHandle_t ulOverlayHandle, float* pfAlpha)
{
	USEH();

	*pfAlpha = overlay->colour.a;

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::SetOverlayTexelAspect(VROverlayHandle_t ulOverlayHandle, float fTexelAspect)
{
	USEH();

	overlay->texelAspect = fTexelAspect;

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::GetOverlayTexelAspect(VROverlayHandle_t ulOverlayHandle, float* pfTexelAspect)
{
	USEH();

	if (!pfTexelAspect)
		OOVR_ABORT("pfTexelAspect == nullptr");

	*pfTexelAspect = overlay->texelAspect;

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::SetOverlaySortOrder(VROverlayHandle_t ulOverlayHandle, uint32_t unSortOrder)
{
	// TODO
	return VROverlayError_None;
}
EVROverlayError BaseOverlay::GetOverlaySortOrder(VROverlayHandle_t ulOverlayHandle, uint32_t* punSortOrder)
{
	STUBBED();
}
EVROverlayError BaseOverlay::SetOverlayWidthInMeters(VROverlayHandle_t ulOverlayHandle, float fWidthInMeters)
{
	USEH();

	overlay->widthMeters = fWidthInMeters;

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::GetOverlayWidthInMeters(VROverlayHandle_t ulOverlayHandle, float* pfWidthInMeters)
{
	USEH();

	*pfWidthInMeters = overlay->widthMeters;

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::SetOverlayCurvature(VROverlayHandle_t ulOverlayHandle, float fCurvature)
{
	STUBBED();
}
EVROverlayError BaseOverlay::GetOverlayCurvature(VROverlayHandle_t ulOverlayHandle, float* pfCurvature)
{
	STUBBED();
}
EVROverlayError BaseOverlay::SetOverlayAutoCurveDistanceRangeInMeters(VROverlayHandle_t ulOverlayHandle, float fMinDistanceInMeters, float fMaxDistanceInMeters)
{
	USEH();

	overlay->autoCurveDistanceRangeMin = fMinDistanceInMeters;
	overlay->autoCurveDistanceRangeMax = fMaxDistanceInMeters;

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::GetOverlayAutoCurveDistanceRangeInMeters(VROverlayHandle_t ulOverlayHandle, float* pfMinDistanceInMeters, float* pfMaxDistanceInMeters)
{
	USEH();

	*pfMinDistanceInMeters = overlay->autoCurveDistanceRangeMin;
	*pfMaxDistanceInMeters = overlay->autoCurveDistanceRangeMax;

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::SetOverlayTextureColorSpace(VROverlayHandle_t ulOverlayHandle, EColorSpace eTextureColorSpace)
{
	USEH();

	overlay->colourSpace = eTextureColorSpace;

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::GetOverlayTextureColorSpace(VROverlayHandle_t ulOverlayHandle, EColorSpace* peTextureColorSpace)
{
	USEH();

	*peTextureColorSpace = overlay->colourSpace;

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::SetOverlayTextureBounds(VROverlayHandle_t ulOverlayHandle, const VRTextureBounds_t* pOverlayTextureBounds)
{
	USEH();

	if (pOverlayTextureBounds)
		overlay->textureBounds = *pOverlayTextureBounds;
	else
		overlay->textureBounds = { 0, 0, 1, 1 };

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::GetOverlayTextureBounds(VROverlayHandle_t ulOverlayHandle, VRTextureBounds_t* pOverlayTextureBounds)
{
	USEH();

	*pOverlayTextureBounds = overlay->textureBounds;

	return VROverlayError_None;
}
uint32_t BaseOverlay::GetOverlayRenderModel(VROverlayHandle_t ulOverlayHandle, char* pchValue, uint32_t unBufferSize, HmdColor_t* pColor, EVROverlayError* pError)
{
	if (pError)
		*pError = VROverlayError_None;

	STUBBED();
}
EVROverlayError BaseOverlay::SetOverlayRenderModel(VROverlayHandle_t ulOverlayHandle, const char* pchRenderModel, const HmdColor_t* pColor)
{
	STUBBED();
}
EVROverlayError BaseOverlay::GetOverlayTransformType(VROverlayHandle_t ulOverlayHandle, VROverlayTransformType* peTransformType)
{
	USEH();

	*peTransformType = overlay->transformType;

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::SetOverlayTransformAbsolute(VROverlayHandle_t ulOverlayHandle, ETrackingUniverseOrigin eTrackingOrigin, const HmdMatrix34_t* pmatTrackingOriginToOverlayTransform)
{
	USEH();

	// TODO account for the universe origin, and if it doesn't match that currently in use then add or
	//  subtract the floor position to match it. This shouldn't usually be an issue though, as I can't
	//  imagine many apps will use a different origin for their overlays.

	overlay->transformType = VROverlayTransform_Absolute;
	S2O_om44(*pmatTrackingOriginToOverlayTransform, overlay->overlayTransform);

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::GetOverlayTransformAbsolute(VROverlayHandle_t ulOverlayHandle, ETrackingUniverseOrigin* peTrackingOrigin, HmdMatrix34_t* pmatTrackingOriginToOverlayTransform)
{
	USEH();

	if (overlay->transformType != VROverlayTransform_Absolute)
		return VROverlayError_WrongTransformType;

	O2S_om34(overlay->overlayTransform, *pmatTrackingOriginToOverlayTransform);

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::SetOverlayTransformTrackedDeviceRelative(VROverlayHandle_t ulOverlayHandle, TrackedDeviceIndex_t unTrackedDevice, const HmdMatrix34_t* pmatTrackedDeviceToOverlayTransform)
{
	USEH();

	overlay->transformType = VROverlayTransform_TrackedDeviceRelative;
	overlay->transformData.deviceRelative.device = unTrackedDevice;
	overlay->transformData.deviceRelative.offset = *pmatTrackedDeviceToOverlayTransform;

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::GetOverlayTransformTrackedDeviceRelative(VROverlayHandle_t ulOverlayHandle, TrackedDeviceIndex_t* punTrackedDevice, HmdMatrix34_t* pmatTrackedDeviceToOverlayTransform)
{
	STUBBED();
}
EVROverlayError BaseOverlay::SetOverlayTransformTrackedDeviceComponent(VROverlayHandle_t ulOverlayHandle, TrackedDeviceIndex_t unDeviceIndex, const char* pchComponentName)
{
	STUBBED();
}
EVROverlayError BaseOverlay::GetOverlayTransformTrackedDeviceComponent(VROverlayHandle_t ulOverlayHandle, TrackedDeviceIndex_t* punDeviceIndex, char* pchComponentName, uint32_t unComponentNameSize)
{
	STUBBED();
}
EVROverlayError BaseOverlay::GetOverlayTransformOverlayRelative(VROverlayHandle_t ulOverlayHandle, VROverlayHandle_t* ulOverlayHandleParent, HmdMatrix34_t* pmatParentOverlayToOverlayTransform)
{
	STUBBED();
}
EVROverlayError BaseOverlay::SetOverlayTransformOverlayRelative(VROverlayHandle_t ulOverlayHandle, VROverlayHandle_t ulOverlayHandleParent, const HmdMatrix34_t* pmatParentOverlayToOverlayTransform)
{
	// TODO
	return VROverlayError_None;
}
EVROverlayError BaseOverlay::SetOverlayTransformCursor(VROverlayHandle_t ulCursorOverlayHandle, const HmdVector2_t* pvHotspot)
{
	STUBBED();
}
EVROverlayError BaseOverlay::GetOverlayTransformCursor(VROverlayHandle_t ulOverlayHandle, HmdVector2_t* pvHotspot)
{
	STUBBED();
}
EVROverlayError BaseOverlay::SetOverlayTransformProjection(VROverlayHandle_t ulOverlayHandle,
    ETrackingUniverseOrigin eTrackingOrigin, const HmdMatrix34_t* pmatTrackingOriginToOverlayTransform,
    const OOVR_VROverlayProjection_t* pProjection, EVREye eEye)
{
	STUBBED();
}
EVROverlayError BaseOverlay::ShowOverlay(VROverlayHandle_t ulOverlayHandle)
{
	USEH();
	overlay->visible = true;
	return VROverlayError_None;
}
EVROverlayError BaseOverlay::HideOverlay(VROverlayHandle_t ulOverlayHandle)
{
	USEH();
	overlay->visible = false;
	return VROverlayError_None;
}
bool BaseOverlay::IsOverlayVisible(VROverlayHandle_t ulOverlayHandle)
{
	USEHB();
	return overlay->visible;
}
EVROverlayError BaseOverlay::GetTransformForOverlayCoordinates(VROverlayHandle_t ulOverlayHandle, ETrackingUniverseOrigin eTrackingOrigin, HmdVector2_t coordinatesInOverlay, HmdMatrix34_t* pmatTransform)
{
	STUBBED();
}
bool BaseOverlay::PollNextOverlayEvent(VROverlayHandle_t ulOverlayHandle, VREvent_t* pEvent, uint32_t eventSize)
{
	USEHB();

	memset(pEvent, 0, eventSize);

	std::lock_guard<std::mutex> lock(overlay->eventQueueMutex);
	if (overlay->eventQueue.empty())
		return false;

	VREvent_t e = overlay->eventQueue.front();
	overlay->eventQueue.pop();

	memcpy(pEvent, &e, std::min((uint32_t)sizeof(e), eventSize));

	return true;
}
EVROverlayError BaseOverlay::GetOverlayInputMethod(VROverlayHandle_t ulOverlayHandle, VROverlayInputMethod* peInputMethod)
{
	USEH();

	if (peInputMethod)
		*peInputMethod = overlay->inputMethod;

	return VROverlayError_None;
}

EVROverlayError BaseOverlay::SetOverlayInputMethod(VROverlayHandle_t ulOverlayHandle, VROverlayInputMethod eInputMethod)
{
	USEH();

	overlay->inputMethod = eInputMethod;

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::GetOverlayMouseScale(VROverlayHandle_t ulOverlayHandle, HmdVector2_t* pvecMouseScale)
{
	USEH();

	*pvecMouseScale = overlay->mouseScale;

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::SetOverlayMouseScale(VROverlayHandle_t ulOverlayHandle, const HmdVector2_t* pvecMouseScale)
{
	USEH();

	if (pvecMouseScale)
		overlay->mouseScale = *pvecMouseScale;
	else
		overlay->mouseScale = HmdVector2_t{ 1.0f, 1.0f };

	return VROverlayError_None;
}
bool BaseOverlay::ComputeOverlayIntersection(VROverlayHandle_t ulOverlayHandle, const OOVR_VROverlayIntersectionParams_t* pParams, OOVR_VROverlayIntersectionResults_t* pResults)
{
	STUBBED();
}
bool BaseOverlay::HandleControllerOverlayInteractionAsMouse(VROverlayHandle_t ulOverlayHandle, TrackedDeviceIndex_t unControllerDeviceIndex)
{
	STUBBED();
}
bool BaseOverlay::IsHoverTargetOverlay(VROverlayHandle_t ulOverlayHandle)
{
	STUBBED();
}
VROverlayHandle_t BaseOverlay::GetGamepadFocusOverlay()
{
	STUBBED();
}
EVROverlayError BaseOverlay::SetGamepadFocusOverlay(VROverlayHandle_t ulNewFocusOverlay)
{
	STUBBED();
}
EVROverlayError BaseOverlay::SetOverlayNeighbor(EOverlayDirection eDirection, VROverlayHandle_t ulFrom, VROverlayHandle_t ulTo)
{
	STUBBED();
}
EVROverlayError BaseOverlay::MoveGamepadFocusToNeighbor(EOverlayDirection eDirection, VROverlayHandle_t ulFrom)
{
	STUBBED();
}
EVROverlayError BaseOverlay::SetOverlayDualAnalogTransform(VROverlayHandle_t ulOverlay, EDualAnalogWhich eWhich, const HmdVector2_t& vCenter, float fRadius)
{
	STUBBED();
}
EVROverlayError BaseOverlay::GetOverlayDualAnalogTransform(VROverlayHandle_t ulOverlay, EDualAnalogWhich eWhich, HmdVector2_t* pvCenter, float* pfRadius)
{
	STUBBED();
}
EVROverlayError BaseOverlay::SetOverlayDualAnalogTransform(VROverlayHandle_t ulOverlay, EDualAnalogWhich eWhich, const HmdVector2_t* pvCenter, float fRadius)
{
	STUBBED();
}
EVROverlayError BaseOverlay::TriggerLaserMouseHapticVibration(VROverlayHandle_t ulOverlayHandle, float fDurationSeconds, float fFrequency, float fAmplitude)
{
	STUBBED();
}
EVROverlayError BaseOverlay::SetOverlayCursor(VROverlayHandle_t ulOverlayHandle, VROverlayHandle_t ulCursorHandle)
{
	STUBBED();
}
EVROverlayError BaseOverlay::SetOverlayCursorPositionOverride(VROverlayHandle_t ulOverlayHandle, const HmdVector2_t* pvCursor)
{
	STUBBED();
}
EVROverlayError BaseOverlay::ClearOverlayCursorPositionOverride(VROverlayHandle_t ulOverlayHandle)
{
	STUBBED();
}
EVROverlayError BaseOverlay::SetOverlayTexture(VROverlayHandle_t ulOverlayHandle, const Texture_t* pTexture)
{
	USEH();
	overlay->texture = *pTexture;

	BackendManager::Instance().OnOverlayTexture(pTexture);

	if (!BackendManager::Instance().IsGraphicsConfigured())
		return VROverlayError_None;

	if (!overlay->compositor) {
		overlay->compositor = GetUnsafeBaseCompositor()->CreateCompositorAPI(pTexture);
	}

	overlay->compositor->LoadSubmitContext();
	auto revertToCallerContext = MakeScopeGuard([&]() {
		overlay->compositor->ResetSubmitContext();
	});

	overlay->compositor->Invoke(&overlay->texture, nullptr, overlay->layerQuad.subImage);
	overlay->layerQuad.space = xr_space_from_ref_space_type(GetUnsafeBaseSystem()->currentSpace);

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::ClearOverlayTexture(VROverlayHandle_t ulOverlayHandle)
{
	USEH();
	overlay->texture = {};

	overlay->compositor.reset();
	return VROverlayError_None;
}
EVROverlayError BaseOverlay::SetOverlayRaw(VROverlayHandle_t ulOverlayHandle, void* pvBuffer, uint32_t unWidth, uint32_t unHeight, uint32_t unDepth)
{
	STUBBED();
}
EVROverlayError BaseOverlay::SetOverlayFromFile(VROverlayHandle_t ulOverlayHandle, const char* pchFilePath)
{
	STUBBED();
}
EVROverlayError BaseOverlay::GetOverlayTexture(VROverlayHandle_t ulOverlayHandle, void** pNativeTextureHandle, void* pNativeTextureRef, uint32_t* pWidth, uint32_t* pHeight, uint32_t* pNativeFormat, ETextureType* pAPIType, EColorSpace* pColorSpace, VRTextureBounds_t* pTextureBounds)
{
	STUBBED();
}
EVROverlayError BaseOverlay::ReleaseNativeOverlayHandle(VROverlayHandle_t ulOverlayHandle, void* pNativeTextureHandle)
{
	STUBBED();
}
EVROverlayError BaseOverlay::GetOverlayTextureSize(VROverlayHandle_t ulOverlayHandle, uint32_t* pWidth, uint32_t* pHeight)
{
	STUBBED();
}
EVROverlayError BaseOverlay::CreateDashboardOverlay(const char* pchOverlayKey, const char* pchOverlayFriendlyName, VROverlayHandle_t* pMainHandle, VROverlayHandle_t* pThumbnailHandle)
{
	STUBBED();
}
bool BaseOverlay::IsDashboardVisible()
{
	// TODO should this be based of whether Dash is open?
	// Probably, but handling focus opens some other issues as it triggers under other conditions.
	return false;
}
bool BaseOverlay::IsActiveDashboardOverlay(VROverlayHandle_t ulOverlayHandle)
{
	STUBBED();
}
EVROverlayError BaseOverlay::SetDashboardOverlaySceneProcess(VROverlayHandle_t ulOverlayHandle, uint32_t unProcessId)
{
	STUBBED();
}
EVROverlayError BaseOverlay::GetDashboardOverlaySceneProcess(VROverlayHandle_t ulOverlayHandle, uint32_t* punProcessId)
{
	STUBBED();
}
void BaseOverlay::ShowDashboard(const char* pchOverlayToShow)
{
	STUBBED();
}
TrackedDeviceIndex_t BaseOverlay::GetPrimaryDashboardDevice()
{
	STUBBED();
}
EVROverlayError BaseOverlay::ShowKeyboardWithDispatch(EGamepadTextInputMode eInputMode, EGamepadTextInputLineMode eLineInputMode,
    const char* pchDescription, uint32_t unCharMax, const char* pchExistingText, bool bUseMinimalMode, uint64_t uUserValue,
    VRKeyboard::eventDispatch_t eventDispatch)
{
	// Store keyboard state
	overlayState.keyboardVisible = true;
	overlayState.currentTextBuffer = pchExistingText ? pchExistingText : "";
	overlayState.currentUserValue = uUserValue;
	overlayState.currentEventDispatch = eventDispatch;
	overlayState.maxCharacters = unCharMax;
	overlayState.shiftActive = false;
	overlayState.capsLocked = false;
	overlayState.hoveredKeys[0] = -1;
	overlayState.hoveredKeys[1] = -1;
	overlayState.lastButtonState[0] = 0;
	overlayState.lastButtonState[1] = 0;
	overlayState.stopRenderThread.store(false);
	overlayState.renderDirty = true;

	// Create/update the text overlay
	UpdateKeyboardOverlay(overlayState.currentTextBuffer);
	overlayState.initialized = true;
	StartKeyboardRenderThread();
	if (BaseSystem* system = GetUnsafeBaseSystem()) {
		system->_BlockInputsUntilReleased();
	}

	return vr::VROverlayError_None;
}

/** Placeholder method for submitting a KeyboardDone event when asked to show the keyboard since it is not implemented yet. **/
void BaseOverlay::SubmitPlaceholderKeyboardEvent(vr::EVREventType ev, VRKeyboard::eventDispatch_t eventDispatch, uint64_t userValue)
{
	VREvent_Keyboard_t data = { 0 };
	data.uUserValue = userValue;

	VREvent_t evt = { 0 };
	evt.eventType = ev;
	evt.trackedDeviceIndex = 0;
	evt.data.keyboard = data;

	eventDispatch(evt);
}


EVROverlayError BaseOverlay::ShowKeyboard(EGamepadTextInputMode eInputMode, EGamepadTextInputLineMode eLineInputMode,
    const char* pchDescription, uint32_t unCharMax, const char* pchExistingText, bool bUseMinimalMode, uint64_t uUserValue)
{

	VRKeyboard::eventDispatch_t dispatch = [](VREvent_t ev) {
		BaseSystem* sys = GetUnsafeBaseSystem();
		if (sys) {
			sys->_EnqueueEvent(ev);
		}
	};

	return ShowKeyboardWithDispatch(eInputMode, eLineInputMode, pchDescription, unCharMax, pchExistingText, bUseMinimalMode, uUserValue, dispatch);
}
EVROverlayError BaseOverlay::ShowKeyboard(EGamepadTextInputMode eInputMode, EGamepadTextInputLineMode eLineInputMode, uint32_t unFlags,
    const char* pchDescription, uint32_t unCharMax, const char* pchExistingText, uint64_t uUserValue)
{
	STUBBED();
}
EVROverlayError BaseOverlay::ShowKeyboardForOverlay(VROverlayHandle_t ulOverlayHandle,
    EGamepadTextInputMode eInputMode, EGamepadTextInputLineMode eLineInputMode,
    const char* pchDescription, uint32_t unCharMax, const char* pchExistingText,
    bool bUseMinimalMode, uint64_t uUserValue)
{

	USEH();

	VRKeyboard::eventDispatch_t dispatch = [overlay](VREvent_t ev) {
		std::lock_guard<std::mutex> lock(overlay->eventQueueMutex);
		overlay->eventQueue.push(ev);
	};

	return ShowKeyboardWithDispatch(eInputMode, eLineInputMode, pchDescription, unCharMax, pchExistingText, bUseMinimalMode, uUserValue, dispatch);
}
EVROverlayError BaseOverlay::ShowKeyboardForOverlay(VROverlayHandle_t ulOverlayHandle, EGamepadTextInputMode eInputMode,
    EGamepadTextInputLineMode eLineInputMode, uint32_t unFlags, const char* pchDescription, uint32_t unCharMax,
    const char* pchExistingText, uint64_t uUserValue)
{
	STUBBED();
}

uint32_t BaseOverlay::GetKeyboardText(char* pchText, uint32_t cchText)
{
	std::string snapshot;
	{
		std::lock_guard<std::mutex> bufferLock(overlayState.textBufferMutex);
		snapshot = overlayState.currentTextBuffer;
	}

	const uint32_t requiredLen = static_cast<uint32_t>(snapshot.size() + 1);
	if (!pchText || cchText == 0) {
		return requiredLen;
	}

	const size_t copyCount = std::min(static_cast<size_t>(cchText - 1), snapshot.size());
	memcpy(pchText, snapshot.data(), copyCount);
	pchText[copyCount] = '\0';
	return requiredLen;
}

void BaseOverlay::HideKeyboard()
{
	// First, if the keyboard is currently open, cache it's contents
	if (keyboard) {
		keyboardCache = VRKeyboard::CHAR_CONV.to_bytes(keyboard->contents());
	}

	// Delete the keyboard instance
	keyboard.reset();

	overlayState.stopRenderThread.store(true);
	if (overlayState.renderThread.joinable()) {
		overlayState.renderThread.join();
	}
	HideKeyboardOverlay();

	BaseSystem* system = GetUnsafeBaseSystem();
	if (system) {
		system->_BlockInputsUntilReleased();
	}
}
void BaseOverlay::SetKeyboardTransformAbsolute(ETrackingUniverseOrigin eTrackingOrigin, const HmdMatrix34_t* pmatTrackingOriginToKeyboardTransform)
{
	if (!keyboard)
		OOVR_ABORT("Cannot set keyboard position when the keyboard is closed!");

#ifdef OC_XR_PORT
	XR_STUBBED();
#else
	BaseCompositor* compositor = GetUnsafeBaseCompositor();
	if (compositor && eTrackingOrigin != compositor->GetTrackingSpace()) {
		OOVR_ABORTF("Origin mismatch - current %d, requested %d", compositor->GetTrackingSpace(), eTrackingOrigin);
	}

	keyboard->SetTransform(*pmatTrackingOriginToKeyboardTransform);
#endif
}
void BaseOverlay::SetKeyboardPositionForOverlay(VROverlayHandle_t ulOverlayHandle, HmdRect2_t avoidRect)
{
	STUBBED();
}
EVROverlayError BaseOverlay::SetOverlayIntersectionMask(VROverlayHandle_t ulOverlayHandle, OOVR_VROverlayIntersectionMaskPrimitive_t* pMaskPrimitives, uint32_t unNumMaskPrimitives, uint32_t unPrimitiveSize)
{
	STUBBED();
}
EVROverlayError BaseOverlay::GetOverlayFlags(VROverlayHandle_t ulOverlayHandle, uint32_t* pFlags)
{
	STUBBED();
}
BaseOverlay::VRMessageOverlayResponse BaseOverlay::ShowMessageOverlay(const char* pchText, const char* pchCaption, const char* pchButton0Text, const char* pchButton1Text, const char* pchButton2Text, const char* pchButton3Text)
{
	STUBBED();
}
void BaseOverlay::CloseMessageOverlay()
{
	STUBBED();
}


EVROverlayError BaseOverlay::SetOverlayPreCurvePitch(vr::VROverlayHandle_t ulOverlayHandle, float fRadians)
{
    STUBBED();
}


EVROverlayError BaseOverlay::GetOverlayPreCurvePitch(vr::VROverlayHandle_t ulOverlayHandle, float* pfRadians)
{
    STUBBED();
}

EVROverlayError BaseOverlay::WaitFrameSync(uint32_t nTimeoutMs)
{
    STUBBED();
}

void BaseOverlay::UpdateKeyboardOverlay(const std::string& text) {
    OOVR_LOGF("UpdateKeyboardOverlay called with text: %s", text.c_str());
	EnsureKeyboardLayout();
    
    // Limit text to max characters
	std::string limitedText = text;
    if (overlayState.maxCharacters > 0 && text.length() >= overlayState.maxCharacters) {
        limitedText = text.substr(0, overlayState.maxCharacters - 1);
        OOVR_LOGF("Text truncated from %d to %d characters", (int)text.length(), (int)overlayState.maxCharacters);
    }
    
	{
		std::lock_guard<std::mutex> bufferLock(overlayState.textBufferMutex);
		overlayState.currentTextBuffer = limitedText;
	}
    overlayState.keyboardVisible = true;
	overlayState.renderDirty = true;
    
	overlayState.overlayLayer = CreateKeyboardTextOverlay(xr_session.get(), limitedText.c_str());
}

// Minimal helper to rotate vec by orientation
static XrVector3f RotateVec(const XrQuaternionf& orientation, const XrVector3f& vec)
{
	// Convert XrQuaternionf to float-based for convenience
	float qx = orientation.x, qy = orientation.y, qz = orientation.z, qw = orientation.w;
	float vx = vec.x, vy = vec.y, vz = vec.z;

	// v' = q * v * conj(q)
	// => use standard quaternion-vector rotation formula
	float uvx = 2.0f * (qy * vz - qz * vy);
	float uvy = 2.0f * (qz * vx - qx * vz);
	float uvz = 2.0f * (qx * vy - qy * vx);

	float uuvx = qy * uvz - qz * uvy;
	float uuvy = qz * uvx - qx * uvz;
	float uuvz = qx * uvy - qy * uvx;

	// Combine them
	float rx = vx + qw * uvx + uuvx;
	float ry = vy + qw * uvy + uuvy;
	float rz = vz + qw * uvz + uuvz;

	return { rx, ry, rz };
}

// Offset behind orientation by forwardDist (since forward = -Z in typical XR coords)
static XrVector3f ApplyForwardOffset(const XrQuaternionf& orientation, float forwardDist)
{
	// "Forward" in this coordinate system is negative Z
	XrVector3f localForward = { 0.0f, 0.0f, -1.0f };
	XrVector3f rotated = RotateVec(orientation, localForward);
	return {
		rotated.x * forwardDist,
		rotated.y * forwardDist,
		rotated.z * forwardDist
	};
}

static XrVector3f NormalizeVec(const XrVector3f& v);

static XrQuaternionf YawOnlyOrientation(const XrQuaternionf& q)
{
	XrVector3f forward = RotateVec(q, { 0.0f, 0.0f, -1.0f });
	forward.y = 0.0f;
	forward = NormalizeVec(forward);
	if (std::fabs(forward.x) < 1e-5f && std::fabs(forward.z) < 1e-5f) {
		forward = { 0.0f, 0.0f, -1.0f };
	}

	float yaw = std::atan2(forward.x, -forward.z);
	float half = yaw * 0.5f;
	return { 0.0f, std::sin(half), 0.0f, std::cos(half) };
}

static XrQuaternionf Conjugate(const XrQuaternionf& q)
{
	return { -q.x, -q.y, -q.z, q.w };
}

static float DotVec(const XrVector3f& a, const XrVector3f& b)
{
	return a.x * b.x + a.y * b.y + a.z * b.z;
}

static XrVector3f CrossVec(const XrVector3f& a, const XrVector3f& b)
{
	return {
		a.y * b.z - a.z * b.y,
		a.z * b.x - a.x * b.z,
		a.x * b.y - a.y * b.x
	};
}

static XrQuaternionf NormalizeQuat(const XrQuaternionf& q)
{
	float len = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
	if (len < 1e-6f)
		return { 0.0f, 0.0f, 0.0f, 1.0f };
	return { q.x / len, q.y / len, q.z / len, q.w / len };
}

static XrQuaternionf RotationBetweenVectors(XrVector3f from, XrVector3f to)
{
	from = NormalizeVec(from);
	to = NormalizeVec(to);
	float dot = DotVec(from, to);

	if (dot > 0.9999f) {
		return { 0.0f, 0.0f, 0.0f, 1.0f };
	}

	if (dot < -0.9999f) {
		XrVector3f axis = NormalizeVec(CrossVec({ 1.0f, 0.0f, 0.0f }, from));
		if (std::fabs(axis.x) < 1e-5f && std::fabs(axis.y) < 1e-5f && std::fabs(axis.z) < 1e-5f) {
			axis = NormalizeVec(CrossVec({ 0.0f, 0.0f, 1.0f }, from));
		}
		return { axis.x, axis.y, axis.z, 0.0f };
	}

	XrVector3f axis = CrossVec(from, to);
	XrQuaternionf q = { axis.x, axis.y, axis.z, 1.0f + dot };
	return NormalizeQuat(q);
}

static XrQuaternionf LookYawForward(const XrVector3f& forward)
{
	XrVector3f flatForward = forward;
	flatForward.y = 0.0f;
	flatForward = NormalizeVec(flatForward);
	if (std::fabs(flatForward.x) < 1e-5f && std::fabs(flatForward.z) < 1e-5f) {
		flatForward = { 0.0f, 0.0f, -1.0f };
	}
	return RotationBetweenVectors({ 0.0f, 0.0f, -1.0f }, flatForward);
}

static XrVector3f NormalizeVec(const XrVector3f& v)
{
	float len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
	if (len < 1e-5f)
		return { 0.0f, 0.0f, 0.0f };
	return { v.x / len, v.y / len, v.z / len };
}

static bool RayToOverlayUV(
	const XrCompositionLayerQuad& layer,
	const XrVector3f& origin,
	const XrVector3f& dir,
	float& u,
	float& v,
	XrVector3f* worldHitPoint)
{
	XrQuaternionf invRot = Conjugate(layer.pose.orientation);

	// Transform ray into overlay-local space
	XrVector3f localOrigin = RotateVec(invRot, { origin.x - layer.pose.position.x, origin.y - layer.pose.position.y, origin.z - layer.pose.position.z });
	XrVector3f localDir = RotateVec(invRot, dir);

	if (std::fabs(localDir.z) < 1e-5f)
		return false;

	float t = -localOrigin.z / localDir.z;
	if (t < 0.0f)
		return false;

	XrVector3f hit = {
		localOrigin.x + localDir.x * t,
		localOrigin.y + localDir.y * t,
		localOrigin.z + localDir.z * t
	};

	float halfW = layer.size.width * 0.5f;
	float halfH = layer.size.height * 0.5f;

	if (hit.x < -halfW || hit.x > halfW || hit.y < -halfH || hit.y > halfH)
		return false;

	u = (hit.x / layer.size.width) + 0.5f;
	v = 0.5f - (hit.y / layer.size.height);
	if (worldHitPoint) {
		*worldHitPoint = {
			origin.x + dir.x * t,
			origin.y + dir.y * t,
			origin.z + dir.z * t
		};
	}
	return true;
}

XrCompositionLayerQuad BaseOverlay::CreateKeyboardTextOverlay(XrSession session, const char* text)
{
    OOVR_LOGF("CreateKeyboardTextOverlay: %s", text ? text : "(null)");

	XrCompositionLayerQuad layer{};
    layer.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
    layer.next = nullptr;
    layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    layer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    layer.subImage.swapchain = overlayState.keyboardSwapchain;
    layer.subImage.imageRect = {{0, 0}, {kKeyboardTexWidth, kKeyboardTexHeight}};
    layer.subImage.imageArrayIndex = 0;
    layer.size = {kKeyboardWidthMeters, kKeyboardHeightMeters};

	XrReferenceSpaceType targetSpace = XR_REFERENCE_SPACE_TYPE_LOCAL;
	if (auto* sys = GetUnsafeBaseSystem()) {
		targetSpace = sys->currentSpace;
	}

    // We'll anchor in the current reference space so it doesn't move with the head afterward.
    XrSpace overlaySpace = xr_space_from_ref_space_type(targetSpace);
    layer.space = overlaySpace;

    // Obtain the user's current head pose in local space
    // 1) Create/view space for head
    XrSpace viewSpace = XR_NULL_HANDLE;
    XrReferenceSpaceCreateInfo viewSpaceInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    viewSpaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    viewSpaceInfo.poseInReferenceSpace = {{0,0,0,1}, {0,0,0}};
    XrResult createViewResult = xrCreateReferenceSpace(session, &viewSpaceInfo, &viewSpace);

    // 2) Locate viewSpace relative to localSpace
    if (XR_SUCCEEDED(createViewResult) && viewSpace != XR_NULL_HANDLE) {
        XrSpaceLocation viewInLocal{XR_TYPE_SPACE_LOCATION};
        XrResult locateRes = xrLocateSpace(
            viewSpace,
            overlaySpace,                         // We want head pose in the same space the overlay uses
            xr_gbl->GetBestTime(),
            &viewInLocal
        );

        if (XR_SUCCEEDED(locateRes) && (viewInLocal.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) {
            XrVector3f lookForward = RotateVec(viewInLocal.pose.orientation, { 0.0f, 0.0f, -1.0f });
            lookForward.y = 0.0f;
            lookForward = NormalizeVec(lookForward);
            if (std::fabs(lookForward.x) < 1e-5f && std::fabs(lookForward.z) < 1e-5f) {
                lookForward = { 0.0f, 0.0f, -1.0f };
            }

            // Keep keyboard level but face where the user is looking horizontally.
            layer.pose.orientation = LookYawForward(lookForward);

            // Place in front of current look direction, centered.
            layer.pose.position.x = viewInLocal.pose.position.x + lookForward.x * kKeyboardForwardDistanceMeters;
            layer.pose.position.y = viewInLocal.pose.position.y + kKeyboardVerticalOffsetMeters;
            layer.pose.position.z = viewInLocal.pose.position.z + lookForward.z * kKeyboardForwardDistanceMeters;
        } else {
            // Could not locate user in local space; fallback
            layer.pose.orientation = {0, 0, 0, 1};
            layer.pose.position    = {0.0f, 1.6f, -0.3f};
        }
        xrDestroySpace(viewSpace);
    } else {
        // Fallback if we couldn't create a view space
        layer.pose.orientation = {0, 0, 0, 1};
        layer.pose.position    = {0.0f, 1.6f, -0.3f};
    }

    // Create the swapchain if needed
    if (overlayState.keyboardSwapchain == XR_NULL_HANDLE) {
        CreateKeyboardSwapchain(session, kKeyboardTexWidth, kKeyboardTexHeight, overlayState);
        if (overlayState.keyboardSwapchain != XR_NULL_HANDLE) {
            layer.subImage.swapchain = overlayState.keyboardSwapchain;
        }
    }
	if (overlayState.pointerSwapchain == XR_NULL_HANDLE) {
		CreatePointerSwapchain(session, overlayState);
	}

    return layer;
}

void BaseOverlay::HideKeyboardOverlay() {
    std::lock_guard<std::mutex> lockState(overlayState.stateMutex);
    
    overlayState.keyboardVisible = false;

	overlayState.stopRenderThread.store(true);
	if (overlayState.renderThread.joinable() && overlayState.renderThread.get_id() != std::this_thread::get_id()) {
		overlayState.renderThread.join();
	}

	// Keep swapchains alive across hide/show to avoid runtime/driver stalls on close.
	overlayState.overlayLayer.subImage.swapchain = overlayState.keyboardSwapchain;
	overlayState.pointerVisible[0] = false;
	overlayState.pointerVisible[1] = false;
	if (BaseSystem* system = GetUnsafeBaseSystem()) {
		system->_BlockInputsUntilReleased();
	}
}

bool BaseOverlay::IsKeyboardVisible()
{
	std::lock_guard<std::mutex> lockState(overlayState.stateMutex);
	return overlayState.keyboardVisible;
}

static void EnsureKeyboardLayout()
{
	if (!overlayState.keys.empty())
		return;

	const float keyHeight = 0.14f;
	const float rowMargin = 0.01f;

	auto addCharRow = [&](const std::string& chars, float y) {
		size_t count = chars.size();
		if (count == 0)
			return;

		const float rowWidth = 0.9f;
		float keyWidth = (rowWidth - rowMargin * (count - 1)) / static_cast<float>(count);
		float startX = (1.0f - rowWidth) * 0.5f;

		for (char c : chars) {
			BaseOverlay::KeyboardKey key;
			key.type = BaseOverlay::KeyboardKey::KeyType::Character;
			key.label = std::string(1, (char)std::tolower(c));
			key.shiftedLabel = std::string(1, (char)std::toupper(c));
			key.x = startX;
			key.y = y;
			key.w = keyWidth;
			key.h = keyHeight;

			overlayState.keys.push_back(key);

			startX += keyWidth + rowMargin;
		}
	};

	float rowY = 0.25f;
	addCharRow("QWERTYUIOP", rowY);
	rowY += keyHeight + rowMargin;
	addCharRow("ASDFGHJKL", rowY);
	rowY += keyHeight + rowMargin;
	addCharRow("ZXCVBNM", rowY);

	rowY += keyHeight + rowMargin;
	// Bottom row: Shift | Space | Back | Enter | Close
	float x = 0.04f;
	auto addSpecial = [&](const std::string& label, BaseOverlay::KeyboardKey::KeyType type, float width) {
		BaseOverlay::KeyboardKey key;
		key.type = type;
		key.label = label;
		key.shiftedLabel = label;
		key.x = x;
		key.y = rowY;
		key.w = width;
		key.h = keyHeight;
		overlayState.keys.push_back(key);
		x += width + rowMargin;
	};

	addSpecial("Shift", BaseOverlay::KeyboardKey::KeyType::Shift, 0.10f);
	addSpecial("Space", BaseOverlay::KeyboardKey::KeyType::Space, 0.36f);
	addSpecial("Back", BaseOverlay::KeyboardKey::KeyType::Backspace, 0.14f);
	addSpecial("Enter", BaseOverlay::KeyboardKey::KeyType::Enter, 0.18f);
	addSpecial("Close", BaseOverlay::KeyboardKey::KeyType::Close, 0.10f);
}

static int HitTestKey(int px, int py)
{
	for (size_t i = 0; i < overlayState.keys.size(); ++i) {
		const auto& key = overlayState.keys[i];
		int keyX = static_cast<int>(key.x * kKeyboardTexWidth);
		int keyY = static_cast<int>(key.y * kKeyboardTexHeight);
		int keyW = static_cast<int>(key.w * kKeyboardTexWidth);
		int keyH = static_cast<int>(key.h * kKeyboardTexHeight);

		if (px >= keyX && px <= keyX + keyW && py >= keyY && py <= keyY + keyH)
			return static_cast<int>(i);
	}

	return -1;
}

static std::string DisplayLabelForKey(const BaseOverlay::KeyboardKey& key)
{
	if (key.type == BaseOverlay::KeyboardKey::KeyType::Character) {
		if (overlayState.shiftActive || overlayState.capsLocked) {
			return key.shiftedLabel.empty() ? key.label : key.shiftedLabel;
		}
		return key.label;
	}
	return key.label;
}

static void DispatchKeyboardEvent(vr::EVREventType ev, wchar_t ch)
{
	if (!overlayState.currentEventDispatch)
		return;

	vr::VREvent_Keyboard_t data = {};
	data.uUserValue = overlayState.currentUserValue;
	memset(data.cNewInput, 0, sizeof(data.cNewInput));

	if (ch != 0) {
		std::string utf8 = VRKeyboard::CHAR_CONV.to_bytes(ch);
		if (utf8.length() < sizeof(data.cNewInput)) {
			memcpy(data.cNewInput, utf8.c_str(), utf8.length());
		}
	}

	vr::VREvent_t evt = {};
	evt.eventType = ev;
	evt.trackedDeviceIndex = 0;
	evt.data.keyboard = data;

	overlayState.currentEventDispatch(evt);
}

static void HandleKeyPress(int keyIndex)
{
	if (keyIndex < 0 || keyIndex >= static_cast<int>(overlayState.keys.size()))
		return;

	const auto key = overlayState.keys[keyIndex];
	bool textChanged = false;
	auto canAddMore = [&](size_t len) {
		if (overlayState.maxCharacters == 0)
			return true;
		if (overlayState.maxCharacters <= 1)
			return false;
		return len < overlayState.maxCharacters - 1;
	};

	switch (key.type) {
	case BaseOverlay::KeyboardKey::KeyType::Character: {
		std::string lbl = DisplayLabelForKey(key);
		if (lbl.empty())
			break;
		char ch = lbl[0];
		{
			std::lock_guard<std::mutex> bufferLock(overlayState.textBufferMutex);
			if (canAddMore(overlayState.currentTextBuffer.length())) {
				overlayState.currentTextBuffer.push_back(ch);
				textChanged = true;
			}
		}
		DispatchKeyboardEvent(vr::VREvent_KeyboardCharInput, (wchar_t)ch);
		if (overlayState.shiftActive && !overlayState.capsLocked) {
			overlayState.shiftActive = false;
		}
		break;
	}
	case BaseOverlay::KeyboardKey::KeyType::Space: {
		std::lock_guard<std::mutex> bufferLock(overlayState.textBufferMutex);
		if (canAddMore(overlayState.currentTextBuffer.length())) {
			overlayState.currentTextBuffer.push_back(' ');
			textChanged = true;
			DispatchKeyboardEvent(vr::VREvent_KeyboardCharInput, L' ');
		}
		break;
	}
	case BaseOverlay::KeyboardKey::KeyType::Backspace: {
		std::lock_guard<std::mutex> bufferLock(overlayState.textBufferMutex);
		if (!overlayState.currentTextBuffer.empty()) {
			overlayState.currentTextBuffer.pop_back();
			textChanged = true;
		}
		DispatchKeyboardEvent(vr::VREvent_KeyboardCharInput, 0);
		break;
	}
	case BaseOverlay::KeyboardKey::KeyType::Enter: {
		DispatchKeyboardEvent(vr::VREvent_KeyboardDone, 0);
		overlayState.stopRenderThread.store(true);
		BaseOverlay::HideKeyboardOverlay();
		break;
	}
	case BaseOverlay::KeyboardKey::KeyType::Shift:
		overlayState.shiftActive = !overlayState.shiftActive;
		textChanged = true; // force re-render highlight
		break;
	case BaseOverlay::KeyboardKey::KeyType::Close:
		DispatchKeyboardEvent(vr::VREvent_KeyboardClosed, 0);
		overlayState.stopRenderThread.store(true);
		BaseOverlay::HideKeyboardOverlay();
		break;
	}

	if (textChanged) {
		overlayState.renderDirty = true;
	}
}

static vr::ETrackingUniverseOrigin GetOriginForCurrentSpace()
{
	if (auto* sys = GetUnsafeBaseSystem()) {
		return sys->currentSpace == XR_REFERENCE_SPACE_TYPE_STAGE ? vr::TrackingUniverseStanding : vr::TrackingUniverseSeated;
	}
	return vr::TrackingUniverseSeated;
}

static void PollKeyboardControllers()
{
	BaseSystem* system = GetUnsafeBaseSystem();
	if (!system)
		return;

	EnsureKeyboardLayout();

	{
		std::lock_guard<std::mutex> lockState(overlayState.stateMutex);
		if (!overlayState.keyboardVisible || overlayState.keyboardSwapchain == XR_NULL_HANDLE)
			return;
	}

	vr::ETrackingUniverseOrigin origin = GetOriginForCurrentSpace();
	int hoveredByHand[2] = { -1, -1 };
	XrCompositionLayerQuad overlayLayerCopy{};
	XrSwapchain pointerSwapchainCopy = XR_NULL_HANDLE;
	uint64_t lastButtonStateLocal[2] = { 0, 0 };
	XrCompositionLayerQuad pointerLayersLocal[2] = {};
	bool pointerVisibleLocal[2] = { false, false };
	{
		std::lock_guard<std::mutex> lockState(overlayState.stateMutex);
		overlayLayerCopy = overlayState.overlayLayer;
		pointerSwapchainCopy = overlayState.pointerSwapchain;
		lastButtonStateLocal[0] = overlayState.lastButtonState[0];
		lastButtonStateLocal[1] = overlayState.lastButtonState[1];
	}

	auto processHand = [&](vr::TrackedDeviceIndex_t handIndex, size_t slot) {
		vr::VRControllerState_t state = {};
		vr::TrackedDevicePose_t pose = {};
		BackendManager::Instance().GetSinglePose(origin, handIndex, &pose, ETrackingStateType::TrackingStateType_Now);

		BaseInput* input = GetUnsafeBaseInput();
		if (!input) {
			auto inputShared = GetBaseInput();
			input = inputShared.get();
		}
		if (!input)
			return;
		if (!input->AreActionsLoaded()) {
			input->LoadEmptyManifestIfRequired();
		}
		if (!input->GetLegacyControllerState(handIndex, &state))
			return;

		if (!pose.bPoseIsValid)
			return;

		XrVector3f rayOrigin = {
			pose.mDeviceToAbsoluteTracking.m[0][3],
			pose.mDeviceToAbsoluteTracking.m[1][3],
			pose.mDeviceToAbsoluteTracking.m[2][3]
		};
		XrVector3f axisX = {
			pose.mDeviceToAbsoluteTracking.m[0][0],
			pose.mDeviceToAbsoluteTracking.m[1][0],
			pose.mDeviceToAbsoluteTracking.m[2][0]
		};
		XrVector3f axisY = {
			pose.mDeviceToAbsoluteTracking.m[0][1],
			pose.mDeviceToAbsoluteTracking.m[1][1],
			pose.mDeviceToAbsoluteTracking.m[2][1]
		};
		XrVector3f axisZ = {
			pose.mDeviceToAbsoluteTracking.m[0][2],
			pose.mDeviceToAbsoluteTracking.m[1][2],
			pose.mDeviceToAbsoluteTracking.m[2][2]
		};

		// Use an "aim-like" direction from the front of the controller instead of the top.
		XrVector3f rayDir = NormalizeVec({
			-(axisY.x * 0.85f + axisZ.x * 0.15f),
			-(axisY.y * 0.85f + axisZ.y * 0.15f),
			-(axisY.z * 0.85f + axisZ.z * 0.15f)
		});
		rayOrigin = {
			rayOrigin.x - axisY.x * 0.045f - axisZ.x * 0.010f + axisX.x * 0.004f,
			rayOrigin.y - axisY.y * 0.045f - axisZ.y * 0.010f + axisX.y * 0.004f,
			rayOrigin.z - axisY.z * 0.045f - axisZ.z * 0.010f + axisX.z * 0.004f
		};

		float u = 0.0f, v = 0.0f;
		XrVector3f hitWorld = {};
		if (!RayToOverlayUV(overlayLayerCopy, rayOrigin, rayDir, u, v, &hitWorld)) {
			lastButtonStateLocal[slot] = state.ulButtonPressed;
			pointerVisibleLocal[slot] = false;
			return;
		}

		if (pointerSwapchainCopy != XR_NULL_HANDLE) {
			XrVector3f delta = {
				hitWorld.x - rayOrigin.x,
				hitWorld.y - rayOrigin.y,
				hitWorld.z - rayOrigin.z
			};
			float pointerLength = std::sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
			if (pointerLength > 1e-4f) {
				XrVector3f pointerDir = {
					delta.x / pointerLength,
					delta.y / pointerLength,
					delta.z / pointerLength
				};
				XrCompositionLayerQuad pointerLayer{};
				pointerLayer.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
				pointerLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
				pointerLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
				pointerLayer.space = overlayLayerCopy.space;
				pointerLayer.subImage.swapchain = pointerSwapchainCopy;
				pointerLayer.subImage.imageRect = { { 0, 0 }, { kPointerTexWidth, kPointerTexHeight } };
				pointerLayer.subImage.imageArrayIndex = 0;
				pointerLayer.size = { kPointerWidthMeters, pointerLength };
				pointerLayer.pose.orientation = RotationBetweenVectors({ 0.0f, 1.0f, 0.0f }, pointerDir);
				pointerLayer.pose.position = {
					rayOrigin.x + delta.x * 0.5f,
					rayOrigin.y + delta.y * 0.5f,
					rayOrigin.z + delta.z * 0.5f
				};
				pointerLayersLocal[slot] = pointerLayer;
				pointerVisibleLocal[slot] = true;
			}
		}

		int px = static_cast<int>(u * kKeyboardTexWidth);
		int py = static_cast<int>(v * kKeyboardTexHeight);
		int hitKey = HitTestKey(px, py);
		hoveredByHand[slot] = hitKey;

		bool triggerPressed = (state.ulButtonPressed & vr::ButtonMaskFromId(vr::k_EButton_SteamVR_Trigger)) != 0;
		bool triggerLast = (lastButtonStateLocal[slot] & vr::ButtonMaskFromId(vr::k_EButton_SteamVR_Trigger)) != 0;
		lastButtonStateLocal[slot] = state.ulButtonPressed;

		if (triggerPressed && !triggerLast && hitKey >= 0) {
			HandleKeyPress(hitKey);
		}
	};

	processHand(BaseSystem::leftHandIndex, 0);
	processHand(BaseSystem::rightHandIndex, 1);

	{
		std::lock_guard<std::mutex> lockState(overlayState.stateMutex);
		if (!overlayState.keyboardVisible) {
			overlayState.pointerVisible[0] = false;
			overlayState.pointerVisible[1] = false;
			return;
		}
		overlayState.lastButtonState[0] = lastButtonStateLocal[0];
		overlayState.lastButtonState[1] = lastButtonStateLocal[1];
		overlayState.pointerLayers[0] = pointerLayersLocal[0];
		overlayState.pointerLayers[1] = pointerLayersLocal[1];
		overlayState.pointerVisible[0] = pointerVisibleLocal[0];
		overlayState.pointerVisible[1] = pointerVisibleLocal[1];
		bool hoverChanged = false;
		for (int i = 0; i < 2; ++i) {
			if (overlayState.hoveredKeys[i] != hoveredByHand[i]) {
				overlayState.hoveredKeys[i] = hoveredByHand[i];
				hoverChanged = true;
			}
		}
		if (hoverChanged) {
			overlayState.renderDirty = true;
		}
	}
}

void BaseOverlay::CreateKeyboardSwapchain(XrSession session, int width, int height, OverlayState& state) {
    if (state.keyboardSwapchain != XR_NULL_HANDLE) {
        OOVR_LOG("Swapchain already exists, skipping creation");
        return;
    }

    XrSwapchainCreateInfo swapchainInfo{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
    swapchainInfo.arraySize = 1;
    swapchainInfo.format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    swapchainInfo.width = width;
    swapchainInfo.height = height;
    swapchainInfo.mipCount = 1;
    swapchainInfo.faceCount = 1;
    swapchainInfo.sampleCount = 1;
    swapchainInfo.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;

    XrResult result = xrCreateSwapchain(session, &swapchainInfo, &state.keyboardSwapchain);
    if (XR_FAILED(result)) {
        OOVR_LOG("Failed to create keyboard swapchain");
        return;
    }

    uint32_t imageCount;
    result = xrEnumerateSwapchainImages(state.keyboardSwapchain, 0, &imageCount, nullptr);
    if (XR_FAILED(result)) {
        OOVR_LOG("Failed to get swapchain image count");
        return;
    }

    state.images.resize(imageCount);
    std::vector<XrSwapchainImageD3D11KHR> d3dImages(imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    result = xrEnumerateSwapchainImages(state.keyboardSwapchain, imageCount, &imageCount, 
        (XrSwapchainImageBaseHeader*)d3dImages.data());

    if (XR_FAILED(result)) {
        OOVR_LOG("Failed to enumerate swapchain images");
        return;
    }

    for (size_t i = 0; i < imageCount; i++) {
        state.images[i].texture = d3dImages[i].texture;
    }
}

static void CreatePointerSwapchain(XrSession session, BaseOverlay::OverlayState& state)
{
	if (state.pointerSwapchain != XR_NULL_HANDLE) {
		return;
	}

	XrSwapchainCreateInfo swapchainInfo{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
	swapchainInfo.arraySize = 1;
	swapchainInfo.format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
	swapchainInfo.width = kPointerTexWidth;
	swapchainInfo.height = kPointerTexHeight;
	swapchainInfo.mipCount = 1;
	swapchainInfo.faceCount = 1;
	swapchainInfo.sampleCount = 1;
	swapchainInfo.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;

	XrResult result = xrCreateSwapchain(session, &swapchainInfo, &state.pointerSwapchain);
	if (XR_FAILED(result)) {
		OOVR_LOG("Failed to create pointer swapchain");
		return;
	}

	uint32_t imageCount = 0;
	result = xrEnumerateSwapchainImages(state.pointerSwapchain, 0, &imageCount, nullptr);
	if (XR_FAILED(result)) {
		OOVR_LOG("Failed to get pointer swapchain image count");
		return;
	}

	state.pointerImages.resize(imageCount);
	std::vector<XrSwapchainImageD3D11KHR> d3dImages(imageCount, { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
	result = xrEnumerateSwapchainImages(state.pointerSwapchain, imageCount, &imageCount, (XrSwapchainImageBaseHeader*)d3dImages.data());
	if (XR_FAILED(result)) {
		OOVR_LOG("Failed to enumerate pointer swapchain images");
		return;
	}

	for (size_t i = 0; i < imageCount; ++i) {
		state.pointerImages[i].texture = d3dImages[i].texture;
	}

	state.pointerTextureInitialized = false;
}

void BaseOverlay::RenderKeyboardText() {
    std::lock_guard<std::mutex> lock(SwapchainManager::Instance().GetSwapchainMutex());

    static std::mutex renderMutex;
    std::lock_guard<std::mutex> lockRender(renderMutex);
    std::lock_guard<std::mutex> lockState(overlayState.stateMutex);

    if (overlayState.keyboardSwapchain == XR_NULL_HANDLE) {
        return;
    }

	if (!overlayState.renderDirty)
		return;
	overlayState.renderDirty = false;

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    GetD3D11DeviceAndContext(&device, &context);
    if (!device || !context) {
        OOVR_LOG("Failed to get D3D11 device/context");
        return;
    }

	if (overlayState.pointerSwapchain != XR_NULL_HANDLE && !overlayState.pointerTextureInitialized) {
		uint32_t pointerImageIndex = 0;
		XrSwapchainImageAcquireInfo pointerAcquire{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
		XrResult pointerRes = xrAcquireSwapchainImage(overlayState.pointerSwapchain, &pointerAcquire, &pointerImageIndex);
		if (XR_SUCCEEDED(pointerRes)) {
			XrSwapchainImageWaitInfo pointerWait{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
			pointerWait.timeout = 500000000;
			pointerRes = xrWaitSwapchainImage(overlayState.pointerSwapchain, &pointerWait);
			if (XR_SUCCEEDED(pointerRes)) {
				D3D11_TEXTURE2D_DESC pointerDesc = {};
				pointerDesc.Width = kPointerTexWidth;
				pointerDesc.Height = kPointerTexHeight;
				pointerDesc.MipLevels = 1;
				pointerDesc.ArraySize = 1;
				pointerDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
				pointerDesc.SampleDesc.Count = 1;
				pointerDesc.Usage = D3D11_USAGE_STAGING;
				pointerDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

				Microsoft::WRL::ComPtr<ID3D11Texture2D> pointerStaging;
				if (SUCCEEDED(device->CreateTexture2D(&pointerDesc, nullptr, pointerStaging.GetAddressOf()))) {
					D3D11_MAPPED_SUBRESOURCE pointerMap = {};
					if (SUCCEEDED(context->Map(pointerStaging.Get(), 0, D3D11_MAP_WRITE, 0, &pointerMap))) {
						for (int y = 0; y < kPointerTexHeight; ++y) {
							uint32_t* row = (uint32_t*)((uint8_t*)pointerMap.pData + y * pointerMap.RowPitch);
							for (int x = 0; x < kPointerTexWidth; ++x) {
								row[x] = 0xAA00FFFF;
							}
						}
						context->Unmap(pointerStaging.Get(), 0);

						if (pointerImageIndex < overlayState.pointerImages.size() && overlayState.pointerImages[pointerImageIndex].texture) {
							context->CopyResource(overlayState.pointerImages[pointerImageIndex].texture, pointerStaging.Get());
							overlayState.pointerTextureInitialized = true;
						}
					}
				}
			}

			XrSwapchainImageReleaseInfo pointerRelease{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
			xrReleaseSwapchainImage(overlayState.pointerSwapchain, &pointerRelease);
		}
	}

    uint32_t imageIndex = 0;
    bool releaseNeeded = false;

    XrSwapchainImageAcquireInfo acquireInfo{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    XrResult xrRes = xrAcquireSwapchainImage(overlayState.keyboardSwapchain, &acquireInfo, &imageIndex);
    if (XR_FAILED(xrRes)) {
        OOVR_LOG("Failed to acquire swapchain image");
		overlayState.renderDirty = true;
        return;
    }
    releaseNeeded = true;

    XrSwapchainImageWaitInfo waitInfo{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    waitInfo.timeout = 500000000; // 500ms like DX11Compositor
    xrRes = xrWaitSwapchainImage(overlayState.keyboardSwapchain, &waitInfo);
    if (XR_FAILED(xrRes)) {
        OOVR_LOG("Failed to wait for swapchain image");
        XrSwapchainImageReleaseInfo releaseInfo{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
        xrReleaseSwapchainImage(overlayState.keyboardSwapchain, &releaseInfo);
		overlayState.renderDirty = true;
        return;
    }

    D3D11_TEXTURE2D_DESC stagingDesc = {};
    stagingDesc.Width = kKeyboardTexWidth;
    stagingDesc.Height = kKeyboardTexHeight;
    stagingDesc.MipLevels = 1;
    stagingDesc.ArraySize = 1;
    stagingDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    stagingDesc.SampleDesc.Count = 1;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	stagingDesc.BindFlags = 0;

	Microsoft::WRL::ComPtr<ID3D11Texture2D> stagingTexture;
	HRESULT hr = device->CreateTexture2D(&stagingDesc, nullptr, stagingTexture.GetAddressOf());
	if (FAILED(hr)) {
		OOVR_LOG("Failed to create staging texture");
		overlayState.renderDirty = true;
		if (releaseNeeded) {
			XrSwapchainImageReleaseInfo releaseInfo{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
			xrReleaseSwapchainImage(overlayState.keyboardSwapchain, &releaseInfo);
		}
		return;
	}

    D3D11_MAPPED_SUBRESOURCE mapped;
    hr = context->Map(stagingTexture.Get(), 0, D3D11_MAP_WRITE, 0, &mapped);
    if (SUCCEEDED(hr)) {
        uint32_t* data = (uint32_t*)mapped.pData;

		auto putPixel = [&](int x, int y, uint32_t colour) {
			if (x < 0 || y < 0 || x >= (int)stagingDesc.Width || y >= (int)stagingDesc.Height)
				return;
			data[y * (mapped.RowPitch / 4) + x] = colour;
		};

		auto fillRect = [&](int x, int y, int w, int h, uint32_t colour) {
			for (int iy = 0; iy < h; ++iy) {
				for (int ix = 0; ix < w; ++ix) {
					putPixel(x + ix, y + iy, colour);
				}
			}
		};

		auto drawChar = [&](int x, int y, char c, uint32_t colour) {
			const uint8_t* pattern = CHAR_PATTERNS[(unsigned char)c];
			for (int py = 0; py < 7; ++py) {
				for (int px = 0; px < 5; ++px) {
					if (pattern[py] & (1 << (7 - px))) {
						for (int sy = 0; sy < 6; ++sy) {
							for (int sx = 0; sx < 3; ++sx) {
								putPixel(x + px * 3 + sx, y + py * 6 + sy, colour);
							}
						}
					}
				}
			}
		};

		auto drawString = [&](int x, int y, const std::string& text, uint32_t colour, int maxWidth) {
			int cursorX = x;
			for (char c : text) {
				if (cursorX + 18 > maxWidth)
					break;
				drawChar(cursorX, y, c, colour);
				cursorX += 18;
			}
		};

		// Background
		fillRect(0, 0, stagingDesc.Width, stagingDesc.Height, 0xB0202020);

		// Text area
		const int padding = 16;
		const int textAreaHeight = 96;
		fillRect(padding, padding, stagingDesc.Width - padding * 2, textAreaHeight - padding, 0xFF2E2E2E);

		std::string textCopy;
		{
			std::lock_guard<std::mutex> bufferLock(overlayState.textBufferMutex);
			textCopy = overlayState.currentTextBuffer;
		}

		int maxChars = (stagingDesc.Width - padding * 4) / 18;
		if ((int)textCopy.length() > maxChars) {
			textCopy = textCopy.substr(textCopy.length() - maxChars);
		}

		drawString(padding * 2, padding * 2, textCopy, 0xFFFFFFFF, stagingDesc.Width - padding * 2);

		// Keyboard keys
		for (size_t idx = 0; idx < overlayState.keys.size(); ++idx) {
			const auto& key = overlayState.keys[idx];
			int keyX = static_cast<int>(key.x * stagingDesc.Width);
			int keyY = static_cast<int>(key.y * stagingDesc.Height);
			int keyW = static_cast<int>(key.w * stagingDesc.Width);
			int keyH = static_cast<int>(key.h * stagingDesc.Height);

			bool hovered =
				overlayState.hoveredKeys[0] == static_cast<int>(idx) ||
				overlayState.hoveredKeys[1] == static_cast<int>(idx);
			bool activeShift = key.type == BaseOverlay::KeyboardKey::KeyType::Shift && overlayState.shiftActive;

			uint32_t baseColour = 0xFF3A3A3A;
			if (hovered || activeShift)
				baseColour = 0xFF565656;
			if (key.type == BaseOverlay::KeyboardKey::KeyType::Enter)
				baseColour = hovered ? 0xFF4AA34A : 0xFF3C7C3C;

			fillRect(keyX, keyY, keyW, keyH, baseColour);

			std::string label = DisplayLabelForKey(key);
			int labelWidth = static_cast<int>(label.length()) * 18;
			int labelX = keyX + (keyW - labelWidth) / 2;
			int labelY = keyY + (keyH - 42) / 2;
			drawString(labelX, labelY, label, 0xFFFFFFFF, keyX + keyW);
		}

        context->Unmap(stagingTexture.Get(), 0);

		if (imageIndex >= overlayState.images.size() || !overlayState.images[imageIndex].texture) {
			OOVR_LOG("Invalid swapchain image index while rendering keyboard");
			overlayState.renderDirty = true;
		} else {
			context->CopyResource(overlayState.images[imageIndex].texture, stagingTexture.Get());
		}
    } else {
		overlayState.renderDirty = true;
	}

	if (releaseNeeded) {
		XrSwapchainImageReleaseInfo releaseInfo{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
		XrResult releaseResult = xrReleaseSwapchainImage(overlayState.keyboardSwapchain, &releaseInfo);

		if (XR_FAILED(releaseResult)) {
			OOVR_LOGF("Failed to release swapchain image (result = %d).", releaseResult);
		}
	}
}


void BaseOverlay::GetD3D11DeviceAndContext(ID3D11Device** device, ID3D11DeviceContext** context) {
	*device = nullptr;
	*context = nullptr;

	auto& backend = GetBackend();
	const XrGraphicsBindingD3D11KHR* binding = (const XrGraphicsBindingD3D11KHR*)backend.GetCurrentGraphicsBinding();
	if (!binding || !binding->device)
		return;

	*device = binding->device;
	binding->device->GetImmediateContext(context);
}

void BaseOverlay::StartKeyboardRenderThread()
{
	// Keep function for call-site compatibility, but avoid using a background thread.
	// D3D11 immediate context operations must stay on the main render/submission flow.
	if (overlayState.renderThread.joinable()) {
		overlayState.stopRenderThread.store(true);
		overlayState.renderThread.join();
	}
	overlayState.stopRenderThread.store(true);
}

XrBackend& BaseOverlay::GetBackend() {
    return *(XrBackend*)BackendManager::Instance().GetBackendInstance();
}
