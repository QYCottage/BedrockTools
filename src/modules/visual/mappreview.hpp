#pragma once

#include "../Module.hpp"
#include <string>

class MapPreviewModule : public Module {
public:
  float m_positionX = 8.0f;
  float m_positionY = 80.0f;
  float m_scale = 1.0f;
  bool m_followSelectedMap = true;

  MapPreviewModule();
  ~MapPreviewModule() override;

  void onInit() override;
  void onEnable() override;
  void onDisable() override;
  void loadConfig(const nlohmann::json &j) override;
  void saveConfig(nlohmann::json &j) override;
};

void MapPreviewHandleSlotSelected(void *controller,
                                  const std::string &collectionName, int index);
void MapPreviewHandleHoverAnchor(float x, float y);
void MapPreviewRender(void *context);
