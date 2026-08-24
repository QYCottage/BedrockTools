add_rules("mode.debug", "mode.release")
set_policy("package.requires_lock", true)

package("preloader")
    set_homepage("https://github.com/LiteLDev/preloader-android")
    set_description("Preloader Android")
    add_urls("https://github.com/LiteLDev/preloader-android.git")
    add_versions("main", "main")
    add_deps("cmake")
    on_install("android", function (package)
        import("package.tools.cmake").install(package)
    end)
package_end()

add_requires("preloader")
add_requires("nlohmann_json v3.11.3")
add_requires("glm")
add_requires("entt")
add_requires("fmt")

target("BedrockTools")
    set_kind("shared")
    set_languages("c++20")
    set_strip("all")
    add_files("src/main.cpp", "src/core/**.cpp", "src/config/*.cpp", "src/launcher/*.cpp", "src/modules/**.cpp")
    add_includedirs("include", {public = true})
    add_includedirs("src", "third_party")
    add_packages("preloader", "nlohmann_json", "glm", "entt", "fmt")

    if is_plat("android") then
        add_cxflags("-fPIC", "-Oz", "-ffunction-sections", "-fdata-sections", "-flto", "-fno-unwind-tables", "-fno-asynchronous-unwind-tables", "-fmerge-all-constants", "-fno-stack-protector", "-fexceptions", "-w", "-fvisibility=hidden")
        add_cxxflags("-fno-rtti", "-fvisibility-inlines-hidden")
        add_shflags("-Wl,--gc-sections", "-Wl,--icf=all", "-flto", "-Wl,--hash-style=gnu", "-Wl,-z,max-page-size=16384")
        add_links("android", "log", "EGL", "GLESv3", "GLESv2")
    end

    after_build(function (target)
        if not target:is_plat("android") then return end
        import("lib.detect.find_tool")
        local python = find_tool("python3") or find_tool("python")
        local args = {}
        if not python then
            python = find_tool("py")
            if python then table.insert(args, "-3") end
        end
        assert(python, "Python 3 is required to package BedrockTools.levipack")
        table.insert(args, path.join(os.projectdir(), "scripts", "package_levipack.py"))
        table.insert(args, "--library")
        table.insert(args, target:targetfile())
        table.insert(args, "--icon")
        table.insert(args, path.join(os.projectdir(), "assets", "bedrocktools.png"))
        table.insert(args, "--font")
        table.insert(args, path.join(os.projectdir(), "resources", "minecraft.ttf"))
        table.insert(args, "--version-header")
        table.insert(args, path.join(os.projectdir(), "include", "bedrocktools", "Version.hpp"))
        table.insert(args, "--output")
        table.insert(args, path.join(target:targetdir(), "BedrockTools.levipack"))
        os.vrunv(python.program, args)

        -- Cape Physics is a Minecraft resource pack, not a native-library
        -- asset. Build it alongside the levipack so releases contain the
        -- separately importable .mcpack required for the animated geometry.
        local capeArgs = {path.join(os.projectdir(), "scripts", "generate_cape_physics_pack.py"),
                          "--output", path.join(target:targetdir(), "cape-physics-source"),
                          "--mcpack", path.join(target:targetdir(), "BedrockTools-Cape-Physics.mcpack")}
        os.vrunv(python.program, capeArgs)
    end)
