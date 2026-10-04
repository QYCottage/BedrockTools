#pragma once

#include <pl/Mod.hpp>
#include <filesystem>
#include <string>

namespace bedrocktools::core {

class Runtime {
public:
    static Runtime& get();
    bool load(pl::mod::ModContext& context);
    bool enable(pl::mod::ModContext& context);
    bool disable(pl::mod::ModContext& context);
    bool unload(pl::mod::ModContext& context);
    void minecraftLoaded();
    const std::filesystem::path& resourceDirectory() const noexcept;
    const std::string& modId() const noexcept;

private:
    bool resolveSignatures();
    bool install();
    bool launcherContext() const;
    void wireEvents();
    std::filesystem::path mResourceDirectory;
    std::string mModId;
};

}
