// Meta Quest presentation for the Android host.
//
// The PS1 core renders on its own "game" thread at the rate the audio clock
// asks for (60 Hz). OpenXR wants frames at the headset rate (72-120 Hz) and
// xrWaitFrame blocks, which would steal time from the emulator, so the two
// run apart:
//
//   game thread  cxr_submit()  copies the finished frame into a mailbox slot
//   XR thread    frame loop    shows the newest slot on a screen layer
//
// The contexts share objects; GL fences order the copy against the read.
// Everything the C# side needs is behind the cxr_* functions at the bottom.

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <android/log.h>
#include <jni.h>
#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#define LOG_TAG "CrashXR"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#define EXPORT __attribute__((visibility("default")))

// Largest picture per eye. The game image is drawn into a sub-rectangle of
// this with the right display aspect. The swapchain is two of these side by
// side: left eye at x = 0, right eye at x = CAPACITY_W.
#define CAPACITY_W 2048
#define CAPACITY_H 1536

#ifndef GL_SRGB8_ALPHA8
#define GL_SRGB8_ALPHA8 0x8C43
#endif

// ---------------------------------------------------------------------------
// PS1 pad bits, matching RecompOne.Runtime.Hardware.Controller.

enum {
	PAD_SELECT = 1 << 0, PAD_START = 1 << 3,
	PAD_UP = 1 << 4, PAD_RIGHT = 1 << 5, PAD_DOWN = 1 << 6, PAD_LEFT = 1 << 7,
	PAD_L2 = 1 << 8, PAD_R2 = 1 << 9, PAD_L1 = 1 << 10, PAD_R1 = 1 << 11,
	PAD_TRIANGLE = 1 << 12, PAD_CIRCLE = 1 << 13, PAD_CROSS = 1 << 14, PAD_SQUARE = 1 << 15,
};

// Status bits reported to C#.
enum {
	STATUS_RUNNING = 1 << 0,  // session begun
	STATUS_VISIBLE = 1 << 1,  // our layers are on the display
	STATUS_FOCUSED = 1 << 2,  // we receive controller input
	STATUS_EXIT = 1 << 3,     // the runtime wants the app to quit
	STATUS_FAILED = 1 << 4,   // the XR thread could not start
};

typedef struct {
	uint16_t buttons;
	float left_x, left_y;   // -1..1, +y up
	float right_x, right_y;
} PadState;

// ---------------------------------------------------------------------------
// EGL shared by both threads.

static EGLDisplay g_display = EGL_NO_DISPLAY;
static EGLConfig g_config;
static EGLContext g_game_context = EGL_NO_CONTEXT;
static EGLSurface g_game_surface = EGL_NO_SURFACE;

static bool ChooseConfig(void)
{
	const EGLint attributes[] = {
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
		EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
		EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
		EGL_NONE,
	};
	EGLint count = 0;
	return eglChooseConfig(g_display, attributes, &g_config, 1, &count) && count > 0;
}

static EGLContext CreateContext(EGLContext share)
{
	const EGLint attributes[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
	return eglCreateContext(g_display, g_config, share, attributes);
}

static EGLSurface CreatePbuffer(void)
{
	const EGLint attributes[] = { EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE };
	return eglCreatePbufferSurface(g_display, g_config, attributes);
}

// ---------------------------------------------------------------------------
// Mailbox: triple buffer of finished game frames.

typedef struct {
	GLuint texture, texture_right;
	GLuint fbo, fbo_right;  // game-context FBOs; FBOs are not shared
	int width, height;
	float aspect;
	bool stereo;
	float edge_mask;  // fraction of the width blanked on each side (stereo)
	GLsync written;      // game thread finished copying
	GLsync read;         // XR thread finished sampling
} Slot;

static Slot g_slots[3];
static int g_back = 0, g_ready = 1, g_front = 2;
static bool g_ready_new;
static pthread_mutex_t g_mailbox_lock = PTHREAD_MUTEX_INITIALIZER;
static GLuint g_source_fbo;  // game context, wraps the texture handed in

// ---------------------------------------------------------------------------
// XR state, owned by the XR thread unless noted.

static JavaVM *g_vm;
static jobject g_activity;  // global ref
static pthread_t g_thread;
static volatile bool g_thread_started;
static volatile bool g_quit;

static XrInstance g_instance = XR_NULL_HANDLE;
static XrSystemId g_system = XR_NULL_SYSTEM_ID;
static XrSession g_session = XR_NULL_HANDLE;
static XrSpace g_local_space = XR_NULL_HANDLE;
static XrSessionState g_session_state = XR_SESSION_STATE_UNKNOWN;
static bool g_session_running;
static bool g_has_refresh_rate_ext;
static bool g_has_perf_settings_ext;
static bool g_has_pico_controller_ext;
static float g_refresh_rate;

static XrSwapchain g_swapchain = XR_NULL_HANDLE;
static XrSwapchainImageOpenGLESKHR g_images[8];
static GLuint g_image_fbos[8];
static uint32_t g_image_count;
static bool g_swapchain_srgb;
static bool g_swapchain_has_image;
static int g_content_w = 4, g_content_h = 3;
static float g_content_aspect = 4.0f / 3.0f;
static bool g_content_stereo;

static EGLContext g_xr_context = EGL_NO_CONTEXT;
static EGLSurface g_xr_surface = EGL_NO_SURFACE;
static GLuint g_program, g_vao;
static GLint g_linearize_uniform, g_mask_uniform;

// Touch controller inputs the player can put a PS1 button on. Order is the
// cxr_set_binding index and matches QuestVrSettings.Inputs in C#.
enum {
	INPUT_A, INPUT_B, INPUT_X, INPUT_Y,
	INPUT_RIGHT_TRIGGER, INPUT_LEFT_TRIGGER,
	INPUT_RIGHT_GRIP, INPUT_LEFT_GRIP,
	INPUT_MENU, INPUT_RIGHT_STICK_CLICK,
	INPUT_COUNT
};

static XrActionSet g_action_set = XR_NULL_HANDLE;
static XrAction g_stick_left, g_stick_right;
static XrAction g_input_actions[INPUT_COUNT];
static XrAction g_passthrough_toggle;  // left stick click, not remappable
static bool g_toggle_was_down;

static bool g_has_cylinder_ext;
static bool g_has_passthrough_ext;
static XrPassthroughFB g_passthrough = XR_NULL_HANDLE;
static XrPassthroughLayerFB g_passthrough_layer = XR_NULL_HANDLE;
static bool g_passthrough_running;

// Shared with the game thread.
static pthread_mutex_t g_state_lock = PTHREAD_MUTEX_INITIALIZER;
static PadState g_pad;
static volatile int g_status;
// Curved, the viewer sits on the axis and 2.8 m of arc at 1.8 m wraps about
// 90 degrees: large and plainly curved, with the edges still inside the
// lenses' sharp area.
static float g_screen_distance = 1.8f;  // metres in front of the viewer
static float g_screen_width = 2.8f;     // metres (arc length when curved)
static bool g_curved = true;
static bool g_want_passthrough;
static uint16_t g_bindings[INPUT_COUNT] = {
	PAD_CROSS, PAD_CIRCLE, PAD_SQUARE, PAD_TRIANGLE,
	PAD_R1, PAD_L1, PAD_R2, PAD_L2,
	PAD_START, PAD_SELECT,
};

static void SetStatusBit(int bit, bool on)
{
	pthread_mutex_lock(&g_state_lock);
	g_status = on ? (g_status | bit) : (g_status & ~bit);
	pthread_mutex_unlock(&g_state_lock);
}

static bool XrOk(XrResult result, const char *what)
{
	if (XR_SUCCEEDED(result))
		return true;
	char text[XR_MAX_RESULT_STRING_SIZE] = "";
	if (g_instance != XR_NULL_HANDLE)
		xrResultToString(g_instance, result, text);
	LOGE("%s failed: %d %s", what, result, text);
	return false;
}

// ---------------------------------------------------------------------------
// Drawing on the XR thread.

static GLuint CompileShader(GLenum type, const char *source)
{
	GLuint shader = glCreateShader(type);
	glShaderSource(shader, 1, &source, NULL);
	glCompileShader(shader);
	GLint ok = 0;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[512];
		glGetShaderInfoLog(shader, sizeof log, NULL, log);
		LOGE("shader: %s", log);
	}
	return shader;
}

static void CreateBlitProgram(void)
{
	static const char *vertex =
		"#version 300 es\n"
		"out vec2 uv;\n"
		"void main() {\n"
		"  vec2 p = vec2(float(gl_VertexID & 1), float((gl_VertexID >> 1) & 1));\n"
		"  uv = p;\n"
		"  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
		"}\n";
	// The game texture holds sRGB-encoded values in a UNORM format. An sRGB
	// swapchain encodes on write, so decode first to store the same values.
	// The PS1 top row sits at texture row zero, hence the flip.
	static const char *fragment =
		"#version 300 es\n"
		"precision highp float;\n"
		"in vec2 uv;\n"
		"uniform sampler2D image;\n"
		"uniform float linearize;\n"
		"uniform float edge_mask;\n"
		"out vec4 color;\n"
		"vec3 decode(vec3 c) {\n"
		"  return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c));\n"
		"}\n"
		"void main() {\n"
		"  vec3 c = texture(image, vec2(uv.x, 1.0 - uv.y)).rgb;\n"
		"  if (uv.x < edge_mask || uv.x > 1.0 - edge_mask) c = vec3(0.0);\n"
		"  color = vec4(linearize > 0.5 ? decode(c) : c, 1.0);\n"
		"}\n";

	GLuint vs = CompileShader(GL_VERTEX_SHADER, vertex);
	GLuint fs = CompileShader(GL_FRAGMENT_SHADER, fragment);
	g_program = glCreateProgram();
	glAttachShader(g_program, vs);
	glAttachShader(g_program, fs);
	glLinkProgram(g_program);
	glDeleteShader(vs);
	glDeleteShader(fs);
	glUseProgram(g_program);
	glUniform1i(glGetUniformLocation(g_program, "image"), 0);
	g_linearize_uniform = glGetUniformLocation(g_program, "linearize");
	g_mask_uniform = glGetUniformLocation(g_program, "edge_mask");
	glGenVertexArrays(1, &g_vao);
}

static bool CreateSwapchain(void)
{
	uint32_t count = 0;
	int64_t *formats = NULL;
	if (XR_SUCCEEDED(xrEnumerateSwapchainFormats(g_session, 0, &count, NULL)) && count > 0) {
		formats = calloc(count, sizeof *formats);
		if (!formats || XR_FAILED(xrEnumerateSwapchainFormats(g_session, count, &count, formats)))
			count = 0;
	}

	int64_t format = GL_RGBA8;
	g_swapchain_srgb = false;
	for (uint32_t i = 0; i < count; i++)
		if (formats[i] == GL_SRGB8_ALPHA8) {
			format = GL_SRGB8_ALPHA8;
			g_swapchain_srgb = true;
			break;
		}
	LOGI("%u swapchain formats offered, sRGB %s", count, g_swapchain_srgb ? "yes" : "no");
	free(formats);

	XrSwapchainCreateInfo info = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
	info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
	info.format = format;
	info.sampleCount = 1;
	info.width = CAPACITY_W * 2;
	info.height = CAPACITY_H;
	info.faceCount = 1;
	info.arraySize = 1;
	info.mipCount = 1;
	if (!XrOk(xrCreateSwapchain(g_session, &info, &g_swapchain), "xrCreateSwapchain"))
		return false;

	xrEnumerateSwapchainImages(g_swapchain, 0, &g_image_count, NULL);
	if (g_image_count == 0 || g_image_count > 8) {
		LOGE("unexpected swapchain image count %u", g_image_count);
		return false;
	}
	for (uint32_t i = 0; i < g_image_count; i++)
		g_images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR;
	if (!XrOk(xrEnumerateSwapchainImages(g_swapchain, g_image_count, &g_image_count,
	                                     (XrSwapchainImageBaseHeader *)g_images),
	          "xrEnumerateSwapchainImages"))
		return false;

	glGenFramebuffers((GLsizei)g_image_count, g_image_fbos);
	for (uint32_t i = 0; i < g_image_count; i++) {
		glBindFramebuffer(GL_FRAMEBUFFER, g_image_fbos[i]);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
		                       g_images[i].image, 0);
	}
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	LOGI("swapchain %dx%d, %u images, %s", CAPACITY_W * 2, CAPACITY_H, g_image_count,
	     g_swapchain_srgb ? "sRGB" : "UNORM");
	return true;
}

// Picks the rectangle of the swapchain the frame is drawn into: the frame's
// own height, within capacity, with the display aspect the core reported.
static void FitContent(const Slot *slot)
{
	float aspect = slot->aspect > 0.1f ? slot->aspect : 4.0f / 3.0f;
	int h = slot->height < 480 ? 480 : slot->height;
	if (h > CAPACITY_H) h = CAPACITY_H;
	int w = (int)lroundf(h * aspect);
	if (w > CAPACITY_W) {
		w = CAPACITY_W;
		h = (int)lroundf(w / aspect);
	}
	g_content_w = w;
	g_content_h = h;
	g_content_aspect = aspect;
}

static void DrawEye(GLuint texture, int x)
{
	glViewport(x, 0, g_content_w, g_content_h);
	glBindTexture(GL_TEXTURE_2D, texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

// Copies the newest mailbox frame into the swapchain. Returns false when
// no new frame has arrived; the layer then keeps showing the last one.
static bool DrawNewFrame(void)
{
	pthread_mutex_lock(&g_mailbox_lock);
	bool fresh = g_ready_new;
	if (fresh) {
		int t = g_front; g_front = g_ready; g_ready = t;
		g_ready_new = false;
	}
	pthread_mutex_unlock(&g_mailbox_lock);
	if (!fresh)
		return false;

	Slot *slot = &g_slots[g_front];
	if (!slot->texture || !slot->written)
		return false;
	glWaitSync(slot->written, 0, GL_TIMEOUT_IGNORED);

	uint32_t index = 0;
	XrSwapchainImageAcquireInfo acquire = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
	if (!XrOk(xrAcquireSwapchainImage(g_swapchain, &acquire, &index), "xrAcquireSwapchainImage"))
		return false;
	XrSwapchainImageWaitInfo wait = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
	wait.timeout = XR_INFINITE_DURATION;
	xrWaitSwapchainImage(g_swapchain, &wait);

	FitContent(slot);
	g_content_stereo = slot->stereo;
	glBindFramebuffer(GL_FRAMEBUFFER, g_image_fbos[index]);
	glDisable(GL_BLEND);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_CULL_FACE);
	glUseProgram(g_program);
	glUniform1f(g_linearize_uniform, g_swapchain_srgb ? 1.0f : 0.0f);
	// Floating window: the right eye has no image where geometry moved in
	// from beyond the frame. Blank the same border in both eyes so the frame
	// edge reads as one consistent window at screen depth.
	glUniform1f(g_mask_uniform, slot->stereo ? slot->edge_mask : 0.0f);
	glBindVertexArray(g_vao);
	glActiveTexture(GL_TEXTURE0);
	DrawEye(slot->texture, 0);
	if (slot->stereo)
		DrawEye(slot->texture_right, CAPACITY_W);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);

	if (slot->read)
		glDeleteSync(slot->read);
	slot->read = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
	glFlush();

	XrSwapchainImageReleaseInfo release = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
	xrReleaseSwapchainImage(g_swapchain, &release);
	g_swapchain_has_image = true;
	return true;
}

// ---------------------------------------------------------------------------
// Input.

static XrPath Path(const char *text)
{
	XrPath path = XR_NULL_PATH;
	xrStringToPath(g_instance, text, &path);
	return path;
}

static XrAction MakeAction(const char *name, const char *label, XrActionType type)
{
	XrActionCreateInfo info = { XR_TYPE_ACTION_CREATE_INFO };
	strncpy(info.actionName, name, sizeof info.actionName - 1);
	strncpy(info.localizedActionName, label, sizeof info.localizedActionName - 1);
	info.actionType = type;
	XrAction action = XR_NULL_HANDLE;
	XrOk(xrCreateAction(g_action_set, &info, &action), name);
	return action;
}

static bool CreateActions(void)
{
	XrActionSetCreateInfo set = { XR_TYPE_ACTION_SET_CREATE_INFO };
	strcpy(set.actionSetName, "gameplay");
	strcpy(set.localizedActionSetName, "Gameplay");
	if (!XrOk(xrCreateActionSet(g_instance, &set, &g_action_set), "xrCreateActionSet"))
		return false;

	g_stick_left = MakeAction("left_stick", "Move", XR_ACTION_TYPE_VECTOR2F_INPUT);
	g_stick_right = MakeAction("right_stick", "Screen size", XR_ACTION_TYPE_VECTOR2F_INPUT);

	static const struct { const char *name, *label, *path; } inputs[INPUT_COUNT] = {
		{ "a",            "A button",           "/user/hand/right/input/a/click" },
		{ "b",            "B button",           "/user/hand/right/input/b/click" },
		{ "x",            "X button",           "/user/hand/left/input/x/click" },
		{ "y",            "Y button",           "/user/hand/left/input/y/click" },
		{ "right_trigger", "Right trigger",     "/user/hand/right/input/trigger/value" },
		{ "left_trigger", "Left trigger",       "/user/hand/left/input/trigger/value" },
		{ "right_grip",   "Right grip",         "/user/hand/right/input/squeeze/value" },
		{ "left_grip",    "Left grip",          "/user/hand/left/input/squeeze/value" },
		{ "menu",         "Menu button",        "/user/hand/left/input/menu/click" },
		{ "right_click",  "Right stick click",  "/user/hand/right/input/thumbstick/click" },
	};

	XrActionSuggestedBinding bindings[INPUT_COUNT + 3];
	uint32_t count = 0;
	for (int i = 0; i < INPUT_COUNT; i++) {
		g_input_actions[i] = MakeAction(inputs[i].name, inputs[i].label,
		                                XR_ACTION_TYPE_BOOLEAN_INPUT);
		bindings[count].action = g_input_actions[i];
		bindings[count].binding = Path(inputs[i].path);
		count++;
	}
	g_passthrough_toggle = MakeAction("passthrough", "Toggle passthrough",
	                                  XR_ACTION_TYPE_BOOLEAN_INPUT);
	bindings[count].action = g_passthrough_toggle;
	bindings[count].binding = Path("/user/hand/left/input/thumbstick/click");
	count++;
	bindings[count].action = g_stick_left;
	bindings[count].binding = Path("/user/hand/left/input/thumbstick");
	count++;
	bindings[count].action = g_stick_right;
	bindings[count].binding = Path("/user/hand/right/input/thumbstick");
	count++;

	XrInteractionProfileSuggestedBinding suggested = {
		XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
	suggested.interactionProfile = Path("/interaction_profiles/oculus/touch_controller");
	suggested.suggestedBindings = bindings;
	suggested.countSuggestedBindings = count;
	if (!XrOk(xrSuggestInteractionProfileBindings(g_instance, &suggested),
	          "xrSuggestInteractionProfileBindings"))
		return false;

	// Pico's controllers answer to a profile of their own, with the same
	// button paths. A runtime that does not know it rejects it, which only
	// matters on a Pico, where the Touch suggestion above is the one ignored.
	if (g_has_pico_controller_ext) {
		suggested.interactionProfile = Path("/interaction_profiles/bytedance/pico4_controller");
		if (XR_FAILED(xrSuggestInteractionProfileBindings(g_instance, &suggested)))
			LOGW("Pico 4 controller profile rejected by this runtime");
		else
			LOGI("Pico 4 controller bindings suggested");
	}

	XrSessionActionSetsAttachInfo attach = { XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
	attach.countActionSets = 1;
	attach.actionSets = &g_action_set;
	return XrOk(xrAttachSessionActionSets(g_session, &attach), "xrAttachSessionActionSets");
}

static bool ReadBool(XrAction action)
{
	XrActionStateGetInfo get = { XR_TYPE_ACTION_STATE_GET_INFO };
	get.action = action;
	XrActionStateBoolean state = { XR_TYPE_ACTION_STATE_BOOLEAN };
	return XR_SUCCEEDED(xrGetActionStateBoolean(g_session, &get, &state)) &&
	       state.isActive && state.currentState;
}

static XrVector2f ReadStick(XrAction action)
{
	XrActionStateGetInfo get = { XR_TYPE_ACTION_STATE_GET_INFO };
	get.action = action;
	XrActionStateVector2f state = { XR_TYPE_ACTION_STATE_VECTOR2F };
	XrVector2f zero = { 0, 0 };
	if (XR_FAILED(xrGetActionStateVector2f(g_session, &get, &state)) || !state.isActive)
		return zero;
	return state.currentState;
}

// Crash reads a digital pad, so the stick becomes eight-way directions.
static uint16_t StickToDpad(XrVector2f stick)
{
	const float deadzone = 0.45f;
	float length = sqrtf(stick.x * stick.x + stick.y * stick.y);
	if (length < deadzone)
		return 0;
	// 22.5 degrees either side of each diagonal counts as that diagonal.
	float angle = atan2f(stick.y, stick.x);
	int sector = (int)lroundf(angle / (float)(M_PI / 4.0)) & 7;
	static const uint16_t directions[8] = {
		PAD_RIGHT, PAD_RIGHT | PAD_UP, PAD_UP, PAD_UP | PAD_LEFT,
		PAD_LEFT, PAD_LEFT | PAD_DOWN, PAD_DOWN, PAD_DOWN | PAD_RIGHT,
	};
	return directions[sector];
}

static void SyncInput(float delta_seconds)
{
	PadState pad = { 0 };
	if (g_session_state == XR_SESSION_STATE_FOCUSED) {
		XrActiveActionSet active = { g_action_set, XR_NULL_PATH };
		XrActionsSyncInfo sync = { XR_TYPE_ACTIONS_SYNC_INFO };
		sync.countActiveActionSets = 1;
		sync.activeActionSets = &active;
		if (XR_SUCCEEDED(xrSyncActions(g_session, &sync))) {
			pthread_mutex_lock(&g_state_lock);
			for (int i = 0; i < INPUT_COUNT; i++)
				if (ReadBool(g_input_actions[i]))
					pad.buttons |= g_bindings[i];
			pthread_mutex_unlock(&g_state_lock);

			bool toggle = ReadBool(g_passthrough_toggle);
			if (toggle && !g_toggle_was_down && g_passthrough != XR_NULL_HANDLE) {
				pthread_mutex_lock(&g_state_lock);
				g_want_passthrough = !g_want_passthrough;
				pthread_mutex_unlock(&g_state_lock);
			}
			g_toggle_was_down = toggle;
			XrVector2f left = ReadStick(g_stick_left);
			XrVector2f right = ReadStick(g_stick_right);
			pad.buttons |= StickToDpad(left);
			pad.left_x = left.x; pad.left_y = left.y;
			pad.right_x = right.x; pad.right_y = right.y;

			// The right stick is free in Crash; pushing it resizes the screen.
			if (fabsf(right.y) > 0.3f) {
				pthread_mutex_lock(&g_state_lock);
				float width = g_screen_width * (1.0f + right.y * 0.9f * delta_seconds);
				g_screen_width = fminf(fmaxf(width, 1.0f), 8.0f);
				pthread_mutex_unlock(&g_state_lock);
			}
		}
	}
	pthread_mutex_lock(&g_state_lock);
	g_pad = pad;
	pthread_mutex_unlock(&g_state_lock);
}

// ---------------------------------------------------------------------------
// Refresh rate: 60 fps content looks smoothest at a multiple of 60.

// The PS1 core runs on one game thread whose frame time spikes in busy
// scenes; the GPU has headroom. Ask for sustained-high CPU clocks and leave
// the GPU level to the system.
static void RequestCpuLevel(void)
{
	if (!g_has_perf_settings_ext)
		return;
	PFN_xrPerfSettingsSetPerformanceLevelEXT set_level = NULL;
	xrGetInstanceProcAddr(g_instance, "xrPerfSettingsSetPerformanceLevelEXT",
	                      (PFN_xrVoidFunction *)&set_level);
	if (set_level && XrOk(set_level(g_session, XR_PERF_SETTINGS_DOMAIN_CPU_EXT,
	                                XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT),
	                      "xrPerfSettingsSetPerformanceLevelEXT"))
		LOGI("CPU performance level: sustained high");
}

static void ChooseRefreshRate(void)
{
	if (!g_has_refresh_rate_ext)
		return;
	PFN_xrEnumerateDisplayRefreshRatesFB enumerate = NULL;
	PFN_xrRequestDisplayRefreshRateFB request = NULL;
	xrGetInstanceProcAddr(g_instance, "xrEnumerateDisplayRefreshRatesFB",
	                      (PFN_xrVoidFunction *)&enumerate);
	xrGetInstanceProcAddr(g_instance, "xrRequestDisplayRefreshRateFB",
	                      (PFN_xrVoidFunction *)&request);
	if (!enumerate || !request)
		return;

	uint32_t count = 0;
	float *rates = NULL;
	if (XR_SUCCEEDED(enumerate(g_session, 0, &count, NULL)) && count > 0) {
		rates = calloc(count, sizeof *rates);
		if (!rates || XR_FAILED(enumerate(g_session, count, &count, rates)))
			count = 0;
	}

	float best = 0;
	for (uint32_t i = 0; i < count; i++) {
		LOGI("display refresh rate available: %.1f", rates[i]);
		bool multiple = fabsf(fmodf(rates[i] + 0.5f, 60.0f) - 0.5f) < 1.0f;
		bool best_multiple = best > 0 && fabsf(fmodf(best + 0.5f, 60.0f) - 0.5f) < 1.0f;
		if (best == 0 || (multiple && !best_multiple) ||
		    (multiple == best_multiple && !multiple && rates[i] < best))
			best = rates[i];
	}
	free(rates);
	if (best > 0 && XR_SUCCEEDED(request(g_session, best))) {
		g_refresh_rate = best;
		LOGI("display refresh rate requested: %.1f", best);
	}
}

// ---------------------------------------------------------------------------
// XR thread.

static bool CreateInstance(void)
{
	PFN_xrInitializeLoaderKHR initialize = NULL;
	if (XR_FAILED(xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR",
	                                    (PFN_xrVoidFunction *)&initialize)) || !initialize) {
		LOGE("loader has no xrInitializeLoaderKHR");
		return false;
	}
	XrLoaderInitInfoAndroidKHR loader = { XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR };
	loader.applicationVM = g_vm;
	loader.applicationContext = g_activity;
	if (!XrOk(initialize((const XrLoaderInitInfoBaseHeaderKHR *)&loader), "xrInitializeLoaderKHR"))
		return false;

	uint32_t count = 0;
	XrExtensionProperties *available = NULL;
	if (XR_SUCCEEDED(xrEnumerateInstanceExtensionProperties(NULL, 0, &count, NULL)) && count > 0) {
		available = calloc(count, sizeof *available);
		if (available) {
			for (uint32_t i = 0; i < count; i++)
				available[i].type = XR_TYPE_EXTENSION_PROPERTIES;
			if (XR_FAILED(xrEnumerateInstanceExtensionProperties(NULL, count, &count, available)))
				count = 0;
		} else {
			count = 0;
		}
	}
	for (uint32_t i = 0; i < count; i++)
	{
		const char *name = available[i].extensionName;
		if (!strcmp(name, XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME))
			g_has_refresh_rate_ext = true;
		else if (!strcmp(name, XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME))
			g_has_cylinder_ext = true;
		else if (!strcmp(name, XR_FB_PASSTHROUGH_EXTENSION_NAME))
			g_has_passthrough_ext = true;
		else if (!strcmp(name, XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME))
			g_has_perf_settings_ext = true;
		else if (!strcmp(name, XR_BD_CONTROLLER_INTERACTION_EXTENSION_NAME))
			g_has_pico_controller_ext = true;
	}
	free(available);

	const char *extensions[7] = {
		XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
		XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME,
	};
	uint32_t extension_count = 2;
	if (g_has_refresh_rate_ext)
		extensions[extension_count++] = XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME;
	if (g_has_cylinder_ext)
		extensions[extension_count++] = XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME;
	if (g_has_passthrough_ext)
		extensions[extension_count++] = XR_FB_PASSTHROUGH_EXTENSION_NAME;
	if (g_has_perf_settings_ext)
		extensions[extension_count++] = XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME;
	if (g_has_pico_controller_ext)
		extensions[extension_count++] = XR_BD_CONTROLLER_INTERACTION_EXTENSION_NAME;
	LOGI("extensions: refresh rate %d, cylinder %d, passthrough %d",
	     g_has_refresh_rate_ext, g_has_cylinder_ext, g_has_passthrough_ext);

	XrInstanceCreateInfoAndroidKHR android = { XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR };
	android.applicationVM = g_vm;
	android.applicationActivity = g_activity;

	XrInstanceCreateInfo create = { XR_TYPE_INSTANCE_CREATE_INFO };
	create.next = &android;
	strcpy(create.applicationInfo.applicationName, "Crash Bandicoot Launcher");
	create.applicationInfo.applicationVersion = 1;
	strcpy(create.applicationInfo.engineName, "RecompOne");
	create.applicationInfo.apiVersion = XR_API_VERSION_1_0;
	create.enabledExtensionCount = extension_count;
	create.enabledExtensionNames = extensions;
	if (!XrOk(xrCreateInstance(&create, &g_instance), "xrCreateInstance"))
		return false;

	XrSystemGetInfo system = { XR_TYPE_SYSTEM_GET_INFO };
	system.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	return XrOk(xrGetSystem(g_instance, &system, &g_system), "xrGetSystem");
}

// Passthrough is created paused and started when the player asks for it,
// so the cameras only run while it is visible.
static void CreatePassthrough(void)
{
	if (!g_has_passthrough_ext)
		return;
	PFN_xrCreatePassthroughFB create = NULL;
	PFN_xrCreatePassthroughLayerFB create_layer = NULL;
	xrGetInstanceProcAddr(g_instance, "xrCreatePassthroughFB", (PFN_xrVoidFunction *)&create);
	xrGetInstanceProcAddr(g_instance, "xrCreatePassthroughLayerFB",
	                      (PFN_xrVoidFunction *)&create_layer);
	if (!create || !create_layer)
		return;

	XrPassthroughCreateInfoFB info = { XR_TYPE_PASSTHROUGH_CREATE_INFO_FB };
	if (!XrOk(create(g_session, &info, &g_passthrough), "xrCreatePassthroughFB")) {
		g_passthrough = XR_NULL_HANDLE;
		return;
	}
	XrPassthroughLayerCreateInfoFB layer = { XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB };
	layer.passthrough = g_passthrough;
	layer.purpose = XR_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION_FB;
	if (!XrOk(create_layer(g_session, &layer, &g_passthrough_layer), "xrCreatePassthroughLayerFB"))
		g_passthrough_layer = XR_NULL_HANDLE;
}

static void SyncPassthrough(bool want)
{
	if (g_passthrough == XR_NULL_HANDLE || g_passthrough_layer == XR_NULL_HANDLE ||
	    want == g_passthrough_running)
		return;
	PFN_xrPassthroughStartFB start = NULL;
	PFN_xrPassthroughPauseFB pause = NULL;
	PFN_xrPassthroughLayerResumeFB resume_layer = NULL;
	PFN_xrPassthroughLayerPauseFB pause_layer = NULL;
	xrGetInstanceProcAddr(g_instance, "xrPassthroughStartFB", (PFN_xrVoidFunction *)&start);
	xrGetInstanceProcAddr(g_instance, "xrPassthroughPauseFB", (PFN_xrVoidFunction *)&pause);
	xrGetInstanceProcAddr(g_instance, "xrPassthroughLayerResumeFB",
	                      (PFN_xrVoidFunction *)&resume_layer);
	xrGetInstanceProcAddr(g_instance, "xrPassthroughLayerPauseFB",
	                      (PFN_xrVoidFunction *)&pause_layer);
	if (!start || !pause || !resume_layer || !pause_layer)
		return;

	bool ok = want
		? XrOk(start(g_passthrough), "xrPassthroughStartFB") &&
		  XrOk(resume_layer(g_passthrough_layer), "xrPassthroughLayerResumeFB")
		: XrOk(pause_layer(g_passthrough_layer), "xrPassthroughLayerPauseFB") &&
		  XrOk(pause(g_passthrough), "xrPassthroughPauseFB");
	if (ok) {
		g_passthrough_running = want;
		LOGI("passthrough %s", want ? "on" : "off");
	}
}

static bool CreateSession(void)
{
	PFN_xrGetOpenGLESGraphicsRequirementsKHR requirements_fn = NULL;
	xrGetInstanceProcAddr(g_instance, "xrGetOpenGLESGraphicsRequirementsKHR",
	                      (PFN_xrVoidFunction *)&requirements_fn);
	XrGraphicsRequirementsOpenGLESKHR requirements = {
		XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR };
	if (!requirements_fn ||
	    !XrOk(requirements_fn(g_instance, g_system, &requirements),
	          "xrGetOpenGLESGraphicsRequirementsKHR"))
		return false;

	XrGraphicsBindingOpenGLESAndroidKHR binding = {
		XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR };
	binding.display = g_display;
	binding.config = g_config;
	binding.context = g_xr_context;

	XrSessionCreateInfo create = { XR_TYPE_SESSION_CREATE_INFO };
	create.next = &binding;
	create.systemId = g_system;
	if (!XrOk(xrCreateSession(g_instance, &create, &g_session), "xrCreateSession"))
		return false;

	XrReferenceSpaceCreateInfo space = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
	space.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
	space.poseInReferenceSpace.orientation.w = 1.0f;
	if (!XrOk(xrCreateReferenceSpace(g_session, &space, &g_local_space), "xrCreateReferenceSpace"))
		return false;

	CreatePassthrough();
	return CreateActions() && CreateSwapchain();
}

static void HandleEvents(void)
{
	XrEventDataBuffer event = { XR_TYPE_EVENT_DATA_BUFFER };
	while (xrPollEvent(g_instance, &event) == XR_SUCCESS) {
		switch (event.type) {
		case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
			const XrEventDataSessionStateChanged *changed =
				(const XrEventDataSessionStateChanged *)&event;
			g_session_state = changed->state;
			LOGI("session state %d", g_session_state);
			switch (g_session_state) {
			case XR_SESSION_STATE_READY: {
				XrSessionBeginInfo begin = { XR_TYPE_SESSION_BEGIN_INFO };
				begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
				if (XrOk(xrBeginSession(g_session, &begin), "xrBeginSession")) {
					g_session_running = true;
					ChooseRefreshRate();
					RequestCpuLevel();
				}
				break;
			}
			case XR_SESSION_STATE_STOPPING:
				xrEndSession(g_session);
				g_session_running = false;
				break;
			case XR_SESSION_STATE_EXITING:
			case XR_SESSION_STATE_LOSS_PENDING:
				SetStatusBit(STATUS_EXIT, true);
				break;
			default:
				break;
			}
			SetStatusBit(STATUS_RUNNING, g_session_running);
			SetStatusBit(STATUS_VISIBLE, g_session_state == XR_SESSION_STATE_VISIBLE ||
			                             g_session_state == XR_SESSION_STATE_FOCUSED);
			SetStatusBit(STATUS_FOCUSED, g_session_state == XR_SESSION_STATE_FOCUSED);
			break;
		}
		case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
			SetStatusBit(STATUS_EXIT, true);
			break;
		default:
			break;
		}
		event = (XrEventDataBuffer){ XR_TYPE_EVENT_DATA_BUFFER };
	}
}

static void RunFrame(void)
{
	XrFrameWaitInfo wait_info = { XR_TYPE_FRAME_WAIT_INFO };
	XrFrameState frame = { XR_TYPE_FRAME_STATE };
	if (!XrOk(xrWaitFrame(g_session, &wait_info, &frame), "xrWaitFrame"))
		return;
	XrFrameBeginInfo begin = { XR_TYPE_FRAME_BEGIN_INFO };
	if (!XrOk(xrBeginFrame(g_session, &begin), "xrBeginFrame"))
		return;

	float delta = frame.predictedDisplayPeriod > 0
		? (float)(frame.predictedDisplayPeriod / 1e9) : 1.0f / 72.0f;
	SyncInput(delta);

	pthread_mutex_lock(&g_state_lock);
	float width = g_screen_width;
	float distance = g_screen_distance;
	bool curved = g_curved && g_has_cylinder_ext;
	bool want_passthrough = g_want_passthrough;
	pthread_mutex_unlock(&g_state_lock);
	SyncPassthrough(want_passthrough);

	static int logged_curved = -1;
	if (logged_curved != (int)curved) {
		logged_curved = curved;
		LOGI("view: %s screen, %.2f m wide at %.2f m", curved ? "curved" : "flat", width, distance);
	}

	XrCompositionLayerPassthroughFB passthrough = { XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB };
	XrCompositionLayerQuad quads[2];
	XrCompositionLayerCylinderKHR cylinders[2];
	const XrCompositionLayerBaseHeader *layers[3];
	uint32_t layer_count = 0;

	if (frame.shouldRender) {
		if (g_passthrough_running) {
			passthrough.flags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
			passthrough.layerHandle = g_passthrough_layer;
			layers[layer_count++] = (const XrCompositionLayerBaseHeader *)&passthrough;
		}

		DrawNewFrame();
		if (g_swapchain_has_image) {
			// Mono: one layer for both eyes. Stereo: each eye its own half.
			int eyes = g_content_stereo ? 2 : 1;
			for (int eye = 0; eye < eyes; eye++) {
				XrEyeVisibility visibility = eyes == 1 ? XR_EYE_VISIBILITY_BOTH
					: eye == 0 ? XR_EYE_VISIBILITY_LEFT : XR_EYE_VISIBILITY_RIGHT;
				XrSwapchainSubImage image = { 0 };
				image.swapchain = g_swapchain;
				image.imageRect.offset.x = eye * CAPACITY_W;
				image.imageRect.extent.width = g_content_w;
				image.imageRect.extent.height = g_content_h;

				if (curved) {
					// The viewer sits on the cylinder's axis, so every column
					// of the picture is the same distance away. The arc length
					// matches the flat screen's width.
					XrCompositionLayerCylinderKHR *cylinder = &cylinders[eye];
					*cylinder = (XrCompositionLayerCylinderKHR){ XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR };
					cylinder->space = g_local_space;
					cylinder->eyeVisibility = visibility;
					cylinder->subImage = image;
					cylinder->pose.orientation.w = 1.0f;
					cylinder->radius = distance;
					cylinder->centralAngle = width / distance;
					cylinder->aspectRatio = g_content_aspect;
					layers[layer_count++] = (const XrCompositionLayerBaseHeader *)cylinder;
				} else {
					XrCompositionLayerQuad *quad = &quads[eye];
					*quad = (XrCompositionLayerQuad){ XR_TYPE_COMPOSITION_LAYER_QUAD };
					quad->space = g_local_space;
					quad->eyeVisibility = visibility;
					quad->subImage = image;
					quad->pose.orientation.w = 1.0f;
					quad->pose.position.z = -distance;
					quad->size.width = width;
					quad->size.height = width / g_content_aspect;
					layers[layer_count++] = (const XrCompositionLayerBaseHeader *)quad;
				}
			}
		}
	}

	XrFrameEndInfo end = { XR_TYPE_FRAME_END_INFO };
	end.displayTime = frame.predictedDisplayTime;
	end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	end.layerCount = layer_count;
	end.layers = layers;
	XrOk(xrEndFrame(g_session, &end), "xrEndFrame");
}

static void Cleanup(void)
{
	if (g_swapchain != XR_NULL_HANDLE) {
		glDeleteFramebuffers((GLsizei)g_image_count, g_image_fbos);
		xrDestroySwapchain(g_swapchain);
		g_swapchain = XR_NULL_HANDLE;
	}
	if (g_program) glDeleteProgram(g_program);
	if (g_vao) glDeleteVertexArrays(1, &g_vao);
	g_program = g_vao = 0;
	if (g_passthrough_layer != XR_NULL_HANDLE) {
		PFN_xrDestroyPassthroughLayerFB destroy = NULL;
		xrGetInstanceProcAddr(g_instance, "xrDestroyPassthroughLayerFB",
		                      (PFN_xrVoidFunction *)&destroy);
		if (destroy) destroy(g_passthrough_layer);
		g_passthrough_layer = XR_NULL_HANDLE;
	}
	if (g_passthrough != XR_NULL_HANDLE) {
		PFN_xrDestroyPassthroughFB destroy = NULL;
		xrGetInstanceProcAddr(g_instance, "xrDestroyPassthroughFB",
		                      (PFN_xrVoidFunction *)&destroy);
		if (destroy) destroy(g_passthrough);
		g_passthrough = XR_NULL_HANDLE;
	}
	g_passthrough_running = false;
	if (g_action_set != XR_NULL_HANDLE) xrDestroyActionSet(g_action_set);
	g_action_set = XR_NULL_HANDLE;
	if (g_local_space != XR_NULL_HANDLE) xrDestroySpace(g_local_space);
	g_local_space = XR_NULL_HANDLE;
	if (g_session != XR_NULL_HANDLE) xrDestroySession(g_session);
	g_session = XR_NULL_HANDLE;
	if (g_instance != XR_NULL_HANDLE) xrDestroyInstance(g_instance);
	g_instance = XR_NULL_HANDLE;

	eglMakeCurrent(g_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	if (g_xr_surface != EGL_NO_SURFACE) eglDestroySurface(g_display, g_xr_surface);
	if (g_xr_context != EGL_NO_CONTEXT) eglDestroyContext(g_display, g_xr_context);
	g_xr_surface = EGL_NO_SURFACE;
	g_xr_context = EGL_NO_CONTEXT;
}

static void *XrThread(void *unused)
{
	(void)unused;
	JNIEnv *env = NULL;
	(*g_vm)->AttachCurrentThread(g_vm, &env, NULL);

	g_xr_context = CreateContext(g_game_context);
	g_xr_surface = CreatePbuffer();
	bool ok = g_xr_context != EGL_NO_CONTEXT && g_xr_surface != EGL_NO_SURFACE &&
	          eglMakeCurrent(g_display, g_xr_surface, g_xr_surface, g_xr_context);
	if (!ok)
		LOGE("XR EGL context failed (0x%x)", eglGetError());
	if (ok) {
		CreateBlitProgram();
		ok = CreateInstance() && CreateSession();
	}
	if (!ok) {
		SetStatusBit(STATUS_FAILED | STATUS_EXIT, true);
	} else {
		while (!g_quit) {
			HandleEvents();
			if (g_session_running)
				RunFrame();
			else
				usleep(10000);
		}
	}

	Cleanup();
	(*g_vm)->DetachCurrentThread(g_vm);
	return NULL;
}

// ---------------------------------------------------------------------------
// API for C#.

// Game thread: creates the context the PS1 renderer draws with and makes it
// current. There is no window; every pass renders into FBOs.
EXPORT int cxr_game_context_create(void)
{
	g_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	if (g_display == EGL_NO_DISPLAY || !eglInitialize(g_display, NULL, NULL))
		return -1;
	if (!ChooseConfig())
		return -2;
	g_game_context = CreateContext(EGL_NO_CONTEXT);
	if (g_game_context == EGL_NO_CONTEXT)
		return -3;
	g_game_surface = CreatePbuffer();
	if (g_game_surface == EGL_NO_SURFACE)
		return -4;
	if (!eglMakeCurrent(g_display, g_game_surface, g_game_surface, g_game_context))
		return -5;
	glGenFramebuffers(1, &g_source_fbo);
	return 0;
}

EXPORT void cxr_game_context_destroy(void)
{
	for (int i = 0; i < 3; i++) {
		Slot *slot = &g_slots[i];
		if (slot->written) glDeleteSync(slot->written);
		if (slot->read) glDeleteSync(slot->read);
		if (slot->fbo) glDeleteFramebuffers(1, &slot->fbo);
		if (slot->fbo_right) glDeleteFramebuffers(1, &slot->fbo_right);
		if (slot->texture) glDeleteTextures(1, &slot->texture);
		if (slot->texture_right) glDeleteTextures(1, &slot->texture_right);
		memset(slot, 0, sizeof *slot);
	}
	if (g_source_fbo) glDeleteFramebuffers(1, &g_source_fbo);
	g_source_fbo = 0;
	eglMakeCurrent(g_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	if (g_game_surface != EGL_NO_SURFACE) eglDestroySurface(g_display, g_game_surface);
	if (g_game_context != EGL_NO_CONTEXT) eglDestroyContext(g_display, g_game_context);
	g_game_surface = EGL_NO_SURFACE;
	g_game_context = EGL_NO_CONTEXT;
}

// Any thread, after cxr_game_context_create. `vm` is the JavaVM*, `activity`
// a JNI reference to the Activity.
EXPORT int cxr_start(JavaVM *vm, jobject activity)
{
	if (g_thread_started)
		return 0;
	g_vm = vm;
	JNIEnv *env = NULL;
	bool attached = false;
	if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) {
		(*vm)->AttachCurrentThread(vm, &env, NULL);
		attached = true;
	}
	g_activity = (*env)->NewGlobalRef(env, activity);
	if (attached)
		(*vm)->DetachCurrentThread(vm);

	g_quit = false;
	g_status = 0;
	if (pthread_create(&g_thread, NULL, XrThread, NULL) != 0)
		return -1;
	g_thread_started = true;
	return 0;
}

EXPORT void cxr_stop(void)
{
	if (!g_thread_started)
		return;
	g_quit = true;
	pthread_join(g_thread, NULL);
	g_thread_started = false;
}

static void AllocateSlotTexture(GLuint *texture, GLuint *fbo, int width, int height)
{
	if (!*texture) {
		glGenTextures(1, texture);
		glGenFramebuffers(1, fbo);
	}
	glBindTexture(GL_TEXTURE_2D, *texture);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, *fbo);
	glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *texture, 0);
}

static void CopyInto(GLuint source, GLuint fbo, int width, int height)
{
	glBindFramebuffer(GL_READ_FRAMEBUFFER, g_source_fbo);
	glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, source, 0);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
	glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
}

// Game thread: hands over the frame the core just finished. `texture` is the
// left-eye (or only) present texture, `texture_right` the right eye or 0 for
// a mono frame. Both are copied, so the core may reuse them straight away.
// `edge_mask` is the stereo border to blank on each side, as a width fraction.
EXPORT void cxr_submit(GLuint texture, GLuint texture_right, int width, int height, float aspect,
                       float edge_mask)
{
	if (!texture || width <= 0 || height <= 0)
		return;
	Slot *slot = &g_slots[g_back];

	// Do not overwrite what the XR thread may still be sampling.
	if (slot->read) {
		glWaitSync(slot->read, 0, GL_TIMEOUT_IGNORED);
		glDeleteSync(slot->read);
		slot->read = 0;
	}
	bool resized = slot->width != width || slot->height != height;
	if (resized || !slot->texture)
		AllocateSlotTexture(&slot->texture, &slot->fbo, width, height);
	if (texture_right && (resized || !slot->texture_right))
		AllocateSlotTexture(&slot->texture_right, &slot->fbo_right, width, height);
	slot->width = width;
	slot->height = height;
	slot->aspect = aspect;
	slot->stereo = texture_right != 0;
	slot->edge_mask = fminf(fmaxf(edge_mask, 0.0f), 0.1f);

	glDisable(GL_SCISSOR_TEST);
	CopyInto(texture, slot->fbo, width, height);
	if (texture_right)
		CopyInto(texture_right, slot->fbo_right, width, height);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);

	if (slot->written)
		glDeleteSync(slot->written);
	slot->written = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
	glFlush();

	pthread_mutex_lock(&g_mailbox_lock);
	int t = g_back; g_back = g_ready; g_ready = t;
	g_ready_new = true;
	pthread_mutex_unlock(&g_mailbox_lock);
}

// Any thread: the latest controller state and session status bits.
EXPORT int cxr_poll(PadState *pad)
{
	pthread_mutex_lock(&g_state_lock);
	if (pad)
		*pad = g_pad;
	int status = g_status;
	pthread_mutex_unlock(&g_state_lock);
	return status;
}

EXPORT float cxr_get_screen_width(void)
{
	pthread_mutex_lock(&g_state_lock);
	float width = g_screen_width;
	pthread_mutex_unlock(&g_state_lock);
	return width;
}

EXPORT void cxr_set_screen(float width, float distance)
{
	pthread_mutex_lock(&g_state_lock);
	if (width >= 1.0f && width <= 8.0f) g_screen_width = width;
	if (distance >= 1.0f && distance <= 10.0f) g_screen_distance = distance;
	pthread_mutex_unlock(&g_state_lock);
}

EXPORT float cxr_refresh_rate(void) { return g_refresh_rate; }

// Puts PS1 buttons (Controller bits, 0 for none) on a Touch input.
EXPORT void cxr_set_binding(int input, int buttons)
{
	if (input < 0 || input >= INPUT_COUNT)
		return;
	pthread_mutex_lock(&g_state_lock);
	g_bindings[input] = (uint16_t)buttons;
	pthread_mutex_unlock(&g_state_lock);
}

EXPORT void cxr_set_view(int curved, int passthrough)
{
	pthread_mutex_lock(&g_state_lock);
	g_curved = curved != 0;
	g_want_passthrough = passthrough != 0;
	pthread_mutex_unlock(&g_state_lock);
}

// Passthrough as the player last left it (the left stick toggles it).
EXPORT int cxr_get_passthrough(void)
{
	pthread_mutex_lock(&g_state_lock);
	int on = g_want_passthrough;
	pthread_mutex_unlock(&g_state_lock);
	return on;
}
