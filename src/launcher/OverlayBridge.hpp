#pragma once

#include <jni.h>

namespace bedrocktools::launcher {

void initializeOverlayBridge(JavaVM* vm);
void setFreelookOverlayVisible(bool visible);
void resetFreelookOverlay();
void pollOverlayBridge();

}
