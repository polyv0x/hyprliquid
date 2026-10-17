#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

static GLuint shader(GLenum kind, const std::string& source) {
    const GLuint id = glCreateShader(kind);
    const char* text = source.c_str();
    glShaderSource(id, 1, &text, nullptr);
    glCompileShader(id);
    GLint valid = 0;
    glGetShaderiv(id, GL_COMPILE_STATUS, &valid);
    if (!valid) {
        char log[8192]{};
        glGetShaderInfoLog(id, sizeof(log), nullptr, log);
        throw std::runtime_error(log);
    }
    return id;
}

int main(int argc, char** argv) try {
    require(argc == 6, "Usage: demo-scene shader.frag output.ppm width height variant");
    const int width = std::stoi(argv[3]), height = std::stoi(argv[4]);
    require(width > 0 && height > 0 && width <= 4096 && height <= 4096, "Invalid render size");
    std::ifstream input(argv[1]);
    require(input.good(), "Cannot read scene shader");
    const std::string fragment{std::istreambuf_iterator<char>(input), {}};
    const auto display = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
    require(display != EGL_NO_DISPLAY && eglInitialize(display, nullptr, nullptr), "Cannot initialize EGL");
    require(eglBindAPI(EGL_OPENGL_ES_API), "Cannot bind GLES");
    EGLConfig config;
    EGLint count;
    const EGLint configAttributes[]{EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
                                    EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE};
    require(eglChooseConfig(display, configAttributes, &config, 1, &count) && count, "Cannot select EGL config");
    const EGLint contextAttributes[]{EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    const auto context = eglCreateContext(display, config, EGL_NO_CONTEXT, contextAttributes);
    require(context != EGL_NO_CONTEXT && eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context), "Cannot create GLES context");
    std::cout << "Scene renderer: " << glGetString(GL_RENDERER) << std::endl;

    const GLuint vertex = shader(GL_VERTEX_SHADER,
        "#version 300 es\nin vec2 position; out vec2 vUV; void main(){vUV=position*.5+.5;gl_Position=vec4(position,0,1);}");
    const GLuint pixel = shader(GL_FRAGMENT_SHADER, fragment);
    const GLuint program = glCreateProgram();
    glAttachShader(program, vertex);
    glAttachShader(program, pixel);
    glBindAttribLocation(program, 0, "position");
    glLinkProgram(program);
    GLint linked = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    require(linked, "Cannot link scene shader");
    glUseProgram(program);
    glUniform2f(glGetUniformLocation(program, "uRes"), width, height);
    glUniform1i(glGetUniformLocation(program, "uVariant"), std::stoi(argv[5]));

    GLuint texture, framebuffer, vao, buffer;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    require(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "Incomplete framebuffer");
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &buffer);
    glBindBuffer(GL_ARRAY_BUFFER, buffer);
    constexpr float vertices[]{-1, -1, 1, -1, -1, 1, 1, 1};
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glViewport(0, 0, width, height);
    glDisable(GL_DITHER);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    std::vector<unsigned char> pixels(width * height * 4), row(width * 3);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    require(glGetError() == GL_NO_ERROR, "OpenGL render error");
    std::ofstream output(argv[2], std::ios::binary);
    require(output.good(), "Cannot write scene image");
    output << "P6\n" << width << ' ' << height << "\n255\n";
    for (int y = height - 1; y >= 0; --y) {
        for (int x = 0; x < width; ++x)
            for (int channel = 0; channel < 3; ++channel)
                row[x * 3 + channel] = pixels[(y * width + x) * 4 + channel];
        output.write(reinterpret_cast<const char*>(row.data()), row.size());
    }
    require(output.good(), "Cannot finish scene image");
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(display, context);
    eglTerminate(display);
    std::cout << "Rendered " << argv[2] << std::endl;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
