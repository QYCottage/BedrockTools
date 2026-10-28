#pragma once

namespace bedrocktools::core::uirender {

using Callback = void (*)(void*, void*);
bool subscribe(Callback callback);

}
