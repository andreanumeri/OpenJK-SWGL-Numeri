/*
===========================================================================
SWGL PiP support for the vanilla renderer.

The PiP window (and its half-resolution framebuffer) is created at renderer
startup so it is always there, showing black.  Idle cost is kept near zero:
no scene is rendered unless cgame submits a RenderScene with refdef.pipRender
set (which cgame only does while the static camera is active).

Command order per frame:
main scene -> HUD 2D -> [pip bind -> pip scene] -> pip present -> swap.

The present command runs every frame: it shows the scene rendered by the pip
bind this frame, or black when nothing was rendered into the pip FBO.

Only rd-vanilla (rdsp_swgl_x86_64.dll) supports this; rd-rend2 ignores pipRender.
===========================================================================
*/

#include "tr_local.h"

#include <stdlib.h>	// malloc/free: the CPU-side buffers must survive a hunk reset

// glext.h in this project lacks a plain GL_RGBA8 define
#ifndef GL_RGBA8
#define GL_RGBA8					0x8058
#endif
#ifndef GL_DEPTH_COMPONENT16
#define GL_DEPTH_COMPONENT16		0x81A5
#endif

// lazily loaded framebuffer entry points (core names with EXT fallback)
typedef PFNGLGENFRAMEBUFFERSPROC		p_pfnGenFramebuffers;
typedef PFNGLBINDFRAMEBUFFERPROC		p_pfnBindFramebuffer;
typedef PFNGLDELETEFRAMEBUFFERSPROC		p_pfnDeleteFramebuffers;
typedef PFNGLGENRENDERBUFFERSPROC		p_pfnGenRenderbuffers;
typedef PFNGLBINDRENDERBUFFERPROC		p_pfnBindRenderbuffer;
typedef PFNGLDELETERENDERBUFFERSPROC	p_pfnDeleteRenderbuffers;
typedef PFNGLRENDERBUFFERSTORAGEPROC	p_pfnRenderbufferStorage;
typedef PFNGLFRAMEBUFFERRENDERBUFFERPROC p_pfnFramebufferRenderbuffer;
typedef PFNGLCHECKFRAMEBUFFERSTATUSPROC	p_pfnCheckFramebufferStatus;

static p_pfnGenFramebuffers			pGenFramebuffers;
static p_pfnBindFramebuffer			pBindFramebuffer;
static p_pfnDeleteFramebuffers		pDeleteFramebuffers;
static p_pfnGenRenderbuffers		pGenRenderbuffers;
static p_pfnBindRenderbuffer		pBindRenderbuffer;
static p_pfnDeleteRenderbuffers		pDeleteRenderbuffers;
static p_pfnRenderbufferStorage		pRenderbufferStorage;
static p_pfnFramebufferRenderbuffer	pFramebufferRenderbuffer;
static p_pfnCheckFramebufferStatus	pCheckFramebufferStatus;

static cvar_t	*r_pipWindow;
static cvar_t	*r_pipWindowX;
static cvar_t	*r_pipWindowY;
static cvar_t	*r_pipTopmost;
static cvar_t	*r_pipCrosshair;
static cvar_t	*r_pipBorderless;	// read by the SDL layer at window creation
static cvar_t	*r_pipRate;			// render the pip scene once every N frames
static cvar_t	*r_pipDirect;		// 1 = render straight into the window framebuffer
static cvar_t	*r_pipReadbackWait;	// µs bounded fence wait to present the CURRENT frame
static cvar_t	*r_pipLatencyLog;	// log which pipeline stage the presented image took
static cvar_t	*r_pipSdlHw;		// SDL layer: hardware renderer for the pip window

static qboolean	pipGLAttempted = qfalse;
static qboolean	pipGLReady = qfalse;

static void		*pipWindowHandle;
static int		pipWidth, pipHeight;
static GLuint	pipFBO, pipColorRB, pipDepthRB;
static byte		*pipReadback;		// CPU destination of the framebuffer readback
static byte		*pipBlack;			// all-black frame for the idle blit (window without a GL path)
static int		pipBufferSize;		// bytes in both CPU buffers (w * h * 4)
static qboolean	pipContentDirty;	// set by RB_PipBind, cleared after the present
static qboolean	pipBoundOnce;		// the first pip scene through the pipeline, announced once
static qboolean	pipViewActive;		// set while the pipeline renders the pip view (bind -> present)
static refdef_t	pipDeferredDef;		// the pip refdef parked by the game, replayed at frame end
static qboolean	pipDeferred;		// a pip refdef is waiting to be replayed
static qboolean	pipRunning;			// the replay is going through the normal front end

/*
=============
R_PipCvar

r_pip_window master switch, registered at renderer startup.
=============
*/
cvar_t *R_PipCvar( void ) {
	if ( !r_pipWindow ) {
		r_pipWindow = ri.Cvar_Get( "r_pip_window", "0", CVAR_ARCHIVE );
	}
	return r_pipWindow;
}

/*
=============
Pip_RegisterControlCvars
=============
*/
static void Pip_RegisterControlCvars( void ) {
	if ( !r_pipWindowX ) {
		r_pipWindowX = ri.Cvar_Get( "r_pip_window_x", "-1", CVAR_ARCHIVE );	// -3 = bottom-right corner of the other display
		r_pipWindowY = ri.Cvar_Get( "r_pip_window_y", "-1", CVAR_ARCHIVE );	// -3 = bottom-right corner of the other display
		r_pipTopmost = ri.Cvar_Get( "r_pip_topmost", "1", CVAR_ARCHIVE );
		r_pipCrosshair = ri.Cvar_Get( "r_pip_crosshair", "1", CVAR_ARCHIVE );
		r_pipBorderless = ri.Cvar_Get( "r_pip_borderless", "1", CVAR_ARCHIVE );
		r_pipRate = ri.Cvar_Get( "r_pip_rate", "1", CVAR_ARCHIVE );		// SWGL PiP perf: render the scene once every N frames (1 = every frame, the window simply keeps the last image on the skipped ones)
		r_pipDirect = ri.Cvar_Get( "r_pip_direct", "1", CVAR_ARCHIVE );	// SWGL PiP perf: render straight into the window framebuffer (native size) and skip the postprocess chain
		r_pipReadbackWait = ri.Cvar_Get( "r_pip_readback_wait", "25000", CVAR_ARCHIVE );	// SWGL PiP latency: µs to wait for this frame's readback so the window can show the CURRENT frame (default tuned for the ~60 fps reshade regime; 0 = the pure async design, one frame of pipeline latency)
		r_pipLatencyLog = ri.Cvar_Get( "r_pip_latencylog", "0", CVAR_ARCHIVE );	// SWGL PiP latency: log which pipeline stage each presented frame took
		r_pipSdlHw = ri.Cvar_Get( "r_pip_sdlhw", "0", CVAR_ARCHIVE );		// SWGL PiP latency: hardware renderer for the SDL window (flip-model present, no CPU copy; applied when the window is created, so a game restart takes it).  0 on vanilla: a machine with no hardware renderer (software GL) fails SDL_CreateRenderer outright, and the SDL layer then destroys the pip window - rend2 registers this with default 1, where the hardware renderer exists.
	}
}

/*
=============
Pip_ShouldRender

True when a pipRender scene may actually go through the pipeline
(master cvar on, GL entry points loaded, window and framebuffer alive).
=============
*/
qboolean Pip_ShouldRender( void ) {
	return (qboolean)( pipGLReady && pipWindowHandle && pipFBO && R_PipCvar()->integer );
}

/*
=============
Pip_SceneViewport

Fills in the view rectangle for a pip scene: the framebuffer is the half-size
one, the refdef geometry the game sends is the main-resolution one, so the scene
renders into the framebuffer's own rectangle from its origin.  The aspect ratio
of the two matches, so the projection the game asked for is already right.
=============
*/
void Pip_SceneViewport( int *x, int *y, int *width, int *height ) {
	if ( !pipFBO || pipWidth < 2 || pipHeight < 2 ) {
		return;
	}

	*x = 0;
	*y = 0;
	*width = pipWidth;
	*height = pipHeight;
}

/*
=============
Pip_Bound

True while the pipeline is working for the PiP view: RB_PipBind sets it and
RB_PipPresent clears it, so the per-view post steps can tell the PiP view from
the main window one.  The dynamic-glow chain is the user: its copy textures and
its final overlay are pinned to the main window's resolution, and the PiP frame
is the half-size framebuffer (the reference implementation gets the same result
by switching the glow cvar off for the PiP scene alone).
=============
*/
qboolean Pip_Bound( void ) {
	return pipViewActive;
}

/*
=============
Pip_DeferScene

Parks the pip refdef the game handed over.  Answers qfalse (so the caller keeps
going) only for the replay below.

The vanilla back end executes its commands in the order the frame built them,
and it draws the main window's 2D (the HUD, the console, the menus) as it goes,
so a pip scene sitting in the middle of the frame would have all that 2D land in
the PiP framebuffer.  Parking the scene and replaying it at the end of the frame
- after everything meant for the main window, right before the present - keeps
the PiP framebuffer to the PiP view alone.
=============
*/
qboolean Pip_DeferScene( const refdef_t *fd ) {
	if ( pipRunning ) {
		return qfalse;
	}

	pipDeferredDef = *fd;
	pipDeferred = qtrue;

	return qtrue;
}

/*
=============
Pip_RunDeferredScene

RE_EndFrame calls this right before the present command: the frame's 2D is all
built by now, so this is where the pip view belongs.
=============
*/
void Pip_RunDeferredScene( void ) {
	if ( !pipDeferred ) {
		return;
	}

	pipDeferred = qfalse;
	pipRunning = qtrue;

	RE_RenderScene( &pipDeferredDef );

	pipRunning = qfalse;
}

/*
=============
R_PipAddFramePresentCmd

Queued by RE_EndFrame every frame so the window always gets updated (black
when idle).  No-op when the PiP window is not alive.
=============
*/
void R_PipAddFramePresentCmd( void ) {
	if ( pipWindowHandle ) {
		R_AddPipPresentCmd();
	}
}

/*
=============
Pip_GLReady

Load the framebuffer entry points once. Core names first, EXT suffix fallback.
=============
*/
static qboolean Pip_GLReady( void ) {
	if ( pipGLReady ) {
		return qtrue;
	}
	if ( pipGLAttempted ) {
		return qfalse;
	}
	pipGLAttempted = qtrue;

	if ( !ri.GL_ExtensionSupported( "GL_EXT_framebuffer_object" ) &&
		 !ri.GL_ExtensionSupported( "GL_ARB_framebuffer_object" ) ) {
		Com_Printf( "PiP: no framebuffer object support, PiP disabled\n" );
		return qfalse;
	}

	pGenFramebuffers = (p_pfnGenFramebuffers)ri.GL_GetProcAddress( "glGenFramebuffers" );
	if ( !pGenFramebuffers ) {
		pGenFramebuffers = (p_pfnGenFramebuffers)ri.GL_GetProcAddress( "glGenFramebuffersEXT" );
	}
	pBindFramebuffer = (p_pfnBindFramebuffer)ri.GL_GetProcAddress( "glBindFramebuffer" );
	if ( !pBindFramebuffer ) {
		pBindFramebuffer = (p_pfnBindFramebuffer)ri.GL_GetProcAddress( "glBindFramebufferEXT" );
	}
	pDeleteFramebuffers = (p_pfnDeleteFramebuffers)ri.GL_GetProcAddress( "glDeleteFramebuffers" );
	if ( !pDeleteFramebuffers ) {
		pDeleteFramebuffers = (p_pfnDeleteFramebuffers)ri.GL_GetProcAddress( "glDeleteFramebuffersEXT" );
	}
	pGenRenderbuffers = (p_pfnGenRenderbuffers)ri.GL_GetProcAddress( "glGenRenderbuffers" );
	if ( !pGenRenderbuffers ) {
		pGenRenderbuffers = (p_pfnGenRenderbuffers)ri.GL_GetProcAddress( "glGenRenderbuffersEXT" );
	}
	pBindRenderbuffer = (p_pfnBindRenderbuffer)ri.GL_GetProcAddress( "glBindRenderbuffer" );
	if ( !pBindRenderbuffer ) {
		pBindRenderbuffer = (p_pfnBindRenderbuffer)ri.GL_GetProcAddress( "glBindRenderbufferEXT" );
	}
	pDeleteRenderbuffers = (p_pfnDeleteRenderbuffers)ri.GL_GetProcAddress( "glDeleteRenderbuffers" );
	if ( !pDeleteRenderbuffers ) {
		pDeleteRenderbuffers = (p_pfnDeleteRenderbuffers)ri.GL_GetProcAddress( "glDeleteRenderbuffersEXT" );
	}
	pRenderbufferStorage = (p_pfnRenderbufferStorage)ri.GL_GetProcAddress( "glRenderbufferStorage" );
	if ( !pRenderbufferStorage ) {
		pRenderbufferStorage = (p_pfnRenderbufferStorage)ri.GL_GetProcAddress( "glRenderbufferStorageEXT" );
	}
	pFramebufferRenderbuffer = (p_pfnFramebufferRenderbuffer)ri.GL_GetProcAddress( "glFramebufferRenderbuffer" );
	if ( !pFramebufferRenderbuffer ) {
		pFramebufferRenderbuffer = (p_pfnFramebufferRenderbuffer)ri.GL_GetProcAddress( "glFramebufferRenderbufferEXT" );
	}
	pCheckFramebufferStatus = (p_pfnCheckFramebufferStatus)ri.GL_GetProcAddress( "glCheckFramebufferStatus" );
	if ( !pCheckFramebufferStatus ) {
		pCheckFramebufferStatus = (p_pfnCheckFramebufferStatus)ri.GL_GetProcAddress( "glCheckFramebufferStatusEXT" );
	}

	if ( !pGenFramebuffers || !pBindFramebuffer || !pDeleteFramebuffers ||
		 !pGenRenderbuffers || !pBindRenderbuffer || !pDeleteRenderbuffers ||
		 !pRenderbufferStorage || !pFramebufferRenderbuffer ||
		 !pCheckFramebufferStatus ) {
		Com_Printf( "PiP: could not load framebuffer entry points, PiP disabled\n" );
		return qfalse;
	}

	pipGLReady = qtrue;
	return qtrue;
}

/*
=============
Pip_DestroyFBO
=============
*/
static void Pip_DestroyFBO( void ) {
	if ( !pipGLReady ) {
		return;
	}
	if ( pipFBO ) {
		pDeleteFramebuffers( 1, &pipFBO );
		pipFBO = 0;
	}
	if ( pipColorRB ) {
		pDeleteRenderbuffers( 1, &pipColorRB );
		pipColorRB = 0;
	}
	if ( pipDepthRB ) {
		pDeleteRenderbuffers( 1, &pipDepthRB );
		pipDepthRB = 0;
	}
}

/*
=============
Pip_FreeBuffers
=============
*/
static void Pip_FreeBuffers( void ) {
	if ( pipReadback ) {
		free( pipReadback );
		pipReadback = NULL;
	}
	if ( pipBlack ) {
		free( pipBlack );
		pipBlack = NULL;
	}
	pipBufferSize = 0;
}

/*
=============
Pip_AllocBuffers

(Re)allocate the CPU-side buffers for a w x h RGBA image: the readback
destination and the all-black idle frame.  malloc rather than the hunk so they
survive a level-load hunk reset.
=============
*/
static qboolean Pip_AllocBuffers( int w, int h ) {
	int	need = w * h * 4;

	if ( pipReadback && pipBlack && pipBufferSize == need ) {
		return qtrue;
	}

	Pip_FreeBuffers();

	pipReadback = (byte *)malloc( need );
	pipBlack = (byte *)malloc( need );

	if ( !pipReadback || !pipBlack ) {
		Pip_FreeBuffers();
		Com_Printf( "PiP: out of memory for the CPU buffers, content disabled\n" );
		return qfalse;
	}

	pipBufferSize = need;
	Com_Memset( pipBlack, 0, need );

	return qtrue;
}

/*
=============
Pip_NoteError

Report a pending GL error, if any, and nothing else.  Reporting only: the
framebuffer status check is the authority on whether an attempt works, and an
error from an earlier PiP call would otherwise be blamed on whichever call
happens to look at the queue next.
=============
*/
static void Pip_NoteError( const char *what, GLenum format ) {
	GLenum	err = qglGetError();

	if ( err == GL_NO_ERROR ) {
		return;
	}

	Com_Printf( "PiP: %s (0x%x) - GL error 0x%x\n", what, (unsigned)format, (unsigned)err );
}

/*
=============
Pip_CreateFBOFormat

One attempt at a framebuffer with the given colour (and optional depth)
renderbuffer formats.  Tears the attempt down and returns qfalse unless the
framebuffer comes out complete.
=============
*/
static qboolean Pip_CreateFBOFormat( int w, int h, GLenum colorFormat, GLenum depthFormat ) {
	GLuint	colorRB = 0, depthRB = 0, fbo = 0;

	pGenRenderbuffers( 1, &colorRB );
	if ( !colorRB ) {
		Pip_NoteError( "genRenderbuffers", 0 );
		return qfalse;
	}
	pBindRenderbuffer( GL_RENDERBUFFER, colorRB );
	pRenderbufferStorage( GL_RENDERBUFFER, colorFormat, w, h );
	Pip_NoteError( "renderbufferStorage colour", colorFormat );
	pBindRenderbuffer( GL_RENDERBUFFER, 0 );

	if ( depthFormat ) {
		pGenRenderbuffers( 1, &depthRB );
		if ( depthRB ) {
			pBindRenderbuffer( GL_RENDERBUFFER, depthRB );
			pRenderbufferStorage( GL_RENDERBUFFER, depthFormat, w, h );
			Pip_NoteError( "renderbufferStorage depth", depthFormat );
			pBindRenderbuffer( GL_RENDERBUFFER, 0 );
		}
		else {
			Pip_NoteError( "genRenderbuffers depth", 0 );
			depthRB = 0;
		}
	}

	pGenFramebuffers( 1, &fbo );
	if ( !fbo ) {
		Pip_NoteError( "genFramebuffers", 0 );
		pDeleteRenderbuffers( 1, &colorRB );
		if ( depthRB ) {
			pDeleteRenderbuffers( 1, &depthRB );
		}
		return qfalse;
	}
	pBindFramebuffer( GL_FRAMEBUFFER, fbo );
	pFramebufferRenderbuffer( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, colorRB );
	Pip_NoteError( "framebufferRenderbuffer colour", GL_COLOR_ATTACHMENT0 );
	if ( depthRB ) {
		pFramebufferRenderbuffer( GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depthRB );
		Pip_NoteError( "framebufferRenderbuffer depth", GL_DEPTH_ATTACHMENT );
	}

	if ( pCheckFramebufferStatus( GL_FRAMEBUFFER ) != GL_FRAMEBUFFER_COMPLETE ) {
		Com_Printf( "PiP: framebuffer incomplete with colour 0x%x, depth 0x%x\n", (unsigned)colorFormat, (unsigned)depthFormat );
		pBindFramebuffer( GL_FRAMEBUFFER, 0 );
		pDeleteFramebuffers( 1, &fbo );
		pDeleteRenderbuffers( 1, &colorRB );
		if ( depthRB ) {
			pDeleteRenderbuffers( 1, &depthRB );
		}
		return qfalse;
	}

	pipFBO = fbo;
	pipColorRB = colorRB;
	pipDepthRB = depthRB;
	pipWidth = w;
	pipHeight = h;

	// the CPU buffers hold the readback and the black idle frame
	Pip_AllocBuffers( w, h );

	// the SDL layer keeps its streaming texture in sync through WIN_Secondary_Create
	if ( pipWindowHandle ) {
		pipWindowHandle = ri.WIN_Secondary_Create( NULL, -1, -1, w, h, qfalse );
	}

	Com_Printf( "PiP: framebuffer %dx%d - colour 0x%x, depth 0x%x\n", w, h, (unsigned)colorFormat, (unsigned)depthFormat );

	return qtrue;
}

/*
=============
Pip_CreateFBO

(Re)create the PiP framebuffer at the requested size.  The colour format is the
one the renderer already uses everywhere (GL_RGBA8, as in the rest of this
renderer); only the depth rungs step down, because a display mode asked for less
depth than the first rung wants.
=============
*/
static void Pip_CreateFBO( int w, int h ) {
	static const GLenum	depthFormats[] = { GL_DEPTH_COMPONENT24, GL_DEPTH_COMPONENT16, 0 };
	int					j;

	Pip_DestroyFBO();

	if ( w < 2 || h < 2 ) {
		return;
	}

	// drain whatever the rest of the renderer left in the error queue, so that a
	// PiP error report below can only be about a PiP call
	while ( qglGetError() != GL_NO_ERROR ) {
		;
	}

	for ( j = 0; j < 3; j++ ) {
		// respect the depth the renderer asked the display mode for
		if ( j == 0 && glConfig.depthBits < 24 ) {
			continue;
		}
		if ( j == 1 && glConfig.depthBits < 16 ) {
			continue;
		}
		if ( Pip_CreateFBOFormat( w, h, GL_RGBA8, depthFormats[j] ) ) {
			return;
		}
	}

	Com_Printf( "PiP: no working framebuffer at %dx%d, content disabled (the window stays up)\n", w, h );
}

/*
=============
Pip_CreateWindow
=============
*/
static void Pip_CreateWindow( int w, int h ) {
	windowDesc_t	desc;

	if ( pipWindowHandle ) {
		return;
	}

	memset( &desc, 0, sizeof( desc ) );
	desc.api = GRAPHICS_API_OPENGL;

	pipWindowHandle = ri.WIN_Secondary_Create( &desc, (int)r_pipWindowX->value, (int)r_pipWindowY->value,
											   w, h, r_pipTopmost->integer ? qtrue : qfalse );
	if ( pipWindowHandle ) {
		// the window size is what the present path works from, framebuffer or not
		pipWidth = w;
		pipHeight = h;
	}
}

/*
=============
R_PipRendererInit

Called from R_Init once the GL context and glConfig are live.  Registers the
control cvars and brings up the framebuffer and the OS window so the PiP
window is always there, showing black until a pipRender scene arrives.
=============
*/
void R_PipRendererInit( void ) {
	int w, h;

	R_PipCvar();
	Pip_RegisterControlCvars();

	w = glConfig.vidWidth / 2;
	h = glConfig.vidHeight / 2;
	if ( w < 2 || h < 2 ) {
		Com_Printf( "PiP: bad window size %dx%d, PiP disabled\n", w, h );
		return;
	}

	// the CPU buffers come first, the window needs them for the idle blit
	if ( !Pip_AllocBuffers( w, h ) ) {
		return;
	}

	// the window is pure SDL (no GL), so it comes up whatever the GL can do
	Pip_CreateWindow( w, h );
	if ( !pipWindowHandle ) {
		// the SDL layer printed the reason
		return;
	}

	// the framebuffer is what makes the window show something other than black
	if ( !Pip_GLReady() ) {
		return;
	}

	Pip_CreateFBO( w, h );
}

/*
=============
R_PipRendererShutdown

Tear everything down (called from RE_Shutdown, which also runs on vid restarts;
the next R_Init rebuilds it).
=============
*/
void R_PipRendererShutdown( void ) {
	if ( pipWindowHandle ) {
		ri.WIN_Secondary_Destroy( pipWindowHandle );
		pipWindowHandle = NULL;
	}
	if ( pipGLReady ) {
		Pip_DestroyFBO();
	}
	Pip_FreeBuffers();
	pipWidth = pipHeight = 0;
	pipContentDirty = qfalse;
	pipBoundOnce = qfalse;
	pipViewActive = qfalse;
	pipGLReady = qfalse;
	pipGLAttempted = qfalse;
	pipFBO = pipColorRB = pipDepthRB = 0;
}

/*
=============
RB_PipBind

Backend command: make the PiP framebuffer the current render target.
=============
*/
const void *RB_PipBind( const void *data ) {
	const pipBindCommand_t	*cmd = (const pipBindCommand_t *)data;

	if ( !pipGLReady || !pipFBO ) {
		return (const void *)(cmd + 1);
	}

	// SWGL PiP: whatever 2D geometry the main window left in the tess (HUD, console
	// text) has to land on the main window, so flush it here the way
	// RB_DrawSurfs and RB_SwapBuffers flush it for each other, before the render
	// target changes.  Otherwise it is flushed inside the pip scene's own
	// RB_DrawSurfs and gets painted into the PiP framebuffer.
	if ( tess.numIndexes ) {
		RB_EndSurface();
	}

	pBindFramebuffer( GL_FRAMEBUFFER, pipFBO );
	qglDrawBuffer( GL_COLOR_ATTACHMENT0 );
	qglViewport( 0, 0, pipWidth, pipHeight );
	qglClearColor( 0.0f, 0.0f, 0.0f, 1.0f );
	qglClear( GL_COLOR_BUFFER_BIT | (pipDepthRB ? GL_DEPTH_BUFFER_BIT : 0) );

	pipViewActive = qtrue;

	if ( !pipBoundOnce ) {
		pipBoundOnce = qtrue;
		Com_Printf( "PiP: first pip scene in the pipeline\n" );
	}

	pipContentDirty = qtrue;

	return (const void *)(cmd + 1);
}

/*
=============
Pip_DrawCrosshair

Fake crosshair drawn CPU-side into the frame that goes to the window, so it
stays visible regardless of cg_draw2d (and on the black idle screen too).
The design is symmetric, so it looks the same after the bottom-up flip.
=============
*/
static void Pip_DrawCrosshair( byte *buf, int w, int h ) {
	int		cx = w / 2;
	int		cy = h / 2;
	int		gap = 6;		// empty space around the centre
	int		len = 12;		// arm length
	int		i;
	byte	*p;

	if ( !r_pipCrosshair || !r_pipCrosshair->integer ) {
		return;
	}

	// centre dot
	p = buf + (((size_t)cy * w) + cx) * 4;
	p[0] = p[1] = p[2] = p[3] = 255;

	for ( i = gap; i < gap + len; i++ ) {
		if ( cx + i < w ) {
			p = buf + (((size_t)cy * w) + (cx + i)) * 4;
			p[0] = p[1] = p[2] = p[3] = 255;
		}
		if ( cx - i >= 0 ) {
			p = buf + (((size_t)cy * w) + (cx - i)) * 4;
			p[0] = p[1] = p[2] = p[3] = 255;
		}
		if ( cy + i < h ) {
			p = buf + (((size_t)(cy + i) * w) + cx) * 4;
			p[0] = p[1] = p[2] = p[3] = 255;
		}
		if ( cy - i >= 0 ) {
			p = buf + (((size_t)(cy - i) * w) + cx) * 4;
			p[0] = p[1] = p[2] = p[3] = 255;
		}
	}
}

/*
=============
RB_PipPresent

Backend command, queued every frame right before the swap: read the PiP
framebuffer back and blit it to the PiP window.  Shows black (idle) unless a
pipRender scene landed in the framebuffer this frame.
=============
*/
const void *RB_PipPresent( const void *data ) {
	const pipPresentCommand_t	*cmd = (const pipPresentCommand_t *)data;
	int							w = glConfig.vidWidth / 2;
	int							h = glConfig.vidHeight / 2;

	if ( !pipWindowHandle ) {
		return (const void *)(cmd + 1);
	}

	// SWGL PiP: the pip view's part of the pipeline is over with this command
	pipViewActive = qfalse;

	// resolution may have changed under us (vid_restart)
	if ( w >= 2 && h >= 2 && ( w != pipWidth || h != pipHeight ) ) {
		if ( pipGLReady ) {
			Pip_CreateFBO( w, h );
		}
		else if ( Pip_AllocBuffers( w, h ) ) {
			pipWidth = w;
			pipHeight = h;
			pipWindowHandle = ri.WIN_Secondary_Create( NULL, -1, -1, w, h, qfalse );
		}
	}

	if ( !pipGLReady || !pipFBO || !pipReadback || !pipBlack ) {
		// no framebuffer path: the window stays up, showing black and the crosshair
		if ( pipBlack && pipWidth >= 2 && pipHeight >= 2 && pipBufferSize == pipWidth * pipHeight * 4 ) {
			Com_Memset( pipBlack, 0, pipBufferSize );
			Pip_DrawCrosshair( pipBlack, pipWidth, pipHeight );
			ri.WIN_Secondary_Present( pipWindowHandle, pipBlack, pipWidth, pipHeight, qfalse );
		}
		return (const void *)(cmd + 1);
	}

	pBindFramebuffer( GL_FRAMEBUFFER, pipFBO );

	if ( !pipContentDirty ) {
		// idle frame: clear the framebuffer to black so the window shows black
		qglClearColor( 0.0f, 0.0f, 0.0f, 1.0f );
		qglClear( GL_COLOR_BUFFER_BIT | (pipDepthRB ? GL_DEPTH_BUFFER_BIT : 0) );
	}

	qglReadPixels( 0, 0, pipWidth, pipHeight, GL_RGBA, GL_UNSIGNED_BYTE, pipReadback );
	pBindFramebuffer( GL_FRAMEBUFFER, 0 );

	Pip_DrawCrosshair( pipReadback, pipWidth, pipHeight );

	// readback rows are bottom-up, the SDL side flips them
	ri.WIN_Secondary_Present( pipWindowHandle, pipReadback, pipWidth, pipHeight, qtrue );

	pipContentDirty = qfalse;

	// restore state for the main window
	qglDrawBuffer( GL_BACK );
	qglViewport( 0, 0, glConfig.vidWidth, glConfig.vidHeight );

	return (const void *)(cmd + 1);
}
