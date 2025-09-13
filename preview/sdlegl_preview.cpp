/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Copyright (C) 2020, Raspberry Pi (Trading) Ltd.
 *
 * sdl_preview.cpp -SDL-based preview window.
 */

#include <map>
#include <string>



#include "core/options.hpp"

#include "preview.hpp"

#include <libdrm/drm_fourcc.h>


#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl31.h>

#include <GLES2/gl2ext.h> // this is correct

#include <SDL2/SDL.h>

#define SDL_ENABLED 1  // or pass -DSDL_ENABLED=1 via compiler


GLchar* checkStatus(GLuint objectID, PFNGLGETSHADERIVPROC objectPropertyGetterFunc, PFNGLGETSHADERINFOLOGPROC getInfoLogFunc, GLenum statusType)
{
    GLint status;
    GLchar* infoLog= nullptr;
    objectPropertyGetterFunc(objectID, statusType, &status);
    if (status != GL_TRUE)
    {
        GLint infoLogLength;
        objectPropertyGetterFunc(objectID, GL_INFO_LOG_LENGTH, &infoLogLength);

        infoLog = new GLchar[infoLogLength + 1];
        getInfoLogFunc(objectID, infoLogLength, NULL, infoLog);
    }
    return infoLog;
}

bool checkShaderStatus(GLuint shaderID,const char* type) 
{   
    GLchar* infoLog= checkStatus(shaderID, glGetShaderiv, glGetShaderInfoLog, GL_COMPILE_STATUS);
    if (infoLog != nullptr) {
        std::cerr << "ERROR::SHADER::" << type << "::COMPILATION_FAILED\n" << infoLog << std::endl;
        return false;
    }
    delete[] infoLog;
    return true;
    
}
bool checkProgramStatus(GLuint programID, const char* type)
{
    GLchar* infoLog=  checkStatus(programID, glGetProgramiv, glGetProgramInfoLog, GL_LINK_STATUS);
    if (infoLog != nullptr) {
        if(type!=nullptr){
            std::cerr << "ERROR::SHADER::PROGRAM::" << type << "::LINKING_FAILED\n" << infoLog << std::endl;
        }else{
            std::cerr << "ERROR::SHADER::PROGRAM::LINKING_FAILED\n" << infoLog << std::endl;
        }
        return false;
    }
    delete[] infoLog;
    return true;
}  


static void no_border(Display *display, Window window)
{
	static const unsigned MWM_HINTS_DECORATIONS = (1 << 1);
	static const int PROP_MOTIF_WM_HINTS_ELEMENTS = 5;

	typedef struct
	{
		unsigned long flags;
		unsigned long functions;
		unsigned long decorations;
		long inputMode;
		unsigned long status;
	} PropMotifWmHints;

	PropMotifWmHints motif_hints;
	Atom prop, proptype;
	unsigned long flags = 0;

	/* setup the property */
	motif_hints.flags = MWM_HINTS_DECORATIONS;
	motif_hints.decorations = flags;

	/* get the atom for the property */
	prop = XInternAtom(display, "_MOTIF_WM_HINTS", True);
	if (!prop)
	{
		/* something went wrong! */
		return;
	}

	/* not sure this is correct, seems to work, XA_WM_HINTS didn't work */
	proptype = prop;

	XChangeProperty(display, window, /* display, window */
					prop, proptype, /* property, type */
					32, /* format: 32-bit datums */
					PropModeReplace, /* mode */
					(unsigned char *)&motif_hints, /* data */
					PROP_MOTIF_WM_HINTS_ELEMENTS /* nelements */
	);
}




class SdlPreview : public Preview
{
public:
	SdlPreview(Options const *options);
	~SdlPreview();
	virtual void SetInfoText(const std::string &text) override;
	// Display the buffer. You get given the fd back in the BufferDoneCallback
	// once its available for re-use.
	virtual void Show(int fd, libcamera::Span<uint8_t> span, StreamInfo const &info) override;
	// Reset the preview window, clearing the current buffers and being ready to
	// show new ones.
	virtual void Reset() override;
	// Check if the window manager has closed the preview.
	virtual bool Quit() override;
	virtual void MaxImageSize(unsigned int &w, unsigned int &h) const override
	{
		w = max_image_width_;
		h = max_image_height_;
	}

private:
	struct Buffer
	{
		Buffer() : fd(-1) {}
		int fd;
		size_t size;
		StreamInfo info;
		GLuint texture;
	};	
	void makeSDLWindow(char const *name, int selected_display=0);
	
	void makeEGLWindow(char const *name);




	
	

	void makeBuffer(int fd, size_t size, StreamInfo const &info, Buffer &buffer);
	::
	Display *display_;
	EGLDisplay egl_display_;
	Window window_;
	EGLContext egl_context_;
	EGLSurface egl_surface_;
	std::map<int, Buffer> buffers_; // map the DMABUF's fd to the Buffer
	int last_fd_;
	bool first_time_;
	// size of preview window

	GLuint VAO, VBO, EBO;
	void gl_setup(int width, int height, int window_width, int window_height);
	Atom wm_delete_window_;
	// size of preview window
	int x_;
	int y_;
	int width_;
	int height_;
	SDL_GLContext sdl_context;
	SDL_GLContext sdl_workercontext;
	SDL_Window* sdl_window;
	unsigned int max_image_width_;
	unsigned int max_image_height_;
};

static GLint compile_shader(GLenum target, const char *source)
{
	GLuint s = glCreateShader(target);
	glShaderSource(s, 1, (const GLchar **)&source, NULL);
	glCompileShader(s);
	checkShaderStatus(s,"shader");
	GLint ok;
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);

	if (!ok)
	{
		GLchar *info;
		GLint size;

		glGetShaderiv(s, GL_INFO_LOG_LENGTH, &size);
		info = (GLchar *)malloc(size);

		glGetShaderInfoLog(s, size, NULL, info);
		throw std::runtime_error("failed to compile shader: " + std::string(info) + "\nsource:\n" +
								 std::string(source));
	}

	return s;
}

static GLint link_program(GLint vs, GLint fs)
{
	GLint prog = glCreateProgram();
	glAttachShader(prog, vs);
	glAttachShader(prog, fs);
	glLinkProgram(prog);

	GLint ok;
	glGetProgramiv(prog, GL_LINK_STATUS, &ok);
	if (!ok)
	{
		/* Some drivers return a size of 1 for an empty log.  This is the size
		 * of a log that contains only a terminating NUL character.
		 */
		GLint size;
		GLchar *info = NULL;
		glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &size);
		if (size > 1)
		{
			info = (GLchar *)malloc(size);
			glGetProgramInfoLog(prog, size, NULL, info);
		}

		throw std::runtime_error("failed to link: " + std::string(info ? info : "<empty log>"));
	}

	return prog;
}

void SdlPreview::gl_setup(int width, int height, int window_width, int window_height)
{

	const char* vvss =
         "#version 300 es\n"
         "layout(location = 0) in vec4 pos;\n"
		"layout (location = 1) in vec2 aTexCoord;\n"

         "out vec2 texcoord;\n"
         "\n"
         "void main() {\n"
         "  gl_Position = pos;\n"
         "  texcoord = aTexCoord;\n"
         
         "}\n";
	GLint vs_s = compile_shader(GL_VERTEX_SHADER, vvss);
	const char* fs =
	"#version 300 es\n"
	"#extension GL_OES_EGL_image_external : require\n"
	"precision mediump float;\n"
	"in vec2 texcoord;\n"
	"out vec4 FragColor;\n"
	"uniform samplerExternalOES s;\n"
	"void main() {\n"
	"  FragColor = texture2D(s, texcoord);\n"
	"}\n";
	GLint fs_s = compile_shader(GL_FRAGMENT_SHADER, fs);
	GLint prog = link_program(vs_s, fs_s);

	glUseProgram(prog);
	glUniform1i(glGetUniformLocation(prog, "s"), 0);

	glGenVertexArrays(1, &VAO);
    glGenBuffers(1, &VBO);
	glBindBuffer(GL_ARRAY_BUFFER, VBO);

	// static const float verts[] = { -w_factor, -h_factor, w_factor, -h_factor, w_factor, h_factor, -w_factor, h_factor };
	static const float verts[] = {
    // x, y, z, u, v
    -1, -1, 0, 0, 0,
     1, -1, 0, 1, 0,
     1,  1, 0, 1, 1,
    -1,  1, 0, 0, 1
	};
	glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);

	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5*sizeof(float), (void*)0);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(1,2, GL_FLOAT, GL_FALSE, 5*sizeof(float),(void*)(3 * sizeof(float)));
	
	glEnableVertexAttribArray(1);
}


void SdlPreview::makeEGLWindow(char const *name)
{
	int screen_num = DefaultScreen(display_);
	XSetWindowAttributes attr;
	unsigned long mask;
	Window root = RootWindow(display_, screen_num);
	int screen_width = DisplayWidth(display_, screen_num);
	int screen_height = DisplayHeight(display_, screen_num);

	// Default behaviour here is to use a 1024x768 window.
	if (width_ == 0 || height_ == 0)
	{
		width_ = 1024;
		height_ = 768;
	}

	if (options_->Get().fullscreen || x_ + width_ > screen_width || y_ + height_ > screen_height)
	{
		x_ = y_ = 0;
		width_ = DisplayWidth(display_, screen_num);
		height_ = DisplayHeight(display_, screen_num);
	}

	width_=300;
	height_=300;
	
	static const EGLint attribs[] =
		{
			EGL_RED_SIZE, 1,
			EGL_GREEN_SIZE, 1,
			EGL_BLUE_SIZE, 1,
			EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
			EGL_NONE
		};
	EGLConfig config;
	EGLint num_configs;
	if (!eglChooseConfig(egl_display_, attribs, &config, 1, &num_configs))
		throw std::runtime_error("couldn't get an EGL visual config");

	EGLint vid;
	if (!eglGetConfigAttrib(egl_display_, config, EGL_NATIVE_VISUAL_ID, &vid))
		throw std::runtime_error("eglGetConfigAttrib() failed\n");

	XVisualInfo visTemplate = {};
	visTemplate.visualid = (VisualID)vid;
	int num_visuals;
	XVisualInfo *visinfo = XGetVisualInfo(display_, VisualIDMask, &visTemplate, &num_visuals);

	/* window attributes */
	attr.background_pixel = 0;
	attr.border_pixel = 0;
	attr.colormap = XCreateColormap(display_, root, visinfo->visual, AllocNone);
	attr.event_mask = StructureNotifyMask | ExposureMask | KeyPressMask;
	/* XXX this is a bad way to get a borderless window! */
	mask = CWBackPixel | CWBorderPixel | CWColormap | CWEventMask;

	window_ = XCreateWindow(display_, root, x_, y_, width_, height_, 0, visinfo->depth, InputOutput, visinfo->visual,
							mask, &attr);

	if (options_->Get().fullscreen)
		no_border(display_, window_);

	/* set hints and properties */
	{
		XSizeHints sizehints;
		sizehints.x = x_;
		sizehints.y = y_;
		sizehints.width = width_;
		sizehints.height = height_;
		sizehints.flags = USSize | USPosition;
		XSetNormalHints(display_, window_, &sizehints);
		XSetStandardProperties(display_, window_, name, name, None, (char **)NULL, 0, &sizehints);
	}

	eglBindAPI(EGL_OPENGL_ES_API);

	static const EGLint ctx_attribs[] = {
		EGL_CONTEXT_CLIENT_VERSION, 2,
		EGL_NONE
	};
	egl_context_ = eglCreateContext(egl_display_, config, EGL_NO_CONTEXT, ctx_attribs);
	if (!egl_context_)
		throw std::runtime_error("eglCreateContext failed");

	XFree(visinfo);

	XMapWindow(display_, window_);

	// This stops the window manager from closing the window, so we get an event instead.
	wm_delete_window_ = XInternAtom(display_, "WM_DELETE_WINDOW", False);
	XSetWMProtocols(display_, window_, &wm_delete_window_, 1);

	egl_surface_ = eglCreateWindowSurface(egl_display_, config, reinterpret_cast<EGLNativeWindowType>(window_), NULL);
	if (!egl_surface_)
		throw std::runtime_error("eglCreateWindowSurface failed");

	// We have to do eglMakeCurrent in the thread where it will run, but we must do it
	// here temporarily so as to get the maximum texture size.
	eglMakeCurrent(egl_display_, EGL_NO_SURFACE, EGL_NO_SURFACE, egl_context_);
	int max_texture_size = 0;
	glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_texture_size);
	max_image_width_ = max_image_height_ = max_texture_size;
	// This "undoes" the previous eglMakeCurrent.
	eglMakeCurrent(egl_display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
}

SdlPreview::SdlPreview(Options const *options) : Preview(options), last_fd_(-1), first_time_(true)
{


	#ifdef SDL_ENABLED
	if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        std::cerr << "SDL could not initialize! SDL_Error: " << SDL_GetError() << std::endl;
        throw std::runtime_error("Failed to initialize SDL");
    }


	
	// makeSDLWindow("my sdl window");

	// SDL_ShowWindow(sdl_window);
	// SDL_RaiseWindow(sdl_window);
	// SDL_SetWindowPosition(sdl_window, 100, 100); 
	// SDL_UpdateWindowSurface(sdl_window);
	
	// printf("Window should now be visible\n");

	#else
		display_ = XOpenDisplay(NULL);
		if (!display_)
			throw std::runtime_error("Couldn't open X display");

		egl_display_ = eglGetDisplay(display_);
		if (!egl_display_)
			throw std::runtime_error("eglGetDisplay() failed");

		EGLint egl_major, egl_minor;

		if (!eglInitialize(egl_display_, &egl_major, &egl_minor))
			throw std::runtime_error("eglInitialize() failed");

		x_ = options_->Get().preview_x;
		y_ = options_->Get().preview_y;
		width_ = options_->Get().preview_width;
		height_ = options_->Get().preview_height;
		makeEGLWindow("rpicam-app");
	#endif

}

SdlPreview::~SdlPreview()
{
	    printf("DESTROYING CONTEXT AND WINDOW\n");

	SdlPreview::Reset();
	SDL_GL_DeleteContext(sdl_context);
    SDL_DestroyWindow(sdl_window);
	SDL_Quit();

}



void SdlPreview::makeSDLWindow(char const *name, int selected_display)
{
    int n = SDL_GetNumVideoDrivers();
    printf("Available video drivers:\n");
    for (int i = 0; i < n; i++) {
        printf("  %s\n", SDL_GetVideoDriver(i));
    }
    const char* driver = SDL_GetCurrentVideoDriver();
    if (driver) {
        printf("SDL video driver in use: %s\n", driver);
    } else {
        printf("No SDL video driver initialized!\n");
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
	SDL_DisplayMode *modes = NULL;

	int num_displays = SDL_GetNumVideoDisplays();
	std::cout <<"Number of displays: " << num_displays << std::endl;
    if (num_displays > 0) {
         modes = (SDL_DisplayMode *)malloc(sizeof(SDL_DisplayMode) * num_displays);
             for (int i = 0; i < num_displays; ++i) {
        if (SDL_GetCurrentDisplayMode(i, &modes[i]) == 0) {
            std::cout << "Display " << i << ": " << modes[i].w << "x" << modes[i].h << " @ " << modes[i].refresh_rate << "Hz" << std::endl;
        } else {
            std::cerr << "Could not get display mode for display " << i << ": " << SDL_GetError() << std::endl;
        }
    }
    std::cout << "Using Resolution for Display " << selected_display << ": " << modes[selected_display].w << "x" << modes[selected_display].h << std::endl;
    } else {
        std::cerr << "No displays found!" << std::endl;
        SDL_Quit();
        throw std::runtime_error("No displays found");
    }

	
	// 3. Create a window (SDL will use the kmsdrm backend if it's the only option)
    sdl_window = SDL_CreateWindow("My SDL Window",
                              SDL_WINDOWPOS_UNDEFINED,
                              SDL_WINDOWPOS_UNDEFINED,
                              400, 400,
                              SDL_WINDOW_OPENGL);

    printf("Window created successfully\n");

	if (!sdl_window) {
        std::cerr << "Window could not be created! SDL_Error: " << SDL_GetError() << std::endl;
        SDL_Quit();
        throw std::runtime_error("Failed to create SDL window");
    }

    sdl_context = SDL_GL_CreateContext(sdl_window);
    if (!sdl_context) {
        std::cerr << "OpenGL context could not be created! SDL_Error: " << SDL_GetError() << std::endl;
        SDL_DestroyWindow(sdl_window);
        SDL_Quit();
        throw std::runtime_error("Failed to create OpenGL context");
    }
 	// We have to do eglMakeCurrent in the thread where it will run, but we must do it
	// here temporarily so as to get the maximum texture size.
	printf("making current in makesdl\n");

	egl_display_=eglGetCurrentDisplay();



}

static void get_colour_space_info(std::optional<libcamera::ColorSpace> const &cs, EGLint &encoding, EGLint &range)
{
	encoding = EGL_ITU_REC601_EXT;
	range = EGL_YUV_NARROW_RANGE_EXT;

	if (cs == libcamera::ColorSpace::Sycc)
		range = EGL_YUV_FULL_RANGE_EXT;
	else if (cs == libcamera::ColorSpace::Smpte170m)
		/* all good */;
	else if (cs == libcamera::ColorSpace::Rec709)
		encoding = EGL_ITU_REC709_EXT;
	else
		LOG(1, "SdlPreview: unexpected colour space " << libcamera::ColorSpace::toString(cs));
}

void SdlPreview::makeBuffer(int fd, size_t size, StreamInfo const &info, Buffer &buffer)
{

	printf("MAKING buffer with fd %d ", fd);
    

	buffer.fd = fd;
	buffer.size = size;
	buffer.info = info;

	EGLint encoding, range;
	get_colour_space_info(info.colour_space, encoding, range);

	EGLint attribs[] = {
		EGL_WIDTH, static_cast<EGLint>(info.width),
		EGL_HEIGHT, static_cast<EGLint>(info.height),
		EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_YUV420,
		EGL_DMA_BUF_PLANE0_FD_EXT, fd,
		EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
		EGL_DMA_BUF_PLANE0_PITCH_EXT, static_cast<EGLint>(info.stride),
		EGL_DMA_BUF_PLANE1_FD_EXT, fd,
		EGL_DMA_BUF_PLANE1_OFFSET_EXT, static_cast<EGLint>(info.stride * info.height),
		EGL_DMA_BUF_PLANE1_PITCH_EXT, static_cast<EGLint>(info.stride / 2),
		EGL_DMA_BUF_PLANE2_FD_EXT, fd,
		EGL_DMA_BUF_PLANE2_OFFSET_EXT, static_cast<EGLint>(info.stride * info.height + (info.stride / 2) * (info.height / 2)),
		EGL_DMA_BUF_PLANE2_PITCH_EXT, static_cast<EGLint>(info.stride / 2),
		EGL_YUV_COLOR_SPACE_HINT_EXT, encoding,
		EGL_SAMPLE_RANGE_HINT_EXT, range,
		EGL_NONE
	};


	const char* vendor = eglQueryString(egl_display_, EGL_VENDOR);
	const char* version = eglQueryString(egl_display_, EGL_VERSION);

	std::cout << "Vendor: " << vendor << "\n";
	std::cout << "Version: " << version << "\n";


	PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR = nullptr;
    eglCreateImageKHR = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));



	EGLImage image = eglCreateImageKHR(egl_display_, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attribs);
	if (!image)
		throw std::runtime_error("failed to import fd " + std::to_string(fd));

		
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES = nullptr;
    glEGLImageTargetTexture2DOES = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(eglGetProcAddress("glEGLImageTargetTexture2DOES"));

	
    PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR = nullptr;
    eglDestroyImageKHR = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));

	glGenTextures(1, &buffer.texture);
	glActiveTexture(GL_TEXTURE0);

	glBindTexture(GL_TEXTURE_EXTERNAL_OES, buffer.texture);
	glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, image);

	eglDestroyImageKHR(egl_display_, image);
}

void SdlPreview::SetInfoText(const std::string &text)
{
	#ifdef SDL_ENABLED
		if (!text.empty())
			SDL_SetWindowTitle(sdl_window, text.c_str());
	#else 
		if (!text.empty())
			XStoreName(display_, window_, text.c_str());
	#endif
	
}

void SdlPreview::Show(int fd, libcamera::Span<uint8_t> span, StreamInfo const &info)
{

	Buffer &buffer = buffers_[fd];
	if (first_time_)
	{
		#ifdef SDL_ENABLED
			makeSDLWindow("my sdl window");

			SDL_ShowWindow(sdl_window);
			SDL_RaiseWindow(sdl_window);
			SDL_SetWindowPosition(sdl_window, 100, 100); 
			SDL_UpdateWindowSurface(sdl_window);
			
			printf("Window should now be visible\n");
			
			sdl_workercontext = SDL_GL_CreateContext(sdl_window);
			if (!sdl_workercontext) {
				std::cerr << "(SDL_WORKERCONTEXT) OpenGL context could not be created! SDL_Error: " << SDL_GetError() << std::endl;
				SDL_DestroyWindow(sdl_window);
				SDL_Quit();
				throw std::runtime_error("Failed to create OpenGL context");
			}
			if (SDL_GL_MakeCurrent(sdl_window, sdl_workercontext) != 0) {
				std::cerr << "SDL_GL_MakeCurrent failed: " << SDL_GetError() << std::endl;
				throw std::runtime_error("SDL_GL_MakeCurrent failed: " + std::string(SDL_GetError()));
			}

			gl_setup(info.width, info.height, width_, height_);
			first_time_ = false;
		#else 
			if (!eglMakeCurrent(egl_display_, egl_surface_, egl_surface_, egl_context_))
				throw std::runtime_error("eglMakeCurrent failed");
			gl_setup(info.width, info.height, width_, height_);
			first_time_ = false;
		#endif

	}

	if (buffer.fd == -1)
		makeBuffer(fd, span.size(), info, buffer);

	glClearColor(1, 0, 0, 0);
	glClear(GL_COLOR_BUFFER_BIT);

	glBindTexture(GL_TEXTURE_EXTERNAL_OES, buffer.texture);
	glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
	#ifdef SDL_ENABLED
		SDL_GL_SwapWindow(sdl_window);
	#else 
		EGLBoolean success [[maybe_unused]] = eglSwapBuffers(egl_display_, egl_surface_);
	#endif

	if (last_fd_ >= 0)
		done_callback_(last_fd_);
	last_fd_ = fd;
}

void SdlPreview::Reset()
{
	for (auto &it : buffers_)
		glDeleteTextures(1, &it.second.texture);
	
	buffers_.clear();
	last_fd_ = -1;
	#ifdef SDL_ENABLED
 		// SDL code would go here
	#else 
		EGLBoolean success [[maybe_unused]] = eglSwapBuffers(egl_display_, egl_surface_);
	#endif
	first_time_ = true;
}

bool SdlPreview::Quit()
{
	#ifdef SDL_ENABLED
 		SDL_Event event;
		Uint32 windowID = SDL_GetWindowID(sdl_window);

		// Drain all pending events
		while (SDL_PollEvent(&event)) {
			// Global quit (like WM_DELETE_WINDOW at app level)
			if (event.type == SDL_QUIT)
				return true;

			// Per-window close request (exact window match)
			if (event.type == SDL_WINDOWEVENT &&
				event.window.event == SDL_WINDOWEVENT_CLOSE &&
				event.window.windowID == windowID) {
				return true;
			}
		}
	#else 
		XEvent event;
		while (XCheckTypedWindowEvent(display_, window_, ClientMessage, &event))
		{
			if (static_cast<Atom>(event.xclient.data.l[0]) == wm_delete_window_)
				return true;
		}
	#endif
	return false;
}

static Preview *Create(Options const *options)
{
	return new SdlPreview(options);
}

static RegisterPreview reg("sdl", &Create);
