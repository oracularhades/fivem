/*
 * This file is part of the CitizenFX project - http://citizen.re/
 *
 * See LICENSE and MENTIONS in the root of the source tree for information
 * regarding licensing.
 */

#pragma once

//
// ServoSharedTexture.h
//
// ANGLE shared texture bridge for Servo WebRender integration.
//
// Architecture overview:
//
//   Game process (D3D11)                   Servo subprocess (ANGLE/EGL)
//   ─────────────────────                  ────────────────────────────
//   CreateTexture2D(SHARED) ──HANDLE──▶  eglCreatePbufferFromClientBuffer
//        │                                       │
//        │                               EGL_D3D_TEXTURE_2D_SHARE_HANDLE_ANGLE
//        │                                       │
//   ID3D11Texture2D ◀────────────────── WebRender renders into EGL surface
//        │
//   Composite onto game screen
//
// The HANDLE is shared via HostSharedData<ServoRenderData>.
// The Servo subprocess reads it, creates an EGL pbuffer surface,
// and uses that as WebRender's render target (GL default framebuffer).
//

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <memory>
#include <string>

// EGL types — only forward-declare when the real EGL headers haven't been included.
// This keeps NUIWindow.h (which includes us) free of the EGL include-path requirement;
// the .cpp files that call EGL functions include <EGL/egl.h> before us.
#ifndef __egl_h_
// These match the real typedefs in Khronos EGL headers (all are pointer-to-struct,
// but since the structs are opaque we can use void* here as a compatible approximation
// for declaration purposes only — the actual calls are in the .cpp files).
typedef void*    EGLDisplay;
typedef void*    EGLSurface;
typedef void*    EGLConfig;
#endif

#include <HostSharedData.h>

//
// Shared memory layout between game process and Servo subprocess.
// Placed in a named file-mapping so both sides can access it.
//
struct ServoRenderData
{
	// The legacy DXGI shared handle (GTA_FIVE path: IDXGIResource::GetSharedHandle).
	// Valid for the life of the texture.  Servo reads this to open the surface.
	HANDLE handle;

	// NT kernel handle duplicated into the Servo process (non-GTA_FIVE path).
	// Zero when not used.
	HANDLE ntHandle;

	int width;
	int height;

	// Incremented by the game when handle/ntHandle change (e.g. resize).
	// Servo polls this and recreates its EGL surface when it changes.
	uint32_t generation;

	// Set by Servo to signal a new frame was composited into the texture.
	// Reset by the game after it reads the texture.
	volatile LONG newFrameReady;

	ServoRenderData()
		: handle(NULL), ntHandle(NULL), width(0), height(0),
		  generation(0), newFrameReady(0)
	{
	}
};

namespace nui
{

//
// ServoSharedTextureBridge  (game-process side)
//
// Creates and owns a D3D11 BGRA texture with D3D11_RESOURCE_MISC_SHARED.
// Writes the DXGI shared handle into ServoRenderData shared memory so
// the Servo subprocess can open it via ANGLE.
//
// The game render loop calls UpdateFrame() once per frame to composite
// the texture onto the screen using the existing NUI pipeline.
//
class ServoSharedTextureBridge
{
public:
	//
	// Factory.  `name` identifies the shared-memory slot
	// (e.g. "CfxServoRender_root").
	// Returns nullptr on failure.
	//
	static ServoSharedTextureBridge* Create(ID3D11Device* device,
	                                        const std::string& name,
	                                        int width, int height);

	~ServoSharedTextureBridge();

	// Non-copyable
	ServoSharedTextureBridge(const ServoSharedTextureBridge&) = delete;
	ServoSharedTextureBridge& operator=(const ServoSharedTextureBridge&) = delete;

	//
	// Recreate the backing texture at a new size.
	// Updates shared memory and bumps the generation counter.
	//
	bool Resize(int newWidth, int newHeight);

	//
	// Raw D3D11 texture for use in the game's composite pass.
	// The game reads from this; Servo writes to it via ANGLE.
	//
	ID3D11Texture2D* GetD3D11Texture() const { return m_texture.Get(); }

	//
	// Returns the legacy DXGI shared handle stored in shared memory.
	// The Servo subprocess uses this with
	//   eglCreatePbufferFromClientBuffer(EGL_D3D_TEXTURE_2D_SHARE_HANDLE_ANGLE, ...)
	//
	HANDLE GetSharedHandle() const { return m_sharedHandle; }

	int GetWidth()  const { return m_width; }
	int GetHeight() const { return m_height; }

	//
	// True when the Servo subprocess has composited a new frame.
	// Call ConsumeFrame() after reading the texture.
	//
	bool HasNewFrame() const;
	void ConsumeFrame();

private:
	ServoSharedTextureBridge() = default;

	bool Init(ID3D11Device* device, const std::string& name,
	          int width, int height);

	bool CreateTexture(ID3D11Device* device, int width, int height);
	void PublishHandle();

	Microsoft::WRL::ComPtr<ID3D11Texture2D> m_texture;
	HANDLE                                   m_sharedHandle = NULL;

	// Named shared-memory mapping, alive for the life of the bridge.
	std::unique_ptr<HostSharedData<ServoRenderData>> m_hostSharedData;

	// Raw pointer into the mapping (convenience; owned by m_hostSharedData).
	ServoRenderData* m_sharedData = nullptr;

	int    m_width  = 0;
	int    m_height = 0;
	std::string m_name;
};

// ─────────────────────────────────────────────────────────────────────────────
//  Servo subprocess / ANGLE side
// ─────────────────────────────────────────────────────────────────────────────

//
// EGL config selection matching the D3D11 BGRA shared texture.
// Finds a config with RGBA 8-bit, pbuffer support, and
// EGL_BIND_TO_TEXTURE_RGBA (required for the shared-handle pbuffer).
//
EGLConfig ServoChooseEGLConfig(EGLDisplay display);

//
// Creates an EGL pbuffer surface backed by a D3D11 shared texture handle.
//
// `d3dHandle`  - The HANDLE from ServoRenderData::handle (game process).
//               On Windows this is a process-local value returned by
//               IDXGIResource::GetSharedHandle().  If you duplicated it
//               into the Servo process as an NT handle, use the NT variant.
//
// `config`     - Result of ServoChooseEGLConfig().
//
// Returns EGL_NO_SURFACE on failure.
//
// Usage (Servo subprocess):
//
//   EGLDisplay dpy  = eglGetCurrentDisplay();
//   EGLConfig  cfg  = ServoChooseEGLConfig(dpy);
//   EGLSurface surf = ServoCreateEGLSurfaceFromD3DHandle(dpy, cfg,
//                         renderData->handle,
//                         renderData->width, renderData->height);
//   EGLContext ctx  = /* create context bound to cfg */;
//   eglMakeCurrent(dpy, surf, surf, ctx);
//   // WebRender now renders into renderData->handle's D3D11 texture
//   // via GL framebuffer 0 → ANGLE → D3D11 render-target.
//
EGLSurface ServoCreateEGLSurfaceFromD3DHandle(EGLDisplay display,
                                               EGLConfig  config,
                                               HANDLE     d3dHandle,
                                               int        width,
                                               int        height);

//
// Destroys the EGL surface created by ServoCreateEGLSurfaceFromD3DHandle.
//
void ServoDestroyEGLSurface(EGLDisplay display, EGLSurface surface);

} // namespace nui
