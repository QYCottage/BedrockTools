#pragma once

#include <string_view>

namespace bedrocktools::launcher {

// The preloader supplies the VM in ModContext during the native module load.
// Keeping it here lets config callbacks call back into the launcher's overlay
// manager without depending on a private preloader header.
void setJavaVm(void* javaVm);

// The launcher keeps an ExternalButtonOverlay snapshot after it is created.
// Ask it to rebuild the buttons after a module changes one of its button
// definitions (for example, a command label or a comment text).
void refreshExternalButtonsForModule(std::string_view moduleId);

} // namespace bedrocktools::launcher
