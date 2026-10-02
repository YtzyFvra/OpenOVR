//
// Created by ZNix on 25/10/2020.
//

#include "XrBackend.h"
#include "XrGenericTracker.h"
#include "generated/interfaces/vrtypes.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

#if defined(SUPPORT_GL) && !defined(_WIN32)
#include <GL/glx.h>
#endif

#include <openxr/openxr_platform.h>

#include <cmath>

// On Android, the app has to pass the OpenGLES setup data through
#ifdef ANDROID
#include "../OpenOVR/Misc/android_api.h"
#endif

#include "../OpenOVR/Drivers/Backend.h"
#include "../OpenOVR/Misc/xrmoreutils.h"
#include "../OpenOVR/Reimpl/BaseInput.h"
#include "../OpenOVR/Reimpl/BaseOverlay.h"
#include "../OpenOVR/Reimpl/BaseSystem.h"
#include "../OpenOVR/convert.h"
#include "generated/static_bases.gen.h"

#include "generated/interfaces/IVRCompositor_018.h"

#include "tmp_gfx/TemporaryGraphics.h"

#if defined(SUPPORT_DX) && defined(SUPPORT_DX12)
#include "../OpenOVR/Compositor/dx12compositor.h"
#endif

#if defined(SUPPORT_VK)
#include "tmp_gfx/TemporaryVk.h"
#endif

#if defined(SUPPORT_DX) && defined(SUPPORT_DX11)
#include "tmp_gfx/TemporaryD3D11.h"
#endif

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <glm/gtc/quaternion.hpp>
#include <mutex>
#include <thread>

#if defined(SUPPORT_DX) && defined(SUPPORT_DX12)
#include <d3d12.h>
#include <wrl/client.h>
#endif

extern XrInstance xr_instance;
extern SessionWrapper xr_session;
extern XrSystemId xr_system;
extern XrSessionGlobals* xr_gbl;

using namespace vr;

#if defined(SUPPORT_DX) && defined(SUPPORT_DX12)
using Microsoft::WRL::ComPtr;
#endif

struct XrBackend::PresenterState {
	std::atomic<bool> owns{ false };
	std::atomic<bool> stop{ false };
	std::atomic<bool> armed{ false };
	std::thread thread;

	std::mutex poseMutex;
	vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount]{};
	bool posesValid = false;

	std::mutex l1Mutex;
	std::chrono::steady_clock::time_point l1Next{};
	bool l1HaveNext = false;

	struct MailEye {
#if defined(SUPPORT_DX) && defined(SUPPORT_DX12)
		ComPtr<ID3D12Resource> staging;
		ID3D12CommandQueue* queue = nullptr;
#endif
		vr::VRTextureBounds_t bounds{};
		vr::EVRSubmitFlags flags = vr::Submit_Default;
		vr::HmdMatrix34_t pose{};
		bool poseValid = false;
		bool have = false;
	};
	struct MailSlot {
		MailEye eyes[XruEyeCount];
		uint64_t id = 0;
		uint64_t pair = 0;
		uint64_t seq = 0;
		bool seqLive = false;
		std::chrono::steady_clock::time_point submittedAt{};
		int kind = 0;
		bool complete = false;
		bool DeclaredPose() const { return eyes[0].poseValid && eyes[1].poseValid; }
	};
	static constexpr int kRing = 4;
	std::mutex mailMutex;
	std::condition_variable slotCv;
	std::mutex copyMutex;
	MailSlot ring[kRing]{};
	std::deque<int> ready;
	int write = 0;
	int inflight = -1;
	int nextKind = 0;
	uint64_t nextPair = 0;
	uint64_t nextShow = 0;
	uint64_t nextAssign = 0;
	uint64_t deferredSeq = ~uint64_t{ 0 };
	bool fixedValid = false;
	uint64_t lastShownRealPair = 0;
	bool haveShownReal = false;
	uint64_t nextId = 1;
	uint64_t fillingId = 0;
	std::atomic<uint32_t> N{ 2 };
	std::atomic<bool> pipelineBeats{ false };
	std::atomic<int> queueDepth{ 0 };
	std::atomic<uint32_t> droppedOldest{ 0 };
	std::atomic<uint64_t> engineGate{ 0 };
	std::mutex engineGateMutex;
	std::condition_variable engineGateCv;
	float lastDrawMs = 0.f;
	float lastRdbMs = 0.f;
	// Continuous admission: targetRatio is real frames admitted per display beat (in
	// (0,1]), accumulated into phase each beat by TickEnginePeriod (Bresenham-style), so
	// the achieved rate can land on any fractional average instead of snapping between
	// fixed beat-count tiers. mEma smooths the measured per-real-frame cost; rateBias is
	// the queue-backlog correction. One formula covers every refresh rate: no constant
	// here is tuned to a specific Hz.
	double targetRatio = 0.5;
	double phase = 0.0;
	double mEma = 0.0;
	double rateBias = 0.0;
	std::chrono::steady_clock::time_point lastApplyTime{};
	// See XrBackend::SetTrueSingle.
	std::atomic<bool> trueSingle{ false };
	std::atomic<bool> overFullPending{ false };
	// Pose lead ramps at most one beat per debounce window, decoupled from N so a pace
	// change doesn't also jump how far ahead the engine's pose is sampled (judder/flash).
	std::atomic<uint32_t> poseLeadBeats{ 2 };
	std::chrono::steady_clock::time_point poseLeadChangedAt{};
	std::atomic<bool> bypassInterp{ false };
	bool sawInterp = false;
	bool wantFgN = false;
	bool haveLastReal = false;
	bool quarantineActive = false;
	int lastRealSlot = -1;
	uint32_t dropInterp = 0;
	uint32_t dropReal = 0;
	uint32_t dropFail = 0;
	uint32_t missInterp = 0;
	uint32_t realAtOrUnderP = 0;
	int64_t lastRns = 0;
	std::chrono::steady_clock::time_point lastRealComplete{};
	std::chrono::steady_clock::time_point l1Released{};
	bool haveL1Released = false;
	std::chrono::steady_clock::time_point quarantineUntil{};
	// A real may sit in ready for one beat while its interp is still in FG.
	bool deferReal = false;

#if defined(SUPPORT_DX) && defined(SUPPORT_DX12)
	ComPtr<ID3D12Device> device;
	ComPtr<ID3D12CommandAllocator> alloc;
	ComPtr<ID3D12GraphicsCommandList> list;
	ComPtr<ID3D12Fence> fence;
	HANDLE fenceEvent = nullptr;
	UINT64 fenceValue = 0;
	ComPtr<ID3D12Resource> fixedTex[XruEyeCount];
	ID3D12CommandQueue* fixedQueue[XruEyeCount]{};
	vr::VRTextureBounds_t fixedBounds[XruEyeCount]{};
	vr::EVRSubmitFlags fixedFlags[XruEyeCount]{};
	vr::HmdMatrix34_t fixedPose[XruEyeCount]{};
	bool fixedPoseValid[XruEyeCount]{};
#endif

	std::mutex skyboxMutex;
	XrCompositionLayerQuad skyboxLayer{ XR_TYPE_COMPOSITION_LAYER_QUAD };
	bool skyboxValid = false;
};

#if defined(SUPPORT_DX) && defined(SUPPORT_DX12)
static void MailBarrier(ID3D12GraphicsCommandList* list, ID3D12Resource* res,
    D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
	if (!list || !res || before == after)
		return;
	D3D12_RESOURCE_BARRIER b{};
	b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	b.Transition.pResource = res;
	b.Transition.StateBefore = before;
	b.Transition.StateAfter = after;
	b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	list->ResourceBarrier(1, &b);
}
#endif

std::mutex inputRestartMutex;
std::unique_ptr<TemporaryGraphics> XrBackend::temporaryGraphics = nullptr;

XrBackend::XrBackend(bool useVulkanTmpGfx, bool useD3D11TmpGfx)
{
	memset(projectionViews, 0, sizeof(projectionViews));

	// setup temporaryGraphics

#if defined(SUPPORT_VK)
	if (useVulkanTmpGfx) {
		temporaryGraphics = std::make_unique<TemporaryVk>();
	}
#endif

#if defined(SUPPORT_DX) && defined(SUPPORT_DX11)
	if (!temporaryGraphics && useD3D11TmpGfx) {
		temporaryGraphics = std::make_unique<TemporaryD3D11>();
	}
#endif

	presenter = std::make_unique<PresenterState>();

	OOVR_FALSE_ABORT(temporaryGraphics);

	// setup the device indexes
	for (vr::TrackedDeviceIndex_t i = 0; i < vr::k_unMaxTrackedDeviceCount; i++) {
		std::shared_ptr<ITrackedDevice> dev = GetDevice(i);

		if (dev)
			dev->InitialiseDevice(i);
	}
}

XrBackend::~XrBackend()
{
	PresenterStop();
	// First clear out the compositors, since they might try and access the OpenXR instance
	// in their destructor.
	PrepareForSessionShutdown();

	DrvOpenXR::FullShutdown();

	graphicsBinding = nullptr;

	// This must happen after session destruction (which occurs in FullShutdown), as runtimes (namely Monado)
	// may try to access these resources while destroying the session.
	temporaryGraphics.reset();
}

XrSessionState XrBackend::GetSessionState()
{
	return sessionState;
}

std::shared_ptr<IHMD> XrBackend::GetPrimaryHMD()
{
	return hmd;
}

std::shared_ptr<ITrackedDevice> XrBackend::GetDevice(
    vr::TrackedDeviceIndex_t index)
{
	switch (index) {
	case vr::k_unTrackedDeviceIndex_Hmd:
		return GetPrimaryHMD();
	case 1:
		return hand_left;
	case 2:
		return hand_right;
	default:
		std::shared_lock lock(generic_trackers_mutex);
		vr::TrackedDeviceIndex_t corrected_index = index - RESERVED_DEVICE_INDICES;
		if (corrected_index >= generic_trackers.size())
			return nullptr;

		return generic_trackers.at(corrected_index);
	}
}

std::shared_ptr<ITrackedDevice> XrBackend::GetDeviceByHand(
    ITrackedDevice::TrackedDeviceType hand)
{
	switch (hand) {
	case ITrackedDevice::HAND_LEFT:
		return hand_left;
	case ITrackedDevice::HAND_RIGHT:
		return hand_right;
	default:
		OOVR_SOFT_ABORTF("Cannot get hand by type '%d'", (int)hand);
		return nullptr;
	}
}

void XrBackend::GetDeviceToAbsoluteTrackingPose(
    vr::ETrackingUniverseOrigin toOrigin,
    float predictedSecondsToPhotonsFromNow,
    vr::TrackedDevicePose_t* poseArray,
    uint32_t poseArrayCount)
{
	if (PresenterOwns() && poseArray && poseArrayCount &&
	    CopyPresenterPoses(poseArray, poseArrayCount))
		return;

	for (uint32_t i = 0; i < poseArrayCount; ++i) {
		std::shared_ptr<ITrackedDevice> dev = GetDevice(i);
		if (dev) {
			dev->GetPose(toOrigin, &poseArray[i], ETrackingStateType::TrackingStateType_Rendering);
		} else {
			poseArray[i] = BackendManager::InvalidPose();
		}
	}
}

static void find_queue_family_and_queue_idx(VkDevice dev, VkPhysicalDevice pdev, VkQueue desired_queue, uint32_t& out_queueFamilyIndex, uint32_t& out_queueIndex)
{
	uint32_t queue_family_count;
	vkGetPhysicalDeviceQueueFamilyProperties(pdev, &queue_family_count, NULL);

	std::vector<VkQueueFamilyProperties> hi(queue_family_count);
	vkGetPhysicalDeviceQueueFamilyProperties(pdev, &queue_family_count, hi.data());
	OOVR_LOGF("number of queue families is %d", queue_family_count);

	for (uint32_t i = 0; i < queue_family_count; i++) {
		OOVR_LOGF("queue family %d has %d queues", i, hi[i].queueCount);
		for (uint32_t j = 0; j < hi[i].queueCount; j++) {
			VkQueue tmp;
			vkGetDeviceQueue(dev, i, j, &tmp);
			if (tmp == desired_queue) {
				OOVR_LOGF("Got desired queue: %d %d", i, j);
				out_queueFamilyIndex = i;
				out_queueIndex = j;
				return;
			}
		}
	}
	OOVR_ABORT("Couldn't find the queue family index/queue index of the queue that the OpenVR app gave us!"
	           "This is really odd and really shouldn't ever happen");
}

/* Submitting Frames */
void XrBackend::CheckOrInitCompositors(const vr::Texture_t* tex)
{
	// Check we're using the session with the application's device
	if (!usingApplicationGraphicsAPI) {
		usingApplicationGraphicsAPI = true;

		OOVR_LOG("Recreating OpenXR session for application graphics API");

		// Shutdown old session - apparently Varjo doesn't like the session being destroyed
		// after querying for graphics requirements.
		DrvOpenXR::ShutdownSession();

		switch (tex->eType) {
		case vr::TextureType_DirectX: {
#if defined(SUPPORT_DX) && defined(SUPPORT_DX11)
			// The spec requires that we call this before starting a session using D3D. Unfortunately we
			// can't actually do anything with this information, since the game has already created the device.
			XrGraphicsRequirementsD3D11KHR graphicsRequirements{ XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR };
			OOVR_FAILED_XR_ABORT(xr_ext->xrGetD3D11GraphicsRequirementsKHR(xr_instance, xr_system, &graphicsRequirements));

			auto* d3dTex = (ID3D11Texture2D*)tex->handle;
			ID3D11Device* dev = nullptr;
			d3dTex->GetDevice(&dev);

			XrGraphicsBindingD3D11KHR d3dInfo{};
			d3dInfo.type = XR_TYPE_GRAPHICS_BINDING_D3D11_KHR;
			d3dInfo.device = dev;
			graphicsBinding = std::make_unique<BindingWrapper<XrGraphicsBindingD3D11KHR>>(d3dInfo);
			DrvOpenXR::SetupSession();

			dev->Release();
#else

 #ifdef __linux__
            OOVR_ABORT("Application is trying to submit a D3D11 texture, this is likely a Steam Proton bug, please report it");
 #else
            OOVR_ABORT("Application is trying to submit a D3D11 texture, which OpenComposite supports but is disabled in this build");
 #endif

#endif
			break;
		}
		case vr::TextureType_DirectX12: {
#if defined(SUPPORT_DX) && defined(SUPPORT_DX12)
			// The spec requires that we call this before starting a session using D3D. Unfortunately we
			// can't actually do anything with this information, since the game has already created the device.
			XrGraphicsRequirementsD3D12KHR graphicsRequirements{ XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR };
			OOVR_FAILED_XR_ABORT(xr_ext->xrGetD3D12GraphicsRequirementsKHR(xr_instance, xr_system, &graphicsRequirements));

			D3D12TextureData_t* d3dTexData = (D3D12TextureData_t*)tex->handle;
			ComPtr<ID3D12Device> device;
			d3dTexData->m_pResource->GetDevice(IID_PPV_ARGS(&device));

			XrGraphicsBindingD3D12KHR d3dInfo{};
			d3dInfo.type = XR_TYPE_GRAPHICS_BINDING_D3D12_KHR;
			d3dInfo.device = device.Get();
			d3dInfo.queue = d3dTexData->m_pCommandQueue;
			graphicsBinding = std::make_unique<BindingWrapper<XrGraphicsBindingD3D12KHR>>(d3dInfo);
			DrvOpenXR::SetupSession();

#ifdef _DEBUG
			ComPtr<ID3D12Debug> debugController;
			D3D12GetDebugInterface(IID_PPV_ARGS(&debugController));
			debugController->EnableDebugLayer();
#endif

			device->Release();
#else

 #ifdef __linux__
           OOVR_ABORT("Application is trying to submit a D3D12 texture, this is likely a Steam Proton bug, please report it");
 #else
           OOVR_ABORT("Application is trying to submit a D3D12 texture, which OpenComposite supports but is disabled in this build");
 #endif

#endif
			break;
		}
		case vr::TextureType_Vulkan: {
			const vr::VRVulkanTextureData_t* vktex = (vr::VRVulkanTextureData_t*)tex->handle;

			VkPhysicalDevice xr_desire;
			// Regardless of error checking, we have to call this or we get crazy validation errors.
			xr_ext->xrGetVulkanGraphicsDeviceKHR(xr_instance, xr_system, vktex->m_pInstance, &xr_desire);

			if (xr_desire != vktex->m_pPhysicalDevice) {
				OOVR_ABORTF("The VkPhysicalDevice that the OpenVR app (%p) used is different from the one that the OpenXR runtime used (%p)!\n"
				            "This should never happen, except for on multi-gpu, in which case DRI_PRIME=1 should fix things on Linux.",
				    (void*)vktex->m_pPhysicalDevice, (void*)xr_desire);
			}

			XrGraphicsBindingVulkanKHR binding;
			binding.type = XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR;
			binding.next = nullptr;
			binding.instance = vktex->m_pInstance;
			binding.physicalDevice = vktex->m_pPhysicalDevice;
			binding.device = vktex->m_pDevice;

			find_queue_family_and_queue_idx( //
			    binding.device, //
			    binding.physicalDevice, //
			    vktex->m_pQueue, //
			    binding.queueFamilyIndex, //
			    binding.queueIndex //
			);

			graphicsBinding = std::make_unique<BindingWrapper<XrGraphicsBindingVulkanKHR>>(binding);
			DrvOpenXR::SetupSession();
			break;
		}
		case vr::TextureType_OpenGL: {
#ifdef SUPPORT_GL
			// The spec requires that we call this before starting a session using OpenGL. Unfortunately we
			// can't actually do anything with this information, since the game has already created the context.
			XrGraphicsRequirementsOpenGLKHR graphicsRequirements{ XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR };
			OOVR_FAILED_XR_ABORT(xr_ext->xrGetOpenGLGraphicsRequirementsKHR(xr_instance, xr_system, &graphicsRequirements));

			// Platform-specific OpenGL context stuff:
#ifdef _WIN32
			XrGraphicsBindingOpenGLWin32KHR binding = { XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR };
			binding.hGLRC = wglGetCurrentContext();
			binding.hDC = wglGetCurrentDC();

			if (!binding.hGLRC || !binding.hDC) {
				OOVR_ABORTF("Null OpenGL GLRC or DC: %p,%p", (void*)binding.hGLRC, (void*)binding.hDC);
			}

			graphicsBinding = std::make_unique<BindingWrapper<XrGraphicsBindingOpenGLWin32KHR>>(binding);
			DrvOpenXR::SetupSession();
#else
			// Only support xlib for now (same as Monado)
			// TODO wayland
			// TODO xcb

			// HACK: Create a separate GLX context since `MaybeRestartForInputs()` may attempt to bind it on a different thread while the app's context is still in use
			const PFNGLXCREATECONTEXTATTRIBSARBPROC pfn_glXCreateContextAttribsARB = (PFNGLXCREATECONTEXTATTRIBSARBPROC)glXGetProcAddress((const GLubyte*)"glXCreateContextAttribsARB");
			if (pfn_glXCreateContextAttribsARB == nullptr)
				OOVR_ABORT("glXCreateContextAttribsARB not available");

			XrGraphicsBindingOpenGLXlibKHR binding = { XR_TYPE_GRAPHICS_BINDING_OPENGL_XLIB_KHR };
			binding.xDisplay = glXGetCurrentDisplay();
			int configCount = 0;
			// clang-format off
			const int configAttribs[] = {
				GLX_CONFIG_CAVEAT, GLX_NONE,
				None,
			};
			// clang-format on
			GLXFBConfig* const configs = glXChooseFBConfig(binding.xDisplay, DefaultScreen(binding.xDisplay), configAttribs, &configCount);
			if (configs == nullptr || configCount < 1)
				OOVR_ABORT("No usable GLXFBConfig found");
			binding.glxFBConfig = configs[0];
			XFree(configs);
			binding.visualid = glXGetVisualFromFBConfig(binding.xDisplay, binding.glxFBConfig)->visualid;
			binding.glxDrawable = glXGetCurrentDrawable();
			// clang-format off
			const int contextAttribs[] = {
				GLX_CONTEXT_MAJOR_VERSION_ARB, 3,
				GLX_CONTEXT_MINOR_VERSION_ARB, 2,
				None,
			};
			// clang-format on
			binding.glxContext = pfn_glXCreateContextAttribsARB(binding.xDisplay, binding.glxFBConfig, glXGetCurrentContext(), true, contextAttribs);

			graphicsBinding = std::make_unique<BindingWrapper<XrGraphicsBindingOpenGLXlibKHR>>(binding);
			DrvOpenXR::SetupSession();
#endif
			// End of platform-specific code

#elif defined(SUPPORT_GLES)
			// The spec requires that we call this before starting a session using OpenGL. We could actually handle this properly
			// on android since the app has to be modified to work with us, but for now don't bother.
			XrGraphicsRequirementsOpenGLESKHR graphicsRequirements{ XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR };
			OOVR_FAILED_XR_ABORT(xr_ext->xrGetOpenGLESGraphicsRequirementsKHR(xr_instance, xr_system, &graphicsRequirements));

			if (!OpenComposite_Android_GLES_Binding_Info)
				OOVR_ABORT("App is trying to use GLES, but OpenComposite_Android_GLES_Binding_Info global is not set.\n"
				           "Please ensure this is set by the application.");

			XrGraphicsBindingOpenGLESAndroidKHR binding = *OpenComposite_Android_GLES_Binding_Info;
			OOVR_FALSE_ABORT(binding.type == XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR);
			binding.next = nullptr;

			graphicsBinding = std::make_unique<BindingWrapper<XrGraphicsBindingOpenGLESAndroidKHR>>(binding);
			DrvOpenXR::SetupSession();

#else
			OOVR_ABORT("Application is trying to submit an OpenGL texture, which OpenComposite supports but is disabled in this build");
#endif
			break;
		}
		default:
			OOVR_ABORTF("Invalid/unknown texture type %d", tex->eType);
		}

		// Real graphics binding should be setup now - get rid of temporary graphics
		temporaryGraphics.reset();
	}

	for (std::unique_ptr<Compositor>& compositor : compositors) {
		// Skip a compositor if it's already set up
		if (compositor)
			continue;

		compositor = BaseCompositor::CreateCompositorAPI(tex);
	}
}

bool XrBackend::PresenterOwns()
{
	return presenter && presenter->owns.load(std::memory_order_acquire);
}

bool XrBackend::CopyPresenterPoses(vr::TrackedDevicePose_t* out, uint32_t count)
{
	if (!out || !presenter)
		return false;
	std::lock_guard lock(presenter->poseMutex);
	if (!presenter->posesValid)
		return false;
	const uint32_t n = (std::min)(count, vr::k_unMaxTrackedDeviceCount);
	memcpy(out, presenter->poses, n * sizeof(vr::TrackedDevicePose_t));
	return true;
}

void XrBackend::PresenterStop()
{
	if (!presenter)
		return;
	presenter->stop.store(true, std::memory_order_release);
	presenter->slotCv.notify_all();
	if (presenter->thread.joinable())
		presenter->thread.join();
	presenter->owns.store(false, std::memory_order_release);
	presenter->armed.store(false, std::memory_order_release);
	presenter->stop.store(false, std::memory_order_release);
#if defined(SUPPORT_DX) && defined(SUPPORT_DX12)
	if (presenter->fenceEvent) {
		CloseHandle(presenter->fenceEvent);
		presenter->fenceEvent = nullptr;
	}
#endif
}

void XrBackend::PresenterTryArm()
{
	if (!presenter || !usingApplicationGraphicsAPI || !sessionActive)
		return;
	if (presenter->armed.exchange(true, std::memory_order_acq_rel))
		return;
	presenter->stop.store(false, std::memory_order_release);
	presenter->owns.store(true, std::memory_order_release);
	OOVR_LOG("[OC] L3 presenter owns runtime");
	presenter->thread = std::thread([this] {
		try {
			PresenterLoop();
		} catch (const std::exception& e) {
			OOVR_LOGF("[OC] L3 presenter exception: %s", e.what());
		} catch (...) {
			OOVR_LOG("[OC] L3 presenter unknown exception");
		}
	});
}

void XrBackend::SetNextEnqueueKind(int kind)
{
	if (!presenter)
		return;
	std::lock_guard lock(presenter->mailMutex);
	presenter->nextKind = kind;
}

void XrBackend::ApplyPace()
{
	if (!presenter)
		return;
	std::lock_guard lock(presenter->l1Mutex);
	double periodMs = lastPredictedPeriodNs > 0 ? static_cast<double>(lastPredictedPeriodNs) / 1.0e6 : 11.111;
	if (periodMs < 0.05)
		periodMs = 11.111;
	const auto now = std::chrono::steady_clock::now();

	// True Single: the engine already blocks its own Submit per frame on RDB's real
	// completion (CS side), so this gate's own job (don't let the engine outrun RDB) is
	// already done elsewhere. Pin to native rate instead of computing N from T_rdb, or the
	// two throttles stack and roughly double the effective frame time.
	if (presenter->trueSingle.load(std::memory_order_acquire)) {
		presenter->targetRatio = 1.0;
		const uint32_t prev = presenter->N.exchange(1u, std::memory_order_acq_rel);
		presenter->pipelineBeats.store(true, std::memory_order_release);
		presenter->poseLeadBeats.store(1u, std::memory_order_release);
		presenter->poseLeadChangedAt = now;
		if (prev != 1u)
			OOVR_LOG("[OC] L1 True Single: pinned to native rate, N-based throttle bypassed");
		return;
	}
	// ApplyPace runs once per display beat (90-120/s), far faster than the control
	// bandwidth either adaptation below needs. Every step is scaled by real elapsed ms, not
	// by call count, so the settling speed is the same at every refresh rate and doesn't
	// turn into a fast underdamped feedback loop just because beats arrive quickly.
	double dtMs = presenter->lastApplyTime.time_since_epoch().count() != 0 ?
		std::chrono::duration<double, std::milli>(now - presenter->lastApplyTime).count() :
		periodMs;
	dtMs = std::clamp(dtMs, 0.5, 100.0);
	presenter->lastApplyTime = now;

	// M: this doorbell frame's real per-frame cost (draw+enhance+generate), GPU-only
	// (T_rdb excludes mailbox/fence wait). Smoothed with a time-constant EMA rather than a
	// discrete tier: slower to believe "got worse" than "got better", so one bad frame
	// doesn't cut the output rate outright and a recovery isn't held back by a debounce.
	const double trdb = presenter->lastRdbMs > 0.f ? static_cast<double>(presenter->lastRdbMs) : 2.0 * periodMs;
	constexpr double kMTauUpMs = 300.0;
	constexpr double kMTauDownMs = 100.0;
	double mEma = presenter->mEma > 0.0 ? presenter->mEma : trdb;
	const double mAlpha = std::clamp(dtMs / (trdb > mEma ? kMTauUpMs : kMTauDownMs), 0.0, 1.0);
	mEma += (trdb - mEma) * mAlpha;
	presenter->mEma = mEma;

	// Never pace the engine below this fps on purpose. Expressing the floor as an fps
	// (not a fixed ms or beat count) is what makes targetRatio below correct at every
	// refresh rate instead of only the one it happened to be tuned at.
	constexpr double kMinRealFps = 30.0;
	const double worstPeriodMs = 1000.0 / kMinRealFps;
	const double mClamped = std::clamp(mEma, periodMs, worstPeriodMs);
	double targetRatio = periodMs / mClamped;

	// Queue backlog biases the ratio down smoothly, and recovers just as smoothly, instead
	// of a discrete step: a full -0.5..0 swing takes ~2 real seconds, so a persistent
	// backlog moves it but a single blip (or one beat's worth of queue-depth noise) can't.
	const int q = presenter->queueDepth.load(std::memory_order_acquire);
	const bool overFull = presenter->overFullPending.exchange(false, std::memory_order_acq_rel);
	constexpr double kBiasSlewPerSec = 0.25;
	const double maxBiasStep = kBiasSlewPerSec * (dtMs / 1000.0);
	if (overFull || q > 2)
		presenter->rateBias = (std::max)(presenter->rateBias - maxBiasStep, -0.5);
	else if (q <= 1)
		presenter->rateBias = (std::min)(presenter->rateBias + maxBiasStep, 0.0);
	targetRatio = std::clamp(targetRatio * (1.0 + presenter->rateBias), periodMs / (worstPeriodMs * 2.0), 1.0);
	presenter->targetRatio = targetRatio;

	// N is no longer the admission test (TickEnginePeriod's phase accumulator is); this is
	// only the characteristic beat count for logging and the cold-start seed in
	// NoteRealEnqueued/L1Throttle, which stop writing it once pipelineBeats latches below.
	const uint32_t beats = std::clamp(static_cast<uint32_t>(std::ceil(1.0 / targetRatio)), 1u, 8u);
	const uint32_t prev = presenter->N.exchange(beats, std::memory_order_acq_rel);
	presenter->pipelineBeats.store(true, std::memory_order_release);

	// Pose lead tracks that characteristic beat count but ramps at most one beat per
	// debounce window: a rate change must not also jump how far ahead the pose is sampled
	// (that jump is the judder/flash a discrete N change used to cause).
	constexpr auto kLeadDebounce = std::chrono::milliseconds(500);
	uint32_t lead = presenter->poseLeadBeats.load(std::memory_order_acquire);
	if (lead != beats && now - presenter->poseLeadChangedAt >= kLeadDebounce) {
		lead += (lead < beats) ? 1 : -1;
		presenter->poseLeadBeats.store(lead, std::memory_order_release);
		presenter->poseLeadChangedAt = now;
	}

	if (prev != beats)
		OOVR_LOGF("[OC] L1 ratio=%.3f (%.1f fps) Q=%d bias=%.2f M=%.1f period=%.2f",
			targetRatio, targetRatio * 1000.0 / periodMs, q, presenter->rateBias, mEma, periodMs);
}

void XrBackend::SetTrueSingle(bool enabled)
{
	if (!presenter)
		return;
	presenter->trueSingle.store(enabled, std::memory_order_release);
}

void XrBackend::SetPipelineMs(float drawMs, float rdbMs)
{
	if (!presenter)
		return;
	std::lock_guard lock(presenter->l1Mutex);
	if (drawMs > 0.f)
		presenter->lastDrawMs = drawMs;
	if (rdbMs > 0.f)
		presenter->lastRdbMs = rdbMs;
}

void XrBackend::NoteProduced()
{
	if (!presenter)
		return;
	int depth = presenter->queueDepth.load(std::memory_order_relaxed);
	if (depth >= 2) {
		const uint32_t n = presenter->droppedOldest.fetch_add(1, std::memory_order_relaxed) + 1;
		if (n <= 8 || (n % 120) == 0)
			OOVR_LOGF("[OC] L2 drop oldest n=%u Q=%d", n, depth);
		presenter->overFullPending.store(true, std::memory_order_release);
		return;
	}
	presenter->queueDepth.fetch_add(1, std::memory_order_acq_rel);
}

void XrBackend::NoteStarted()
{
	if (!presenter)
		return;
	int depth = presenter->queueDepth.load(std::memory_order_relaxed);
	if (depth > 0)
		presenter->queueDepth.fetch_sub(1, std::memory_order_acq_rel);
}

void XrBackend::WaitEngineBeat()
{
	if (!presenter)
		return;
	static thread_local uint64_t seen = 0;
	uint64_t gate = presenter->engineGate.load(std::memory_order_acquire);
	if (gate == seen) {
		std::unique_lock lock(presenter->engineGateMutex);
		presenter->engineGateCv.wait(lock, [&] {
			return presenter->engineGate.load(std::memory_order_acquire) != seen ||
				presenter->stop.load(std::memory_order_acquire);
		});
		gate = presenter->engineGate.load(std::memory_order_acquire);
	}
	seen = gate;
	presenter->l1Released = std::chrono::steady_clock::now();
	presenter->haveL1Released = true;
}

void XrBackend::TickEnginePeriod()
{
	if (!presenter)
		return;
	// targetRatio is recomputed once per real presenter beat, not once per
	// producer/consumer event.
	ApplyPace();
	// Bresenham-style admission: accumulate targetRatio (real frames per beat) each beat
	// and admit once it crosses 1.0, carrying the remainder. This is what lets the achieved
	// rate land on any fractional average (e.g. 35 fps) instead of only exact beat-count
	// divisors of the display's refresh rate.
	bool admit = false;
	{
		std::lock_guard lock(presenter->l1Mutex);
		presenter->phase += presenter->targetRatio;
		if (presenter->phase >= 1.0) {
			presenter->phase -= 1.0;
			admit = true;
		}
	}
	if (!admit)
		return;
	std::lock_guard lock(presenter->engineGateMutex);
	presenter->engineGate.fetch_add(1, std::memory_order_acq_rel);
	presenter->engineGateCv.notify_all();
}

void XrBackend::SetNextEnqueuePair(uint64_t pair)
{
	if (!presenter)
		return;
	std::lock_guard lock(presenter->mailMutex);
	presenter->nextPair = pair;
}

int XrBackend::AllocMailSlot()
{
	bool used[PresenterState::kRing]{};
	if (presenter->inflight >= 0 && presenter->inflight < PresenterState::kRing)
		used[presenter->inflight] = true;
	if (presenter->lastRealSlot >= 0 && presenter->lastRealSlot < PresenterState::kRing)
		used[presenter->lastRealSlot] = true;
	if (presenter->fillingId && presenter->write >= 0 && presenter->write < PresenterState::kRing)
		used[presenter->write] = true;
	for (int q : presenter->ready) {
		if (q >= 0 && q < PresenterState::kRing)
			used[q] = true;
	}
	for (int i = 0; i < PresenterState::kRing; ++i) {
		if (!used[i])
			return i;
	}
	// PresenterLoop only plays an interp once its own real is already queued behind it
	// (same pair match, see the ready.front() check there); an interp that's already
	// matched is correct and about to play next beat even though it's the oldest ready
	// entry. Evicting it anyway is what turned "two pairs needing slots within one
	// presenter beat" into a dropped interp that was never stale.
	for (auto it = presenter->ready.begin(); it != presenter->ready.end(); ++it) {
		if (presenter->ring[*it].kind != 1)
			continue;
		bool matched = false;
		for (int slot : presenter->ready) {
			if (slot == *it)
				continue;
			if (presenter->ring[slot].kind == 0 && presenter->ring[slot].pair == presenter->ring[*it].pair) {
				matched = true;
				break;
			}
		}
		if (matched)
			continue;
		const int drop = *it;
		presenter->ready.erase(it);
		++presenter->dropInterp;
		return drop;
	}
	// lastRealSlot is not in ready: Presenter still repeats it.
	// Never overwrite inflight or lastReal. Caller must abort on -1.
	for (auto it = presenter->ready.begin(); it != presenter->ready.end(); ++it) {
		if (*it != presenter->lastRealSlot) {
			const int drop = *it;
			if (presenter->ring[drop].kind == 1)
				++presenter->dropInterp;
			else
				++presenter->dropReal;
			presenter->ready.erase(it);
			return drop;
		}
	}
	++presenter->dropFail;
	return -1;
}

void XrBackend::RefreshQuarantine()
{
	if (!presenter || !presenter->quarantineActive)
		return;
	if (std::chrono::steady_clock::now() < presenter->quarantineUntil)
		return;
	presenter->quarantineActive = false;
	presenter->bypassInterp.store(false, std::memory_order_release);
	presenter->missInterp = 0;
	OOVR_LOG("[OC] L2 quarantine ended");
}

void XrBackend::EnterQuarantine(const char* reason)
{
	if (!presenter)
		return;
	presenter->quarantineUntil = std::chrono::steady_clock::now() + std::chrono::seconds(1);
	presenter->quarantineActive = true;
	presenter->bypassInterp.store(true, std::memory_order_release);
	++quarantineEnterCount;
	OOVR_LOGF("[OC] L2 quarantine 1s reason=%s n=%u", reason ? reason : "?", quarantineEnterCount);
}

void XrBackend::RescheduleAfterDiscard()
{
	if (!presenter)
		return;
	{
		std::lock_guard lock(presenter->mailMutex);
		presenter->ready.clear();
		if (presenter->lastRealSlot >= 0)
			presenter->inflight = presenter->lastRealSlot;
		EnterQuarantine("discarded");
	}
	std::lock_guard l1(presenter->l1Mutex);
	presenter->l1HaveNext = false;
}

bool XrBackend::InterpAllowed()
{
	if (!presenter)
		return true;
	std::lock_guard lock(presenter->mailMutex);
	RefreshQuarantine();
	return !presenter->bypassInterp.load(std::memory_order_acquire);
}

void XrBackend::NoteRealEnqueued()
{
	const auto now = std::chrono::steady_clock::now();
	RefreshQuarantine();
	// The real is submitted before its interp. sawInterp is the interp that
	// arrived after the previous real, so this enqueue is not itself a miss.
	const bool bypass = presenter->bypassInterp.load(std::memory_order_acquire);
	const bool expectInterp = presenter->wantFgN;
	// N itself is measured at the next Wait (engine work only). Enqueue
	// wall time includes doorbell/copy and must not raise N.
	uint32_t n = presenter->N.load(std::memory_order_relaxed);
	if (n < 1)
		n = 1;
	if (presenter->sawInterp && !bypass) {
		n = (std::max)(n, 2u);
		presenter->wantFgN = true;
	} else {
		presenter->wantFgN = false;
	}
	if (n > PresenterState::kRing)
		n = PresenterState::kRing;
	if (!presenter->pipelineBeats.load(std::memory_order_acquire))
		presenter->N.store(n, std::memory_order_release);
	if (presenter->sawInterp)
		presenter->missInterp = 0;
	else if (!bypass && expectInterp) {
		++presenter->missInterp;
		if (presenter->missInterp >= 3)
			EnterQuarantine("interp_miss");
	}
	presenter->lastRealComplete = now;
	presenter->haveLastReal = true;
	presenter->sawInterp = false;
}

void XrBackend::L1Throttle()
{
	if (!presenter)
		return;
	const auto arrive = std::chrono::steady_clock::now();
	// Engine produce time: previous Wait return -> this Wait call.
	// Do not include this sleep or later doorbell/enqueue.
	if (presenter->haveL1Released && lastPredictedPeriodNs > 0) {
		const int64_t rNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
		    arrive - presenter->l1Released)
		                        .count();
		presenter->lastRns = rNs;
		uint32_t measured = static_cast<uint32_t>((rNs + lastPredictedPeriodNs - 1) / lastPredictedPeriodNs);
		if (rNs > 0 && rNs <= lastPredictedPeriodNs)
			++presenter->realAtOrUnderP;
		else
			presenter->realAtOrUnderP = 0;
		if (measured < 1)
			measured = 1;
		if (measured > PresenterState::kRing)
			measured = PresenterState::kRing;
		const bool bypass = presenter->bypassInterp.load(std::memory_order_acquire);
		if (presenter->wantFgN && !bypass)
			measured = (std::max)(measured, 2u);
		if (!presenter->pipelineBeats.load(std::memory_order_acquire))
			presenter->N.store(measured, std::memory_order_release);
	}
	// Period is N display beats, released from the presenter. Do not sleep here.
}

void XrBackend::PublishPoseSnapshot()
{
	if (!presenter)
		return;
	vr::TrackedDevicePose_t tmp[vr::k_unMaxTrackedDeviceCount]{};
	const vr::ETrackingUniverseOrigin origin = GetUnsafeBaseSystem()
	    ? (GetUnsafeBaseSystem()->currentSpace == XR_REFERENCE_SPACE_TYPE_LOCAL
	              ? vr::TrackingUniverseSeated
	              : vr::TrackingUniverseStanding)
	    : vr::TrackingUniverseStanding;
	for (uint32_t i = 0; i < vr::k_unMaxTrackedDeviceCount; ++i) {
		// HMD must live-locate. GetPose reads this latch when owns, so
		// writing via GetSinglePose would freeze the head after frame 1.
		if (i == vr::k_unTrackedDeviceIndex_Hmd && xr_gbl)
			xr_utils::PoseFromSpace(&tmp[i], xr_gbl->viewSpace, origin);
		else if (GetDevice(i))
			BackendManager::Instance().GetSinglePose(origin, i, &tmp[i], TrackingStateType_Rendering);
	}
	std::lock_guard lock(presenter->poseMutex);
	memcpy(presenter->poses, tmp, sizeof(tmp));
	presenter->posesValid = true;
}

void XrBackend::ApplyMailboxPoses(const vr::HmdMatrix34_t pose[XruEyeCount], const bool valid[XruEyeCount], bool force)
{
	// force only skips the "new frame" miss counter. It must not write a
	// leftover or zero matrix (quat_cast -> NaN).
	for (int i = 0; i < XruEyeCount; ++i) {
		if (!valid[i]) {
			if (!force) {
				++missingTwpCount;
				if (missingTwpCount <= 8 || (missingTwpCount % 120) == 0)
					OOVR_LOGF("[OC] M3 TwP missing eye=%d n=%u", i, missingTwpCount);
			}
			continue;
		}
		// TwP pose is HMD (head). Eye offset is VIEW-space, same GetBestTime
		// cache the engine used to render. Do not slerp per eye.
		const glm::mat4 worldFromEye = S2G_m34(pose[i]) * S2G_m34(hmd->GetEyeToHeadTransform(static_cast<vr::EVREye>(i)));
		projectionViews[i].pose.position = G2X_v3f(glm::vec3(worldFromEye[3]));
		projectionViews[i].pose.orientation = G2X_quat(glm::normalize(glm::quat_cast(worldFromEye)));
	}
}

void XrBackend::EnqueueEye(vr::EVREye eye, const vr::Texture_t* texture, const vr::VRTextureBounds_t* bounds,
    vr::EVRSubmitFlags submitFlags)
{
	if (!presenter || !texture)
		return;
	const int ei = (eye == vr::Eye_Right) ? 1 : 0;
#if defined(SUPPORT_DX) && defined(SUPPORT_DX12)
	ID3D12Resource* srcRes = nullptr;
	ID3D12CommandQueue* srcQueue = nullptr;
	ID3D12Resource* staging = nullptr;
	bool needStaging = false;
	D3D12_RESOURCE_DESC stagingDesc{};
#endif
	int slot = -1;
	{
		std::lock_guard lock(presenter->mailMutex);
		slot = presenter->write;
		if (!presenter->fillingId) {
			slot = AllocMailSlot();
			if (slot < 0)
				return;
			presenter->write = slot;
			presenter->ring[slot].eyes[0].have = false;
			presenter->ring[slot].eyes[1].have = false;
			presenter->ring[slot].eyes[0].poseValid = false;
			presenter->ring[slot].eyes[1].poseValid = false;
			presenter->ring[slot].eyes[0].pose = {};
			presenter->ring[slot].eyes[1].pose = {};
			presenter->ring[slot].complete = false;
			presenter->ring[slot].kind = 0;
		}
		auto& dest = presenter->ring[slot].eyes[ei];
		if (bounds)
			dest.bounds = *bounds;
		else
			dest.bounds = { 0.f, 0.f, 1.f, 1.f };
		dest.flags = submitFlags;
		if (submitFlags & vr::Submit_TextureWithPose) {
			const auto* withPose = reinterpret_cast<const vr::VRTextureWithPose_t*>(texture);
			dest.pose = withPose->mDeviceToAbsoluteTracking;
			dest.poseValid = true;
		} else {
			dest.poseValid = false;
		}
#if defined(SUPPORT_DX) && defined(SUPPORT_DX12)
		if (texture->eType == vr::TextureType_DirectX12) {
			auto* src = static_cast<vr::D3D12TextureData_t*>(texture->handle);
			if (src && src->m_pResource && src->m_pCommandQueue) {
				const D3D12_RESOURCE_DESC desc = src->m_pResource->GetDesc();
				needStaging = !dest.staging || dest.staging->GetDesc().Width != desc.Width
				    || dest.staging->GetDesc().Height != desc.Height
				    || dest.staging->GetDesc().Format != desc.Format;
				stagingDesc = desc;
				dest.queue = src->m_pCommandQueue;
				srcRes = src->m_pResource;
				srcQueue = src->m_pCommandQueue;
				if (!needStaging)
					staging = dest.staging.Get();
			}
		}
#endif
		if (!presenter->fillingId)
			presenter->fillingId = presenter->nextId++;
		presenter->ring[slot].id = presenter->fillingId;
	}

#if defined(SUPPORT_DX) && defined(SUPPORT_DX12)
	bool copied = false;
	if (srcRes && srcQueue) {
		std::lock_guard copyLock(presenter->copyMutex);
		if (!presenter->device) {
			srcRes->GetDevice(IID_PPV_ARGS(&presenter->device));
			if (presenter->device) {
				presenter->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&presenter->alloc));
				presenter->device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, presenter->alloc.Get(), nullptr,
				    IID_PPV_ARGS(&presenter->list));
				if (presenter->list)
					presenter->list->Close();
				presenter->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&presenter->fence));
				presenter->fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
				if (presenter->alloc)
					presenter->alloc->SetName(L"OC::MailAlloc");
				if (presenter->list)
					presenter->list->SetName(L"OC::MailList");
				if (presenter->fence)
					presenter->fence->SetName(L"OC::MailFence");
			}
		}
		if (needStaging && presenter->device) {
			D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
			D3D12_RESOURCE_DESC rd = stagingDesc;
			rd.Flags = D3D12_RESOURCE_FLAG_NONE;
			ComPtr<ID3D12Resource> created;
			if (SUCCEEDED(presenter->device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &rd,
			        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&created)))) {
				created->SetName(L"OC::MailStaging");
				std::lock_guard lock(presenter->mailMutex);
				auto& dest = presenter->ring[slot].eyes[ei];
				dest.staging = created;
				staging = dest.staging.Get();
			}
		} else if (!staging) {
			std::lock_guard lock(presenter->mailMutex);
			staging = presenter->ring[slot].eyes[ei].staging.Get();
		}
		if (staging && presenter->alloc && presenter->list && presenter->fence && presenter->fenceEvent) {
			presenter->alloc->Reset();
			presenter->list->Reset(presenter->alloc.Get(), nullptr);
			MailBarrier(presenter->list.Get(), srcRes, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
			MailBarrier(presenter->list.Get(), staging, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
			presenter->list->CopyResource(staging, srcRes);
			MailBarrier(presenter->list.Get(), staging, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
			MailBarrier(presenter->list.Get(), srcRes, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
			if (SUCCEEDED(presenter->list->Close())) {
				ID3D12CommandList* lists[] = { presenter->list.Get() };
				srcQueue->ExecuteCommandLists(1, lists);
				++presenter->fenceValue;
				if (SUCCEEDED(srcQueue->Signal(presenter->fence.Get(), presenter->fenceValue))) {
					if (presenter->fence->GetCompletedValue() < presenter->fenceValue) {
						presenter->fence->SetEventOnCompletion(presenter->fenceValue, presenter->fenceEvent);
						WaitForSingleObject(presenter->fenceEvent, 100);
					}
					copied = presenter->fence->GetCompletedValue() >= presenter->fenceValue;
				}
			}
		}
	}
	if (srcRes && !copied) {
		std::lock_guard lock(presenter->mailMutex);
		presenter->fillingId = 0;
		if (slot >= 0) {
			presenter->ring[slot].eyes[0].have = false;
			presenter->ring[slot].eyes[1].have = false;
			presenter->ring[slot].complete = false;
		}
		return;
	}
#endif

	std::lock_guard lock(presenter->mailMutex);
	auto& dest = presenter->ring[slot].eyes[ei];
	dest.have = true;
	if (presenter->ring[slot].eyes[0].have && presenter->ring[slot].eyes[1].have) {
		presenter->ring[slot].kind = presenter->nextKind;
		presenter->ring[slot].pair = presenter->nextPair;
		presenter->nextKind = 0;
		presenter->fillingId = 0;
		RefreshQuarantine();
		if (presenter->ring[slot].kind == 1 && presenter->bypassInterp.load(std::memory_order_acquire)) {
			presenter->ring[slot].complete = false;
			presenter->ring[slot].eyes[0].have = false;
			presenter->ring[slot].eyes[1].have = false;
		} else {
			presenter->ring[slot].complete = true;
			presenter->ready.push_back(slot);
			if (presenter->ring[slot].kind == 1) {
				presenter->sawInterp = true;
				if (presenter->N.load(std::memory_order_relaxed) < 2)
					presenter->N.store(2, std::memory_order_release);
			} else {
				if (presenter->ring[slot].DeclaredPose())
					presenter->lastRealSlot = slot;
				NoteRealEnqueued();
			}
		}
	}
}

void XrBackend::PresenterEndFrame(bool includeProjection, bool includeSkybox)
{
	XrFrameEndInfo info{ XR_TYPE_FRAME_END_INFO };
	info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	info.displayTime = xr_gbl->nextPredictedFrameTime;

	XrCompositionLayerProjection mainLayer{ XR_TYPE_COMPOSITION_LAYER_PROJECTION };
	XrCompositionLayerBaseHeader* app_layer = nullptr;
	if (includeProjection && submittedEyeTextures) {
		mainLayer.space = xr_space_from_ref_space_type(GetUnsafeBaseSystem()->currentSpace);
		mainLayer.views = projectionViews;
		mainLayer.viewCount = 2;
		app_layer = reinterpret_cast<XrCompositionLayerBaseHeader*>(&mainLayer);
		for (uint32_t i = 0; i < mainLayer.viewCount; ++i) {
			projectionViews[i].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
			if (projectionViews[i].subImage.swapchain == XR_NULL_HANDLE)
				app_layer = nullptr;
		}
	}

	XrCompositionLayerBaseHeader const* const* headers = nullptr;
	int layer_count = 0;
	XrCompositionLayerBaseHeader* stacked[2]{};
	int stackedCount = 0;
	if (app_layer)
		stacked[stackedCount++] = app_layer;
	{
		std::lock_guard lock(presenter->skyboxMutex);
		if (includeSkybox && presenter->skyboxValid)
			stacked[stackedCount++] = reinterpret_cast<XrCompositionLayerBaseHeader*>(&presenter->skyboxLayer);
	}

	BaseOverlay* overlay = GetUnsafeBaseOverlay();
	if (overlay && app_layer) {
		layer_count = overlay->_BuildLayers(app_layer, headers);
	} else if (stackedCount) {
		layer_count = stackedCount;
		headers = stacked;
	}

	info.layers = headers;
	info.layerCount = lastShouldRender ? layer_count : 0;
	const auto endEnter = std::chrono::steady_clock::now();
	OOVR_FAILED_XR_SOFT_ABORT(xrEndFrame(xr_session.get(), &info));
	lastEndFrameMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - endEnter).count();

	BaseSystem* sys = GetUnsafeBaseSystem();
	if (sys)
		sys->_OnPostFrame();
	auto now = std::chrono::system_clock::now().time_since_epoch();
	frameSubmitTimeUs = (double)std::chrono::duration_cast<std::chrono::microseconds>(now).count() / 1000000.0;
	nFrameIndex++;
}

void XrBackend::PresenterLoop()
{
	uint32_t logged = 0;
	while (presenter && !presenter->stop.load(std::memory_order_acquire)) {
		PumpEvents();
		if (!sessionActive || !usingApplicationGraphicsAPI || deferredRenderingStart) {
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
			continue;
		}
		// Right after arming, or any time the doorbell truly has nothing
		// (no ready slot and no prior real to repeat), skip Wait/Begin/End
		// entirely rather than closing the period with 0 layers. Closing
		// with 0 layers in a tight loop (xrWaitFrame has no throttle of its
		// own here) is what drove the runtime into a sustained
		// XR_FRAME_DISCARDED spin and a session that never reached its exit
		// state. The engine side already stopped calling its own
		// Wait/Begin/End once PresenterOwns() flips true, so this is a bare
		// gap with no OpenXR call in flight at all, not a competing caller.
		{
			std::lock_guard lock(presenter->mailMutex);
			// Bootstrap exception: the engine's first post-arm Wait is gated on engineGate,
			// which only advances once TickEnginePeriod runs below. If the engine's actual
			// first frame went out through the pre-arm TwP path (arming races the engine's
			// own submit cadence; which one wins varies run to run) the mailbox starts empty
			// and stays that way forever unless this loop is let through at least once to
			// reach TickEnginePeriod. Once engineGate has moved, go back to skipping on a
			// genuinely empty mailbox so this doesn't become the 0-layer discard spin again.
			const bool neverReleasedEngine = presenter->engineGate.load(std::memory_order_acquire) == 0;
			if (presenter->ready.empty() && presenter->lastRealSlot < 0 && !neverReleasedEngine) {
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
				continue;
			}
		}

		XrFrameWaitInfo waitInfo{ XR_TYPE_FRAME_WAIT_INFO };
		XrFrameState state{ XR_TYPE_FRAME_STATE };
		double waitBlockMs = 0;
		std::chrono::steady_clock::time_point beginLeave{};
		bool haveBegin = false;
		{
			auto lock = xr_session.lock_shared();
			const auto waitEnter = std::chrono::steady_clock::now();
			OOVR_FAILED_XR_ABORT(xrWaitFrame(xr_session.get(), &waitInfo, &state));
			waitBlockMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - waitEnter).count();
			xr_gbl->nextPredictedFrameTime = state.predictedDisplayTime;
			xr_gbl->predictedDisplayPeriod = state.predictedDisplayPeriod;
			lastPredictedPeriodNs = state.predictedDisplayPeriod;
			lastShouldRender = state.shouldRender;
			++waitSeq;
			++presenterSeq;
			if (!state.shouldRender)
				++shouldRenderFalseCount;

			const auto now = std::chrono::steady_clock::now();
			static std::chrono::steady_clock::time_point lastReturn{};
			static bool haveReturn = false;
			double gapMs = 0;
			if (haveReturn)
				gapMs = std::chrono::duration<double, std::milli>(now - lastReturn).count();
			lastReturn = now;
			haveReturn = true;
			lastWaitGapMs = gapMs;

			XrFrameBeginInfo beginInfo{ XR_TYPE_FRAME_BEGIN_INFO };
			const XrResult beginRes = xrBeginFrame(xr_session.get(), &beginInfo);
			if (beginRes == XR_FRAME_DISCARDED) {
				++discardedBeginCount;
				lastBeginDiscarded = 1;
				OOVR_LOGF("[OC] L3 XR_FRAME_DISCARDED n=%u", discardedBeginCount);
				RescheduleAfterDiscard();
			} else {
				lastBeginDiscarded = 0;
			}
			if (XR_FAILED(beginRes)) {
				OOVR_LOGF("[OC] L3 xrBeginFrame failed %d", (int)beginRes);
				continue;
			}
			TickEnginePeriod();
			beginLeave = std::chrono::steady_clock::now();
			haveBegin = true;
		}

		// Pose lead and L2's N are the same beat count. Scanout time stays this slot.
		if (PresenterOwns() && xr_gbl && state.predictedDisplayPeriod > 0) {
			// poseLeadBeats, not N: pacing can react immediately, the pose lead ramps slowly.
			uint32_t beats = presenter->pipelineBeats.load(std::memory_order_acquire) ?
				presenter->poseLeadBeats.load(std::memory_order_acquire) : 4u;
			if (beats < 1)
				beats = 1;
			if (beats > PresenterState::kRing)
				beats = PresenterState::kRing;
			const XrTime scanout = state.predictedDisplayTime;
			xr_gbl->nextPredictedFrameTime = scanout + state.predictedDisplayPeriod * static_cast<XrTime>(beats);
			PublishPoseSnapshot();
			xr_gbl->nextPredictedFrameTime = scanout;
			static uint32_t loggedBeats = 0;
			if (loggedBeats != beats) {
				loggedBeats = beats;
				OOVR_LOGF("[OC] engine pose is xrWait + %u periods (%.2f ms)",
				    beats, static_cast<double>(state.predictedDisplayPeriod) * beats / 1.0e6);
			}
		}

		xr_gbl->ClearCachedViews();
		XrSpace projectionSpace = xr_space_from_ref_space_type(GetUnsafeBaseSystem()->currentSpace);
		const XruCachedViews& cachedViews = xr_gbl->GetCachedViews(projectionSpace);
		const std::array<XrView, XruEyeCount>& views = cachedViews.views;
		for (int eye = 0; eye < XruEyeCount; eye++) {
			projectionViews[eye].fov = views[eye].fov;
			// Pose is the generation viewpoint. Locate-at-displayTime is ATW's
			// job; writing it here would tell the runtime the image matches now.
		}
		vr::HmdMatrix34_t slotPose[XruEyeCount]{};
		bool slotValid[XruEyeCount]{};
		bool haveNew = false;
		int used = -1;
		int kind = 0;
		size_t readyCount = 0;
		uint32_t nSnap = 1;
		uint32_t dropI = 0;
		uint32_t dropR = 0;
		uint32_t dropF = 0;
		{
			std::lock_guard lock(presenter->mailMutex);
			RefreshQuarantine();
			readyCount = presenter->ready.size();
			nSnap = presenter->N.load(std::memory_order_relaxed);
			dropI = presenter->dropInterp;
			dropR = presenter->dropReal;
			dropF = presenter->dropFail;
			// An interp plays only when its own real is already queued behind it.
			// Scanning the whole queue for any interp let the next pair's
			// interp jump in front of an older real.
			if (!presenter->ready.empty()) {
				const int front = presenter->ready.front();
				if (presenter->ring[front].kind == 1) {
					bool matched = false;
					for (int slot : presenter->ready) {
						if (slot == front)
							continue;
						if (presenter->ring[slot].kind == 0 &&
							presenter->ring[slot].pair == presenter->ring[front].pair) {
							matched = true;
							break;
						}
					}
					if (matched) {
						used = front;
						presenter->ready.pop_front();
					}
				} else {
					used = front;
					presenter->ready.pop_front();
				}
			}
			if (used >= 0) {
				presenter->inflight = used;
				haveNew = true;
				kind = presenter->ring[used].kind;
				if (kind == 0 && presenter->ring[used].DeclaredPose())
					presenter->lastRealSlot = used;
			} else if (presenter->lastRealSlot >= 0 && presenter->ring[presenter->lastRealSlot].DeclaredPose()) {
				used = presenter->lastRealSlot;
				presenter->inflight = used;
				kind = 0;
			}
			if (used >= 0) {
				for (int e = 0; e < XruEyeCount; ++e) {
					slotPose[e] = presenter->ring[used].eyes[e].pose;
					slotValid[e] = presenter->ring[used].eyes[e].poseValid;
				}
			}
		}

		submittedEyeTextures = false;
		uint64_t acqUs = 0;
		uint64_t fenceUs = 0;
		uint64_t waitImgUs = 0;
		if (used >= 0 && compositors[0] && compositors[1]) {
			for (int e = 0; e < XruEyeCount; ++e) {
				auto& eyeMail = presenter->ring[used].eyes[e];
#if defined(SUPPORT_DX) && defined(SUPPORT_DX12)
				if (eyeMail.staging && eyeMail.queue) {
					vr::D3D12TextureData_t dx12{};
					dx12.m_pResource = eyeMail.staging.Get();
					dx12.m_pCommandQueue = eyeMail.queue;
					vr::Texture_t tex{};
					tex.handle = &dx12;
					tex.eType = vr::TextureType_DirectX12;
					tex.eColorSpace = vr::ColorSpace_Auto;
					projectionViews[e].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
					compositors[e]->Invoke(&tex, &eyeMail.bounds, projectionViews[e].subImage, (XruEye)e, eyeMail.flags);
					submittedEyeTextures = true;
				}
#endif
			}
#if defined(SUPPORT_DX) && defined(SUPPORT_DX12)
			const DX12SwapWaitUs swapWait = DX12TakeSwapWaitUs();
			acqUs = swapWait.acquireUs;
			fenceUs = swapWait.fenceUs;
			waitImgUs = swapWait.waitImgUs;
#endif
			if (haveNew) {
				std::lock_guard lock(presenter->mailMutex);
				presenter->ring[used].complete = false;
			} else if (submittedEyeTextures)
				++presenterRepeatCount;
		}
		lastSlotRepeat = haveNew ? 0 : 1;

		ApplyMailboxPoses(slotPose, slotValid, !haveNew);
		bool skybox = false;
		{
			std::lock_guard lock(presenter->skyboxMutex);
			skybox = presenter->skyboxValid;
		}
		PresenterEndFrame(lastShouldRender == XR_TRUE, skybox);
		{
			// Displayed slot, not the drop. I/R = new interp/real, '.' = repeat.
			static char trace[61];
			static int traceN = 0;
			trace[traceN++] = haveNew ? (kind == 1 ? 'I' : 'R') : '.';
			if (traceN == 60) {
				trace[60] = '\0';
				OOVR_LOGF("[OC] slot-trace %s", trace);
				traceN = 0;
			}
		}
		const double beginEndMs = haveBegin
		    ? std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - beginLeave).count()
		    : 0;

		static double accWait = 0;
		static double accBegin = 0;
		static double accEnd = 0;
		static uint64_t accAcq = 0;
		static uint64_t accFence = 0;
		static uint64_t accImg = 0;
		static uint32_t accN = 0;
		if (haveBegin) {
			accWait += waitBlockMs;
			accBegin += beginEndMs;
			accEnd += lastEndFrameMs;
			accAcq += acqUs;
			accFence += fenceUs;
			accImg += waitImgUs;
			++accN;
		}

		if (++logged <= 8 || (logged % 120) == 0) {
			const double periodMs = static_cast<double>(state.predictedDisplayPeriod) / 1.0e6;
			const double waitHz = periodMs > 0.05 ? 1000.0 / periodMs : 0;
			OOVR_LOGF("[OC] L3 n=%u N=%u kind=%s period=%.2f ms (%.1f Hz) shouldRender=%d gap=%.2f ms mail=%d ready=%zu repeat=%u discarded=%u quarantine=%u twpMiss=%u dropI=%u dropR=%u dropF=%u",
			    presenterSeq, nSnap, kind == 1 ? "interp" : "real", periodMs, waitHz, state.shouldRender ? 1 : 0, lastWaitGapMs,
			    haveNew ? 1 : 0, readyCount, presenterRepeatCount, discardedBeginCount, quarantineEnterCount, missingTwpCount,
			    dropI, dropR, dropF);
			if (accN > 0) {
				const double n = static_cast<double>(accN);
				OOVR_LOGF("[OC] M2 waitBlock=%.2f beginEnd=%.2f endFrame=%.2f acq=%.2f fence=%.2f waitImg=%.2f ms (both eyes, n=%u)",
				    accWait / n, accBegin / n, accEnd / n,
				    static_cast<double>(accAcq) / 1000.0 / n,
				    static_cast<double>(accFence) / 1000.0 / n,
				    static_cast<double>(accImg) / 1000.0 / n, accN);
				accWait = accBegin = accEnd = 0;
				accAcq = accFence = accImg = 0;
				accN = 0;
			}
		}
	}
}

void XrBackend::WaitForTrackingData()
{
	if (PresenterOwns()) {
		WaitEngineBeat();
		return;
	}

	// Make sure the OpenXR session is active before doing anything else, and if not then skip
	if (!sessionActive) {
		renderingFrame = false;
		return;
	}

	XrFrameWaitInfo waitInfo{ XR_TYPE_FRAME_WAIT_INFO };
	XrFrameState state{ XR_TYPE_FRAME_STATE };

	{
		auto lock = xr_session.lock_shared();
		OOVR_FAILED_XR_ABORT(xrWaitFrame(xr_session.get(), &waitInfo, &state));
		xr_gbl->nextPredictedFrameTime = state.predictedDisplayTime;
		xr_gbl->predictedDisplayPeriod = state.predictedDisplayPeriod;

		const auto now = std::chrono::steady_clock::now();
		static std::chrono::steady_clock::time_point lastWaitReturn{};
		static bool haveLastWaitReturn = false;
		double gapMs = 0;
		if (haveLastWaitReturn) {
			gapMs = std::chrono::duration<double, std::milli>(now - lastWaitReturn).count();
			lastWaitGapMs = gapMs;
		}
		lastWaitReturn = now;
		haveLastWaitReturn = true;
		lastPredictedPeriodNs = state.predictedDisplayPeriod;
		lastShouldRender = state.shouldRender;
		++waitSeq;
		if (!state.shouldRender)
			++shouldRenderFalseCount;

		const double periodMs = static_cast<double>(state.predictedDisplayPeriod) / 1.0e6;
		const double waitHz = periodMs > 0.05 ? 1000.0 / periodMs : 0;
		if (waitSeq <= 8 || (waitSeq % 120) == 0) {
			OOVR_LOGF("[OC] wait n=%u period=%.2f ms (%.1f Hz) shouldRender=%d gap=%.2f ms discarded=%u srFalse=%u",
			    waitSeq, periodMs, waitHz, state.shouldRender ? 1 : 0, gapMs,
			    discardedBeginCount, shouldRenderFalseCount);
		}

		// FIXME loop until this returns true?
		// OOVR_FALSE_ABORT(state.shouldRender);

		// XR_FRAME_DISCARDED is a success code; the abort macro would swallow it.
		XrFrameBeginInfo beginInfo{ XR_TYPE_FRAME_BEGIN_INFO };
		const XrResult beginRes = xrBeginFrame(xr_session.get(), &beginInfo);
		if (beginRes == XR_FRAME_DISCARDED) {
			++discardedBeginCount;
			OOVR_LOGF("[OC] XR_FRAME_DISCARDED n=%u wait=%u", discardedBeginCount, waitSeq);
		}
		if (XR_FAILED(beginRes)) {
			if (xr_instance != XR_NULL_HANDLE) {
				char buf[XR_MAX_RESULT_STRING_SIZE];
				xrResultToString(xr_instance, beginRes, buf);
				OOVR_ABORTF("OpenXR Call failed, aborting. %s:%d %s. Error code: %s\nxrBeginFrame",
				    __FILE__, __LINE__, __func__, buf);
			} else {
				OOVR_ABORTF("OpenXR Call failed, aborting. %s:%d %s. Error code: %d\nxrBeginFrame",
				    __FILE__, __LINE__, __func__, beginRes);
			}
		}
	}

	xr_gbl->ClearCachedViews();

	XrSpace projectionSpace = xr_space_from_ref_space_type(GetUnsafeBaseSystem()->currentSpace);
	const XruCachedViews& cachedViews = xr_gbl->GetCachedViews(projectionSpace);
	const XrViewState& viewState = cachedViews.viewState;
	const std::array<XrView, XruEyeCount>& views = cachedViews.views;

	for (int eye = 0; eye < XruEyeCount; eye++) {
		projectionViews[eye].fov = views[eye].fov;

		XrPosef pose = views[eye].pose;

		// Make sure we at least have halfway-sane values if the runtime isn't providing them. In particular
		// if the runtime gives us an invalid orientation, that'd otherwise cause XR_ERROR_POSE_INVALID errors later.
		if ((viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) == 0) {
			pose.orientation = XrQuaternionf{ 0, 0, 0, 1 };
		}
		if ((viewState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) == 0) {
			// About 1.75m up above the origin
			pose.position = XrVector3f{ 0, 1.75, 0 };
		}

		projectionViews[eye].pose = pose;
	}

	// If we're not on the game's graphics API yet, don't actually mark us as having started the frame.
	// Instead, set a different flag so we'll call this method again when it's available.
	if (!usingApplicationGraphicsAPI) {
		deferredRenderingStart = true;
	} else {
		renderingFrame = true;
	}
}

void XrBackend::StoreEyeTexture(
    vr::EVREye eye,
    const vr::Texture_t* texture,
    const vr::VRTextureBounds_t* bounds,
    vr::EVRSubmitFlags submitFlags,
    bool isFirstEye)
{
	if (PresenterOwns()) {
		CheckOrInitCompositors(texture);
		EnqueueEye(eye, texture, bounds, submitFlags);
		return;
	}

	CheckOrInitCompositors(texture);

	XrCompositionLayerProjectionView& layer = projectionViews[eye];
	layer.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;

	std::unique_ptr<Compositor>& compPtr = compositors[eye];
	OOVR_FALSE_ABORT(compPtr.get() != nullptr);
	Compositor& comp = *compPtr;

	// If the session is inactive, we may be unable to write to the surface
	if (sessionActive && renderingFrame)
		comp.Invoke(texture, bounds, layer.subImage, (XruEye)eye, submitFlags);

	submittedEyeTextures = true;

	// TwP is a VRTextureWithPose_t overlay; Texture_t is the prefix, so the DX12 copy above is unchanged.
	if (submitFlags & vr::Submit_TextureWithPose) {
		const auto* withPose = reinterpret_cast<const vr::VRTextureWithPose_t*>(texture);
		twpPoseValid[eye] = true;
		twpHmdPose[eye] = withPose->mDeviceToAbsoluteTracking;
	} else {
		twpPoseValid[eye] = false;
	}

	// If WaitGetPoses was called before the first texture was submitted, we're in a kinda weird state
	// The application will expect it can submit it's frames (and we do too) however xrBeginFrame was
	// never called for this session - it was called for the early session, then when the first texture
	// was published we switched to that, but this new session hasn't had xrBeginFrame called yet.
	// To get around this, we set a flag if we should begin a frame but are still in the early session. At
	// this point we can check for that flag and call xrBeginFrame a second time, on the right session.
	if (deferredRenderingStart && usingApplicationGraphicsAPI) {
		deferredRenderingStart = false;
		WaitForTrackingData();
	}
}

void XrBackend::SubmitFrames(bool showSkybox, bool postPresent)
{
	// L3 already pumps. Engine SubmitFrames must not race that path.
	if (PresenterOwns())
		return;

	// Always pump events, even if the session isn't active - this is what makes the session active
	// in the first place.
	static std::mutex submitMutex;
    std::lock_guard<std::mutex> lock(submitMutex);

	PumpEvents();

	// If we are getting calls from PostPresentHandOff then skip the calls from other functions as
	//  there will be other data such as GUI layers to be added before ending the frame.
	bool skipRender = postPresentStatus && !postPresent;
	postPresentStatus = postPresent;

	if (!renderingFrame || skipRender)
		return;

	// All data submitted, rendering has finished, frame can be ended.
	renderingFrame = false;

	// Make sure the OpenXR session is active before doing anything else
	// Note that if the session becomes ready after WaitGetTrackingPoses was called, then
	// renderingFrame will still be false so this won't be a problem in that case.
	if (!sessionActive)
		return;

	XrFrameEndInfo info{ XR_TYPE_FRAME_END_INFO };
	info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	info.displayTime = xr_gbl->nextPredictedFrameTime;

	XrCompositionLayerBaseHeader const* const* headers = nullptr;
	XrCompositionLayerBaseHeader* app_layer = nullptr;

	int layer_count = 0;

	// Apps can use layers to provide GUIs and loading screens where a 3D environment is not being rendered.
	// Only create the projection layer if we have a 3D environment to submit.
	XrCompositionLayerProjection mainLayer{ XR_TYPE_COMPOSITION_LAYER_PROJECTION };
	if (submittedEyeTextures) {
		// We have eye textures so setup a projection layer
		mainLayer.space = xr_space_from_ref_space_type(GetUnsafeBaseSystem()->currentSpace);
		mainLayer.views = projectionViews;
		mainLayer.viewCount = 2;

		app_layer = (XrCompositionLayerBaseHeader*)&mainLayer;
		for (uint32_t i = 0; i < mainLayer.viewCount; ++i) {
			XrCompositionLayerProjectionView& layer = projectionViews[i];
			layer.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
			if (layer.subImage.swapchain == XR_NULL_HANDLE)
				app_layer = nullptr;
		}
		submittedEyeTextures = false;
	}

	// If we have an overlay then add
	BaseOverlay* overlay = GetUnsafeBaseOverlay();
	if (overlay) {
		layer_count = overlay->_BuildLayers(app_layer, headers);
	} else if (app_layer) {
		layer_count = 1;
		headers = &app_layer;
	}

	// It's ok if no layers have been added at this point,
	// it will just cause the display to be blanked
	info.layers = headers;
	info.layerCount = layer_count;

	// deferredRenderingStart Wait rewrites locate poses; overwrite TwP immediately before EndFrame.
	for (int i = 0; i < XruEyeCount; ++i) {
		if (!twpPoseValid[i])
			continue;

		const glm::mat4 worldFromEye = S2G_m34(twpHmdPose[i]) * S2G_m34(hmd->GetEyeToHeadTransform(static_cast<vr::EVREye>(i)));
		projectionViews[i].pose.position = G2X_v3f(glm::vec3(worldFromEye[3]));
		projectionViews[i].pose.orientation = G2X_quat(glm::normalize(glm::quat_cast(worldFromEye)));
		OOVR_LOG_ONCE("Applied Submit_TextureWithPose to projectionViews");
	}

	OOVR_FAILED_XR_SOFT_ABORT(xrEndFrame(xr_session.get(), &info));

	for (int i = 0; i < XruEyeCount; ++i)
		twpPoseValid[i] = false;

	BaseSystem* sys = GetUnsafeBaseSystem();
	if (sys) {
		sys->_OnPostFrame();
	}

	auto now = std::chrono::system_clock::now().time_since_epoch();
	frameSubmitTimeUs = (double)std::chrono::duration_cast<std::chrono::microseconds>(now).count() / 1000000.0;

	nFrameIndex++;
	PresenterTryArm();
}

IBackend::openvr_enum_t XrBackend::SetSkyboxOverride(const vr::Texture_t* pTextures, uint32_t unTextureCount)
{
	// Needed for rFactor2 loading screens
	if (unTextureCount && pTextures) {
		CheckOrInitCompositors(pTextures);

		if (!sessionActive || !usingApplicationGraphicsAPI)
			return 0;

		if (PresenterOwns()) {
			if (skybox_compositor == nullptr)
				skybox_compositor = BaseCompositor::CreateCompositorAPI(pTextures);
			vr::VRTextureBounds_t bounds{ 0.0, 1.0, 1.0, 0.0 };
			std::lock_guard lock(presenter->skyboxMutex);
			presenter->skyboxLayer = { XR_TYPE_COMPOSITION_LAYER_QUAD };
			skybox_compositor->Invoke(pTextures, &bounds, presenter->skyboxLayer.subImage);
			presenter->skyboxLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
			presenter->skyboxLayer.space = xr_space_from_ref_space_type(GetUnsafeBaseSystem()->currentSpace);
			presenter->skyboxLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
			presenter->skyboxLayer.pose = { { 0.f, 0.f, 0.f, 1.f }, { 0.0f, 0.0f, -0.65f } };
			presenter->skyboxLayer.size = { 1.0f, 1.0f / 1.333f };
			presenter->skyboxValid = true;
			++skyboxWaitCount;
			if (skyboxWaitCount <= 8 || (skyboxWaitCount % 120) == 0)
				OOVR_LOGF("[OC] skybox injected to L3 n=%u (no Wait)", skyboxWaitCount);
			return 0;
		}

		// Make sure any unfinished frames don't call xrEndFrame after this call
		renderingFrame = false;

		++skyboxWaitCount;
		if (skyboxWaitCount <= 8 || (skyboxWaitCount % 120) == 0)
			OOVR_LOGF("[OC] skybox Wait hit n=%u (second xrWaitFrame path still live)", skyboxWaitCount);

		XrFrameWaitInfo waitInfo{ XR_TYPE_FRAME_WAIT_INFO };
		XrFrameState state{ XR_TYPE_FRAME_STATE };

		OOVR_FAILED_XR_ABORT(xrWaitFrame(xr_session.get(), &waitInfo, &state));
		xr_gbl->nextPredictedFrameTime = state.predictedDisplayTime;
		xr_gbl->predictedDisplayPeriod = state.predictedDisplayPeriod;

		// This submits a frame when a skybox override is set. This is designed around rFactor2 where the skybox is used as
		// a loading screen and is frequently updated, and most other games probably behave in a similar manner. It'd be
		// ideal to run a separate thread while the skybox override is set to submit frames if IVRCompositor->Submit is not
		// being called frequently enough, and that'd need to be carefully synchronised with the main submit thread. That's
		// not yet implemented since it's not currently worth the hassle, but if someone in the future wants to do it:
		// TODO submit skybox frames in their own thread.
		XrFrameBeginInfo beginInfo{ XR_TYPE_FRAME_BEGIN_INFO };
		OOVR_FAILED_XR_ABORT(xrBeginFrame(xr_session.get(), &beginInfo));

		if (skybox_compositor == nullptr)
			skybox_compositor = BaseCompositor::CreateCompositorAPI(pTextures);

		vr::VRTextureBounds_t bounds;
		bounds.uMin = 0.0;
		bounds.uMax = 1.0;
		bounds.vMin = 1.0;
		bounds.vMax = 0.0;

		XrCompositionLayerQuad layerQuad = { XR_TYPE_COMPOSITION_LAYER_QUAD };
		skybox_compositor->Invoke(pTextures, &bounds, layerQuad.subImage);
		layerQuad.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
		layerQuad.next = NULL;
		layerQuad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
		layerQuad.space = xr_space_from_ref_space_type(GetUnsafeBaseSystem()->currentSpace);
		layerQuad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
		layerQuad.pose = { { 0.f, 0.f, 0.f, 1.f },
			{ 0.0f, 0.0f, -0.65f } };
		layerQuad.size = { 1.0f, 1.0f / 1.333f };

		XrCompositionLayerBaseHeader* layers[1];
		layers[0] = (XrCompositionLayerBaseHeader*)&layerQuad;
		XrFrameEndInfo info{ XR_TYPE_FRAME_END_INFO };
		info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
		info.displayTime = xr_gbl->nextPredictedFrameTime;
		info.layers = layers;
		info.layerCount = 1;

		OOVR_FAILED_XR_SOFT_ABORT(xrEndFrame(xr_session.get(), &info));

	} else {
		OOVR_SOFT_ABORT("Unsupported texture count");
	}

	return 0;
}

void XrBackend::ClearSkyboxOverride()
{
	OOVR_SOFT_ABORT("No implementation");
}

/* Misc compositor */

/**
 * Get frame timing information to be passed to the application
 *
 * Returns true if successful
 */
bool XrBackend::GetFrameTiming(OOVR_Compositor_FrameTiming* pTiming, uint32_t unFramesAgo)
{
	// Zero everything except the size field
	memset(reinterpret_cast<unsigned char*>(pTiming) + sizeof(pTiming->m_nSize), 0, pTiming->m_nSize - sizeof(pTiming->m_nSize));

	if (pTiming->m_nSize >= sizeof(IVRCompositor_018::Compositor_FrameTiming)) {
		pTiming->m_flSystemTimeInSeconds = frameSubmitTimeUs;
		pTiming->m_nFrameIndex = nFrameIndex;

		// A lot of these values we can't get the data for so just use sensible values
		pTiming->m_nNumFramePresents = 1;
		pTiming->m_nNumMisPresented = lastBeginDiscarded;
		pTiming->m_nNumDroppedFrames = 0;
		pTiming->m_nReprojectionFlags = 0;

		// Just use sensible values until GPU timers implemented
		pTiming->m_flPreSubmitGpuMs = 8.0f;
		pTiming->m_flPostSubmitGpuMs = 1.0f;
		pTiming->m_flTotalRenderGpuMs = 9.0f;

		// Use very conservative guesses for these. They are used in F1 22 for dynamic resolution calculations but are not something that is provided
		// through OpenXR. Using conservative values will give a bit more headroom for the game to target realistic frame times.
		pTiming->m_flCompositorRenderGpuMs = 1.5f; // time spend performing distortion correction, rendering chaperone, overlays, etc.
		pTiming->m_flCompositorRenderCpuMs = 3.0f; // time spent on cpu submitting the above work for this frame

		pTiming->m_flCompositorIdleCpuMs = 0.1f;

		// L3 cadence is runtime P. Repeat is reuse for ATW, not a drop.
		// Engine fps (N) is not this field.
		const float pMs = lastPredictedPeriodNs > 0
		    ? static_cast<float>(lastPredictedPeriodNs) / 1.0e6f
		    : (lastWaitGapMs > 0.0 ? static_cast<float>(lastWaitGapMs) : 8.333f);
		pTiming->m_flClientFrameIntervalMs = pMs;
		pTiming->m_flPresentCallCpuMs = 0.0f; // time blocked on call to present (usually 0.0, but can go long)
		pTiming->m_flWaitForPresentCpuMs = 0.0f; // time spent spin-waiting for frame index to change (not near-zero indicates wait object failure)
		pTiming->m_flSubmitFrameMs = 0.0f; // time spent in IVRCompositor::Submit (not near-zero indicates driver issue)

		/** The following are all relative to this frame's SystemTimeInSeconds */
		pTiming->m_flWaitGetPosesCalledMs = 0.0f;
		pTiming->m_flNewPosesReadyMs = 0.0f;
		pTiming->m_flNewFrameReadyMs = 0.0f; // second call to IVRCompositor::Submit
		pTiming->m_flCompositorUpdateStartMs = 0.0f;
		pTiming->m_flCompositorUpdateEndMs = 0.0f;
		pTiming->m_flCompositorRenderStartMs = 0.0f;

		GetPrimaryHMD()->GetPose(vr::ETrackingUniverseOrigin::TrackingUniverseSeated, &pTiming->m_HmdPose, ETrackingStateType::TrackingStateType_Rendering);

		return true;
	}

	return false;
}

/* D3D Mirror textures */
/* #if defined(SUPPORT_DX) */
IBackend::openvr_enum_t XrBackend::GetMirrorTextureD3D11(vr::EVREye eEye, void* pD3D11DeviceOrResource, void** ppD3D11ShaderResourceView)
{
	OOVR_SOFT_ABORT("No implementation");
	return 0;
}
void XrBackend::ReleaseMirrorTextureD3D11(void* pD3D11ShaderResourceView)
{
	OOVR_SOFT_ABORT("No implementation");
}
/* #endif */
/** Returns the points of the Play Area. */
bool XrBackend::GetPlayAreaPoints(vr::HmdVector3_t* points, int* count)
{
	if (count)
		*count = 0;

	XrExtent2Df bounds;
	XrResult res = xrGetReferenceSpaceBoundsRect(xr_session.get(), XR_REFERENCE_SPACE_TYPE_STAGE, &bounds);

	if (res == XR_SPACE_BOUNDS_UNAVAILABLE)
		return false;

	OOVR_FAILED_XR_ABORT(res);

	if (count)
		*count = 4;

	// The origin of the free space is centred around the player
	// TODO if we're using the Oculus runtime, grab it's native handle and get the full polygon
	if (points) {
		points[0] = vr::HmdVector3_t{ -bounds.width / 2, 0, -bounds.height / 2 };
		points[1] = vr::HmdVector3_t{ bounds.width / 2, 0, -bounds.height / 2 };
		points[2] = vr::HmdVector3_t{ bounds.width / 2, 0, bounds.height / 2 };
		points[3] = vr::HmdVector3_t{ -bounds.width / 2, 0, bounds.height / 2 };
	}

	return true;
}
/** Determine whether the bounds are showing right now **/
bool XrBackend::AreBoundsVisible()
{
	OOVR_SOFT_ABORT("No implementation");
	return false;
}
/** Set the boundaries to be visible or not (although setting this to false shouldn't affect
 * what happens if the player moves their hands too close and shows it that way) **/
void XrBackend::ForceBoundsVisible(bool status)
{
	OOVR_SOFT_ABORT("No implementation");
}

bool XrBackend::IsInputAvailable()
{
	return sessionState == XR_SESSION_STATE_FOCUSED;
}

void XrBackend::PumpEvents()
{
	BaseInput* input = GetUnsafeBaseInput();
	if (input && sessionState == XR_SESSION_STATE_FOCUSED && !hand_left && !hand_right) {
		if (input->AreActionsLoaded()) {
			// Once app/legacy action sets are loaded, query current interaction profiles directly so
			// controller devices can be created even if no profile-changed event is emitted.
			UpdateInteractionProfile();
		}
	}
	// Keyboard text input is handled by the in-VR overlay interaction path.
	// Poll for OpenXR events
	while (true) {
		XrEventDataBuffer ev = { XR_TYPE_EVENT_DATA_BUFFER };
		XrResult res;

		OOVR_FAILED_XR_ABORT(res = xrPollEvent(xr_instance, &ev));

		if (res == XR_EVENT_UNAVAILABLE) {
			break;
		}

		if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
			auto* changed = (XrEventDataSessionStateChanged*)&ev;
			OOVR_FALSE_ABORT(changed->session == xr_session.get());
			sessionState = changed->state;

			// Monado bug: it returns 0 for this value (at least for the first two states)
			// Make sure this is actually greater than 0, otherwise this will mess up xr_gbl->GetBestTime()
			if (changed->time > 0 && xr_gbl)
				xr_gbl->latestTime = changed->time;

			OOVR_LOGF("Switch to OpenXR state %d", sessionState);

			switch (sessionState) {
			case XR_SESSION_STATE_READY: {
				OOVR_LOG("Hit ready state, begin session...");
				// Start the session running - this means we're supposed to start submitting frames
				XrSessionBeginInfo beginInfo{ XR_TYPE_SESSION_BEGIN_INFO };
				beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
				OOVR_FAILED_XR_ABORT(xrBeginSession(xr_session.get(), &beginInfo));
				sessionActive = true;
				break;
			}
			case XR_SESSION_STATE_STOPPING: {
				// End the session. The session is still valid and we can still query some information
				// from it, but we're not allowed to submit frames anymore. This is done when the engagement
				// sensor detects the user has taken off the headset, for example.
				if (sessionActive) // could be the case if we missed XR_SESSION_STATE_READY for some reason
					OOVR_FAILED_XR_ABORT(xrEndSession(xr_session.get()));
				sessionActive = false;
				renderingFrame = false;
				break;
			}
			case XR_SESSION_STATE_EXITING: {
				OOVR_LOGF("Exiting");
				break;
			}
			case XR_SESSION_STATE_LOSS_PENDING: {
				// If the headset is unplugged or the user decides to exit the app
				VREvent_t quit = { VREvent_Quit };
				auto system = GetBaseSystem();
				if (system)
					system->_EnqueueEvent(quit);
				break;
			}
			default:
				// suppress clion warning about missing branches
				break;
			}
		} else if (ev.type == XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED) {
			UpdateInteractionProfile();
			break;
		}
	} // while loop

	/*
	   We check for AreActionsLoaded here because:
	   1. Games using legacy input call xrSyncActions every frame anyway, so the runtime should
	      give us an interaction profile without us forcing it
	   2. Games using an action manifest should be calling UpdateActionState every frame, which calls xrSyncActions.
	      This means the only games we wouldn't be able to confidently grab an interaction profile from
	      would be ones where an action manifest is loaded but UpdateActionState is not being called
	      (because the game checks IsTrackedDeviceConnected or something),
	      and hopefully no game like that exists.
	   Some runtimes (WMR) do not instantly return an interaction profile,
	   so we will keep tryinig to query it until it does.

	   Note that we check that the session is focused because this means that the application
	   has already submitted a frame, that frame is visible, and we have input focus.
	   Waiting until the application has input focus allows us to avoid unnecessarily restarting the
	   session when we can't even receive input anyway, as well as before the session is restarted for
	   the temporary session.
   */
	input = GetUnsafeBaseInput();
	if (input && !input->AreActionsLoaded() && sessionState == XR_SESSION_STATE_FOCUSED && !hand_left && !hand_right) {
		QueryForInteractionProfile();
	}
}

void XrBackend::OnSessionCreated()
{
	sessionState = XR_SESSION_STATE_UNKNOWN;
	sessionActive = false;
	renderingFrame = false;

	PumpEvents();

	// Wait until we transition to the idle state.
	// This sets the time, so OpenXR calls which use that will work correctly.
	while (sessionState == XR_SESSION_STATE_UNKNOWN) {
		const int durationMs = 250;

		OOVR_LOGF("No session transition yet received, waiting %dms ...", durationMs);

#ifdef _WIN32
		Sleep(durationMs);
#else
		struct timespec ts = { 0, durationMs * 1000000 };
		nanosleep(&ts, &ts);
#endif

		PumpEvents();
	}
}

void XrBackend::PrepareForSessionShutdown()
{
	PresenterStop();
	for (std::unique_ptr<Compositor>& c : compositors) {
		c.reset();
	}
	skybox_compositor.reset();
	overlay_compositors.clear();
	if (infoSet != XR_NULL_HANDLE) {
		OOVR_FAILED_XR_ABORT(xrDestroyActionSet(infoSet));
		infoSet = XR_NULL_HANDLE;
		infoAction = XR_NULL_HANDLE;
	}
}

// On Android, add an event poll function for use while sleeping
#ifdef ANDROID
void OpenComposite_Android_EventPoll()
{
	BackendManager::Instance().PumpEvents();
}
#endif

bool XrBackend::IsGraphicsConfigured()
{
	return usingApplicationGraphicsAPI;
}

void XrBackend::OnOverlayTexture(const vr::Texture_t* texture)
{
	if (!usingApplicationGraphicsAPI)
		CheckOrInitCompositors(texture);
}

void XrBackend::RegisterOverlayCompositor(std::shared_ptr<Compositor> compositor)
{
	overlay_compositors.push_back(compositor);
}

void XrBackend::UnregisterOverlayCompositor(std::shared_ptr<Compositor> compositor)
{
	std::erase_if(overlay_compositors, [c = std::move(compositor)](std::shared_ptr<Compositor>& comp) {
		return c == comp;
	});
}

void XrBackend::UpdateInteractionProfile()
{
	struct hand_info {
		const char* pathstr;
		std::shared_ptr<XrController>& controller;
		const XrController::XrControllerType hand;
	};

	hand_info hands[] = {
		{ .pathstr = "/user/hand/left", .controller = hand_left, .hand = XrController::XCT_LEFT },
		{ .pathstr = "/user/hand/right", .controller = hand_right, .hand = XrController::XCT_RIGHT }
	};

	for (hand_info& info : hands) {
		XrInteractionProfileState state{ XR_TYPE_INTERACTION_PROFILE_STATE };
		XrPath path;
		OOVR_FAILED_XR_ABORT(xrStringToPath(xr_instance, info.pathstr, &path));
		OOVR_FAILED_XR_ABORT(xrGetCurrentInteractionProfile(xr_session.get(), path, &state));

		// interaction profile detected
		if (state.interactionProfile != XR_NULL_PATH) {
			uint32_t tmp;
			char path_name[XR_MAX_PATH_LENGTH];
			OOVR_FAILED_XR_ABORT(xrPathToString(xr_instance, state.interactionProfile, XR_MAX_PATH_LENGTH, &tmp, path_name));

			for (const std::unique_ptr<InteractionProfile>& profile : InteractionProfile::GetProfileList()) {
				if (profile->GetPath() == path_name) {
					OOVR_LOGF("%s - Using interaction profile: %s", info.pathstr, path_name);
					info.controller = std::make_unique<XrController>(info.hand, *profile);
					hmd->SetInteractionProfile(profile.get());
					BaseSystem* system = GetUnsafeBaseSystem();
					if (system) {
						VREvent_t event = {
							.eventType = VREvent_TrackedDeviceActivated,
							.trackedDeviceIndex = (TrackedDeviceIndex_t)info.hand + 1
						};
						system->_EnqueueEvent(event);
						event = {
							.eventType = VREvent_TrackedDeviceUpdated,
							.trackedDeviceIndex = 0
						};
						system->_EnqueueEvent(event);
					}
					break;
				}
			}
			if (!hand_left && !hand_right) {
				// Runtime returned an unknown interaction profile!
				OOVR_ABORTF("Runtime unexpectedly returned an unknown interaction profile: %s", path_name);
			}
		} else {
			// interaction profile lost/not detected
			OOVR_LOGF("%s - No interaction profile detected", info.pathstr);
			if (info.controller) {
				info.controller.reset();
				BaseSystem* system = GetUnsafeBaseSystem();
				if (system) {
					VREvent_t event = {
						.eventType = VREvent_TrackedDeviceDeactivated,
						.trackedDeviceIndex = (TrackedDeviceIndex_t)info.hand + 1
					};
					system->_EnqueueEvent(event);
				}
			}
		}
	}

	CreateGenericTrackers();
}

void XrBackend::CreateGenericTrackers()
{
	if (!xr_ext->xrMndxXdevSpace_Available())
		return;

	std::unique_lock lock(generic_trackers_mutex);

	OOVR_LOGF("Checking for generic trackers...");

	// should get cleared every time this function is run to avoid stale trackers
	generic_trackers.clear();

	// list containing all generic tracked devices
	XrXDevListMNDX generic_tracker_list;
	std::vector<XrXDevIdMNDX> generic_tracker_ids(MAX_GENERIC_TRACKERS);
	InteractionProfile* profile = InteractionProfile::GetProfileByPath("/interaction_profiles/htc/vive_tracker_htcx");

	uint32_t xdev_count = 0;

	XrCreateXDevListInfoMNDX create_info = {
		.type = XR_TYPE_CREATE_XDEV_LIST_INFO_MNDX
	};

	XrXDevPropertiesMNDX cur_properties = {
		.type = XR_TYPE_XDEV_PROPERTIES_MNDX,
	};

	XrGetXDevInfoMNDX cur_info = {
		.type = XR_TYPE_GET_XDEV_INFO_MNDX,
	};

	OOVR_FAILED_XR_ABORT(xr_ext->xrCreateXDevListMNDX(xr_session.get(), &create_info, &generic_tracker_list));
	OOVR_FAILED_XR_ABORT(xr_ext->xrEnumerateXDevsMNDX(generic_tracker_list, MAX_GENERIC_TRACKERS, &xdev_count, generic_tracker_ids.data()));

	std::vector<std::string> forced_tracker_serials{};
	{
		auto forced_tracker_serials_str = GetEnv("OPENCOMPOSITE_TRACKER_SERIALS");
		if (!forced_tracker_serials_str.empty()) {
			size_t separator_pos{ std::string::npos };
			while ((separator_pos = forced_tracker_serials_str.find(';')) != std::string::npos) {
				auto serial = forced_tracker_serials_str.substr(0, separator_pos);
				forced_tracker_serials.push_back(serial);
				forced_tracker_serials_str.erase(0, separator_pos + 1);
			}
			if (!forced_tracker_serials_str.empty()) {
				forced_tracker_serials.push_back(forced_tracker_serials_str);
			}
		}
	}

	// filter out non-tracker devices
	std::erase_if(generic_tracker_ids, [&](XrXDevIdMNDX id) {
		if (xdev_count == 0) {
			return true;
		}
		cur_info.id = id;

		OOVR_FAILED_XR_ABORT(xr_ext->xrGetXDevPropertiesMNDX(generic_tracker_list, &cur_info, &cur_properties));

		xdev_count--;

		std::string name = cur_properties.name;
		std::string serial = cur_properties.serial;

		if (!cur_properties.canCreateSpace)
			return true;

		OOVR_LOGF("Found usable xdev '%s', serial '%s'", name.c_str(), serial.c_str());

		if (forced_tracker_serials.size() > 0 && std::find(forced_tracker_serials.cbegin(), forced_tracker_serials.cend(), serial) != forced_tracker_serials.cend()) {
			return false;
		}
		return name.find("Tracker") == std::string::npos;
	});

	OOVR_LOGF("Found %zu generic trackers", generic_tracker_ids.size());

	OOVR_LOG("Creating generic trackers...");
	for (const XrXDevIdMNDX id : generic_tracker_ids) {
		int index = generic_trackers.size();

		if (index >= MAX_GENERIC_TRACKERS) {
			OOVR_LOGF("More trackers present than allowed (%d), skipping..", MAX_GENERIC_TRACKERS);
			break;
		}

		cur_info.id = id;

		OOVR_FAILED_XR_ABORT(xr_ext->xrGetXDevPropertiesMNDX(generic_tracker_list, &cur_info, &cur_properties));

		std::string serial = cur_properties.serial;
		std::string name = cur_properties.name;

		OOVR_LOGF("Generic Tracker: %s, Serial: %s, Index: %d, ID: %" PRIu64, cur_properties.name, cur_properties.serial, index, id);

		// create space
		XrSpace space;
		XrPosef pose = { .orientation = { 0, 0, 0, 1 }, .position = { 0, 0, 0 } };
		XrCreateXDevSpaceInfoMNDX create_space_info = {
			.type = XR_TYPE_CREATE_XDEV_SPACE_INFO_MNDX,
			.next = NULL,
			.xdevList = generic_tracker_list,
			.id = id,
			.offset = pose,
		};

		OOVR_FAILED_XR_ABORT(xr_ext->xrCreateXDevSpaceMNDX(xr_session.get(), &create_space_info, &space));

		generic_trackers.push_back(std::make_unique<XrGenericTracker>(*profile, cur_properties, index, space));

		BaseSystem* system = GetUnsafeBaseSystem();
		if (system) {
			VREvent_t event = {
				.eventType = VREvent_TrackedDeviceActivated,
				.trackedDeviceIndex = (TrackedDeviceIndex_t)generic_trackers.at(index)->DeviceIndex()
			};
			system->_EnqueueEvent(event);
		}
	}

	// cleanup
	xr_ext->xrDestroyXDevListMNDX(generic_tracker_list);
}

void XrBackend::MaybeRestartForInputs()
{
	// if we haven't attached any actions to the session (infoSet or game actions), no need to restart
	const std::lock_guard<std::mutex> lock(inputRestartMutex);
	BaseInput* input = GetUnsafeBaseInput();
	if (infoSet == XR_NULL_HANDLE && (!input || !input->AreActionsLoaded()))
		return;

	OOVR_LOG("Restarting session for inputs...");
	DrvOpenXR::SetupSession();
	OOVR_LOG("Session restart successful!");
}

void XrBackend::QueryForInteractionProfile()
{
	// Note that we want to avoid using BaseInput here because it would allow for games to call GetControllerState before rendering
	// and then we'd have to recreate the session twice, once for the input state and once for when the game submits a frame
	const std::lock_guard<std::mutex> lock(inputRestartMutex);
	if (subactionPaths[0] == XR_NULL_PATH) {
		OOVR_FAILED_XR_ABORT(xrStringToPath(xr_instance, "/user/hand/left", &subactionPaths[0]));
		OOVR_FAILED_XR_ABORT(xrStringToPath(xr_instance, "/user/hand/right", &subactionPaths[1]));
	}

	if (infoSet == XR_NULL_HANDLE) {
		OOVR_LOG("Creating infoset");
		CreateInfoSet();
		BindInfoSet();
	}

	// Interaction profiles are updated after xrSyncActions, so we'll try to make the runtime give us one by calling xrSyncActions.
	XrActiveActionSet active[2] = {
		{ .actionSet = infoSet, .subactionPath = subactionPaths[0] },
		{ .actionSet = infoSet, .subactionPath = subactionPaths[1] },
	};

	XrActionsSyncInfo info{ XR_TYPE_ACTIONS_SYNC_INFO };
	info.countActiveActionSets = 2;
	info.activeActionSets = active;

	OOVR_FAILED_XR_ABORT(xrSyncActions(xr_session.get(), &info));
}

void XrBackend::CreateInfoSet()
{
	XrActionSetCreateInfo set_info{ XR_TYPE_ACTION_SET_CREATE_INFO };
	strcpy_s(set_info.actionSetName, XR_MAX_ACTION_SET_NAME_SIZE, "opencomposite-internal-info-set");
	strcpy_s(set_info.localizedActionSetName, XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE, "OpenComposite internal info set");
	OOVR_FAILED_XR_ABORT(xrCreateActionSet(xr_instance, &set_info, &infoSet));

	XrActionCreateInfo act_info{ XR_TYPE_ACTION_CREATE_INFO };
	strcpy_s(act_info.actionName, XR_MAX_ACTION_NAME_SIZE, "opencomposite-internal-info-act");
	strcpy_s(act_info.localizedActionName, XR_MAX_LOCALIZED_ACTION_NAME_SIZE, "OpenComposite internal info action");
	act_info.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
	act_info.countSubactionPaths = std::size(subactionPaths);
	act_info.subactionPaths = subactionPaths;
	OOVR_FAILED_XR_ABORT(xrCreateAction(infoSet, &act_info, &infoAction));
}

void XrBackend::BindInfoSet()
{
	for (const std::unique_ptr<InteractionProfile>& profile : InteractionProfile::GetProfileList()) {
		XrPath interactionProfilePath;
		OOVR_FAILED_XR_ABORT(xrStringToPath(xr_instance, profile->GetPath().c_str(), &interactionProfilePath));

		if (!profile->CanHaveBindings())
			continue;

		XrInteractionProfileSuggestedBinding suggestedBindings{ XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };

		std::vector<XrActionSuggestedBinding> bindings;
		using namespace std::string_literals;
		// grabs the first found paths ending in /click for each subaction path
		for (const std::string& path_name : { "/user/hand/left"s, "/user/hand/right"s }) {
			auto click_path = std::ranges::find_if(profile->GetValidInputPaths(),
			    [&path_name](std::string s) -> bool {
				    return s.find("/click") != s.npos && s.find(path_name) != s.npos;
			    });
			if (click_path == profile->GetValidInputPaths().end()) {
				continue;
			}
			XrPath path;
			OOVR_FAILED_XR_ABORT(xrStringToPath(xr_instance, click_path->c_str(), &path));
			bindings.push_back({ .action = infoAction, .binding = path });

			suggestedBindings.interactionProfile = interactionProfilePath;
			suggestedBindings.suggestedBindings = bindings.data();
			suggestedBindings.countSuggestedBindings = bindings.size();

			OOVR_FAILED_XR_ABORT(xrSuggestInteractionProfileBindings(xr_instance, &suggestedBindings));
		}
	}

	// Attach the info set by itself. We will have to restart the session once the game attaches its real inputs.
	XrSessionActionSetsAttachInfo info{ XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
	info.countActionSets = 1;
	info.actionSets = &infoSet;
	OOVR_FAILED_XR_ABORT(xrAttachSessionActionSets(xr_session.get(), &info));
}

const void* XrBackend::GetCurrentGraphicsBinding()
{
	if (graphicsBinding) {
		return graphicsBinding->asVoid();
	}
	OOVR_FALSE_ABORT(temporaryGraphics);
	return temporaryGraphics->GetGraphicsBinding();
}

#ifdef SUPPORT_VK
void XrBackend::VkGetPhysicalDevice(VkInstance instance, VkPhysicalDevice* out)
{
	*out = VK_NULL_HANDLE;

	TemporaryVk* vk = temporaryGraphics->GetAsVk();
	if (vk == nullptr)
		OOVR_ABORT("Not using temporary Vulkan instance");

	// Find the UUID of the physical device the temporary instance is running on
	VkPhysicalDeviceIDProperties idProps = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
	VkPhysicalDeviceProperties2 props = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &idProps };
	vkGetPhysicalDeviceProperties2(vk->physicalDevice, &props);

	// Look through all the physical devices on the target instance and find the matching one
	uint32_t devCount;
	OOVR_FAILED_VK_ABORT(vkEnumeratePhysicalDevices(instance, &devCount, nullptr));
	std::vector<VkPhysicalDevice> physicalDevices(devCount);
	OOVR_FAILED_VK_ABORT(vkEnumeratePhysicalDevices(instance, &devCount, physicalDevices.data()));

	for (VkPhysicalDevice phy : physicalDevices) {
		VkPhysicalDeviceIDProperties devIdProps = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
		VkPhysicalDeviceProperties2 devProps = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &devIdProps };
		vkGetPhysicalDeviceProperties2(phy, &devProps);

		if (memcmp(devIdProps.deviceUUID, idProps.deviceUUID, sizeof(devIdProps.deviceUUID)) != 0)
			continue;

		// Found it
		*out = phy;
		return;
	}

	OOVR_ABORT("Could not find matching Vulkan physical device for instance");
}

#endif
