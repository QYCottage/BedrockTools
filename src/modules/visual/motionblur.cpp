#include "motionblur.hpp"
#include "core/GameHooks.hpp"
#include "core/memory/Hooks.hpp"
#include "modules/ModuleRegistry.hpp"
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <android/log.h>
#include <pl/ModMenu.hpp>
#include <pl/ModMenuConfig.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>
#include <unordered_map>

namespace {

using Clock = std::chrono::steady_clock;
MotionBlurModule* instance = nullptr;
void (*clearOriginal)(GLbitfield) = nullptr;
void (*clearBufferFvOriginal)(GLenum, GLint, const GLfloat*) = nullptr;
void (*clearBufferFiOriginal)(GLenum, GLint, GLfloat, GLint) = nullptr;
void (*renderLevelOriginal)(void*, void*, void*) = nullptr;
void (*cameraOriginal)(void*, void*, void*) = nullptr;
float (*fovOriginal)(void*, float, int) = nullptr;
std::atomic<float> worldFov{70.0f};
thread_local bool processing = false;
std::vector<bedrocktools::hooks::Handle> hooks;

struct GlState {
    GLint drawFramebuffer = 0;
    GLint readFramebuffer = 0;
    GLint program = 0;
    GLint vertexArray = 0;
    GLint activeTexture = 0;
    GLint renderbuffer = 0;
    GLint unpackBuffer = 0;
    std::array<GLint, 4> viewport{};
    std::array<GLboolean, 4> colorMask{};
    std::array<GLint, 3> textures{};
    std::array<GLint, 3> samplers{};
    std::array<GLboolean, 8> capabilities{};
    static constexpr std::array<GLenum, 8> names{
        GL_BLEND, GL_DEPTH_TEST, GL_STENCIL_TEST, GL_CULL_FACE,
        GL_SCISSOR_TEST, GL_RASTERIZER_DISCARD, GL_SAMPLE_ALPHA_TO_COVERAGE, GL_SAMPLE_COVERAGE
    };

    GlState() {
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFramebuffer);
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFramebuffer);
        glGetIntegerv(GL_CURRENT_PROGRAM, &program);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vertexArray);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTexture);
        glGetIntegerv(GL_RENDERBUFFER_BINDING, &renderbuffer);
        glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpackBuffer);
        glGetIntegerv(GL_VIEWPORT, viewport.data());
        glGetBooleanv(GL_COLOR_WRITEMASK, colorMask.data());
        for (std::size_t index = 0; index < names.size(); ++index) capabilities[index] = glIsEnabled(names[index]);
        for (GLuint index = 0; index < textures.size(); ++index) {
            glActiveTexture(GL_TEXTURE0 + index);
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &textures[index]);
            glGetIntegerv(GL_SAMPLER_BINDING, &samplers[index]);
        }
        glActiveTexture(activeTexture);
    }

    ~GlState() {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, drawFramebuffer);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, readFramebuffer);
        glBindRenderbuffer(GL_RENDERBUFFER, renderbuffer);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, unpackBuffer);
        glUseProgram(program);
        glBindVertexArray(vertexArray);
        glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
        glColorMask(colorMask[0], colorMask[1], colorMask[2], colorMask[3]);
        for (std::size_t index = 0; index < names.size(); ++index) {
            if (capabilities[index]) glEnable(names[index]);
            else glDisable(names[index]);
        }
        for (GLuint index = 0; index < textures.size(); ++index) {
            glActiveTexture(GL_TEXTURE0 + index);
            glBindTexture(GL_TEXTURE_2D, textures[index]);
            glBindSampler(index, samplers[index]);
        }
        glActiveTexture(activeTexture);
    }

    void prepare(GLuint vao) const {
        for (auto capability : names) glDisable(capability);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glBindVertexArray(vao);
        for (GLuint index = 0; index < textures.size(); ++index) glBindSampler(index, 0);
    }
};

constexpr const char* vertexShader = R"(#version 300 es
precision highp float;
out vec2 uv;
void main() {
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    uv = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

constexpr const char* blurShader = R"(#version 300 es
precision highp float;
precision highp sampler2D;
in vec2 uv;
layout(location = 0) out vec4 result;
uniform sampler2D scene;
uniform sampler2D depthImage;
uniform mat4 inverseProjection;
uniform mat4 previousProjection;
uniform mat3 rotationDelta;
uniform vec3 translationDelta;
uniform vec2 sceneSize;
uniform float exposure;
uniform float maxStreak;
uniform float depthEnabled;
uniform int samples;
void main() {
    vec2 pixel = 1.0 / sceneSize;
    vec2 safeUv = clamp(uv, pixel * 0.5, 1.0 - pixel * 0.5);
    vec4 center = texture(scene, safeUv);
    float depth = depthEnabled > 0.5 ? texture(depthImage, safeUv).r : 1.0;
    vec4 view = inverseProjection * vec4(safeUv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec3 point = view.xyz / max(abs(view.w), 0.00001);
    vec3 previous = rotationDelta * point;
    if (depthEnabled > 0.5 && depth < 0.9999) previous += translationDelta;
    vec4 projected = previousProjection * vec4(previous, 1.0);
    if (projected.w <= 0.0001) { result = vec4(center.rgb, 0.0); return; }
    vec2 velocity = (safeUv - (projected.xy / projected.w * 0.5 + 0.5)) * exposure;
    float distancePixels = length(velocity * sceneSize);
    velocity *= min(1.0, maxStreak / max(distancePixels, 0.001));
    if (distancePixels < 0.65) { result = vec4(center.rgb, 0.0); return; }
    vec3 total = vec3(0.0);
    float weight = 0.0;
    for (int index = 0; index < 8; ++index) {
        if (index >= samples) break;
        float offset = float(index) / float(samples - 1) - 0.5;
        vec2 position = clamp(safeUv + velocity * offset, pixel * 0.5, 1.0 - pixel * 0.5);
        float tapWeight = 1.0;
        if (depthEnabled > 0.5) {
            float tapDepth = texture(depthImage, position).r;
            tapWeight = exp2(-min(abs(tapDepth - depth) / max(0.0002, (1.0 - depth) * 0.15), 12.0));
        }
        vec3 color = texture(scene, position).rgb;
        total += color * color * tapWeight;
        weight += tapWeight;
    }
    vec3 blurred = weight > 0.001 ? sqrt(total / weight) : center.rgb;
    result = vec4(blurred, smoothstep(0.65, 2.0, distancePixels));
}
)";

constexpr const char* compositeShader = R"(#version 300 es
precision mediump float;
in vec2 uv;
layout(location = 0) out vec4 result;
uniform sampler2D scene;
uniform sampler2D blurred;
uniform float opacity;
void main() {
    vec4 original = texture(scene, uv);
    vec4 blur = texture(blurred, uv);
    result = vec4(mix(original.rgb, blur.rgb, opacity * blur.a), original.a);
}
)";

GLuint compile(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    if (!shader) return 0;
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint success = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (success == GL_TRUE) return shader;
    std::array<char, 1024> message{};
    glGetShaderInfoLog(shader, message.size(), nullptr, message.data());
    __android_log_print(ANDROID_LOG_WARN, "BedrockTools", "Motion blur shader: %s", message.data());
    glDeleteShader(shader);
    return 0;
}

GLuint program(const char* fragmentSource) {
    GLuint vertex = compile(GL_VERTEX_SHADER, vertexShader);
    GLuint fragment = compile(GL_FRAGMENT_SHADER, fragmentSource);
    if (!vertex || !fragment) {
        if (vertex) glDeleteShader(vertex);
        if (fragment) glDeleteShader(fragment);
        return 0;
    }
    GLuint value = glCreateProgram();
    glAttachShader(value, vertex);
    glAttachShader(value, fragment);
    glLinkProgram(value);
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    GLint success = GL_FALSE;
    glGetProgramiv(value, GL_LINK_STATUS, &success);
    if (success == GL_TRUE) return value;
    std::array<char, 1024> message{};
    glGetProgramInfoLog(value, message.size(), nullptr, message.data());
    __android_log_print(ANDROID_LOG_WARN, "BedrockTools", "Motion blur program: %s", message.data());
    glDeleteProgram(value);
    return 0;
}

void bindTexture(GLuint unit, GLuint texture) {
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_2D, texture);
}

GLuint texture(GLenum internal, GLenum format, GLenum type, int width, int height, GLint filter) {
    GLuint value = 0;
    glGenTextures(1, &value);
    bindTexture(0, value);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, internal, width, height, 0, format, type, nullptr);
    return value;
}

void clearHook(GLbitfield mask) {
    if (instance && !processing) instance->beforeClear(mask);
    if (clearOriginal) clearOriginal(mask);
}

void clearBufferFvHook(GLenum buffer, GLint drawBuffer, const GLfloat* values) {
    if (instance && !processing && drawBuffer == 0)
        instance->beforeClear(buffer == GL_DEPTH ? GL_DEPTH_BUFFER_BIT : buffer == GL_COLOR ? GL_COLOR_BUFFER_BIT : 0);
    if (clearBufferFvOriginal) clearBufferFvOriginal(buffer, drawBuffer, values);
}

void clearBufferFiHook(GLenum buffer, GLint drawBuffer, GLfloat depth, GLint stencil) {
    if (instance && !processing && buffer == GL_DEPTH_STENCIL)
        instance->beforeClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    if (clearBufferFiOriginal) clearBufferFiOriginal(buffer, drawBuffer, depth, stencil);
}

void renderLevelHook(void* renderer, void* context, void* view) {
    if (instance) instance->captureWorld(renderer);
    if (renderLevelOriginal) renderLevelOriginal(renderer, context, view);
}

void cameraHook(void* camera, void* component, void* player) {
    if (cameraOriginal) cameraOriginal(camera, component, player);
    MotionBlurCaptureCamera(camera, player);
}

float fovHook(void* self, float delta, int variable) {
    float value = fovOriginal ? fovOriginal(self, delta, variable) : 70.0f;
    if (variable && std::isfinite(value) && value > 1.0f && value < 179.0f) worldFov.store(value, std::memory_order_relaxed);
    return value;
}

void hookAddress(std::uintptr_t target, void* detour, void** original) {
    if (!target) return;
    auto handle = bedrocktools::hooks::install(reinterpret_cast<void*>(target), detour, original);
    if (handle) hooks.push_back(handle);
}

struct ProjectionUniform {
    bool found = false;
    bool matrix = false;
    std::array<GLint, 4> locations{-1, -1, -1, -1};
};

bool readProjection(GLuint programId, std::unordered_map<GLuint, ProjectionUniform>& cache, glm::mat4& projection) {
    if (!programId) return false;
    auto found = cache.find(programId);
    if (found == cache.end()) {
        ProjectionUniform uniform;
        GLint count = 0;
        glGetProgramiv(programId, GL_ACTIVE_UNIFORMS, &count);
        for (GLint index = 0; index < count; ++index) {
            std::array<char, 128> name{};
            GLsizei length = 0;
            GLint size = 0;
            GLenum type = 0;
            glGetActiveUniform(programId, index, name.size(), &length, &size, &type, name.data());
            std::string_view value(name.data(), static_cast<std::size_t>(std::max(0, length)));
            if (value != "u_proj" && value != "u_proj[0]" && value != "PROJ" && value != "PROJ[0]") continue;
            if (type == GL_FLOAT_MAT4 && size == 1) {
                uniform.locations[0] = glGetUniformLocation(programId, name.data());
                uniform.matrix = true;
                uniform.found = uniform.locations[0] >= 0;
            } else if (type == GL_FLOAT_VEC4 && size == 4) {
                std::string base(value);
                if (base.ends_with("[0]")) base.resize(base.size() - 3);
                uniform.found = true;
                for (int column = 0; column < 4; ++column) {
                    uniform.locations[column] = glGetUniformLocation(programId, (base + "[" + std::to_string(column) + "]").c_str());
                    uniform.found = uniform.found && uniform.locations[column] >= 0;
                }
            }
            if (uniform.found) break;
        }
        if (cache.size() >= 256) cache.clear();
        found = cache.emplace(programId, uniform).first;
    }
    const auto& uniform = found->second;
    if (!uniform.found) return false;
    if (uniform.matrix) glGetUniformfv(programId, uniform.locations[0], glm::value_ptr(projection));
    else for (int column = 0; column < 4; ++column) glGetUniformfv(programId, uniform.locations[column], glm::value_ptr(projection[column]));
    for (int column = 0; column < 4; ++column)
        for (int row = 0; row < 4; ++row) if (!std::isfinite(projection[column][row])) return false;
    return std::abs(projection[2][3]) > 0.5f && std::abs(projection[3][3]) < 0.001f &&
        std::abs(projection[0][0]) > 0.01f && std::abs(projection[1][1]) > 0.01f && glGetError() == GL_NO_ERROR;
}

float setting(float value, float fallback, float low, float high) {
    return std::isfinite(value) ? std::clamp(value, low, high) : fallback;
}

}

struct MotionBlurModule::State {
    struct Camera {
        glm::quat orientation{1.0f, 0.0f, 0.0f, 0.0f};
        glm::vec3 position{0.0f};
        void* level = nullptr;
        void* dimension = nullptr;
        Clock::time_point recorded{};
        bool valid = false;
    };

    std::mutex cameraMutex;
    Camera camera;
    Camera previous;
    std::atomic_bool worldActive{false};
    std::atomic_bool reset{true};
    std::atomic_bool releaseRequested{false};
    EGLContext context = EGL_NO_CONTEXT;
    EGLSurface surface = EGL_NO_SURFACE;
    GLuint sceneTexture = 0;
    GLuint depthTexture = 0;
    GLuint blurTexture = 0;
    GLuint sceneFramebuffer = 0;
    GLuint blurFramebuffer = 0;
    GLuint vao = 0;
    GLuint blurProgram = 0;
    GLuint compositeProgram = 0;
    GLint inverseProjection = -1;
    GLint previousProjection = -1;
    GLint rotationDelta = -1;
    GLint translationDelta = -1;
    GLint sceneSize = -1;
    GLint exposure = -1;
    GLint maxStreak = -1;
    GLint depthEnabled = -1;
    GLint samples = -1;
    GLint opacity = -1;
    int width = 0;
    int height = 0;
    int blurWidth = 0;
    int blurHeight = 0;
    GLenum depthFormat = 0;
    GLint worldFramebuffer = -1;
    GLint sceneStartProgram = 0;
    GLenum rejectedDepthFormat = 0;
    bool sceneStarted = false;
    bool processed = false;
    bool failed = false;
    float previousFov = 70.0f;
    Clock::time_point lastFrame{};
    glm::mat4 projection{1.0f};
    bool projectionValid = false;
    std::unordered_map<GLuint, ProjectionUniform> projectionUniforms;

    void release() {
        if (sceneTexture) glDeleteTextures(1, &sceneTexture);
        if (depthTexture) glDeleteTextures(1, &depthTexture);
        if (blurTexture) glDeleteTextures(1, &blurTexture);
        if (sceneFramebuffer) glDeleteFramebuffers(1, &sceneFramebuffer);
        if (blurFramebuffer) glDeleteFramebuffers(1, &blurFramebuffer);
        if (vao) glDeleteVertexArrays(1, &vao);
        if (blurProgram) glDeleteProgram(blurProgram);
        if (compositeProgram) glDeleteProgram(compositeProgram);
        forget();
    }

    void forget() {
        sceneTexture = depthTexture = blurTexture = 0;
        sceneFramebuffer = blurFramebuffer = vao = 0;
        blurProgram = compositeProgram = 0;
        width = height = blurWidth = blurHeight = 0;
        depthFormat = 0;
        failed = false;
        previous.valid = false;
        lastFrame = {};
        projectionValid = false;
        projectionUniforms.clear();
        rejectedDepthFormat = 0;
    }

    bool initialize() {
        if (failed) return false;
        if (blurProgram && compositeProgram && vao) return true;
        blurProgram = program(blurShader);
        compositeProgram = program(compositeShader);
        glGenVertexArrays(1, &vao);
        if (!blurProgram || !compositeProgram || !vao) { failed = true; return false; }
        glUseProgram(blurProgram);
        glUniform1i(glGetUniformLocation(blurProgram, "scene"), 0);
        glUniform1i(glGetUniformLocation(blurProgram, "depthImage"), 1);
        inverseProjection = glGetUniformLocation(blurProgram, "inverseProjection");
        previousProjection = glGetUniformLocation(blurProgram, "previousProjection");
        rotationDelta = glGetUniformLocation(blurProgram, "rotationDelta");
        translationDelta = glGetUniformLocation(blurProgram, "translationDelta");
        sceneSize = glGetUniformLocation(blurProgram, "sceneSize");
        exposure = glGetUniformLocation(blurProgram, "exposure");
        maxStreak = glGetUniformLocation(blurProgram, "maxStreak");
        depthEnabled = glGetUniformLocation(blurProgram, "depthEnabled");
        samples = glGetUniformLocation(blurProgram, "samples");
        glUseProgram(compositeProgram);
        glUniform1i(glGetUniformLocation(compositeProgram, "scene"), 0);
        glUniform1i(glGetUniformLocation(compositeProgram, "blurred"), 2);
        opacity = glGetUniformLocation(compositeProgram, "opacity");
        return glGetError() == GL_NO_ERROR;
    }

    bool allocate(int newWidth, int newHeight, float scale, GLenum format) {
        int bw = std::max(1, static_cast<int>(std::ceil(newWidth * scale)));
        int bh = std::max(1, static_cast<int>(std::ceil(newHeight * scale)));
        if (width == newWidth && height == newHeight && blurWidth == bw && blurHeight == bh && depthFormat == format) return true;
        if (sceneTexture) glDeleteTextures(1, &sceneTexture);
        if (depthTexture) glDeleteTextures(1, &depthTexture);
        if (blurTexture) glDeleteTextures(1, &blurTexture);
        sceneTexture = depthTexture = blurTexture = 0;
        if (!sceneFramebuffer) glGenFramebuffers(1, &sceneFramebuffer);
        if (!blurFramebuffer) glGenFramebuffers(1, &blurFramebuffer);
        sceneTexture = texture(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, newWidth, newHeight, GL_LINEAR);
        blurTexture = texture(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, bw, bh, GL_LINEAR);
        glBindFramebuffer(GL_FRAMEBUFFER, sceneFramebuffer);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sceneTexture, 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
        if (format) {
            const bool packed = format == GL_DEPTH24_STENCIL8 || format == GL_DEPTH32F_STENCIL8;
            const GLenum pixelFormat = packed ? GL_DEPTH_STENCIL : GL_DEPTH_COMPONENT;
            const GLenum type = format == GL_DEPTH_COMPONENT16 ? GL_UNSIGNED_SHORT :
                format == GL_DEPTH_COMPONENT32F ? GL_FLOAT : format == GL_DEPTH32F_STENCIL8 ?
                GL_FLOAT_32_UNSIGNED_INT_24_8_REV : packed ? GL_UNSIGNED_INT_24_8 : GL_UNSIGNED_INT;
            depthTexture = texture(format, pixelFormat, type, newWidth, newHeight, GL_NEAREST);
            glFramebufferTexture2D(GL_FRAMEBUFFER, packed ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT,
                GL_TEXTURE_2D, depthTexture, 0);
        }
        bool complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        glBindFramebuffer(GL_FRAMEBUFFER, blurFramebuffer);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, blurTexture, 0);
        complete = complete && glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        if (!complete || glGetError() != GL_NO_ERROR) { width = height = 0; return false; }
        width = newWidth;
        height = newHeight;
        blurWidth = bw;
        blurHeight = bh;
        depthFormat = format;
        previous.valid = false;
        return true;
    }
};

MotionBlurModule::MotionBlurModule()
    : Module("Motion Blur", "Blurs camera motion in the world while keeping the hand and HUD sharp."), m_state(std::make_unique<State>()) {
    instance = this;
}

MotionBlurModule::~MotionBlurModule() {
    onDisable();
    for (auto handle : hooks) bedrocktools::hooks::remove(handle);
    hooks.clear();
    if (instance == this) instance = nullptr;
}

void MotionBlurModule::onInit() {
    using bedrocktools::memory::SignatureId;
    hookAddress(bedrocktools::memory::resolve(SignatureId::RenderLevel), reinterpret_cast<void*>(renderLevelHook), reinterpret_cast<void**>(&renderLevelOriginal));
    hookAddress(bedrocktools::memory::resolve(SignatureId::UpdatePlayerFromCamera), reinterpret_cast<void*>(cameraHook), reinterpret_cast<void**>(&cameraOriginal));
    hookAddress(bedrocktools::memory::resolve(SignatureId::GetFov), reinterpret_cast<void*>(fovHook), reinterpret_cast<void**>(&fovOriginal));
    void* library = bedrocktools::hooks::openLibrary("libGLESv2.so");
    if (library) {
        hookAddress(bedrocktools::hooks::symbol(library, "glClear"), reinterpret_cast<void*>(clearHook), reinterpret_cast<void**>(&clearOriginal));
        hookAddress(bedrocktools::hooks::symbol(library, "glClearBufferfv"), reinterpret_cast<void*>(clearBufferFvHook), reinterpret_cast<void**>(&clearBufferFvOriginal));
        hookAddress(bedrocktools::hooks::symbol(library, "glClearBufferfi"), reinterpret_cast<void*>(clearBufferFiHook), reinterpret_cast<void**>(&clearBufferFiOriginal));
        bedrocktools::hooks::closeLibrary(library);
    }
    bedrocktools::events::bus().subscribe<bedrocktools::events::ClientInstanceUpdateEvent>([this](auto& event) {
        void* client = event.clientInstance;
        void* player = nullptr;
        if (client) {
            auto table = *reinterpret_cast<void***>(client);
            using Fn = void* (*)(void*);
            if (table) player = reinterpret_cast<Fn>(table[bedrocktools::sdk::offsets::VTable::ClientInstanceGetLocalPlayer])(client);
        }
        m_state->worldActive.store(player != nullptr && !ModuleRegistry::get().keybindBlocked(), std::memory_order_release);
        if (!player || ModuleRegistry::get().keybindBlocked()) m_state->reset.store(true, std::memory_order_release);
    });
}

void MotionBlurModule::onEnable() {
    m_state->reset.store(true, std::memory_order_release);
    m_active.store(true, std::memory_order_release);
}

void MotionBlurModule::onDisable() {
    m_active.store(false, std::memory_order_release);
    m_state->releaseRequested.store(true, std::memory_order_release);
    m_state->reset.store(true, std::memory_order_release);
}

void MotionBlurCaptureCamera(void* camera, void* player) {
    if (instance) instance->captureCamera(camera, player);
}

void MotionBlurModule::captureCamera(void* camera, void* player) {
    if (!m_active.load(std::memory_order_acquire) || !camera || !player) return;
    void* client = bedrocktools::core::gamehooks::clientInstance();
    if (!client) return;
    auto table = *reinterpret_cast<void***>(client);
    using PlayerFn = void* (*)(void*);
    if (!table || reinterpret_cast<PlayerFn>(table[bedrocktools::sdk::offsets::VTable::ClientInstanceGetLocalPlayer])(client) != player) return;
    std::array<float, 4> values{};
    std::memcpy(values.data(), static_cast<std::byte*>(camera) + bedrocktools::sdk::offsets::CameraComponent::mOrientation, sizeof(values));
    glm::quat orientation(values[3], values[0], values[1], values[2]);
    float length = glm::dot(orientation, orientation);
    if (!std::isfinite(length) || length < 0.5f || length > 1.5f) return;
    std::lock_guard lock(m_state->cameraMutex);
    auto& sample = m_state->camera;
    sample.orientation = glm::normalize(orientation);
    sample.level = *reinterpret_cast<void**>(static_cast<std::byte*>(player) + bedrocktools::sdk::offsets::Actor::mLevel);
    sample.dimension = *reinterpret_cast<void**>(static_cast<std::byte*>(player) + bedrocktools::sdk::offsets::Actor::mDimension);
    sample.recorded = Clock::now();
    sample.valid = sample.level != nullptr && sample.dimension != nullptr;
}

void MotionBlurModule::captureWorld(void* renderer) {
    if (!m_active.load(std::memory_order_acquire) || !renderer) return;
    auto player = *reinterpret_cast<std::byte**>(static_cast<std::byte*>(renderer) + bedrocktools::sdk::offsets::LevelRenderer::mLevelRendererPlayer);
    if (!player) return;
    glm::vec3 position;
    std::memcpy(glm::value_ptr(position), player + bedrocktools::sdk::offsets::LevelRendererPlayer::mCamPos, sizeof(float) * 3);
    if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z)) return;
    std::lock_guard lock(m_state->cameraMutex);
    m_state->camera.position = position;
}

void MotionBlurModule::onFrame() {
    auto& state = *m_state;
    if (eglGetCurrentContext() != state.context || eglGetCurrentSurface(EGL_DRAW) != state.surface) return;
    state.sceneStarted = false;
    state.processed = false;
    state.worldFramebuffer = -1;
}

void MotionBlurModule::beforeClear(unsigned int mask) {
    auto& state = *m_state;
    if (state.releaseRequested.load(std::memory_order_acquire) && eglGetCurrentContext() == state.context && state.context != EGL_NO_CONTEXT) {
        GlState saved;
        state.release();
        state.releaseRequested.store(false, std::memory_order_release);
    }
    if (!m_active.load(std::memory_order_acquire) || !state.worldActive.load(std::memory_order_acquire)) return;
    if (!(mask & (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT))) return;
    EGLContext context = eglGetCurrentContext();
    EGLSurface surface = eglGetCurrentSurface(EGL_DRAW);
    EGLDisplay display = eglGetCurrentDisplay();
    if (context == EGL_NO_CONTEXT || surface == EGL_NO_SURFACE || display == EGL_NO_DISPLAY) return;
    GLint major = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &major);
    if (major < 3) return;
    GLint viewport[4]{};
    glGetIntegerv(GL_VIEWPORT, viewport);
    EGLint width = 0, height = 0;
    if (!eglQuerySurface(display, surface, EGL_WIDTH, &width) || !eglQuerySurface(display, surface, EGL_HEIGHT, &height)) return;
    if (viewport[0] != 0 || viewport[1] != 0 || viewport[2] != width || viewport[3] != height || width < 64 || height < 64) return;
    GLint scissor[4]{};
    if (glIsEnabled(GL_SCISSOR_TEST)) {
        glGetIntegerv(GL_SCISSOR_BOX, scissor);
        if (scissor[0] > 0 || scissor[1] > 0 || scissor[0] + scissor[2] < width || scissor[1] + scissor[3] < height) return;
    }
    GLint targetDepthBits = 0;
    glGetIntegerv(GL_DEPTH_BITS, &targetDepthBits);
    if (targetDepthBits == 0) return;
    if (state.context != context || state.surface != surface) {
        if (state.context == context) state.release();
        else state.forget();
        state.context = context;
        state.surface = surface;
        state.sceneStarted = false;
        state.processed = false;
    }
    if (state.reset.exchange(false, std::memory_order_acq_rel)) {
        state.previous.valid = false;
        state.sceneStarted = false;
        state.processed = false;
        state.lastFrame = {};
        state.projectionValid = false;
    }
    GLint framebuffer = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &framebuffer);
    if (mask & GL_COLOR_BUFFER_BIT) {
        if (!state.processed) {
            state.sceneStarted = true;
            state.worldFramebuffer = framebuffer;
            glGetIntegerv(GL_CURRENT_PROGRAM, &state.sceneStartProgram);
        }
        return;
    }
    if (!state.sceneStarted || state.processed || framebuffer != state.worldFramebuffer) return;
    GLint sceneProgram = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &sceneProgram);
    if (!sceneProgram || sceneProgram == state.sceneStartProgram) return;
    state.processed = true;
    State::Camera camera;
    {
        std::lock_guard lock(state.cameraMutex);
        camera = state.camera;
    }
    const auto now = Clock::now();
    const float elapsed = state.lastFrame == Clock::time_point{} ? 0.0f : std::chrono::duration<float>(now - state.lastFrame).count();
    const auto previous = state.previous;
    const float fov = worldFov.load(std::memory_order_relaxed);
    const float oldFov = state.previousFov;
    state.previous = camera;
    state.previousFov = fov;
    state.lastFrame = now;
    if (!camera.valid || !previous.valid || camera.level != previous.level || camera.dimension != previous.dimension ||
        elapsed <= 0.0f || elapsed > 0.15f || std::chrono::duration<float>(now - camera.recorded).count() > 0.25f) return;
    const float rotation = 1.0f - std::abs(glm::dot(camera.orientation, previous.orientation));
    const float distance = glm::length(camera.position - previous.position);
    const bool useDepth = m_depthBlur.load(std::memory_order_relaxed);
    if (rotation > 0.08f || distance > 8.0f || std::abs(fov - oldFov) > 8.0f) return;
    const float intensity = m_intensity.load(std::memory_order_relaxed);
    const float opacityValue = m_opacity.load(std::memory_order_relaxed);
    const float blendBoost = std::max(1.0f, opacityValue);
    if (intensity <= 0.0f || opacityValue <= 0.0f || (rotation < 0.0000002f && (!useDepth || distance < 0.001f) && std::abs(fov - oldFov) < 0.01f)) return;
    if (glGetError() != GL_NO_ERROR) return;
    processing = true;
    struct ProcessingGuard { ~ProcessingGuard() { processing = false; } } processingGuard;
    GlState saved;
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    glm::mat4 projection{1.0f};
    const bool nativeProjection = readProjection(saved.program, state.projectionUniforms, projection);
    if (!nativeProjection && rotation < 0.0000002f && std::abs(fov - oldFov) < 0.01f) return;
    if (!nativeProjection) projection = glm::perspective(glm::radians(fov), static_cast<float>(width) / height, 0.1f, 1000.0f);
    const glm::mat4 previousProjection = state.projectionValid ? state.projection : projection;
    state.projection = projection;
    state.projectionValid = nativeProjection;
    GLint depthBits = 0, stencilBits = 0;
    glGetIntegerv(GL_DEPTH_BITS, &depthBits);
    glGetIntegerv(GL_STENCIL_BITS, &stencilBits);
    GLenum depthFormat = !useDepth || !nativeProjection || depthBits == 0 ? 0 : depthBits <= 16 ? GL_DEPTH_COMPONENT16 :
        depthBits <= 24 ? (stencilBits ? GL_DEPTH24_STENCIL8 : GL_DEPTH_COMPONENT24) :
        (stencilBits ? GL_DEPTH32F_STENCIL8 : GL_DEPTH_COMPONENT32F);
    if (depthFormat == state.rejectedDepthFormat) depthFormat = 0;
    if (framebuffer) {
        GLint component = 0;
        glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE, &component);
        if (component != GL_UNSIGNED_NORMALIZED) return;
    }
    if (!state.initialize() || !state.allocate(width, height, m_resolutionScale.load(std::memory_order_relaxed), depthFormat)) return;
    saved.prepare(state.vao);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer);
    GLint readBuffer = 0;
    glGetIntegerv(GL_READ_BUFFER, &readBuffer);
    glReadBuffer(framebuffer ? GL_COLOR_ATTACHMENT0 : GL_BACK);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, state.sceneFramebuffer);
    glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    const bool colorCopied = glGetError() == GL_NO_ERROR;
    bool depthCopied = false;
    if (colorCopied && depthFormat) {
        glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
        depthCopied = glGetError() == GL_NO_ERROR;
        if (!depthCopied) state.rejectedDepthFormat = depthFormat;
    }
    glReadBuffer(readBuffer);
    if (!colorCopied) return;
    const glm::mat3 previousView = glm::mat3_cast(glm::conjugate(previous.orientation));
    const glm::mat3 rotationDelta = previousView * glm::mat3_cast(camera.orientation);
    const glm::vec3 translation = previousView * (camera.position - previous.position);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, state.blurFramebuffer);
    glViewport(0, 0, state.blurWidth, state.blurHeight);
    glUseProgram(state.blurProgram);
    bindTexture(0, state.sceneTexture);
    bindTexture(1, depthCopied ? state.depthTexture : state.sceneTexture);
    const auto inverse = glm::inverse(projection);
    glUniformMatrix4fv(state.inverseProjection, 1, GL_FALSE, glm::value_ptr(inverse));
    glUniformMatrix4fv(state.previousProjection, 1, GL_FALSE, glm::value_ptr(previousProjection));
    glUniformMatrix3fv(state.rotationDelta, 1, GL_FALSE, glm::value_ptr(rotationDelta));
    glUniform3fv(state.translationDelta, 1, glm::value_ptr(translation));
    glUniform2f(state.sceneSize, static_cast<float>(width), static_cast<float>(height));
    glUniform1f(state.exposure, intensity * blendBoost * 0.5f * std::clamp(1.0f / (60.0f * elapsed), 0.25f, 4.0f));
    glUniform1f(state.maxStreak, 24.0f * std::max(1.0f, intensity) * blendBoost);
    glUniform1f(state.depthEnabled, depthCopied ? 1.0f : 0.0f);
    glUniform1i(state.samples, m_samples.load(std::memory_order_relaxed));
    glDrawArrays(GL_TRIANGLES, 0, 3);
    if (glGetError() != GL_NO_ERROR) return;
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, framebuffer);
    glViewport(0, 0, width, height);
    glUseProgram(state.compositeProgram);
    bindTexture(2, state.blurTexture);
    glUniform1f(state.opacity, std::min(opacityValue, 1.0f));
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

void MotionBlurModule::onMenuRegistered() {
    using namespace pl::modmenu;
    ConfigSchemaBuilder schema;
    schema.defaultCategory("blur")
        .category("blur", "Motion Blur", "World motion and appearance")
        .category("quality", "Quality", "Balance sharpness and performance");
    auto slider = [&](const char* key, const char* title, const char* category,
                      ConfigControlTypeV2 type, const char* low, const char* high, const char* step,
                      const char* description = "") {
        ConfigNodeV2 node;
        node.id = key;
        node.key = key;
        node.title = title;
        node.description = description;
        node.category = category;
        node.type = type;
        node.minValue = low;
        node.maxValue = high;
        node.step = step;
        schema.node(std::move(node));
    };
    slider("intensity", "Strength", "blur", ConfigControlTypeV2::SliderFloat, "0", "5", "0.05",
        "Amount of camera-motion blur. Values above 1 allow longer blur streaks.");
    slider("opacity", "Blend", "blur", ConfigControlTypeV2::SliderFloat, "0", "2", "0.05",
        "Blend in the blurred world. Values above 1 boost blur strength further.");
    slider("samples", "Samples", "quality", ConfigControlTypeV2::SliderInt, "4", "8", "1");
    slider("resolutionScale", "Render Scale", "quality", ConfigControlTypeV2::SliderFloat, "0.5", "1", "0.05");
    ConfigNodeV2 depth;
    depth.id = "depthBlur";
    depth.key = "depthBlur";
    depth.title = "Movement Blur";
    depth.description = "Blur movement through the world when depth information is available.";
    depth.category = "blur";
    depth.type = ConfigControlTypeV2::Toggle;
    schema.node(std::move(depth));
    pl::modmenu::setConfigSchemaJson(moduleId, schema.toJson());
}

void MotionBlurModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    m_intensity.store(setting(j.value("intensity", m_intensity.load()), 0.9f, 0.0f, 5.0f));
    m_opacity.store(setting(j.value("opacity", m_opacity.load()), 0.4f, 0.0f, 2.0f));
    m_resolutionScale.store(setting(j.value("resolutionScale", m_resolutionScale.load()), 0.5f, 0.5f, 1.0f));
    m_samples.store(std::clamp(j.value("samples", m_samples.load()), 4, 8));
    m_depthBlur.store(j.value("depthBlur", m_depthBlur.load()));
}

void MotionBlurModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["intensity"] = m_intensity.load();
    j["opacity"] = m_opacity.load();
    j["resolutionScale"] = m_resolutionScale.load();
    j["samples"] = m_samples.load();
    j["depthBlur"] = m_depthBlur.load();
}
