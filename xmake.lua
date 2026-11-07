
 -- Add CTRE and Xbyak package dependencies from xmake-repo, for logger.h
add_requires("ctre")
add_requires("xbyak")

-- include subprojects
-- includes("lib/commonlibsse-ng")

-- include CommonLibSSE-NG: use the shared copy from the CommonLibSSE_PATH environment variable if it's set. 
-- CommonLibSSE_PATH Should point to where the xmake.lua is in commonlib. E.G C:\MODS\SksePlugins\commonlibsse-ng
-- Otherwise fall back to the local submodule
local commonlib_dir = os.getenv("CommonLibSSE_PATH")
if commonlib_dir and os.isfile(path.join(commonlib_dir, "xmake.lua")) then
    includes(commonlib_dir)
else
    if commonlib_dir then
		print("CommonLibSSE_PATH (%s) has no xmake.lua, using lib/commonlibsse-ng", commonlib_dir)
    end
    includes("lib/commonlibsse-ng")
end

-- set project constants
-- replace all McmInjector with YourPluginName in this file. There's 3 instances.
set_project("McmInjector")
set_version("1.0.0")
set_license("GPL-3.0")
set_languages("c++23")
set_warnings("all")

-- add common rules
add_rules("mode.debug", "mode.releasedbg")
set_defaultmode("releasedbg")
add_rules("plugin.vsxmake.autoupdate")
add_rules("plugin.compile_commands.autoupdate", { outputdir = "." })

-- =====================================================================================================================
-- config. Run commands in the vscode terminal.
-- for the first time building, run this command:
-- xmake f -c -y -p windows -a x64 -m releasedbg --toolchain=msvc
-- xmake -r

-- with releasedbg
-- xmake f -m releasedbg
-- xmake -r

-- check options
-- xmake f --menu

-- update intellisence
-- xmake project -k compile_commands 
-- reset clangd language server

-- =====================================================================================================================

-- define targets
target("McmInjector")
    add_rules("commonlibsse-ng.plugin", {
        name = "McmInjector",
        author = "Dylbill",
        description = "SKSE plugin built with CommonLibSSE-NG"
    })

    -- Link the managed CTRE and Xbyak packages into this target
    add_packages("ctre", "xbyak")
	
    -- Windows SDK headers define ANSI/Unicode macro pairs that collide with
    -- this project's function names:
    --   winspool.h : AddForm -> AddFormA   (also DeleteForm/GetForm/SetForm/EnumForms)
    --   mmsystem.h : PlaySound -> PlaySoundA
    -- windows.h only pulls those headers in when WIN32_LEAN_AND_MEAN is absent.
    -- The old CMake/vcpkg build got these defines from the CommonLibSSE target's
    -- usage requirements; xmake does not set them, so set them here.
    -- add_defines("WIN32_LEAN_AND_MEAN", "NOMINMAX")

    -- 1. Source files: Track root plugin.cpp AND any fallback src files
    add_files("plugin.cpp")
    add_files("src/**.cpp")
    
    -- 2. Include paths and header monitoring
    add_headerfiles("pch.h")
    add_headerfiles("src/**.h")
    add_headerfiles("include/**.h")
    
    -- Add root folder, src, and include to the compiler scan path
    add_includedirs(".", "src", "include")

    -- 3. Precompiled Header updated to root path location
    set_pcxxheader("pch.h")

	-- only needed for large projects
	-- add_cxxflags("/bigobj", {force = true})
	
    -- Custom deployment step: Copies a config folder to your output directory if it exists
    after_build(function (target)
        import("core.base.option")
        local src_config_folder = path.join(os.projectdir(), "config")
        
        -- Check if you have a local config folder to deploy
        if os.isdir(src_config_folder) then
            local targetdir = target:targetdir()
            local dll_folder = path.join(targetdir, "SKSE", "Plugins")
            local dest_config_folder = path.join(dll_folder, target:name())
			
            os.mkdir(dest_config_folder)
            os.copydir(src_config_folder, dest_config_folder)
            cprint("${green}[deploy]: Config directory copied to output successfully!")
        end
		
		-- Copy the .dll and .pdb specific folders after every build.
        -- Use forward slashes (or doubled backslashes) in these paths.
		-- folders not found are skipped.
        local deploy_folders = {
			"C:/Program Files (x86)/Steam/steamapps/common/Skyrim Special Edition AE 1-7-104/Data/SKSE/Plugins",
			"C:/Program Files (x86)/Steam/steamapps/common/Skyrim Special Edition SE 1-5-97/Data/SKSE/Plugins",
			"C:/MODS/My Mods/McmInjector/Releases/McmInjector/SKSE/Plugins"
        }

        local dll = target:targetfile()
        local pdb = target:symbolfile()

        for _, folder in ipairs(deploy_folders) do
            -- Skip a game folder that doesn't exist, e.g. on another PC
            local data_folder = path.directory(path.directory(folder))
            if os.isdir(data_folder) then
                os.mkdir(folder)
                os.cp(dll, folder)
                if pdb and os.isfile(pdb) then
                    os.cp(pdb, folder)
                end
                cprint("${green}[deploy]: copied %s to %s", path.filename(dll), folder)
            else
                cprint("${yellow}[deploy]: skipped, folder not found: %s", data_folder)
            end
        end
    end)
	