#include "viewmodel.hpp"
#include "core/memory/Hooks.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Offsets.hpp>

static ViewModelModule* g_viewModelMod = nullptr;
static std::vector<bedrocktools::hooks::Handle> g_viewModelHooks;

struct ViewModelMatrixBase {
    bool valid = false;
    glm::mat4 matrix = glm::mat4(1.0f);
    glm::mat4 inverse = glm::mat4(1.0f);
};

struct ViewModelFirstPersonFrame {
    void* renderContext = nullptr;
    ViewModelMatrixBase base;
    ViewModelMatrixBase mainBase;
};

struct ViewModelHandFrame {
    void* renderContext = nullptr;
    bool mainHand = true;
    ViewModelMatrixBase base;
};

struct ViewModelRenderState {
    void* renderContext = nullptr;
    bool mainHand = true;
    bool firstPerson = false;
    bool blockItem = false;
    ViewModelMatrixBase base;
    ViewModelMatrixBase itemBase;
};

static thread_local ViewModelFirstPersonFrame* g_firstPersonFrame = nullptr;
static thread_local ViewModelHandFrame* g_handFrame = nullptr;
static thread_local ViewModelRenderState* g_viewModelRenderState = nullptr;
static constexpr int kFirstPersonPass = 1 << 0;

static glm::mat4* getCurrentMatrix(void* renderContext) {
    if (!renderContext) return nullptr;

    auto rcBase = reinterpret_cast<std::uintptr_t>(renderContext);
    auto wrapper = *reinterpret_cast<std::uintptr_t*>(
        rcBase + bedrocktools::sdk::offsets::RenderContext::mMatrixStackWrapper
    );
    if (!wrapper) return nullptr;

    auto matrixStack = *reinterpret_cast<std::uintptr_t*>(
        wrapper + bedrocktools::sdk::offsets::MatrixStackWrapper::mMatrixStack
    );
    if (!matrixStack) return nullptr;

    auto blocks = *reinterpret_cast<std::uintptr_t**>(
        matrixStack + bedrocktools::sdk::offsets::MatrixStack::mBlocks
    );
    auto start = *reinterpret_cast<std::size_t*>(
        matrixStack + bedrocktools::sdk::offsets::MatrixStack::mStart
    );
    auto size = *reinterpret_cast<std::size_t*>(
        matrixStack + bedrocktools::sdk::offsets::MatrixStack::mSize
    );
    if (!blocks || size == 0) return nullptr;

    constexpr std::size_t elementsPerBlock = sizeof(glm::mat4) < 256
        ? 4096 / sizeof(glm::mat4)
        : 16;
    auto last = start + size - 1;
    auto blockIndex = last / elementsPerBlock;
    auto elementIndex = last % elementsPerBlock;
    auto block = blocks[blockIndex];
    if (!block) return nullptr;

    return reinterpret_cast<glm::mat4*>(block) + elementIndex;
}

static float finiteOr(float value, float fallback) {
    return std::isfinite(value) ? value : fallback;
}

static float normalizeRotation(float value) {
    value = finiteOr(value, 0.0f);
    value = std::fmod(value, 360.0f);
    if (value > 180.0f) value -= 360.0f;
    if (value < -180.0f) value += 360.0f;
    return value;
}

static float normalizeScale(float value) {
    value = finiteOr(value, 1.0f);
    return std::clamp(value, 0.05f, 4.0f);
}

static void sanitizeTransform(ViewModelHandTransform& transform) {
    transform.positionX = finiteOr(transform.positionX, 0.0f);
    transform.positionY = finiteOr(transform.positionY, 0.0f);
    transform.positionZ = finiteOr(transform.positionZ, 0.0f);
    transform.rotationX = normalizeRotation(transform.rotationX);
    transform.rotationY = normalizeRotation(transform.rotationY);
    transform.rotationZ = normalizeRotation(transform.rotationZ);
    transform.scaleX = normalizeScale(transform.scaleX);
    transform.scaleY = normalizeScale(transform.scaleY);
    transform.scaleZ = normalizeScale(transform.scaleZ);
}

static bool finiteMatrix(const glm::mat4& matrix) {
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            if (!std::isfinite(matrix[column][row])) return false;
        }
    }
    return true;
}

static void captureBaseMatrix(ViewModelMatrixBase& base, void* renderContext) {
    base.valid = false;
    base.matrix = glm::mat4(1.0f);
    base.inverse = glm::mat4(1.0f);

    auto* matrix = getCurrentMatrix(renderContext);
    if (!matrix || !finiteMatrix(*matrix)) return;

    float determinant = glm::determinant(*matrix);
    if (!std::isfinite(determinant) || std::abs(determinant) < 0.000001f) return;

    glm::mat4 inverse = glm::inverse(*matrix);
    if (!finiteMatrix(inverse)) return;

    base.matrix = *matrix;
    base.inverse = inverse;
    base.valid = true;
}

static void prepareRenderState(ViewModelRenderState& state, void* renderContext) {
    state.renderContext = renderContext;

    auto* hand = g_handFrame;
    if (state.firstPerson && hand && hand->renderContext == renderContext && hand->base.valid) {
        state.base = hand->base;
        return;
    }

    auto* frame = g_firstPersonFrame;
    if (state.firstPerson && frame && frame->renderContext == renderContext) {
        if (state.mainHand && frame->mainBase.valid) {
            state.base = frame->mainBase;
            return;
        }
        if (frame->base.valid) {
            state.base = frame->base;
            return;
        }
    }

    captureBaseMatrix(state.base, renderContext);
}

static void captureItemBase() {
    auto* state = g_viewModelRenderState;
    if (!state || !state->renderContext || state->itemBase.valid) return;
    if (!state->firstPerson && !(g_viewModelMod && g_viewModelMod->m_applyThirdPerson)) return;
    captureBaseMatrix(state->itemBase, state->renderContext);
}

static void mirrorAffineX(glm::mat4& matrix) {
    constexpr float signs[3] = {-1.0f, 1.0f, 1.0f};
    for (int column = 0; column < 3; ++column) {
        for (int row = 0; row < 3; ++row) {
            matrix[column][row] *= signs[column] * signs[row];
        }
    }
    matrix[3][0] = -matrix[3][0];
}

static bool swapItemHandTransform(
    glm::mat4& matrix,
    const ViewModelMatrixBase& cameraBase,
    const ViewModelMatrixBase& itemBase,
    bool mirrorItemLocal
) {
    if (!cameraBase.valid || !itemBase.valid) return false;

    glm::mat4 handLocal = cameraBase.inverse * itemBase.matrix;
    glm::mat4 itemLocal = itemBase.inverse * matrix;
    if (!finiteMatrix(handLocal) || !finiteMatrix(itemLocal)) return false;

    mirrorAffineX(handLocal);
    if (mirrorItemLocal) mirrorAffineX(itemLocal);

    glm::mat4 rebuilt = cameraBase.matrix * handLocal * itemLocal;
    if (!finiteMatrix(rebuilt)) return false;

    matrix = rebuilt;
    return true;
}

static bool swapItemPoseFallback(glm::mat4& matrix, const ViewModelMatrixBase& cameraBase) {
    if (!cameraBase.valid) return false;

    glm::mat4 local = cameraBase.inverse * matrix;
    if (!finiteMatrix(local)) return false;

    mirrorAffineX(local);
    glm::mat4 rebuilt = cameraBase.matrix * local;
    if (!finiteMatrix(rebuilt)) return false;

    matrix = rebuilt;
    return true;
}

static bool swapItemPositionFallback(glm::mat4& matrix, const ViewModelMatrixBase& cameraBase) {
    if (!cameraBase.valid) return false;

    glm::mat4 local = cameraBase.inverse * matrix;
    if (!finiteMatrix(local)) return false;

    local[3][0] = -local[3][0];
    glm::mat4 rebuilt = cameraBase.matrix * local;
    if (!finiteMatrix(rebuilt)) return false;

    matrix = rebuilt;
    return true;
}

static void applyLocalRotationAndScale(glm::mat4& matrix, const ViewModelHandTransform& transform) {
    if (transform.rotationX != 0.0f) {
        matrix = glm::rotate(matrix, glm::radians(transform.rotationX), glm::vec3(1.0f, 0.0f, 0.0f));
    }
    if (transform.rotationY != 0.0f) {
        matrix = glm::rotate(matrix, glm::radians(transform.rotationY), glm::vec3(0.0f, 1.0f, 0.0f));
    }
    if (transform.rotationZ != 0.0f) {
        matrix = glm::rotate(matrix, glm::radians(transform.rotationZ), glm::vec3(0.0f, 0.0f, 1.0f));
    }
    if (transform.scaleX != 1.0f || transform.scaleY != 1.0f || transform.scaleZ != 1.0f) {
        matrix = glm::scale(matrix, glm::vec3(transform.scaleX, transform.scaleY, transform.scaleZ));
    }
}

static void applyTransform(
    glm::mat4& matrix,
    const ViewModelMatrixBase& base,
    const ViewModelHandTransform& transform,
    bool swapHands
) {
    const glm::vec3 position(transform.positionX, transform.positionY, transform.positionZ);
    const bool hasPosition = transform.positionX != 0.0f || transform.positionY != 0.0f || transform.positionZ != 0.0f;

    if (!base.valid) {
        if (hasPosition) matrix = glm::translate(matrix, position);
        applyLocalRotationAndScale(matrix, transform);
        return;
    }

    glm::mat4 local = base.inverse * matrix;
    if (!finiteMatrix(local)) return;

    if (swapHands) mirrorAffineX(local);
    if (hasPosition) local = glm::translate(glm::mat4(1.0f), position) * local;
    applyLocalRotationAndScale(local, transform);

    glm::mat4 rebuilt = base.matrix * local;
    if (finiteMatrix(rebuilt)) matrix = rebuilt;
}

static void (*_renderFirstPerson_orig)(void*, void*, void*, int) = nullptr;

static void _renderFirstPerson_hook(void* self, void* renderContext, void* previousProjection, int itemFlags) {
    ViewModelFirstPersonFrame frame;
    frame.renderContext = renderContext;
    captureBaseMatrix(frame.base, renderContext);

    auto* previous = g_firstPersonFrame;
    g_firstPersonFrame = &frame;

    if (_renderFirstPerson_orig) {
        _renderFirstPerson_orig(self, renderContext, previousProjection, itemFlags);
    }

    g_firstPersonFrame = previous;
}

static void (*_renderHand_orig)(void*, void*, void*, int, float) = nullptr;

static void _renderHand_hook(
    void* self,
    void* renderContext,
    void* player,
    int renderingMainHand,
    float frameAlpha
) {
    ViewModelHandFrame frame;
    frame.renderContext = renderContext;
    frame.mainHand = renderingMainHand != 0;
    captureBaseMatrix(frame.base, renderContext);

    auto* previous = g_handFrame;
    g_handFrame = &frame;

    if (_renderHand_orig) {
        _renderHand_orig(self, renderContext, player, renderingMainHand, frameAlpha);
    }

    g_handFrame = previous;
}

static void (*_renderItem_orig)(void*, void*, void*, void*, int, int, int, int) = nullptr;

static void _renderItem_hook(
    void* self,
    void* renderContext,
    void* entity,
    void* item,
    int posAndRotSet,
    int itemFlags,
    int useMatrixAsIs,
    int renderingMainHand
) {
    const bool inFirstPersonFrame = g_firstPersonFrame && g_firstPersonFrame->renderContext == renderContext;
    ViewModelRenderState state;
    state.mainHand = renderingMainHand != 0;
    state.firstPerson = inFirstPersonFrame || ((itemFlags & kFirstPersonPass) != 0);
    prepareRenderState(state, renderContext);

    auto* previous = g_viewModelRenderState;
    g_viewModelRenderState = &state;

    if (_renderItem_orig) {
        _renderItem_orig(
            self,
            renderContext,
            entity,
            item,
            posAndRotSet,
            itemFlags,
            useMatrixAsIs,
            renderingMainHand
        );
    }

    g_viewModelRenderState = previous;
}

static void (*_transformWorldMatrixFromJson_orig)(void*, void*, int, int, float) = nullptr;

static void _transformWorldMatrixFromJson_hook(
    void* matrixContext,
    void* item,
    int renderingMainHand,
    int itemFlags,
    float textureScale
) {
    captureItemBase();

    if (_transformWorldMatrixFromJson_orig) {
        _transformWorldMatrixFromJson_orig(
            matrixContext,
            item,
            renderingMainHand,
            itemFlags,
            textureScale
        );
    }
}

static void (*_applyDefaultItemTransforms_orig)(
    void*, void*, void*, int, void*, int, void*, float, int
) = nullptr;

static void _applyDefaultItemTransforms_hook(
    void* self,
    void* worldMatrix,
    void* item,
    int isInHandItem,
    void* blockType,
    int blockShape,
    void* renderObjectCall,
    float heldItemScale,
    int posAndRotSetByJson
) {
    auto* state = g_viewModelRenderState;
    if (state && blockType != nullptr) state->blockItem = true;
    captureItemBase();

    if (_applyDefaultItemTransforms_orig) {
        _applyDefaultItemTransforms_orig(
            self,
            worldMatrix,
            item,
            isInHandItem,
            blockType,
            blockShape,
            renderObjectCall,
            heldItemScale,
            posAndRotSetByJson
        );
    }
}

static void (*_renderOffhandItem_orig)(void*, void*, void*, int) = nullptr;

static void _renderOffhandItem_hook(void* self, void* renderContext, void* player, int itemFlags) {
    const bool inFirstPersonFrame = g_firstPersonFrame && g_firstPersonFrame->renderContext == renderContext;

    ViewModelHandFrame handFrame;
    handFrame.renderContext = renderContext;
    handFrame.mainHand = false;
    captureBaseMatrix(handFrame.base, renderContext);

    auto* previousHand = g_handFrame;
    g_handFrame = &handFrame;

    ViewModelRenderState state;
    state.mainHand = false;
    state.firstPerson = inFirstPersonFrame || ((itemFlags & kFirstPersonPass) != 0);
    state.renderContext = renderContext;
    if (handFrame.base.valid) {
        state.base = handFrame.base;
    } else {
        prepareRenderState(state, renderContext);
    }

    auto* previousState = g_viewModelRenderState;
    g_viewModelRenderState = &state;

    if (_renderOffhandItem_orig) {
        _renderOffhandItem_orig(self, renderContext, player, itemFlags);
    }

    if (inFirstPersonFrame && g_firstPersonFrame && g_firstPersonFrame->renderContext == renderContext) {
        captureBaseMatrix(g_firstPersonFrame->mainBase, renderContext);
    }

    g_viewModelRenderState = previousState;
    g_handFrame = previousHand;
}

static void (*_renderObject_orig)(void*, void*, void*, void*, int) = nullptr;

static void _renderObject_hook(void* self, void* renderContext, void* renderObject, void* renderMetadata, int itemFlags) {
    auto* state = g_viewModelRenderState;
    const bool firstPerson = state && (state->firstPerson || ((itemFlags & kFirstPersonPass) != 0));
    const bool shouldApply = g_viewModelMod && g_viewModelMod->enabled && state &&
        (firstPerson || g_viewModelMod->m_applyThirdPerson);

    glm::mat4* matrix = shouldApply ? getCurrentMatrix(renderContext) : nullptr;
    glm::mat4 original(1.0f);
    const bool restore = matrix && finiteMatrix(*matrix);

    if (restore) {
        original = *matrix;
        const auto& transform = state->mainHand ? g_viewModelMod->m_mainHand : g_viewModelMod->m_offHand;

        if (firstPerson && g_viewModelMod->m_swapHands) {
            const bool mirrorItemLocal = state->mainHand && !state->blockItem;
            if (!swapItemHandTransform(*matrix, state->base, state->itemBase, mirrorItemLocal)) {
                if (state->mainHand) {
                    swapItemPoseFallback(*matrix, state->base);
                } else {
                    swapItemPositionFallback(*matrix, state->base);
                }
            }

            if (state->mainHand && !state->blockItem) {
                glm::mat4 corrected = glm::rotate(
                    *matrix,
                    glm::radians(180.0f),
                    glm::vec3(0.0f, 0.0f, 1.0f)
                );
                if (finiteMatrix(corrected)) *matrix = corrected;
            }
        }

        applyTransform(*matrix, state->base, transform, false);
    }

    if (_renderObject_orig) {
        _renderObject_orig(self, renderContext, renderObject, renderMetadata, itemFlags);
    }

    if (restore) *matrix = original;
}

static void applyFirstPersonArmTransform(
    glm::mat4& matrix,
    const ViewModelMatrixBase& base,
    const ViewModelHandTransform& transform,
    bool swapHands
) {
    const glm::vec3 position(transform.positionX, transform.positionY, transform.positionZ);
    const bool hasPosition = transform.positionX != 0.0f || transform.positionY != 0.0f || transform.positionZ != 0.0f;

    if (!base.valid) {
        if (hasPosition) matrix = glm::translate(matrix, position);
        applyLocalRotationAndScale(matrix, transform);
        return;
    }

    glm::mat4 local = base.inverse * matrix;
    if (!finiteMatrix(local)) return;

    if (swapHands) {
        glm::mat4 reflection(1.0f);
        reflection[0][0] = -1.0f;
        local = reflection * local;
    }

    if (hasPosition) local = glm::translate(glm::mat4(1.0f), position) * local;
    applyLocalRotationAndScale(local, transform);

    glm::mat4 rebuilt = base.matrix * local;
    if (finiteMatrix(rebuilt)) matrix = rebuilt;
}

static void (*_actorRenderDispatcherRender_orig)(void*, void*, void*, void*, void*, int) = nullptr;

static void _actorRenderDispatcherRender_hook(
    void* self,
    void* renderContext,
    void* actor,
    void* position,
    void* rotation,
    int ignoreLighting
) {
    auto* hand = g_handFrame;
    auto* firstPerson = g_firstPersonFrame;
    const bool handScoped = hand && hand->renderContext == renderContext;
    const bool firstPersonScoped = !handScoped && firstPerson && firstPerson->renderContext == renderContext;
    const bool shouldApply = g_viewModelMod && g_viewModelMod->enabled && !g_viewModelRenderState &&
        (handScoped || firstPersonScoped);

    glm::mat4* matrix = shouldApply ? getCurrentMatrix(renderContext) : nullptr;
    glm::mat4 original(1.0f);
    const bool restore = matrix && finiteMatrix(*matrix);

    if (restore) {
        original = *matrix;
        if (handScoped) {
            const auto& transform = hand->mainHand ? g_viewModelMod->m_mainHand : g_viewModelMod->m_offHand;
            applyTransform(*matrix, hand->base, transform, g_viewModelMod->m_swapHands);
        } else {
            applyFirstPersonArmTransform(
                *matrix,
                firstPerson->base,
                g_viewModelMod->m_mainHand,
                g_viewModelMod->m_swapHands
            );
        }
    }

    if (_actorRenderDispatcherRender_orig) {
        _actorRenderDispatcherRender_orig(self, renderContext, actor, position, rotation, ignoreLighting);
    }

    if (restore) *matrix = original;
}

ViewModelModule::ViewModelModule()
    : Module("View Model", "Advanced main-hand and offhand position, rotation, scale, hand swap and item FOV controls.") {
    g_viewModelMod = this;
}

ViewModelModule::~ViewModelModule() {
    removeHooks();
    if (g_viewModelMod == this) g_viewModelMod = nullptr;
}

float ViewModelModule::getItemFov() const {
    return m_itemFov;
}

ViewModelModule* GetViewModelModule() {
    return g_viewModelMod;
}

void ViewModelModule::installHooks() {
    if (m_hooksInstalled) return;

    if (!m_renderFirstPersonHooked) {
        auto address = bedrocktools::memory::resolve(
            bedrocktools::memory::SignatureId::ItemInHandRendererRenderFirstPerson
        );
        if (address) {
            auto hook = bedrocktools::hooks::install(
                reinterpret_cast<void*>(address),
                reinterpret_cast<void*>(_renderFirstPerson_hook),
                reinterpret_cast<void**>(&_renderFirstPerson_orig)
            );
            if (hook) g_viewModelHooks.push_back(hook);
            m_renderFirstPersonHooked = hook != nullptr;
        }
    }

    if (!m_renderHandHooked) {
        auto address = bedrocktools::memory::resolve(
            bedrocktools::memory::SignatureId::ItemInHandRendererRenderHand
        );
        if (address) {
            auto hook = bedrocktools::hooks::install(
                reinterpret_cast<void*>(address),
                reinterpret_cast<void*>(_renderHand_hook),
                reinterpret_cast<void**>(&_renderHand_orig)
            );
            if (hook) g_viewModelHooks.push_back(hook);
            m_renderHandHooked = hook != nullptr;
        }
    }

    if (!m_actorRenderDispatcherHooked) {
        auto address = bedrocktools::memory::resolve(
            bedrocktools::memory::SignatureId::ActorRenderDispatcherRender
        );
        if (address) {
            auto hook = bedrocktools::hooks::install(
                reinterpret_cast<void*>(address),
                reinterpret_cast<void*>(_actorRenderDispatcherRender_hook),
                reinterpret_cast<void**>(&_actorRenderDispatcherRender_orig)
            );
            if (hook) g_viewModelHooks.push_back(hook);
            m_actorRenderDispatcherHooked = hook != nullptr;
        }
    }

    if (!m_renderObjectHooked) {
        auto address = bedrocktools::memory::resolve(
            bedrocktools::memory::SignatureId::ItemInHandRenderObject
        );
        if (address) {
            auto hook = bedrocktools::hooks::install(
                reinterpret_cast<void*>(address),
                reinterpret_cast<void*>(_renderObject_hook),
                reinterpret_cast<void**>(&_renderObject_orig)
            );
            if (hook) g_viewModelHooks.push_back(hook);
            m_renderObjectHooked = hook != nullptr;
        }
    }

    if (!m_renderItemHooked) {
        auto address = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::RenderItem);
        if (address) {
            auto hook = bedrocktools::hooks::install(
                reinterpret_cast<void*>(address),
                reinterpret_cast<void*>(_renderItem_hook),
                reinterpret_cast<void**>(&_renderItem_orig)
            );
            if (hook) g_viewModelHooks.push_back(hook);
            m_renderItemHooked = hook != nullptr;
        }
    }

    if (!m_jsonTransformHooked) {
        auto address = bedrocktools::memory::resolve(
            bedrocktools::memory::SignatureId::ItemInHandRendererTransformWorldMatrixFromJson
        );
        if (address) {
            auto hook = bedrocktools::hooks::install(
                reinterpret_cast<void*>(address),
                reinterpret_cast<void*>(_transformWorldMatrixFromJson_hook),
                reinterpret_cast<void**>(&_transformWorldMatrixFromJson_orig)
            );
            if (hook) g_viewModelHooks.push_back(hook);
            m_jsonTransformHooked = hook != nullptr;
        }
    }

    if (!m_defaultTransformHooked) {
        auto address = bedrocktools::memory::resolve(
            bedrocktools::memory::SignatureId::ItemInHandRendererApplyDefaultItemTransforms
        );
        if (address) {
            auto hook = bedrocktools::hooks::install(
                reinterpret_cast<void*>(address),
                reinterpret_cast<void*>(_applyDefaultItemTransforms_hook),
                reinterpret_cast<void**>(&_applyDefaultItemTransforms_orig)
            );
            if (hook) g_viewModelHooks.push_back(hook);
            m_defaultTransformHooked = hook != nullptr;
        }
    }

    if (!m_renderOffhandItemHooked) {
        auto address = bedrocktools::memory::resolve(
            bedrocktools::memory::SignatureId::ItemInHandRenderOffhandItem
        );
        if (address) {
            auto hook = bedrocktools::hooks::install(
                reinterpret_cast<void*>(address),
                reinterpret_cast<void*>(_renderOffhandItem_hook),
                reinterpret_cast<void**>(&_renderOffhandItem_orig)
            );
            if (hook) g_viewModelHooks.push_back(hook);
            m_renderOffhandItemHooked = hook != nullptr;
        }
    }

    m_hooksInstalled = g_viewModelHooks.size() == 8;
    if (!m_hooksInstalled) removeHooks();
}

void ViewModelModule::removeHooks() {
    for (auto it = g_viewModelHooks.rbegin(); it != g_viewModelHooks.rend(); ++it) {
        bedrocktools::hooks::remove(*it);
    }
    g_viewModelHooks.clear();
    m_hooksInstalled = false;
    m_renderFirstPersonHooked = false;
    m_renderHandHooked = false;
    m_actorRenderDispatcherHooked = false;
    m_renderItemHooked = false;
    m_jsonTransformHooked = false;
    m_defaultTransformHooked = false;
    m_renderOffhandItemHooked = false;
    m_renderObjectHooked = false;
    _renderFirstPerson_orig = nullptr;
    _renderHand_orig = nullptr;
    _actorRenderDispatcherRender_orig = nullptr;
    _renderItem_orig = nullptr;
    _transformWorldMatrixFromJson_orig = nullptr;
    _applyDefaultItemTransforms_orig = nullptr;
    _renderOffhandItem_orig = nullptr;
    _renderObject_orig = nullptr;
}

void ViewModelModule::onInit() {
}

void ViewModelModule::onEnable() {
    installHooks();
}

void ViewModelModule::onDisable() {
    removeHooks();
}

void ViewModelModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);

    bool hasNewMain = j.contains("m_mainHandPositionX") ||
                      j.contains("m_mainHandPositionY") ||
                      j.contains("m_mainHandPositionZ") ||
                      j.contains("m_mainHandRotationX") ||
                      j.contains("m_mainHandRotationY") ||
                      j.contains("m_mainHandRotationZ") ||
                      j.contains("m_mainHandScaleX") ||
                      j.contains("m_mainHandScaleY") ||
                      j.contains("m_mainHandScaleZ");

    if (j.contains("m_mainHandPositionX")) m_mainHand.positionX = j["m_mainHandPositionX"].get<float>();
    if (j.contains("m_mainHandPositionY")) m_mainHand.positionY = j["m_mainHandPositionY"].get<float>();
    if (j.contains("m_mainHandPositionZ")) m_mainHand.positionZ = j["m_mainHandPositionZ"].get<float>();
    if (j.contains("m_mainHandRotationX")) m_mainHand.rotationX = j["m_mainHandRotationX"].get<float>();
    if (j.contains("m_mainHandRotationY")) m_mainHand.rotationY = j["m_mainHandRotationY"].get<float>();
    if (j.contains("m_mainHandRotationZ")) m_mainHand.rotationZ = j["m_mainHandRotationZ"].get<float>();
    if (j.contains("m_mainHandScaleX")) m_mainHand.scaleX = j["m_mainHandScaleX"].get<float>();
    if (j.contains("m_mainHandScaleY")) m_mainHand.scaleY = j["m_mainHandScaleY"].get<float>();
    if (j.contains("m_mainHandScaleZ")) m_mainHand.scaleZ = j["m_mainHandScaleZ"].get<float>();

    if (!hasNewMain) {
        if (j.contains("m_posX")) m_mainHand.positionX = j["m_posX"].get<float>();
        if (j.contains("m_posY")) m_mainHand.positionY = j["m_posY"].get<float>();
        if (j.contains("m_posZ")) m_mainHand.positionZ = j["m_posZ"].get<float>();
        if (j.contains("m_rotX")) m_mainHand.rotationX = j["m_rotX"].get<float>();
        if (j.contains("m_rotY")) m_mainHand.rotationY = j["m_rotY"].get<float>();
        if (j.contains("m_rotZ")) m_mainHand.rotationZ = j["m_rotZ"].get<float>();
        if (j.contains("m_scaleX")) m_mainHand.scaleX = j["m_scaleX"].get<float>();
        if (j.contains("m_scaleY")) m_mainHand.scaleY = j["m_scaleY"].get<float>();
        if (j.contains("m_scaleZ")) m_mainHand.scaleZ = j["m_scaleZ"].get<float>();
    }

    if (j.contains("m_offHandPositionX")) m_offHand.positionX = j["m_offHandPositionX"].get<float>();
    if (j.contains("m_offHandPositionY")) m_offHand.positionY = j["m_offHandPositionY"].get<float>();
    if (j.contains("m_offHandPositionZ")) m_offHand.positionZ = j["m_offHandPositionZ"].get<float>();
    if (j.contains("m_offHandRotationX")) m_offHand.rotationX = j["m_offHandRotationX"].get<float>();
    if (j.contains("m_offHandRotationY")) m_offHand.rotationY = j["m_offHandRotationY"].get<float>();
    if (j.contains("m_offHandRotationZ")) m_offHand.rotationZ = j["m_offHandRotationZ"].get<float>();
    if (j.contains("m_offHandScaleX")) m_offHand.scaleX = j["m_offHandScaleX"].get<float>();
    if (j.contains("m_offHandScaleY")) m_offHand.scaleY = j["m_offHandScaleY"].get<float>();
    if (j.contains("m_offHandScaleZ")) m_offHand.scaleZ = j["m_offHandScaleZ"].get<float>();

    if (j.contains("m_itemFov")) m_itemFov = j["m_itemFov"].get<float>();
    if (j.contains("m_swapHands")) m_swapHands = j["m_swapHands"].get<bool>();
    if (j.contains("m_applyThirdPerson")) m_applyThirdPerson = j["m_applyThirdPerson"].get<bool>();

    sanitizeTransform(m_mainHand);
    sanitizeTransform(m_offHand);
    m_itemFov = std::clamp(finiteOr(m_itemFov, 70.0f), 1.0f, 179.0f);
}

void ViewModelModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);

    j["m_mainHandPositionX"] = m_mainHand.positionX;
    j["m_mainHandPositionY"] = m_mainHand.positionY;
    j["m_mainHandPositionZ"] = m_mainHand.positionZ;
    j["m_mainHandRotationX"] = m_mainHand.rotationX;
    j["m_mainHandRotationY"] = m_mainHand.rotationY;
    j["m_mainHandRotationZ"] = m_mainHand.rotationZ;
    j["m_mainHandScaleX"] = m_mainHand.scaleX;
    j["m_mainHandScaleY"] = m_mainHand.scaleY;
    j["m_mainHandScaleZ"] = m_mainHand.scaleZ;

    j["m_offHandPositionX"] = m_offHand.positionX;
    j["m_offHandPositionY"] = m_offHand.positionY;
    j["m_offHandPositionZ"] = m_offHand.positionZ;
    j["m_offHandRotationX"] = m_offHand.rotationX;
    j["m_offHandRotationY"] = m_offHand.rotationY;
    j["m_offHandRotationZ"] = m_offHand.rotationZ;
    j["m_offHandScaleX"] = m_offHand.scaleX;
    j["m_offHandScaleY"] = m_offHand.scaleY;
    j["m_offHandScaleZ"] = m_offHand.scaleZ;

    j["m_itemFov"] = m_itemFov;
    j["m_swapHands"] = m_swapHands;
    j["m_applyThirdPerson"] = m_applyThirdPerson;
}
