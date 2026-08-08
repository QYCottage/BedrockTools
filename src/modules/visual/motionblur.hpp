#pragma once

// Motion blur implementation based on code by CrackedMatter
// Original source: https://github.com/CrackedMatter/mcpelauncher-motion-blur
// GitHub: https://github.com/CrackedMatter

#include "../Module.hpp"
#include <GLES2/gl2.h>
#include <cstdint>

class MotionBlurModule : public Module {
public:
    MotionBlurModule();
    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void onFrame() override;

    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

private:
    float m_intensity = 0.90f;
    float m_opacity   = 0.40f;

    bool   m_glInitialized = false;
    bool   m_hasPreviousFrame = false;
    GLuint m_currentFrameTexture   = 0;
    GLuint m_previousFrameTexture  = 0;
    GLuint m_shaderProgram         = 0;
    GLuint m_vertexBuffer          = 0;
    GLuint m_indexBuffer           = 0;
    GLint  m_positionLocation      = -1;
    GLint  m_texCoordLocation      = -1;
    GLint  m_currentFrameLocation  = -1;
    GLint  m_previousFrameLocation = -1;
    GLint  m_blendFactorLocation   = -1;
    GLint  m_opacityLocation       = -1;

    int m_texWidth  = 0;
    int m_texHeight = 0;

    void initGL();
    void allocateTextures(int w, int h);
};
