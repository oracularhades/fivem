/*
 * This file is part of the CitizenFX project - http://citizen.re/
 *
 * See LICENSE and MENTIONS in the root of the source tree for information
 * regarding licensing.
 */

//
// ServoSharedTexture.cpp
//
// Game-process (D3D11) half of the ANGLE shared texture bridge.
//
// Responsibilities:
//   1. Allocate a D3D11_RESOURCE_MISC_SHARED BGRA texture on GTA's device.
//   2. Retrieve its legacy DXGI shared handle.
//   3. Write width / height / handle / generation into ServoRenderData
//      shared memory so the Servo subprocess can find it.
//   4. Expose the D3D11 texture pointer so the game's composite pass can
//      sample from it directly (zero-copy; Servo already rendered into it).
//
// The EGL / ANGLE side (Servo subprocess) lives in ServoEGLSurface.cpp.
//

#include "StdInc.h"

// EGL headers must precede ServoSharedTexture.h so that __egl_h_ is defined
// (suppressing the void* forward-declarations) and so all EGL constants are
// available to the ServoChooseEGLConfig / ServoCreateEGLSurfaceFromD3DHandle
// implementations below.
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include "ServoSharedTexture.h"

#include <HostSharedData.h>   // HostSharedData<T>
#include <Error.h>

#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

namespace WRL = Microsoft::WRL;

namespace nui
{

// ─────────────────────────────────────────────────────────────────────────────
//  ServoSharedTextureBridge
// ─────────────────────────────────────────────────────────────────────────────

/* static */
ServoSharedTextureBridge* ServoSharedTextureBridge::Create(ID3D11Device* device,
                                                            const std::string& name,
                                                            int width, int height)
{
	auto* bridge = new ServoSharedTextureBridge();
	if (!bridge->Init(device, name, width, height))
	{
		delete bridge;
		return nullptr;
	}
	return bridge;
}

ServoSharedTextureBridge::~ServoSharedTextureBridge()
{
	// Invalidate shared-memory slot so Servo knows the texture is gone.
	if (m_sharedData)
	{
		m_sharedData->handle     = NULL;
		m_sharedData->ntHandle   = NULL;
		m_sharedData->width      = 0;
		m_sharedData->height     = 0;
		InterlockedIncrement(reinterpret_cast<volatile LONG*>(&m_sharedData->generation));
	}

	// The D3D11 texture itself is freed when m_texture (ComPtr) goes out of scope.
	// The shared HANDLE becomes invalid at that point automatically.
}

bool ServoSharedTextureBridge::Init(ID3D11Device* device,
                                    const std::string& name,
                                    int width, int height)
{
	m_name   = name;
	m_width  = width;
	m_height = height;

	if (!CreateTexture(device, width, height))
	{
		return false;
	}

	// HostSharedData owns a named file-mapping shared with the Servo subprocess.
	// Store it as a unique_ptr so it's destroyed with the bridge.
	m_hostSharedData = std::make_unique<HostSharedData<ServoRenderData>>(name);
	m_sharedData     = m_hostSharedData->operator->();

	PublishHandle();
	return true;
}

bool ServoSharedTextureBridge::CreateTexture(ID3D11Device* /*device*/, int width, int height)
{
	// Release old texture if resizing.
	m_texture.Reset();
	m_sharedHandle = NULL;

	// CreateSharedNUITexture (NUIInitialize.cpp) calls g_origCreateTexture2D directly,
	// bypassing the CreateTexture2DHook / Texture2DWrap wrapper.
	// That wrapper intercepts D3D11_RESOURCE_MISC_SHARED textures and wraps them in
	// Texture2DWrap which exposes IDXGIResourceHack (custom UUID) instead of the
	// standard IDXGIResource — making GetSharedHandle() inaccessible via normal QI.
	extern HRESULT CreateSharedNUITexture(int, int, ID3D11Texture2D**, HANDLE*);

	ID3D11Texture2D* rawTex  = nullptr;
	HANDLE           handle  = NULL;

	HRESULT hr = CreateSharedNUITexture(width, height, &rawTex, &handle);
	if (FAILED(hr) || !rawTex || !handle)
	{
		trace("[ServoSharedTexture] CreateSharedNUITexture failed: 0x%08X\n", hr);
		if (rawTex) rawTex->Release();
		return false;
	}

	m_texture.Attach(rawTex);
	m_sharedHandle = handle;
	return true;
}

void ServoSharedTextureBridge::PublishHandle()
{
	if (!m_sharedData)
	{
		return;
	}

	m_sharedData->handle   = m_sharedHandle;
	m_sharedData->ntHandle = NULL; // populated by NT-handle path if needed
	m_sharedData->width    = m_width;
	m_sharedData->height   = m_height;
	// Bump generation so Servo knows to recreate its EGL surface.
	InterlockedIncrement(reinterpret_cast<volatile LONG*>(&m_sharedData->generation));
}

bool ServoSharedTextureBridge::Resize(int newWidth, int newHeight)
{
	if (newWidth == m_width && newHeight == m_height)
	{
		return true;
	}

	// We need the D3D11 device to recreate the texture.  Ask NUIInitialize
	// for the raw device pointer (same as the rest of nui-core does).
	extern ID3D11Device* GetRawD3D11Device();
	ID3D11Device* device = GetRawD3D11Device();
	if (!device)
	{
		return false;
	}

	m_width  = newWidth;
	m_height = newHeight;

	if (!CreateTexture(device, newWidth, newHeight))
	{
		return false;
	}

	PublishHandle();
	return true;
}

bool ServoSharedTextureBridge::HasNewFrame() const
{
	return m_sharedData &&
	       (InterlockedCompareExchange(&m_sharedData->newFrameReady, 0, 0) != 0);
}

void ServoSharedTextureBridge::ConsumeFrame()
{
	if (m_sharedData)
	{
		InterlockedExchange(&m_sharedData->newFrameReady, 0);
	}
}

// ─────────────────────────────────────────────────────────────────────────────
//  EGL / ANGLE helper (game-process side shim)
// ─────────────────────────────────────────────────────────────────────────────
//
// These thin wrappers load libEGL.dll at runtime (the same DLL that CEF's
// renderer loads) so nui-core doesn't need a link-time dependency on EGL.
//
// In a Servo subprocess these would be called directly via EGL symbols;
// the dynamic-load is only needed when called from the game process
// which may not have a GL context itself.
//

namespace
{
// Lazily resolve an EGL function from libEGL.dll (already loaded by CEF/ANGLE).
template<typename FnPtr>
static FnPtr ResolveEGL(const char* name)
{
	static HMODULE hEGL = GetModuleHandleW(L"libEGL.dll");
	if (!hEGL)
	{
		return nullptr;
	}
	return reinterpret_cast<FnPtr>(GetProcAddress(hEGL, name));
}
} // namespace

/* static */
EGLConfig ServoChooseEGLConfig(EGLDisplay display)
{
	auto _eglChooseConfig    = ResolveEGL<decltype(&eglChooseConfig)>("eglChooseConfig");
	auto _eglGetConfigAttrib = ResolveEGL<decltype(&eglGetConfigAttrib)>("eglGetConfigAttrib");

	if (!_eglChooseConfig || !_eglGetConfigAttrib)
	{
		return nullptr;
	}

	// We need a surface that:
	//   • is 32-bit RGBA
	//   • supports pbuffer creation  (EGL_PBUFFER_BIT)
	//   • can be used as a render target (EGL_SURFACE_TYPE pbuffer)
	//   • can be bound as a GL texture  (EGL_BIND_TO_TEXTURE_RGBA)
	//     – this last attribute is required by eglCreatePbufferFromClientBuffer
	//       when the EGL_TEXTURE_FORMAT / EGL_TEXTURE_TARGET attributes are set.
	const EGLint attribs[] = {
		EGL_RED_SIZE,           8,
		EGL_GREEN_SIZE,         8,
		EGL_BLUE_SIZE,          8,
		EGL_ALPHA_SIZE,         8,
		EGL_BUFFER_SIZE,        32,
		EGL_SURFACE_TYPE,       EGL_PBUFFER_BIT,
		EGL_BIND_TO_TEXTURE_RGBA, EGL_TRUE,
		EGL_NONE
	};

	EGLint numConfigs = 0;
	if (_eglChooseConfig(display, attribs, nullptr, 0, &numConfigs) != EGL_TRUE || numConfigs == 0)
	{
		return nullptr;
	}

	std::vector<EGLConfig> configs(numConfigs);
	if (_eglChooseConfig(display, attribs, configs.data(), numConfigs, &numConfigs) != EGL_TRUE)
	{
		return nullptr;
	}

	// Prefer the first config that exactly matches 8-bit per channel.
	for (EGLConfig cfg : configs)
	{
		EGLint r = 0, g = 0, b = 0, a = 0;
		_eglGetConfigAttrib(display, cfg, EGL_RED_SIZE,   &r);
		_eglGetConfigAttrib(display, cfg, EGL_GREEN_SIZE, &g);
		_eglGetConfigAttrib(display, cfg, EGL_BLUE_SIZE,  &b);
		_eglGetConfigAttrib(display, cfg, EGL_ALPHA_SIZE, &a);
		if (r == 8 && g == 8 && b == 8 && a == 8)
		{
			return cfg;
		}
	}

	return configs.empty() ? nullptr : configs[0];
}

EGLSurface ServoCreateEGLSurfaceFromD3DHandle(EGLDisplay display,
                                               EGLConfig  config,
                                               HANDLE     d3dHandle,
                                               int        width,
                                               int        height)
{
	// EGL_D3D_TEXTURE_2D_SHARE_HANDLE_ANGLE tells ANGLE to open the legacy
	// DXGI shared handle and create an EGL pbuffer surface backed by that
	// D3D11 texture.  Rendering into this surface (with eglMakeCurrent) goes
	// directly into the shared D3D11 texture with no CPU copy.
	//
	// Extension: EGL_ANGLE_d3d_texture_client_buffer (for in-process textures)
	//            EGL_D3D_TEXTURE_2D_SHARE_HANDLE_ANGLE (for cross-process handles)
	//
	// Reference: https://chromium.googlesource.com/angle/angle/+/HEAD/extensions/
	//            EGL_ANGLE_d3d_texture_client_buffer.txt

	auto _eglCreatePbufferFromClientBuffer =
		ResolveEGL<decltype(&eglCreatePbufferFromClientBuffer)>("eglCreatePbufferFromClientBuffer");

	if (!_eglCreatePbufferFromClientBuffer)
	{
		trace("[ServoSharedTexture] eglCreatePbufferFromClientBuffer not found in libEGL.dll\n");
		return EGL_NO_SURFACE;
	}

	// When EGL_TEXTURE_FORMAT / EGL_TEXTURE_TARGET are specified the pbuffer
	// surface can also be used with eglBindTexImage; we include them to match
	// the config chosen by ServoChooseEGLConfig (which requires
	// EGL_BIND_TO_TEXTURE_RGBA).  Servo/WebRender only needs the surface as a
	// render target, not as a bound texture, so these could be omitted if the
	// config doesn't require them.
	const EGLint pbufferAttribs[] = {
		EGL_WIDTH,          width,
		EGL_HEIGHT,         height,
		EGL_TEXTURE_FORMAT, EGL_TEXTURE_RGBA,
		EGL_TEXTURE_TARGET, EGL_TEXTURE_2D,
		EGL_NONE
	};

	EGLSurface surface = _eglCreatePbufferFromClientBuffer(
		display,
		EGL_D3D_TEXTURE_2D_SHARE_HANDLE_ANGLE,
		reinterpret_cast<EGLClientBuffer>(d3dHandle),
		config,
		pbufferAttribs);

	if (surface == EGL_NO_SURFACE)
	{
		auto _eglGetError = ResolveEGL<decltype(&eglGetError)>("eglGetError");
		EGLint err = _eglGetError ? _eglGetError() : -1;
		trace("[ServoSharedTexture] eglCreatePbufferFromClientBuffer failed: EGL error 0x%04X\n", err);
	}

	return surface;
}

void ServoDestroyEGLSurface(EGLDisplay display, EGLSurface surface)
{
	if (surface == EGL_NO_SURFACE)
	{
		return;
	}

	auto _eglDestroySurface = ResolveEGL<decltype(&eglDestroySurface)>("eglDestroySurface");
	if (_eglDestroySurface)
	{
		_eglDestroySurface(display, surface);
	}
}

} // namespace nui
