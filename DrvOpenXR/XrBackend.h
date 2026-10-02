//
// Created by ZNix on 25/10/2020.
//

#pragma once

#include "XrDriverPrivate.h"

#include "XrController.h"
#include "XrHMD.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

class XrGenericTracker;

class XrBackend : public IBackend {
public:
	DECLARE_BACKEND_FUNCS(virtual, override)

	XrBackend(bool useVulkanTmpGfx, bool useD3D11TmpGfx);
	~XrBackend() override;

	/**
	 * The current state of the OpenXR session, or XR_SESSION_STATE_UNKNOWN if there is no session.
	 */
	XrSessionState sessionState = XR_SESSION_STATE_UNKNOWN;

	XrSessionState GetSessionState();

	/**
	 * Whether the session is active or not. This cannot be determined from just the session state, since
	 * we're allowed to send frames after calling xrBeginSession but before the event comes through. Same
	 * with ending the session - we're allowed to submit frames after we receive the stopping event, until
	 * we call xrEndSession.
	 */
	bool sessionActive = false;

	/**
	 * To be called after xrCreateSession. Should only be used by DrvOpenXR.
	 */
	void OnSessionCreated();

	void PrepareForSessionShutdown();

	const void* GetCurrentGraphicsBinding();

	void RegisterOverlayCompositor(std::shared_ptr<Compositor> compositor);
	void UnregisterOverlayCompositor(std::shared_ptr<Compositor> compositor);

	/**
	 * Restarts the session to allow for inputs to be attached to the session, if necessary.
	 * To be called from BaseInput whenever it's attempting to attach the game actions.
	 * Restarting the session should only be necessary if we've already created the infoSet.
	 */
	static void MaybeRestartForInputs();

	/** @brief True after L3 took Wait/Begin/End. */
	bool PresenterOwns() override;
	/** @brief Copy the last L3 pose snapshot. */
	bool CopyPresenterPoses(vr::TrackedDevicePose_t* out, uint32_t count) override;
	/** @brief Tag the next completed L1 slot: 0 real, 1 interpolated. */
	void SetNextEnqueueKind(int kind);
	/** @brief RDB reports measured draw time and T_rdb. L2 turns that into N. */
	void SetPipelineMs(float drawMs, float rdbMs);
	/** @brief Producer delivered one frame. Depth 2; a third drops the oldest. Does not block. */
	void NoteProduced();
	/** @brief RDB started the oldest delivered frame. */
	void NoteStarted();
	/** @brief Engine frame boundary. Returns on every Nth display period. No extra sleep. */
	void WaitEngineBeat();
	/** @brief Presenter display tick. Releases the engine every N periods. */
	void TickEnginePeriod();
	/** @brief Recomputes N from telemetry queued by SetPipelineMs/NoteProduced/NoteStarted.
		Called once per presenter beat from TickEnginePeriod; those setters only stage state. */
	void ApplyPace();
	/** @brief CS's engine now blocks its own Submit per frame until RDB's full pipeline for
		that frame reaches the beat (see Upscaling::Settings::vrSingleBlock). That already
		keeps the engine from running ahead of RDB, so L1's own N-based throttle (tuned for
		the async case, where nothing else enforces that) would only double-pace it on top:
		pins targetRatio to 1 (no beats skipped) instead of computing it from T_rdb. */
	void SetTrueSingle(bool enabled);
	/** @brief Items (not pairs) currently queued in the presenter's ready list, not yet
		consumed by a display beat. For True Single's bounded-backlog admission gate: CS
		throttles the engine's next frame when this is already at the configured limit,
		instead of letting backlog (and therefore latency) grow without bound. */
	int GetReadyDepth();
	/** @brief Pair id shared by a real and the interp that plays before it. */
	void SetNextEnqueuePair(uint64_t pair);
	/** @brief False while L2 quarantine is holding synthesis. */
	bool InterpAllowed();

#ifdef SUPPORT_VK
	static void VkGetPhysicalDevice(VkInstance instance, VkPhysicalDevice* out);
#endif

private:
	std::shared_ptr<XrHMD> hmd = std::make_shared<XrHMD>();
	std::shared_ptr<XrController> hand_left;
	std::shared_ptr<XrController> hand_right;

	std::vector<std::shared_ptr<XrGenericTracker>> generic_trackers;
	std::shared_mutex generic_trackers_mutex;

	void CheckOrInitCompositors(const vr::Texture_t* tex);
	std::unique_ptr<Compositor> compositors[XruEyeCount];
	std::unique_ptr<Compositor> skybox_compositor;
	std::vector<std::shared_ptr<Compositor>> overlay_compositors;

	/**
	 * Updates the current interaction profile in use according to the runtime.
	 * This will set the XrHMD's interaction profile, as well as create the XrControllers
	 * and set their interaction profiles accordingly.
	 * This allows for games to retrieve correct per-controller OpenVR properties that they request.
	 * Called from PumpEvents on an INTERACTION_PROFILE_CHANGED event.
	 */
	void UpdateInteractionProfile();

	/**
	 * Queries xdevs from MNDX_xdev_space, if the runtime supports it. Ignores HMD & Controllers.
	 * These "Generic Trackers" act like an HTC vive tracker, but support no bindings.
	 */
	void CreateGenericTrackers();

	/**
	 * Attempts to force the runtime to expose an interaction profile
	 * (i.e., send an INTERACTION_PROFILE_CHANGED event).
	 * If this is called before the game's actions have been attached to the session by BaseInput
	 * (which is the case for essentially all legacy input games), the session will have to be restarted
	 * to attach the actual inputs. BaseInput handles this when attaching its inputs.
	 */
	void QueryForInteractionProfile();
	void CreateInfoSet();
	void BindInfoSet();

	// Whether we've restarted the session to use the application's rendering API yet
	bool usingApplicationGraphicsAPI = false;

	// The views for the two main eye layers
	XrCompositionLayerProjectionView projectionViews[XruEyeCount];

	// Have we started rendering a frame yet? If not, calling xrEndFrame would result in an error
	bool renderingFrame = false;

	// Were we supposed to start rendering a frame, but couldn't since we were on the
	// early (pre switch to application graphics instance) OpenXR session?
	bool deferredRenderingStart = false;

	// Keep track of if eye textures have been submitted and if we need to create a projection layer for them
	bool submittedEyeTextures = false;

	/** HMD pose from Submit_TextureWithPose; applied at EndFrame so deferred Wait cannot overwrite it. */
	bool twpPoseValid[XruEyeCount] = {};
	vr::HmdMatrix34_t twpHmdPose[XruEyeCount]{};

	// If the app is using PostPresentHandoff then we need to delay when we submit frame data through xrEndFrame
	// until after all frame and layer data has been submitted and PostPresentHandoff is called. Otherwise we
	// might miss overlay elements for GUI or HUDs
	bool postPresentStatus = false;

	// Number of frames rendered for use in frame timing data
	uint32_t nFrameIndex = 0;

	double frameSubmitTimeUs = 0.0;

	// Measured xrWaitFrame returns. Used for logs and GetFrameTiming interval.
	// Cross-thread diagnostic; tear is tolerated.
	double lastWaitGapMs = 0;
	double lastEndFrameMs = 0;
	XrDuration lastPredictedPeriodNs = 0;
	XrBool32 lastShouldRender = XR_TRUE;
	uint32_t waitSeq = 0;
	uint32_t discardedBeginCount = 0;
	uint32_t shouldRenderFalseCount = 0;
	uint32_t skyboxWaitCount = 0;
	uint32_t presenterRepeatCount = 0;
	uint32_t presenterSeq = 0;
	uint32_t lastSlotRepeat = 0;
	uint32_t lastBeginDiscarded = 0;
	uint32_t quarantineEnterCount = 0;
	uint32_t missingTwpCount = 0;

	struct PresenterState;
	std::unique_ptr<PresenterState> presenter;
	void PresenterLoop();
	void PresenterStop();
	void PresenterTryArm();
	void L1Throttle();
	int AllocMailSlot();
	void NoteRealEnqueued();
	void RefreshQuarantine();
	void EnterQuarantine(const char* reason);
	void RescheduleAfterDiscard();
	void EnqueueEye(vr::EVREye eye, const vr::Texture_t* texture, const vr::VRTextureBounds_t* bounds,
	    vr::EVRSubmitFlags submitFlags);
	void PublishPoseSnapshot();
	void ApplyMailboxPoses(const vr::HmdMatrix34_t pose[XruEyeCount], const bool valid[XruEyeCount], bool force);
	void PresenterEndFrame(bool includeProjection, bool includeSkybox);

	// Action set and action used for querying for the interaction profile
	inline static XrActionSet infoSet = XR_NULL_HANDLE;
	XrAction infoAction = XR_NULL_HANDLE;
	XrPath subactionPaths[2] = { XR_NULL_PATH, XR_NULL_PATH };

	// Abstract class for holding a graphics binding.
	class BindingBase {
	public:
		virtual const void* asVoid() = 0;
		virtual ~BindingBase() = default;
	};

	// A wrapper around a graphics binding type, subclassing BindingBase
	// It is templated to allow storing any of the possible graphics bindings and getting a void* to it,
	// as necessary for xrCreateSession (in DrvOpenXR::SetupSession)
	template <typename T>
	class BindingWrapper : public BindingBase {
		const T data;

	public:
		BindingWrapper(T data)
		    : data(data) {}
		~BindingWrapper() override
		{
#if defined(SUPPORT_GL) && !defined(_WIN32)
			if constexpr (std::is_same_v<T, struct XrGraphicsBindingOpenGLXlibKHR>)
				glXDestroyContext(data.xDisplay, data.glxContext);
#endif
		}
		const void* asVoid() override { return &data; }
	};

	// The current graphics binding, used for restarting the session
	inline static std::unique_ptr<BindingBase> graphicsBinding = nullptr;
	static std::unique_ptr<class TemporaryGraphics> temporaryGraphics;
};
