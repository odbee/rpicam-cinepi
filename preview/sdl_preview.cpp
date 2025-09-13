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
	void makeBuffer(int fd, size_t size, StreamInfo const &info, Buffer &buffer);
	// ::Display *display_;
	EGLDisplay egl_display_;
	std::map<int, Buffer> buffers_; // map the DMABUF's fd to the Buffer
	int last_fd_;
	bool first_time_;
	// size of preview window
	GLuint VAO, VBO, EBO;
	void gl_setup(int width, int height, int window_width, int window_height);
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
	"  FragColor = texture2D(s, texcoord)+vec4(0.3,0.0,0.0,0.0);\n"
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


SdlPreview::SdlPreview(Options const *options) : Preview(options), last_fd_(-1), first_time_(true)
{
	if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        std::cerr << "SDL could not initialize! SDL_Error: " << SDL_GetError() << std::endl;
        throw std::runtime_error("Failed to initialize SDL");
    }
	makeSDLWindow("my sdl window");

	// printf("Window should now be visible\n");

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
	// SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);
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
	sdl_workercontext = SDL_GL_CreateContext(sdl_window);


	printf("AHAHAHAHAHAHAHAHASDFASDFASF\n");

	// egl_display_=eglGetCurrentDisplay();
	SDL_ShowWindow(sdl_window);
	SDL_RaiseWindow(sdl_window);
	SDL_SetWindowPosition(sdl_window, 100, 100); 
	SDL_UpdateWindowSurface(sdl_window);
	SDL_GL_MakeCurrent(sdl_window, 0);


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
	egl_display_=eglGetCurrentDisplay();

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
		if (!text.empty())
			SDL_SetWindowTitle(sdl_window, text.c_str());

	
}

void SdlPreview::Show(int fd, libcamera::Span<uint8_t> span, StreamInfo const &info)
{

	Buffer &buffer = buffers_[fd];
	if (first_time_)
	{
		// makeSDLWindow("my sdl window");

		printf("Window should now be visible\n");
		
		// sdl_workercontext = SDL_GL_CreateContext(sdl_window);
		// if (!sdl_workercontext) {
		// 	std::cerr << "(SDL_WORKERCONTEXT) OpenGL context could not be created! SDL_Error: " << SDL_GetError() << std::endl;
		// 	SDL_DestroyWindow(sdl_window);
		// 	SDL_Quit();
		// 	throw std::runtime_error("Failed to create OpenGL context");
		// }

		if (SDL_GL_MakeCurrent(sdl_window, sdl_workercontext) != 0) {
			std::cerr << "SDL_GL_MakeCurrent failed: " << SDL_GetError() << std::endl;
			throw std::runtime_error("SDL_GL_MakeCurrent failed: " + std::string(SDL_GetError()));
		}

		gl_setup(info.width, info.height, width_, height_);
		first_time_ = false;


	}

	if (buffer.fd == -1)
		makeBuffer(fd, span.size(), info, buffer);
	glClearColor(1, 0, 0, 0);
	glClear(GL_COLOR_BUFFER_BIT);
	glBindTexture(GL_TEXTURE_EXTERNAL_OES, buffer.texture);
	glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
	SDL_GL_SwapWindow(sdl_window);

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
	first_time_ = true;
}

bool SdlPreview::Quit()
{
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

	return false;
}

static Preview *Create(Options const *options)
{
	return new SdlPreview(options);
}

static RegisterPreview reg("sdl", &Create);
