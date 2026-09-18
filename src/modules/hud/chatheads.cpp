#include "chatheads.hpp"

#include "core/memory/Hooks.hpp"
#include "modules/ModuleRegistry.hpp"
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <pl/ModMenu.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <unordered_set>
#include <utility>

extern "C" void chatHeadsPlayerListReadHook();
extern "C" void *chatHeadsGetPlayerListReadOriginal();
extern "C" void chatHeadsCapturePlayerListPacketAfterRead(const void *packet);

namespace {

constexpr std::size_t MaxPlayerListEntries = 4096;
constexpr std::size_t MaxGuiMessages = 4096;
constexpr std::size_t MaxRememberedMessages = 100;
constexpr std::size_t MaxMessageLength = 4096;
constexpr auto OverlayRequestHoldTime = std::chrono::milliseconds(250);

#pragma pack(push, 4)
struct RectangleArea {
  float x0;
  float x1;
  float y0;
  float y1;
};
#pragma pack(pop)

class Font;
struct TextMeasureData;
struct CaretMeasureData;
enum class TextAlignment : std::uint8_t;

namespace mce {
struct Color {
  float r;
  float g;
  float b;
  float a;
};
} // namespace mce

using DrawDebugTextFn = void (*)(void *, const RectangleArea &, std::string &&,
                                 const mce::Color &, float, TextAlignment,
                                 const TextMeasureData &,
                                 const CaretMeasureData &);
using DrawTextFn = void (*)(void *, Font &, const RectangleArea &,
                            std::string &&, const mce::Color &, float,
                            TextAlignment, const TextMeasureData &,
                            const CaretMeasureData &);
using GuiDataAddMessageFn = void (*)(void *, void *, std::int32_t);
using GetRuntimeActorListFn = std::vector<void *> (*)(void *);
using ActorIsPlayerFn = bool (*)(void *);
using ActorGetNameTagFn = std::string (*)(void *);

ChatHeadsModule *g_chatHeads = nullptr;
void *g_playerListReadOriginal = nullptr;
DrawDebugTextFn g_drawDebugTextOriginal = nullptr;
DrawTextFn g_drawTextOriginal = nullptr;
GuiDataAddMessageFn g_guiDataAddMessageOriginal = nullptr;
bedrocktools::hooks::Handle g_playerListReadHook = nullptr;
bedrocktools::hooks::Handle g_drawDebugTextHook = nullptr;
bedrocktools::hooks::Handle g_drawTextHook = nullptr;
bedrocktools::hooks::Handle g_guiDataAddMessageHook = nullptr;
GetRuntimeActorListFn g_getRuntimeActorList = nullptr;
ActorIsPlayerFn g_actorIsPlayer = nullptr;
ActorGetNameTagFn g_actorGetNameTag = nullptr;

std::string stripColors(const std::string &input) {
  std::string output;
  output.reserve(input.size());
  for (std::size_t index = 0; index < input.size(); ++index) {
    if (static_cast<unsigned char>(input[index]) == 0xC2 &&
        index + 1 < input.size() &&
        static_cast<unsigned char>(input[index + 1]) == 0xA7) {
      index += 2;
    } else {
      output.push_back(input[index]);
    }
  }
  return output;
}

std::string cleanPlayerName(const std::string &input) {
  std::string output = stripColors(input);
  output.erase(std::remove_if(output.begin(), output.end(),
                              [](unsigned char value) {
                                return value < 0x20 || value == 0x7F;
                              }),
               output.end());

  const auto first = output.find_first_not_of(' ');
  if (first == std::string::npos)
    return {};
  const auto last = output.find_last_not_of(' ');
  output = output.substr(first, last - first + 1);
  if (output.size() > 128)
    output.resize(128);
  return output;
}

bool extractChatHead(const void *image, ChatHeadPixels &output) {
  if (!image)
    return false;

  const auto imageAddress = reinterpret_cast<std::uintptr_t>(image);
  const auto width = *reinterpret_cast<const std::uint32_t *>(
      imageAddress + bedrocktools::sdk::offsets::SkinImage::mWidth);
  const auto height = *reinterpret_cast<const std::uint32_t *>(
      imageAddress + bedrocktools::sdk::offsets::SkinImage::mHeight);
  const auto *pixels = *reinterpret_cast<const std::uint8_t *const *>(
      imageAddress + bedrocktools::sdk::offsets::Image::mBytesOffset);

  if (!pixels || width < 64 || width > 256 || width % 64 != 0 || height < 32 ||
      height > 256 || height % 32 != 0) {
    return false;
  }

  const int scale = static_cast<int>(width / 64);
  if (scale == 0)
    return false;
  constexpr int upScale = ChatHeadTextureSize / 8;

  output.fill(0);
  auto copyLayer = [&](int skinX, bool overlay) {
    for (int y = 0; y < 8; ++y) {
      for (int x = 0; x < 8; ++x) {
        const int sourceX = skinX + x * scale + scale / 2;
        const int sourceY = 8 * scale + y * scale + scale / 2;
        const auto *source =
            pixels + (static_cast<std::size_t>(sourceY) * width + sourceX) * 4;

        for (int scaleY = 0; scaleY < upScale; ++scaleY) {
          for (int scaleX = 0; scaleX < upScale; ++scaleX) {
            const int destinationX = x * upScale + scaleX;
            const int destinationY = y * upScale + scaleY;
            auto *destination =
                output.data() +
                (destinationY * ChatHeadTextureSize + destinationX) * 4;

            if (!overlay) {
              std::memcpy(destination, source, 4);
              continue;
            }

            const auto sourceAlpha = source[3];
            if (sourceAlpha == 0)
              continue;
            if (sourceAlpha == 255) {
              std::memcpy(destination, source, 4);
              continue;
            }

            const float alpha = sourceAlpha / 255.0f;
            const float inverseAlpha = 1.0f - alpha;
            destination[0] = static_cast<std::uint8_t>(
                source[0] * alpha + destination[0] * inverseAlpha + 0.5f);
            destination[1] = static_cast<std::uint8_t>(
                source[1] * alpha + destination[1] * inverseAlpha + 0.5f);
            destination[2] = static_cast<std::uint8_t>(
                source[2] * alpha + destination[2] * inverseAlpha + 0.5f);
          }
        }
      }
    }
  };

  copyLayer(8 * scale, false);
  copyLayer(40 * scale, true);
  return true;
}

const void *getSkinImageFromActor(void *actor) {
  if (!actor)
    return nullptr;

  auto *skinRef =
      *reinterpret_cast<void **>(reinterpret_cast<std::byte *>(actor) +
                                 bedrocktools::sdk::offsets::Player::mSkin);
  if (!skinRef)
    return nullptr;

  auto *threadOwner = *reinterpret_cast<void **>(
      reinterpret_cast<std::byte *>(skinRef) +
      bedrocktools::sdk::offsets::SerializedSkinRef::mSkinImpl);
  if (!threadOwner)
    return nullptr;

  const auto skinImplementation =
      reinterpret_cast<std::uintptr_t>(threadOwner) +
      bedrocktools::sdk::offsets::ThreadOwner::mObject;
  return reinterpret_cast<const void *>(
      skinImplementation +
      bedrocktools::sdk::offsets::SerializedSkinImpl::mSkinImage);
}

void appendNameAlias(std::vector<std::string> &aliases,
                     const std::string &rawName) {
  auto append = [&](std::string_view value) {
    const auto cleaned = cleanPlayerName(std::string(value));
    if (cleaned.empty())
      return;
    if (std::find(aliases.begin(), aliases.end(), cleaned) == aliases.end())
      aliases.push_back(cleaned);
  };

  append(rawName);
  std::size_t lineStart = 0;
  while (lineStart < rawName.size()) {
    const auto lineEnd = rawName.find('\n', lineStart);
    append(std::string_view(rawName).substr(
        lineStart, lineEnd == std::string::npos ? rawName.size() - lineStart
                                                : lineEnd - lineStart));
    if (lineEnd == std::string::npos)
      break;
    lineStart = lineEnd + 1;
  }
}

std::vector<std::string> getActorNames(void *actor) {
  std::vector<std::string> aliases;
  if (!actor)
    return aliases;

  const auto *playerName = reinterpret_cast<const std::string *>(
      reinterpret_cast<std::byte *>(actor) +
      bedrocktools::sdk::offsets::Player::mName);
  if (!playerName->empty() && playerName->size() <= 256)
    appendNameAlias(aliases, *playerName);

  const auto *filteredName = reinterpret_cast<const std::string *>(
      reinterpret_cast<std::byte *>(actor) +
      bedrocktools::sdk::offsets::Actor::mFilteredNameTag);
  if (!filteredName->empty() && filteredName->size() <= 256)
    appendNameAlias(aliases, *filteredName);

  if (g_actorGetNameTag) {
    appendNameAlias(aliases, g_actorGetNameTag(actor));
  }

  return aliases;
}

std::string lowerAscii(std::string_view input) {
  std::string output(input);
  std::transform(output.begin(), output.end(), output.begin(),
                 [](unsigned char value) {
                   return static_cast<char>(std::tolower(value));
                 });
  return output;
}

std::size_t findNamePosition(std::string_view cleanText, std::string_view alias,
                             std::size_t authorLimit) {
  if (alias.empty())
    return std::string::npos;
  const auto textLower = lowerAscii(cleanText);
  const auto aliasLower = lowerAscii(alias);
  auto position = textLower.find(aliasLower);
  while (position != std::string::npos && position <= authorLimit) {
    const bool validLeft =
        position == 0 ||
        !std::isalnum(static_cast<unsigned char>(textLower[position - 1]));
    const auto rightPosition = position + aliasLower.size();
    const bool validRight =
        rightPosition == textLower.size() ||
        !std::isalnum(static_cast<unsigned char>(textLower[rightPosition]));
    if (validLeft && validRight)
      return position;
    position = textLower.find(aliasLower, position + 1);
  }
  return std::string::npos;
}

float getGuiScale(void *renderContext) {
  if (!renderContext)
    return 1.0f;

  auto *client = *reinterpret_cast<void **>(
      reinterpret_cast<std::byte *>(renderContext) +
      bedrocktools::sdk::offsets::MinecraftUIRenderContext::mClient);
  if (!client)
    return 1.0f;

  auto **vtable = *reinterpret_cast<void ***>(client);
  if (!vtable)
    return 1.0f;
  const auto target =
      vtable[bedrocktools::sdk::offsets::VTable::ClientInstanceGetGuiScale];
  if (!target)
    return 1.0f;

  const float scale = reinterpret_cast<float (*)(void *)>(target)(client);
  return std::isfinite(scale) && scale >= 0.25f && scale <= 8.0f ? scale : 1.0f;
}

bool isCurrentChatText(void *renderContext, const std::string &cleanText) {
  if (!renderContext || cleanText.empty()) {
    return g_chatHeads && g_chatHeads->isRecentChatText(cleanText);
  }

  auto *client = *reinterpret_cast<void **>(
      reinterpret_cast<std::byte *>(renderContext) +
      bedrocktools::sdk::offsets::MinecraftUIRenderContext::mClient);
  if (!client)
    return g_chatHeads && g_chatHeads->isRecentChatText(cleanText);

  auto *guiData = *reinterpret_cast<void **>(
      reinterpret_cast<std::byte *>(client) +
      bedrocktools::sdk::offsets::ClientInstance::mGuiData);
  if (!guiData)
    return g_chatHeads && g_chatHeads->isRecentChatText(cleanText);

  const auto guiDataAddress = reinterpret_cast<std::uintptr_t>(guiData);
  const auto begin = *reinterpret_cast<const std::uintptr_t *>(
      guiDataAddress + bedrocktools::sdk::offsets::GuiData::mGuiMessages);
  const auto end = *reinterpret_cast<const std::uintptr_t *>(
      guiDataAddress + bedrocktools::sdk::offsets::GuiData::mGuiMessagesEnd);
  if (!begin || end < begin)
    return g_chatHeads && g_chatHeads->isRecentChatText(cleanText);

  const auto byteCount = end - begin;
  constexpr auto stride = bedrocktools::sdk::offsets::GuiMessage::Size;
  if (byteCount % stride != 0 || byteCount / stride > MaxGuiMessages) {
    return g_chatHeads && g_chatHeads->isRecentChatText(cleanText);
  }

  auto matches = [&](const std::string &candidate) {
    if (candidate.empty() || candidate.size() > MaxMessageLength)
      return false;
    const std::string cleanCandidate = stripColors(candidate);
    return !cleanCandidate.empty() &&
           (cleanCandidate.find(cleanText) != std::string::npos ||
            cleanText.find(cleanCandidate) != std::string::npos);
  };

  for (auto entry = begin; entry < end; entry += stride) {
    const auto &fullString = *reinterpret_cast<const std::string *>(
        entry + bedrocktools::sdk::offsets::GuiMessage::FullString);
    const auto &message = *reinterpret_cast<const std::string *>(
        entry + bedrocktools::sdk::offsets::GuiMessage::Message);
    if (matches(fullString) || matches(message))
      return true;
  }

  return g_chatHeads && g_chatHeads->isRecentChatText(cleanText);
}

void guiDataAddMessageHook(void *guiData, void *guiMessage,
                           std::int32_t profanityFilterContext) {
  if (g_chatHeads && g_chatHeads->enabled)
    g_chatHeads->captureGuiMessage(guiMessage);
  if (g_guiDataAddMessageOriginal) {
    g_guiDataAddMessageOriginal(guiData, guiMessage, profanityFilterContext);
  }
}

void localPlayerTickCallback(void *localPlayer) {
  if (g_chatHeads && g_chatHeads->enabled)
    g_chatHeads->refreshPlayersFromWorld(localPlayer);
}

void chatScreenStateCallback(bedrocktools::events::ScreenPhase phase) {
  if (g_chatHeads) {
    g_chatHeads->setChatScreenOpen(phase ==
                                   bedrocktools::events::ScreenPhase::Opened);
  }
}

void drawNativeHead(void *renderContext, const ChatHeadPixels &pixels, float x,
                    float y, float size, float alpha) {
  if (!renderContext || size <= 0.0f || alpha <= 0.01f)
    return;

  auto **vtable = *reinterpret_cast<void ***>(renderContext);
  if (!vtable)
    return;
  const auto target = vtable[bedrocktools::sdk::offsets::VTable::
                                 MinecraftUIRenderContextFillRectangle];
  if (!target)
    return;

  using FillRectangleFn =
      void (*)(void *, const RectangleArea &, const mce::Color &, float);
  const auto fillRectangle = reinterpret_cast<FillRectangleFn>(target);
  constexpr int FaceSize = 8;
  constexpr int SampleStep = ChatHeadTextureSize / FaceSize;
  const float cellSize = size / static_cast<float>(FaceSize);

  for (int row = 0; row < FaceSize; ++row) {
    for (int column = 0; column < FaceSize; ++column) {
      const int sampleX = column * SampleStep + SampleStep / 2;
      const int sampleY = row * SampleStep + SampleStep / 2;
      const auto *pixel =
          pixels.data() + (sampleY * ChatHeadTextureSize + sampleX) * 4;
      if (pixel[3] == 0)
        continue;

      const RectangleArea cell{
          x + column * cellSize,
          x + (column + 1) * cellSize,
          y + row * cellSize,
          y + (row + 1) * cellSize,
      };
      const mce::Color color{
          pixel[0] / 255.0f,
          pixel[1] / 255.0f,
          pixel[2] / 255.0f,
          1.0f,
      };
      fillRectangle(renderContext, cell, color, alpha * (pixel[3] / 255.0f));
    }
  }
}

void drawDebugTextHook(void *renderContext, const RectangleArea &rectangle,
                       std::string &&text, const mce::Color &color, float alpha,
                       TextAlignment alignment, const TextMeasureData &measure,
                       const CaretMeasureData &caret) {
  if (!g_drawDebugTextOriginal)
    return;

  if (g_chatHeads && g_chatHeads->enabled &&
      g_chatHeads->queueHeadForText(renderContext, text, rectangle.x0,
                                    rectangle.y0, alpha)) {
    RectangleArea shifted = rectangle;
    const float offset = g_chatHeads->m_headSize + g_chatHeads->m_padding;
    shifted.x0 += offset;
    shifted.x1 += offset;
    g_drawDebugTextOriginal(renderContext, shifted, std::move(text), color,
                            alpha, alignment, measure, caret);
    return;
  }

  g_drawDebugTextOriginal(renderContext, rectangle, std::move(text), color,
                          alpha, alignment, measure, caret);
}

void drawTextHook(void *renderContext, Font &font,
                  const RectangleArea &rectangle, std::string &&text,
                  const mce::Color &color, float alpha, TextAlignment alignment,
                  const TextMeasureData &measure,
                  const CaretMeasureData &caret) {
  if (!g_drawTextOriginal)
    return;

  if (g_chatHeads && g_chatHeads->enabled &&
      g_chatHeads->queueHeadForText(renderContext, text, rectangle.x0,
                                    rectangle.y0, alpha)) {
    RectangleArea shifted = rectangle;
    const float offset = g_chatHeads->m_headSize + g_chatHeads->m_padding;
    shifted.x0 += offset;
    shifted.x1 += offset;
    g_drawTextOriginal(renderContext, font, shifted, std::move(text), color,
                       alpha, alignment, measure, caret);
    return;
  }

  g_drawTextOriginal(renderContext, font, rectangle, std::move(text), color,
                     alpha, alignment, measure, caret);
}

} // namespace

extern "C" __attribute__((used, noinline)) void *
chatHeadsGetPlayerListReadOriginal() {
  return g_playerListReadOriginal;
}

extern "C" __attribute__((used, noinline)) void
chatHeadsCapturePlayerListPacketAfterRead(const void *packet) {
  if (g_chatHeads && g_chatHeads->enabled)
    g_chatHeads->capturePlayerListPacket(packet);
}

#if defined(__aarch64__)
// Bedrock::Result<void> is returned through AArch64's hidden x8 result pointer.
// Preserve it while calling the original parser, then inspect the populated
// packet.
__asm__(".text\n"
        ".p2align 2\n"
        ".global chatHeadsPlayerListReadHook\n"
        ".hidden chatHeadsPlayerListReadHook\n"
        ".type chatHeadsPlayerListReadHook, %function\n"
        "chatHeadsPlayerListReadHook:\n"
        "stp x29, x30, [sp, #-64]!\n"
        "mov x29, sp\n"
        "stp x19, x20, [sp, #16]\n"
        "str x21, [sp, #32]\n"
        "mov x19, x0\n"
        "mov x20, x1\n"
        "mov x21, x8\n"
        "bl chatHeadsGetPlayerListReadOriginal\n"
        "mov x9, x0\n"
        "mov x0, x19\n"
        "mov x1, x20\n"
        "mov x8, x21\n"
        "blr x9\n"
        "mov x0, x19\n"
        "bl chatHeadsCapturePlayerListPacketAfterRead\n"
        "ldr x21, [sp, #32]\n"
        "ldp x19, x20, [sp, #16]\n"
        "ldp x29, x30, [sp], #64\n"
        "ret\n"
        ".size chatHeadsPlayerListReadHook, .-chatHeadsPlayerListReadHook\n");
#else
extern "C" void chatHeadsPlayerListReadHook() {}
#endif

ChatHeadsModule::ChatHeadsModule()
    : Module("Chat Heads",
             "Shows player heads next to chat messages natively.") {
  g_chatHeads = this;
  hideInHudEditor = true;
}

ChatHeadsModule::~ChatHeadsModule() {
  if (m_hooksInstalled) onDisable();
  if (g_chatHeads == this)
    g_chatHeads = nullptr;
  clearCache();
  std::vector<PLModMenu_DrawCommand> empty;
  ::submitDrawCommands(moduleId, empty);
}

void ChatHeadsModule::onInit() {
  g_getRuntimeActorList =
      reinterpret_cast<GetRuntimeActorListFn>(bedrocktools::memory::resolve(
          bedrocktools::memory::SignatureId::ActorManagerList));
  g_actorIsPlayer =
      reinterpret_cast<ActorIsPlayerFn>(bedrocktools::memory::resolve(
          bedrocktools::memory::SignatureId::ActorIsPlayer));
  g_actorGetNameTag =
      reinterpret_cast<ActorGetNameTagFn>(bedrocktools::memory::resolve(
          bedrocktools::memory::SignatureId::ActorGetNameTag));
  bedrocktools::events::bus()
      .subscribe<bedrocktools::events::LocalPlayerTickEvent>(
          [](auto &event) { localPlayerTickCallback(event.player); });
  bedrocktools::events::bus().subscribe<bedrocktools::events::ScreenStateEvent>(
      [](auto &event) {
        if (event.screen == bedrocktools::events::ScreenKind::Chat) {
          chatScreenStateCallback(event.phase);
        }
      });
}

void ChatHeadsModule::onEnable() {
  m_refreshTicks = 20;
  if (m_hooksInstalled) return;

  const auto playerListRead = bedrocktools::memory::resolve(
      bedrocktools::memory::SignatureId::PlayerListPacketRead);
  if (playerListRead) {
    g_playerListReadHook = bedrocktools::hooks::install(
        reinterpret_cast<void *>(playerListRead),
        reinterpret_cast<void *>(chatHeadsPlayerListReadHook),
        &g_playerListReadOriginal);
  }

  const auto drawDebugText = bedrocktools::memory::resolve(
      bedrocktools::memory::SignatureId::MinecraftUIRenderContextDrawDebugText);
  if (drawDebugText) {
    g_drawDebugTextHook = bedrocktools::hooks::install(
        reinterpret_cast<void *>(drawDebugText),
        reinterpret_cast<void *>(drawDebugTextHook),
        reinterpret_cast<void **>(&g_drawDebugTextOriginal));
  }

  const auto drawText = bedrocktools::memory::resolve(
      bedrocktools::memory::SignatureId::MinecraftUIRenderContextDrawText);
  if (drawText) {
    g_drawTextHook = bedrocktools::hooks::install(
        reinterpret_cast<void *>(drawText),
        reinterpret_cast<void *>(drawTextHook),
        reinterpret_cast<void **>(&g_drawTextOriginal));
  }

  const auto addMessage = bedrocktools::memory::resolve(
      bedrocktools::memory::SignatureId::GuiDataAddMessage);
  if (addMessage) {
    g_guiDataAddMessageHook = bedrocktools::hooks::install(
        reinterpret_cast<void *>(addMessage),
        reinterpret_cast<void *>(guiDataAddMessageHook),
        reinterpret_cast<void **>(&g_guiDataAddMessageOriginal));
  }

  m_hooksInstalled = g_playerListReadHook && g_drawDebugTextHook &&
      g_drawTextHook && g_guiDataAddMessageHook;
  if (!m_hooksInstalled) {
    if (g_guiDataAddMessageHook) bedrocktools::hooks::remove(g_guiDataAddMessageHook);
    if (g_drawTextHook) bedrocktools::hooks::remove(g_drawTextHook);
    if (g_drawDebugTextHook) bedrocktools::hooks::remove(g_drawDebugTextHook);
    if (g_playerListReadHook) bedrocktools::hooks::remove(g_playerListReadHook);
    g_guiDataAddMessageHook = nullptr;
    g_drawTextHook = nullptr;
    g_drawDebugTextHook = nullptr;
    g_playerListReadHook = nullptr;
    g_guiDataAddMessageOriginal = nullptr;
    g_drawTextOriginal = nullptr;
    g_drawDebugTextOriginal = nullptr;
    g_playerListReadOriginal = nullptr;
  }
}

void ChatHeadsModule::onDisable() {
  if (g_guiDataAddMessageHook) bedrocktools::hooks::remove(g_guiDataAddMessageHook);
  if (g_drawTextHook) bedrocktools::hooks::remove(g_drawTextHook);
  if (g_drawDebugTextHook) bedrocktools::hooks::remove(g_drawDebugTextHook);
  if (g_playerListReadHook) bedrocktools::hooks::remove(g_playerListReadHook);
  g_guiDataAddMessageHook = nullptr;
  g_drawTextHook = nullptr;
  g_drawDebugTextHook = nullptr;
  g_playerListReadHook = nullptr;
  g_guiDataAddMessageOriginal = nullptr;
  g_drawTextOriginal = nullptr;
  g_drawDebugTextOriginal = nullptr;
  g_playerListReadOriginal = nullptr;
  m_hooksInstalled = false;
  {
    std::lock_guard<std::mutex> lock(m_requestMutex);
    m_frameRequests.clear();
    m_retainedOverlayRequests.clear();
    m_lastOverlayRequestTime = {};
  }
  {
    std::lock_guard<std::mutex> lock(m_messageMutex);
    m_messages.clear();
  }
  std::vector<PLModMenu_DrawCommand> empty;
  ::submitDrawCommands(moduleId, empty);
}

void ChatHeadsModule::setChatScreenOpen(bool open) {
  m_chatScreenOpen.store(open, std::memory_order_release);
}

void ChatHeadsModule::clearCache() {
  {
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    m_cache.clear();
  }
  {
    std::lock_guard<std::mutex> lock(m_requestMutex);
    m_frameRequests.clear();
    m_retainedOverlayRequests.clear();
    m_lastOverlayRequestTime = {};
  }
}

void ChatHeadsModule::capturePlayerListPacket(const void *packet) {
  if (!packet)
    return;

  const auto packetAddress = reinterpret_cast<std::uintptr_t>(packet);
  const auto action = *reinterpret_cast<const std::int32_t *>(
      packetAddress + bedrocktools::sdk::offsets::PlayerListPacket::mAction);
  if (action != bedrocktools::sdk::offsets::PlayerListPacket::ActionAdd)
    return;

  const auto begin = *reinterpret_cast<const std::uintptr_t *>(
      packetAddress +
      bedrocktools::sdk::offsets::PlayerListPacket::mEntriesBegin);
  const auto end = *reinterpret_cast<const std::uintptr_t *>(
      packetAddress +
      bedrocktools::sdk::offsets::PlayerListPacket::mEntriesEnd);
  if (!begin || end < begin)
    return;

  const auto byteCount = end - begin;
  constexpr auto stride = bedrocktools::sdk::offsets::PlayerListEntry::Size;
  if (byteCount % stride != 0)
    return;

  const auto count = byteCount / stride;
  if (count > MaxPlayerListEntries)
    return;

  struct CapturedPlayer {
    std::string name;
    bool hasHead = false;
    ChatHeadPixels pixels{};
  };
  std::vector<CapturedPlayer> captured;
  captured.reserve(count);

  for (std::size_t index = 0; index < count; ++index) {
    const auto entry = begin + index * stride;
    const auto &name = *reinterpret_cast<const std::string *>(
        entry + bedrocktools::sdk::offsets::PlayerListEntry::mName);
    if (name.empty() || name.size() > 64)
      continue;

    CapturedPlayer player;
    player.name = name;

    if (const auto threadOwner = *reinterpret_cast<const std::uintptr_t *>(
            entry + bedrocktools::sdk::offsets::PlayerListEntry::mSkin)) {
      const auto skinImplementation =
          threadOwner + bedrocktools::sdk::offsets::ThreadOwner::mObject;
      const auto *image = reinterpret_cast<const void *>(
          skinImplementation +
          bedrocktools::sdk::offsets::SerializedSkinImpl::mSkinImage);
      player.hasHead = extractChatHead(image, player.pixels);
    }

    captured.push_back(std::move(player));
  }

  if (count > 1)
    clearCache();

  std::lock_guard<std::mutex> lock(m_cacheMutex);
  for (auto &capturedPlayer : captured) {
    const auto existing =
        std::find_if(m_cache.begin(), m_cache.end(), [&](const auto &player) {
          return player.name == capturedPlayer.name ||
                 std::find(player.aliases.begin(), player.aliases.end(),
                           capturedPlayer.name) != player.aliases.end();
        });

    if (existing != m_cache.end()) {
      if (std::find(existing->aliases.begin(), existing->aliases.end(),
                    capturedPlayer.name) == existing->aliases.end()) {
        existing->aliases.push_back(capturedPlayer.name);
      }
      if (capturedPlayer.hasHead) {
        existing->pixels = capturedPlayer.pixels;
        existing->imageId.clear();
        existing->needsTexture = true;
      }
      continue;
    }

    ChatHeadsPlayer player;
    player.name = std::move(capturedPlayer.name);
    player.aliases.push_back(player.name);
    if (capturedPlayer.hasHead) {
      player.pixels = capturedPlayer.pixels;
      player.needsTexture = true;
    }
    m_cache.push_back(std::move(player));
  }
}

void ChatHeadsModule::captureGuiMessage(const void *guiMessage) {
  if (!guiMessage)
    return;

  const auto address = reinterpret_cast<std::uintptr_t>(guiMessage);
  const auto type = *reinterpret_cast<const std::int32_t *>(
      address + bedrocktools::sdk::offsets::GuiMessage::Type);
  if (type < 1 || type > 8)
    return;

  const auto &fullString = *reinterpret_cast<const std::string *>(
      address + bedrocktools::sdk::offsets::GuiMessage::FullString);
  const auto &message = *reinterpret_cast<const std::string *>(
      address + bedrocktools::sdk::offsets::GuiMessage::Message);
  if ((fullString.empty() && message.empty()) ||
      fullString.size() > MaxMessageLength ||
      message.size() > MaxMessageLength) {
    return;
  }

  ChatHeadsMessage captured{fullString, message};
  std::lock_guard<std::mutex> lock(m_messageMutex);
  if (!m_messages.empty() &&
      m_messages.back().fullString == captured.fullString &&
      m_messages.back().message == captured.message) {
    return;
  }
  m_messages.push_back(std::move(captured));
  while (m_messages.size() > MaxRememberedMessages)
    m_messages.pop_front();
}

bool ChatHeadsModule::isRecentChatText(const std::string &cleanText) {
  if (cleanText.empty())
    return false;

  auto matches = [&](const std::string &candidate) {
    if (candidate.empty() || candidate.size() > MaxMessageLength)
      return false;
    const auto cleanCandidate = stripColors(candidate);
    return !cleanCandidate.empty() &&
           (cleanCandidate.find(cleanText) != std::string::npos ||
            cleanText.find(cleanCandidate) != std::string::npos);
  };

  std::lock_guard<std::mutex> lock(m_messageMutex);
  for (auto entry = m_messages.rbegin(); entry != m_messages.rend(); ++entry) {
    if (matches(entry->fullString) || matches(entry->message))
      return true;
  }
  return false;
}

void ChatHeadsModule::refreshPlayersFromWorld(void *localPlayer) {
  if (!localPlayer)
    return;
  if (++m_refreshTicks < 20)
    return;
  m_refreshTicks = 0;

  std::vector<void *> actors;
  const auto level =
      *reinterpret_cast<void **>(reinterpret_cast<std::byte *>(localPlayer) +
                                 bedrocktools::sdk::offsets::Actor::mLevel);
  if (level && g_getRuntimeActorList) {
    const auto actorManager = *reinterpret_cast<void **>(
        reinterpret_cast<std::byte *>(level) +
        bedrocktools::sdk::offsets::Level::mActorManager);
    if (actorManager)
      actors = g_getRuntimeActorList(actorManager);
  }

  struct CapturedPlayer {
    std::vector<std::string> aliases;
    ChatHeadPixels pixels{};
  };
  std::vector<CapturedPlayer> captured;
  captured.reserve(actors.size() + 1);
  std::unordered_set<void *> seen;
  seen.reserve(actors.size() + 1);

  auto captureActor = [&](void *actor, bool trustedPlayer) {
    if (!actor || !seen.emplace(actor).second)
      return;
    if (!trustedPlayer && (!g_actorIsPlayer || !g_actorIsPlayer(actor)))
      return;

    CapturedPlayer player;
    player.aliases = getActorNames(actor);
    if (player.aliases.empty())
      return;

    if (!extractChatHead(getSkinImageFromActor(actor), player.pixels))
      return;
    captured.push_back(std::move(player));
  };

  captureActor(localPlayer, true);
  for (auto *actor : actors)
    captureActor(actor, false);

  std::lock_guard<std::mutex> lock(m_cacheMutex);
  for (auto &capturedPlayer : captured) {
    const auto existing =
        std::find_if(m_cache.begin(), m_cache.end(), [&](const auto &player) {
          return std::any_of(
              capturedPlayer.aliases.begin(), capturedPlayer.aliases.end(),
              [&](const auto &alias) {
                return player.name == alias ||
                       std::find(player.aliases.begin(), player.aliases.end(),
                                 alias) != player.aliases.end();
              });
        });
    if (existing != m_cache.end()) {
      for (const auto &alias : capturedPlayer.aliases) {
        if (std::find(existing->aliases.begin(), existing->aliases.end(),
                      alias) == existing->aliases.end()) {
          existing->aliases.push_back(alias);
        }
      }
      if (existing->pixels != capturedPlayer.pixels) {
        existing->pixels = capturedPlayer.pixels;
        existing->imageId.clear();
        existing->needsTexture = true;
      } else if (existing->imageId.empty()) {
        existing->needsTexture = true;
      }
      continue;
    }

    ChatHeadsPlayer player;
    player.name = capturedPlayer.aliases.front();
    player.aliases = std::move(capturedPlayer.aliases);
    player.pixels = capturedPlayer.pixels;
    player.needsTexture = true;
    m_cache.push_back(std::move(player));
  }
}

bool ChatHeadsModule::queueHeadForText(void *renderContext,
                                       const std::string &text, float x,
                                       float y, float alpha) {
  if (alpha <= 0.01f)
    return false;

  const std::string cleanText = stripColors(text);
  if (cleanText.empty())
    return false;

  if (!isCurrentChatText(renderContext, cleanText))
    return false;

  std::size_t authorLimit = std::min<std::size_t>(cleanText.size(), 96);
  for (const std::string_view delimiter :
       {std::string_view("\xC2\xBB"), std::string_view(">")}) {
    const auto position = cleanText.find(delimiter);
    if (position != std::string::npos)
      authorLimit = std::min(authorLimit, position);
  }
  std::size_t colon = cleanText.find(':');
  while (colon != std::string::npos) {
    const auto openBracket = cleanText.rfind('[', colon);
    const auto closeBracket = cleanText.rfind(']', colon);
    if (openBracket == std::string::npos ||
        (closeBracket != std::string::npos && closeBracket > openBracket)) {
      authorLimit = std::min(authorLimit, colon);
      break;
    }
    colon = cleanText.find(':', colon + 1);
  }

  const bool useNativeRenderer =
      m_chatScreenOpen.load(std::memory_order_acquire);
  std::string imageId;
  ChatHeadPixels pixels{};
  bool foundHead = false;
  std::size_t bestPosition = std::string::npos;
  {
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    for (const auto &player : m_cache) {
      if (player.imageId.empty() &&
          (!useNativeRenderer || !player.needsTexture))
        continue;

      std::size_t position = std::string::npos;
      if (player.aliases.empty()) {
        position = findNamePosition(cleanText, player.name, authorLimit);
      } else {
        for (const auto &alias : player.aliases) {
          position = std::min(position,
                              findNamePosition(cleanText, alias, authorLimit));
        }
      }
      if (position == std::string::npos || position >= bestPosition)
        continue;

      bestPosition = position;
      imageId = player.imageId;
      pixels = player.pixels;
      foundHead = true;
    }
  }
  if (!foundHead)
    return false;

  if (useNativeRenderer) {
    drawNativeHead(renderContext, pixels, x, y, m_headSize, alpha);
    return true;
  }
  if (imageId.empty())
    return false;

  const float scale = getGuiScale(renderContext);
  HeadDrawRequest request;
  request.imageId = std::move(imageId);
  request.x = x * scale;
  request.y = y * scale;
  request.size = m_headSize * scale;
  request.alpha = alpha;

  std::lock_guard<std::mutex> lock(m_requestMutex);
  m_frameRequests.push_back(std::move(request));
  return true;
}

void ChatHeadsModule::onFrame() {
  {
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    for (auto &player : m_cache) {
      if (!player.needsTexture)
        continue;

      player.imageId =
          "chatheads_" + std::to_string(++m_nextImageId);
      if (pl::modmenu::registerImage(player.imageId, player.pixels,
                                     ChatHeadTextureSize,
                                     ChatHeadTextureSize)) {
        player.needsTexture = false;
      } else {
        player.imageId.clear();
      }
    }
  }

  std::vector<HeadDrawRequest> requests;
  const auto now = std::chrono::steady_clock::now();
  const bool chatScreenOpen = m_chatScreenOpen.load(std::memory_order_acquire);
  {
    std::lock_guard<std::mutex> lock(m_requestMutex);
    if (chatScreenOpen) {
      m_frameRequests.clear();
      m_retainedOverlayRequests.clear();
      m_lastOverlayRequestTime = {};
    } else if (!m_frameRequests.empty()) {
      requests.swap(m_frameRequests);
      m_retainedOverlayRequests = requests;
      m_lastOverlayRequestTime = now;
    } else if (!m_retainedOverlayRequests.empty() &&
               now - m_lastOverlayRequestTime <= OverlayRequestHoldTime) {
      requests = m_retainedOverlayRequests;
    } else {
      m_retainedOverlayRequests.clear();
      m_lastOverlayRequestTime = {};
    }
  }

  std::vector<PLModMenu_DrawCommand> commands;
  commands.reserve(requests.size());
  for (const auto &request : requests) {
    if (request.imageId.empty())
      continue;

    PLModMenu_DrawCommand command{};
    command.type = PL_DRAW_IMAGE;
    command.x = request.x;
    command.y = request.y;
    command.w = request.size;
    command.h = request.size;
    const auto alphaByte = static_cast<std::uint32_t>(
        std::clamp(request.alpha, 0.0f, 1.0f) * 255.0f + 0.5f);
    command.color = (alphaByte << 24) | 0x00FFFFFFu;
    command.imageId = request.imageId;
    commands.push_back(std::move(command));
  }

  ::submitDrawCommands(moduleId, commands);
}

void ChatHeadsModule::loadConfig(const nlohmann::json &j) {
  Module::loadConfig(j);
  if (j.contains("m_headSize"))
    m_headSize = j["m_headSize"].get<float>();
  if (j.contains("m_padding"))
    m_padding = j["m_padding"].get<float>();
}

void ChatHeadsModule::saveConfig(nlohmann::json &j) {
  Module::saveConfig(j);
  j["m_headSize"] = m_headSize;
  j["m_padding"] = m_padding;
}
