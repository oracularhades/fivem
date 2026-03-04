/*
 * This file is part of the CitizenFX project - http://citizen.re/
 *
 * See LICENSE and MENTIONS in the root of the source tree for information
 * regarding licensing.
 */

//
// ServoEGLSurface.cpp
//
// Servo-subprocess side of the ANGLE shared texture bridge.
//
// This translation unit runs in the Servo renderer subprocess (not the game
// process).  It:
//   1. Reads ServoRenderData from HostSharedData shared memory.
//   2. Opens the D3D11 shared handle as an ANGLE EGL pbuffer surface.
//   3. Makes that surface current so WebRender's GL calls go into the
//      D3D11 texture owned by the game process.
//   4. Exposes a plain C API (ServoEGL_*) that the Rust Servo integration
//      layer calls via FFI without needing C++ headers.
//
// Threading model:
//   All EGL calls must happen on the same thread that owns the EGL context
//   (WebRender's render thread).  The C API is not thread-safe; the caller
//   (Rust) is responsible for synchronisation.
//

#include "StdInc.h"
#include "ServoSharedTexture.h"

#include <HostSharedData.h>
#include <CfxState.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <string>
#include <cassert>

// ─────────────────────────────────────────────────────────────────────────────
//  Internal state (per "window slot")
// ─────────────────────────────────────────────────────────────────────────────

struct ServoEGLState
{
	// Shared memory, kept alive for the life of the state block.
	HostSharedData<ServoRenderData>* sharedData = nullptr;

	// Current EGL objects.
	EGLDisplay display  = EGL_NO_DISPLAY;
	EGLContext context  = EGL_NO_CONTEXT;
	EGLConfig  config   = nullptr;
	EGLSurface surface  = EGL_NO_SURFACE;

	// Last generation for which we built the surface.
	uint32_t lastGeneration = UINT32_MAX;

	// Dimensions of the current surface.
	int width  = 0;
	int height = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
//  Helpers
// ─────────────────────────────────────────────────────────────────────────────

namespace
{

template<typename FnPtr>
static FnPtr ResolveEGL(const char* sym)
{
	static HMODULE hEGL = GetModuleHandleW(L"libEGL.dll");
	if (!hEGL)
	{
		// Try to load it; on the Servo subprocess path libEGL.dll may not be
		// loaded yet if ANGLE is being initialised for the first time.
		hEGL = LoadLibraryW(L"libEGL.dll");
	}
	return hEGL ? reinterpret_cast<FnPtr>(GetProcAddress(hEGL, sym)) : nullptr;
}

// Create a minimal EGL context on `display` with `config`.
// We need an EGL 2.0 (GLES2) context because that is what ANGLE / WebRender uses.
static EGLContext CreateEGLContext(EGLDisplay display, EGLConfig config,
                                   EGLContext shareContext = EGL_NO_CONTEXT)
{
	auto _eglCreateContext = ResolveEGL<decltype(&eglCreateContext)>("eglCreateContext");
	if (!_eglCreateContext)
	{
		return EGL_NO_CONTEXT;
	}

	const EGLint ctxAttribs[] = {
		EGL_CONTEXT_CLIENT_VERSION, 2,  // OpenGL ES 2.0 compatible
		EGL_NONE
	};

	return _eglCreateContext(display, config, shareContext, ctxAttribs);
}

// (Re-)create the EGL pbuffer surface from the current SharedData handle.
// Destroys the old surface first if one exists.
static bool RebuildSurface(ServoEGLState* state)
{
	assert(state);

	ServoRenderData* rd = state->sharedData->operator->();
	if (!rd || !rd->handle || rd->width <= 0 || rd->height <= 0)
	{
		return false;
	}

	// Tear down the old surface (if any).
	if (state->surface != EGL_NO_SURFACE)
	{
		nui::ServoDestroyEGLSurface(state->display, state->surface);
		state->surface = EGL_NO_SURFACE;
	}

	// Build a new one from the (possibly new) handle.
	state->surface = nui::ServoCreateEGLSurfaceFromD3DHandle(
		state->display,
		state->config,
		rd->handle,
		rd->width,
		rd->height);

	if (state->surface == EGL_NO_SURFACE)
	{
		return false;
	}

	state->lastGeneration = rd->generation;
	state->width          = rd->width;
	state->height         = rd->height;
	return true;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
//  C API  (called from Rust via FFI)
// ─────────────────────────────────────────────────────────────────────────────

extern "C"
{

//
// ServoEGL_Create
//
// Initialises ANGLE for a named servo render slot.
// `slotName`   - must match the name used in ServoSharedTextureBridge::Create()
//                on the game side (e.g. "CfxServoRender_root").
// `display`    - the EGLDisplay to use (obtained from eglGetDisplay or the
//                WebRender EGL helper).
//
// Returns an opaque handle on success, NULL on failure.
//
DLL_EXPORT void* ServoEGL_Create(const char* slotName, EGLDisplay display)
{
	auto* state = new ServoEGLState();

	state->sharedData = new HostSharedData<ServoRenderData>(std::string(slotName));
	state->display    = display;

	// Choose an EGL config matching the game's BGRA texture.
	state->config = nui::ServoChooseEGLConfig(display);
	if (!state->config)
	{
		delete state;
		return nullptr;
	}

	// Create the GLES2 context that WebRender will use.
	state->context = CreateEGLContext(display, state->config);
	if (state->context == EGL_NO_CONTEXT)
	{
		delete state;
		return nullptr;
	}

	// Build the initial render surface.
	if (!RebuildSurface(state))
	{
		// The game may not have published the handle yet; that's fine —
		// ServoEGL_MakeCurrent / ServoEGL_Tick will retry.
	}

	return static_cast<void*>(state);
}

//
// ServoEGL_Destroy
//
// Frees all EGL resources.  Must be called from the render thread.
//
DLL_EXPORT void ServoEGL_Destroy(void* handle)
{
	if (!handle)
	{
		return;
	}

	auto* state = static_cast<ServoEGLState*>(handle);

	auto _eglDestroyContext = ResolveEGL<decltype(&eglDestroyContext)>("eglDestroyContext");
	auto _eglMakeCurrent    = ResolveEGL<decltype(&eglMakeCurrent)>("eglMakeCurrent");

	if (_eglMakeCurrent)
	{
		_eglMakeCurrent(state->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	}

	nui::ServoDestroyEGLSurface(state->display, state->surface);

	if (_eglDestroyContext && state->context != EGL_NO_CONTEXT)
	{
		_eglDestroyContext(state->display, state->context);
	}

	delete state->sharedData;
	delete state;
}

//
// ServoEGL_MakeCurrent
//
// Calls eglMakeCurrent so WebRender's GL calls target the shared D3D11 texture.
// Call this at the start of each Servo render frame, on the render thread.
//
// Returns 1 on success, 0 on failure.
//
DLL_EXPORT int ServoEGL_MakeCurrent(void* handle)
{
	if (!handle)
	{
		return 0;
	}

	auto* state = static_cast<ServoEGLState*>(handle);
	auto _eglMakeCurrent = ResolveEGL<decltype(&eglMakeCurrent)>("eglMakeCurrent");
	if (!_eglMakeCurrent)
	{
		return 0;
	}

	// Check whether the game has updated the shared texture (resize).
	{
		ServoRenderData* rd = state->sharedData->operator->();
		if (rd && rd->generation != state->lastGeneration)
		{
			// Game resized or recreated the texture; rebuild EGL surface.
			RebuildSurface(state);
		}
	}

	if (state->surface == EGL_NO_SURFACE)
	{
		// Still no surface (game hasn't published handle yet); try again.
		RebuildSurface(state);
		if (state->surface == EGL_NO_SURFACE)
		{
			return 0;
		}
	}

	EGLBoolean ok = _eglMakeCurrent(
		state->display,
		state->surface,  // draw
		state->surface,  // read
		state->context);

	return ok == EGL_TRUE ? 1 : 0;
}

//
// ServoEGL_SwapBuffers
//
// Signals to ANGLE that a complete frame has been rendered.
// For a pbuffer surface this is a no-op on most ANGLE back-ends,
// but calling it ensures any deferred flushes are committed to the
// underlying D3D11 render target.
//
// Also increments ServoRenderData::newFrameReady so the game knows
// a new frame is available.
//
DLL_EXPORT void ServoEGL_SwapBuffers(void* handle)
{
	if (!handle)
	{
		return;
	}

	auto* state = static_cast<ServoEGLState*>(handle);
	auto _eglSwapBuffers = ResolveEGL<decltype(&eglSwapBuffers)>("eglSwapBuffers");

	if (_eglSwapBuffers && state->surface != EGL_NO_SURFACE)
	{
		_eglSwapBuffers(state->display, state->surface);
	}

	// Signal the game.
	if (ServoRenderData* rd = state->sharedData->operator->())
	{
		InterlockedExchange(&rd->newFrameReady, 1);
	}
}

//
// ServoEGL_GetDimensions
//
// Returns current surface width/height via out-parameters.
// Returns 1 if a valid surface exists, 0 otherwise.
//
DLL_EXPORT int ServoEGL_GetDimensions(void* handle, int* outWidth, int* outHeight)
{
	if (!handle || !outWidth || !outHeight)
	{
		return 0;
	}

	auto* state = static_cast<ServoEGLState*>(handle);
	if (state->surface == EGL_NO_SURFACE)
	{
		return 0;
	}

	*outWidth  = state->width;
	*outHeight = state->height;
	return 1;
}

//
// ServoEGL_GetProcAddress
//
// Thin shim over eglGetProcAddress so WebRender (Rust) can resolve GL
// extension functions without linking libEGL.dll at compile time.
//
DLL_EXPORT void* ServoEGL_GetProcAddress(const char* name)
{
	auto _eglGetProcAddress = ResolveEGL<decltype(&eglGetProcAddress)>("eglGetProcAddress");
	return _eglGetProcAddress ? reinterpret_cast<void*>(_eglGetProcAddress(name)) : nullptr;
}

} // extern "C"
