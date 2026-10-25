-- Extensions xmake.lua
-- Includes all vendored third-party extensions.

-- ============================================================================
-- mimalloc (memory allocator)
-- ============================================================================
local mimalloc_dir = path.join(os.scriptdir(), "mimalloc")
target("mimalloc")
    _config_project({
        project_kind = "object",
        no_rtti = true
    })
    add_files(path.join(mimalloc_dir, "src/static.c"))
    add_includedirs(path.join(mimalloc_dir, "include"), {public = true})
    -- Shared-heap design: mimalloc is exported from runtime_py.pyd and imported
    -- by every test executable (MI_SHARED_LIB(_EXPORT) public gives plain
    -- __declspec(dllexport) declarations to dependents, never dllimport). This
    -- keeps ONE mimalloc instance across the pyd and its native C++ tests, so
    -- kimix::string values passed across the boundary free in the right heap.
    add_defines("MI_SHARED_LIB", {public = true})
    add_defines("MI_XMALLOC=1", "MI_WIN_NOREDIRECT", "MI_SHARED_LIB_EXPORT", {public = true})
    if is_plat("windows") then
        add_syslinks("advapi32", "bcrypt", {public = true})
        add_defines("_CRT_SECURE_NO_WARNINGS")
    elseif is_plat("linux") then
        add_syslinks("pthread", "atomic", {public = true})
        -- MI_NO_THP must NOT be defined here: vendored mimalloc 3.6.0
        -- (src/ext/mimalloc, prim/unix/prim.c) leaves a dangling `else` when
        -- MI_NO_THP is combined with MADV_NOHUGEPAGE (always defined on Linux
        -- glibc), so the unix primitive translation unit fails to compile.
        -- The THP madvise path stays runtime-gated by mi_option_allow_thp
        -- (default off) and MADV_NOHUGEPAGE prevention still applies, so
        -- dropping the define does not change allocator behavior.
    else
        add_syslinks("pthread", {public = true})
    end
target_end()

-- ============================================================================
-- yyjson (JSON library)
-- ============================================================================
target("kimix-yyjson")
    _config_project({
        project_kind = "static"
    })
    on_load(function(target)
        local src_path = path.join(os.scriptdir(), "yyjson/src")
        target:add("files", path.join(src_path, "yyjson.c"))
        target:add("includedirs", src_path, {
            public = true
        })
        target:add("cxflags", "/utf-8", {
            tools = "cl"
        })
    end)
target_end()

-- ============================================================================
-- xxHash (fast hash library) — header-only, included by kimix-core
-- ============================================================================
target("kimix-xxhash")
    set_kind("headeronly")
    on_load(function(target)
        target:add("includedirs", path.join(os.scriptdir(), "xxHash"), {
            public = true
        })
    end)
target_end()

-- ============================================================================
-- ============================================================================
-- pdqsort (Orson Peters' pattern-defeating quicksort, vendored copy as shipped
-- by LuisaCompute) — header-only; kimix-core re-exports it through
-- core/stl/pdqsort.h, consumers call it via `pdqsort(begin, end)`.
-- ============================================================================
target("kimix-pdqsort")
    set_kind("headeronly")
    on_load(function(target)
        target:add("includedirs", path.join(os.scriptdir(), "pdqsort"), {
            public = true
        })
    end)
target_end()

-- pybind11 (Python binding) — header-only
-- ============================================================================
target("kimix-pybind11")
    set_kind("headeronly")
    on_load(function(target)
        target:add("includedirs", path.join(os.scriptdir(), "pybind11/include"), {
            public = true
        })
    end)
target_end()

-- ============================================================================
-- reproc (cross-platform C99/C++11 subprocess library) — vendored at
-- src/ext/reproc (https://github.com/LuisaGroup/reproc.git, same fork + pin as
-- C:/dev/LuisaCompute). Built as a static lib named "kimix-reproc" with the
-- same layout as LuisaCompute/src/ext/reproc/xmake.lua: the C sources are
-- selected by platform suffix (".posix" vs ".windows") and the C++ wrapper
-- (reproc++) is compiled from reproc++/src.  Static (not shared) so neither
-- REPROC_SHARED nor REPROC_BUILDING is defined; export.h then expands
-- REPROC_EXPORT to nothing, which is exactly what a static link needs.
-- ============================================================================
local reproc_dir = path.join(os.scriptdir(), "reproc")
target("kimix-reproc")
    _config_project({
        project_kind = "static"
    })
    -- Linked into the runtime_py shared module: Linux requires -fPIC objects.
    if is_plat("linux") then
        add_cxflags("-fPIC", {public = true})
    end
    -- NOTE: this target lives in src/ext/xmake.lua (not in the submodule's own
    -- xmake.lua), so every relative path must be anchored at os.scriptdir()
    -- (= src/ext); plain "reproc/..." would resolve against src/ext as well
    -- but add_files/add_includedirs are evaluated at parse time, so build them
    -- explicitly with path.join for clarity.
    add_headerfiles(path.join(reproc_dir, "reproc/include/**.h"))
    add_headerfiles(path.join(reproc_dir, "reproc++/include/**.hpp"))
    add_includedirs(path.join(reproc_dir, "reproc/include/"),
                    path.join(reproc_dir, "reproc++/include/"), {
        public = true
    })
    add_rules("c++.build", "c.build")
    on_config(function(target)
        local src_path = path.join(reproc_dir, "reproc/src")
        local keyword
        if is_plat("windows") then
            keyword = ".windows"
        else
            keyword = ".posix"
        end
        for _, filepath in ipairs(os.files(path.join(src_path, "*.c"))) do
            local file_name = path.filename(filepath)
            file_name = file_name:sub(1, #file_name - 2)
            local ext = path.extension(file_name)
            if (#ext == 0 or ext == keyword) then
                target:add("files", filepath)
            end
        end
        if is_plat("windows") then
            target:add("links", "ws2_32", {public = true})
        else
            target:add("syslinks", "pthread", {public = true})
        end
    end)
    add_files(path.join(reproc_dir, "reproc++/src/reproc.cpp"))
target_end()

-- ============================================================================
-- marl (fibers + work-stealing scheduler) — vendored at src/ext/marl
-- (https://github.com/LuisaGroup/marl.git, same fork + pin as
  -- C:/dev/compute/src/ext/marl, commit 4ed34cc). Built as an OBJECT
  -- target (like mimalloc): its objects are merged into kimix-core, so the
  -- fiber scheduler and its thread_local state exist exactly ONCE per process.
  -- A static archive would be re-linked privately into every consumer dll/exe
  -- (each getting its own copy of marl's thread_local Scheduler::bound slot),
  -- which breaks fiber scheduling across the kimix-core.dll boundary
  -- (observed: deterministic test_fiber failure). The C++ sources are the
  -- single manual-unity TU
-- src/build.marl.cpp, which #includes debug/memory/scheduler/thread/trace.cpp
-- and - on _WIN32 only - osfiber_windows.cpp (the Windows Fiber API:
-- CreateFiberEx/SwitchToFiber, kernel32, already on the default link line). On
-- every other platform the context switch is hand-written assembly, so the
-- matching osfiber_<arch>.c + osfiber_asm_<arch>.S pair is added for the target
-- arch only (globbing all of them breaks cross-arch builds, e.g. -mfpu= on
-- ARM64 macOS).
--
-- Two defines carry the configuration:
--   * MARL_USE_SYSTEM_STL=1 - this fork's default STL is EASTL
--     (marl/memory.h, marl/future.h, marl/finally.h and src/memory.cpp all
--     guard their <EASTL/...> includes with it) and KimixBase does not vendor
--     EASTL, so marl is built against std::unique_ptr/shared_ptr/function/
--     optional - the mode CMake selects via LUISA_COMPUTE_USE_SYSTEM_STL.
--     Public: the define changes marl's own header declarations, so every
--     consumer must compile with it.
--   * NO MARL_DLL - a static link needs neither MARL_DLL nor
--     MARL_BUILDING_DLL; marl/export.h then expands MARL_EXPORT/MARL_NO_EXPORT
--     to nothing, which is exactly what an archived object wants.
-- ============================================================================
local marl_dir = path.join(os.scriptdir(), "marl")
  target("kimix-marl")
      _config_project({
          project_kind = "object"
      })
      -- Merged into kimix-core (shared): Linux requires -fPIC objects.
    if is_plat("linux") then
        add_cxflags("-fPIC", {public = true})
        -- osfiber_asm_<arch>.S carries no .note.GNU-stack section, so without
        -- this the linker marks every consumer (kimix_api.so) as needing an
        -- executable stack, and glibc >= 2.41 refuses to dlopen it ("cannot
        -- enable executable stack as shared object requires").
        add_asflags("-Wa,--noexecstack")
    end
    add_headerfiles(path.join(marl_dir, "include/**.h"))
    add_includedirs(path.join(marl_dir, "include"), {
        public = true
    })
    add_defines("MARL_USE_SYSTEM_STL=1", {
        public = true
    })
    add_files(path.join(marl_dir, "src/build.marl.cpp"))
    on_config(function(target)
        if target:is_plat("windows") then
            return
        end
        local arch = target:arch()
        local stem
        if arch == "arm64" or arch == "aarch64" or arch == "arm64-v8a" then
            stem = "aarch64"
        elseif arch == "x86_64" or arch == "x64" then
            stem = "x64"
        elseif arch == "x86" or arch == "i386" then
            stem = "x86"
        elseif arch == "arm" or arch == "armv7" or arch == "armeabi-v7a" then
            stem = "arm"
        elseif arch == "mips64" or arch == "mips64el" then
            stem = "mips64"
        elseif arch == "loongarch64" then
            stem = "loongarch64"
        elseif arch == "riscv64" or arch == "rv64" then
            stem = "rv64"
        elseif arch == "ppc64" or arch == "ppc64le" then
            stem = "ppc64"
        end
        if stem ~= nil then
            target:add("files", path.join(marl_dir, "src/osfiber_" .. stem .. ".c"),
                                path.join(marl_dir, "src/osfiber_asm_" .. stem .. ".S"))
        end
        -- marl spawns std::thread worker threads.
        target:add("syslinks", "pthread", {public = true})
    end)
target_end()

-- ============================================================================
-- Mbed TLS (TLS/crypto) — vendored; compiled purely by xmake (no perl/scripts).
-- ============================================================================
target("kimix-mbedtls")
    set_kind("static")
    add_rules("kimix_basic_settings") -- project-wide flags, but NO unity build
    on_load(function(target)
        local dir = path.join(os.scriptdir(), "mbedtls")
        target:add("files", path.join(dir, "library/*.c"))
        target:add("includedirs", path.join(dir, "include"), {public = true})
        if target:is_plat("windows") then
            target:add("defines", "_CRT_SECURE_NO_WARNINGS")
            target:add("syslinks", "ws2_32", "crypt32", {public = true})
        end
    end)
target_end()

-- ============================================================================
-- Lua (5.5.x, vendored at src/ext/lua) — built as the SHARED library `lua`
-- (lua.dll / liblua.so) exporting the plain C API (LUA_API) for FFI consumers.
-- Every *.c in the tree is compiled except lua.c (the standalone interpreter
-- main()), onelua.c (the single-TU distribution that #includes the others) and
-- ltests.c (Lua's internal test harness, gated by LUA_LIB + ltests.h). Three
-- guarded edits to the vendored sources carry the kimix wiring (see luaconf.h
-- and lauxlib.c); a stock build that defines none of them is unaffected:
--   * KIMIX_LUA_BUILD_SHARED (private, building this dll): luaconf.h emits
--     __declspec(dllexport) on Windows / visibility("default") elsewhere on
--     LUA_API (LUALIB_API and LUAMOD_API alias it).
--   * KIMIX_LUA_USE_SHARED (public, consumers): dllimport on Windows, plain
--     extern elsewhere — FFI/native consumers just need the declarations.
-- * KIMIX_LUA_USE_MIMALLOC (private): lauxlib.c's default allocator
-- luaL_alloc (what luaL_newstate passes to lua_newstate) routes through
-- mi_free/mi_realloc, so a Lua state shares the kimix mimalloc heap.
-- The two guarded source edits live as uncommitted changes inside the
-- src/ext/lua submodule (pinned at upstream 8cb93bad); their exported diff
-- is kept at src/ext/lua.kimix.patch — re-apply it after a submodule update.
-- The mimalloc include dir + the MI_SHARED_LIB import declarations arrive
-- transitively through kimix-core (this target's only dependency), matching
-- the project rule that third-party deps are declared on kimix-core, never
-- pulled directly.
-- ============================================================================
local lua_dir = path.join(os.scriptdir(), "lua")
target("lua")
    _config_project({
        batch_size = 8,
        project_kind = "shared",
        rtti = false,
        enable_exception = false
    })
    -- All library sources; the interpreter, single-TU and internal-test files
    -- are excluded explicitly so a glob never drags in a main() or a duplicate
    -- copy of every symbol.
    add_files(path.join(lua_dir, "*.c"))
    remove_files(path.join(lua_dir, "lua.c"),
                 path.join(lua_dir, "onelua.c"),
                 path.join(lua_dir, "ltests.c"))
    -- Public: consumers need lua.h / lauxlib.h / lualib.h (and luaconf.h).
    add_includedirs(lua_dir, {
        public = true
    })
    add_headerfiles(path.join(lua_dir, "lua.h"),
                    path.join(lua_dir, "luaconf.h"),
                    path.join(lua_dir, "lauxlib.h"),
                    path.join(lua_dir, "lualib.h"))
    -- mimalloc (mi_free/mi_realloc) and its include dir + MI_SHARED_LIB import
    -- declarations come transitively through kimix-core, per the project rule.
    add_deps("kimix-core")
    -- Building this dll: dllexport/visibility branch + the mimalloc allocator.
    add_defines("KIMIX_LUA_BUILD_SHARED", "KIMIX_LUA_USE_MIMALLOC")
    -- Consumers: dllimport/extern branch for the C API.
    add_defines("KIMIX_LUA_USE_SHARED", {
        public = true
    })
    on_load(function(target)
        if target:is_plat("windows") then
            -- C stdio/str functions (fopen, getenv, ...) otherwise warn as
            -- "deprecated" under MSVC; loadlib.c uses LoadLibraryExA.
            target:add("defines", "_CRT_SECURE_NO_WARNINGS")
        elseif target:is_plat("linux") then
            -- loadlib.c uses dlopen/dlsym/dlclose (-ldl); lmathlib.c uses the
            -- libm math functions (-lm); loslib.c's system()/clock_gettime and
            -- the pthread-backed scheduler hooks need -lpthread.
            target:add("syslinks", "dl", "m", "pthread", {
                public = true
            })
            -- liblua.so NEEDEDs libkimix-core.so; an $ORIGIN RUNPATH lets a
            -- consumer resolve the transitive dep without LD_LIBRARY_PATH
            -- (RUNPATH is not transitive, so the consumer's own $ORIGIN does
            -- not cover it).
            target:add("rpathdirs", "$ORIGIN")
        end
    end)
target_end()

-- ============================================================================
-- xNVMe (NVMe / high-performance I/O library) — vendored at src/ext/xnvme
-- (https://github.com/xnvme/xnvme, git submodule, pinned post-v0.7.5). Built
-- as the static library "kimix-xnvme" providing byte-granular vectored
-- pread/pwrite (xnvme_file_*), async command queues (xnvme_queue) and NVMe
-- passthrough for the kimix accelerated file I/O backends.
--
-- Upstream builds with meson, which does two things this target reproduces:
--   * generates 'xnvme_config.h' (per-backend XNVME_*_ENABLED defines) and
--     force-includes it into every C translation unit via
--     add_project_arguments('-include', 'xnvme_config.h'). All guards in the
--     sources are #ifdef, so enabled backends get '#define X 1' and disabled
--     ones get '/* #undef X */'. The header is written by before_build into
--     the target autogen dir, which is added as a PUBLIC include dir — public
--     because the xnvme C ABI shim target (src/xmake.lua, kimix-xnvme-shim)
--     includes <libxnvme.h> as well and consumes the same generated config.
--   * generates 'xnvme_vcs.h' (XNVME_VCS_TAG) via vcs_tag(); written here as
--     a static string with the pinned submodule describe output.
--
-- Backend selection mirrors the meson defaults with every optional external
-- dependency off (no SPDK, libvfn, libaio, liburing, libisal, CUDA, HIP,
-- uPCIE, CUSE — none of them are vendored or required):
--   * Linux:  platform linux + cbi (admin shim, sync psync, async posix /
--     thrpool / emu / nil, mem posix) + ramdisk + linux block/ioctl NVMe
--   * Windows: platform windows + cbi (admin shim, async thrpool / emu / nil)
--     + ramdisk + windows fs/async (Win32 file API + IOCP)
-- Every lib/*.c compiles unconditionally (disabled backends are empty TUs
-- through their #ifdef guards), exactly like the meson source list;
-- lib/xnvme_libconf_entries.c is excluded because xnvme_libconf.c #includes
-- it directly, and lib/{upcie,cuse}/ are separate meson subdirs left out.
-- ============================================================================
local xnvme_dir = path.join(os.scriptdir(), "xnvme")
local xnvme_gen_dir = nil -- set in on_load; shared with before_build via target
target("kimix-xnvme")
    set_kind("static")
    add_rules("kimix_basic_settings") -- project-wide flags, but NO unity build
    add_rules("c++.build", "c.build")
    if is_plat("windows") then
        -- xnvme needs a POSIX-ish C environment on Windows (pthread.h via
        -- winpthreads, __attribute__((packed)), CLOCK_MONOTONIC): upstream
        -- builds with MinGW/MSYS2, MSVC cannot compile it. This target alone
        -- switches to the MinGW toolchain; the UCRT-based static archive
        -- still links into MSVC-built binaries because both runtimes sit on
        -- UCRT. Requires a MinGW-w64 gcc on PATH (e.g. WinLibs UCRT).
        set_toolchains("mingw")
    end
    -- All library sources except the #include'd entries file; the upcie/cuse
    -- subdirectories are not part of this glob (matching the meson list).
    add_files(path.join(xnvme_dir, "lib/*.c"))
    remove_files(path.join(xnvme_dir, "lib/xnvme_libconf_entries.c"))
    add_headerfiles(path.join(xnvme_dir, "include/libxnvme*.h"))
    on_load(function(target)
        local gen_dir = path.join(target:autogendir(), "xnvme_gen")
        xnvme_gen_dir = gen_dir
        target:data_set("xnvme_gen_dir", gen_dir)
        -- Public C API headers for consumers of kimix-xnvme.
        target:add("includedirs", path.join(xnvme_dir, "include"), {
            public = true
        })
        -- Generated xnvme_config.h / xnvme_vcs.h. PUBLIC: the C ABI shim
        -- (kimix-xnvme-shim) includes <libxnvme.h> too and has to see the same
        -- generated config, so the directory must be inherited by it.
        target:add("includedirs", gen_dir, {
            public = true
        })
        -- Force-include the generated config header into every C TU
        -- (mirrors meson add_project_arguments('-include', 'xnvme_config.h')).
        -- NOTE: gated by TOOLSET, not platform — on Windows this target uses
        -- the MinGW gcc toolchain, which takes the gcc/clang form.
        target:add("cflags", "-include", "xnvme_config.h", {
            tools = {"gcc", "clang"}
        })
        target:add("cflags", "/FIxnvme_config.h", {
            tools = {"cl", "clang-cl"}
        })
        if target:is_plat("windows") then
            -- sys/uio.h + sys/queue.h shims used by the windows sources.
            target:add("includedirs", path.join(xnvme_dir, "toolbox/third-party/windows"))
        end
        if target:is_plat("linux") then
            -- xnvme_be_linux_hugepage.c includes the header-only uPCIe hostmem
            -- heap vendored at toolbox/third-party/linux (mirrors the meson
            -- conf_inc); memfd_create and friends need _GNU_SOURCE.
            target:add("includedirs", path.join(xnvme_dir, "toolbox/third-party/linux"))
            target:add("defines", "_GNU_SOURCE")
        end
        target:add("languages", "gnu11")
        if target:is_plat("windows") then
            target:add("defines", "_CRT_SECURE_NO_WARNINGS")
            -- Device enumeration (SetupDi*) in the windows platform layer.
            target:add("syslinks", "setupapi", {
                public = true
            })
        elseif target:is_plat("linux") then
            -- Linked into runtime_py: Linux requires -fPIC objects.
            target:add("cxflags", "-fPIC", {
                public = true
            })
            target:add("syslinks", "pthread", "rt", "dl", {
                public = true
            })
        else
            target:add("syslinks", "pthread", {
                public = true
            })
        end
    end)
    before_build(function(target)
        local gen_dir = target:data("xnvme_gen_dir")
        os.mkdir(gen_dir)
        -- Per-backend enable table: enabled -> '#define X 1', disabled ->
        -- '/* #undef X */' (all guards in the sources are #ifdef).
        local enabled = {
            -- common
            "XNVME_BE_CBI_ADMIN_SHIM_ENABLED",
            "XNVME_BE_CBI_ASYNC_EMU_ENABLED",
            "XNVME_BE_CBI_ASYNC_NIL_ENABLED",
            "XNVME_BE_CBI_ASYNC_THRPOOL_ENABLED",
            "XNVME_BE_RAMDISK_ENABLED"
        }
        if is_mode("debug") then
            table.insert(enabled, "XNVME_DEBUG_ENABLED")
        end
        if is_plat("linux") then
            table.join2(enabled, {
                "XNVME_PLATFORM_LINUX_ENABLED",
                "XNVME_BE_LINUX_BLOCK_ENABLED",
                "XNVME_BE_LINUX_BLOCK_ZONED_ENABLED",
                "XNVME_BE_LINUX_BLK_ZONE_CAPACITY_ENABLED",
                "XNVME_BE_CBI_ASYNC_POSIX_ENABLED",
                "XNVME_BE_CBI_MEM_POSIX_ENABLED",
                "XNVME_BE_CBI_SYNC_PSYNC_ENABLED",
                "XNVME_PTHREAD_SETAFFINITY_NP_ENABLED",
                "XNVME_RAND_R_ENABLED"
            })
        elseif is_plat("windows") then
            table.join2(enabled, {
                "XNVME_PLATFORM_WINDOWS_ENABLED",
                "XNVME_BE_WINDOWS_FS_ENABLED",
                "XNVME_BE_WINDOWS_ASYNC_ENABLED"
            })
        end
        -- Disabled keys are emitted as /* #undef */ so #ifdef stays false.
        local all_keys = {
            "XNVME_DEBUG_ENABLED",
            "XNVME_BE_CBI_ADMIN_SHIM_ENABLED",
            "XNVME_BE_CBI_ASYNC_EMU_ENABLED",
            "XNVME_BE_CBI_ASYNC_NIL_ENABLED",
            "XNVME_BE_CBI_ASYNC_POSIX_ENABLED",
            "XNVME_BE_CBI_ASYNC_THRPOOL_ENABLED",
            "XNVME_BE_CBI_MEM_POSIX_ENABLED",
            "XNVME_BE_CBI_SYNC_PSYNC_ENABLED",
            "XNVME_BE_RAMDISK_ENABLED",
            "XNVME_BE_UPCIE_ENABLED",
            "XNVME_BE_UPCIE_CUDA_ENABLED",
            "XNVME_BE_UPCIE_HIP_ENABLED",
            "XNVME_PLATFORM_WINDOWS_ENABLED",
            "XNVME_BE_WINDOWS_FS_ENABLED",
            "XNVME_BE_WINDOWS_ASYNC_ENABLED",
            "XNVME_CUSE_ENABLED",
            "XNVME_PLATFORM_LINUX_ENABLED",
            "XNVME_BE_LINUX_BLOCK_ENABLED",
            "XNVME_BE_LINUX_BLOCK_ZONED_ENABLED",
            "XNVME_BE_LINUX_BLK_ZONE_CAPACITY_ENABLED",
            "XNVME_BE_LINUX_LIBAIO_ENABLED",
            "XNVME_BE_LINUX_LIBURING_ENABLED",
            "XNVME_BE_LIBVFN_ENABLED",
            "XNVME_BE_LINUX_LIBISAL_ENABLED",
            "XNVME_BE_SPDK_ENABLED",
            "XNVME_BE_SPDK_TRANSPORT_PCIE_ENABLED",
            "XNVME_BE_SPDK_TRANSPORT_TCP_ENABLED",
            "XNVME_BE_SPDK_TRANSPORT_RDMA_ENABLED",
            "XNVME_BE_SPDK_TRANSPORT_FC_ENABLED",
            "XNVME_PLATFORM_MACOS_ENABLED",
            "XNVME_PLATFORM_FREEBSD_ENABLED",
            "XNVME_PTHREAD_SETAFFINITY_NP_ENABLED",
            "XNVME_RAND_R_ENABLED"
        }
        local lines = {
            "/* Auto-generated by src/ext/xmake.lua (kimix-xnvme before_build).",
            " * Mirrors the meson configure_file output: enabled backends are",
            " * '#define X 1', disabled ones are left undefined for #ifdef. */",
            "#ifndef __XNVME_CONFIG_H_KIMIX__",
            "#define __XNVME_CONFIG_H_KIMIX__",
            "#define XNVME_VERSION_MAJOR 0",
            "#define XNVME_VERSION_MINOR 7",
            "#define XNVME_VERSION_PATCH 5"
        }
        for _, key in ipairs(all_keys) do
            if table.contains(enabled, key) then
                table.insert(lines, "#define " .. key .. " 1")
            else
                table.insert(lines, "/* #undef " .. key .. " */")
            end
        end
        table.insert(lines, "#endif")
        -- Rewrite ONLY when the content actually differs (same reasoning as
        -- kimix_version.h in src/xmake.lua): before_build runs on every xmake
        -- invocation, and an unconditional io.writefile bumps the mtime of a
        -- FORCE-INCLUDED header, which marks every xnvme translation unit stale
        -- and rebuilds the whole vendored library even when nothing changed.
        -- io.readfile returns the file byte-exactly, so this is a real content
        -- comparison; os.isfile guards the missing-file case (io.readfile
        -- raises on it).
        local config_h = path.join(gen_dir, "xnvme_config.h")
        local config_content = table.concat(lines, "\n") .. "\n"
        if not os.isfile(config_h) or io.readfile(config_h) ~= config_content then
            io.writefile(config_h, config_content)
        end
        local vcs_h = path.join(gen_dir, "xnvme_vcs.h")
        local vcs_content =
            "/* Auto-generated by src/ext/xmake.lua (kimix-xnvme before_build). */\n" ..
            "#define XNVME_VCS_TAG \"kimix-xnvme\"\n"
        if not os.isfile(vcs_h) or io.readfile(vcs_h) ~= vcs_content then
            io.writefile(vcs_h, vcs_content)
        end
        -- macOS keeps building (its platform layer exists upstream), but no
        -- backend is wired for it here; anything beyond linux/windows/macosx is
        -- even less supported. One line, no raise: the resulting archive is
        -- backend-less, so every xnvme open fails at runtime.
        local plat = target:plat()
        if plat ~= "linux" and plat ~= "windows" and plat ~= "macosx" then
            cprint("${yellow}note${reset}: xnvme backends are only wired for linux/windows/macosx " ..
                "(src/ext/xmake.lua) — on '%s' kimix-xnvme builds WITHOUT any backend", plat)
        end
    end)
target_end()

-- SQLite3 amalgamation (vendored pregenerated amalgamation at
-- sqlite_amalgamation/) — see the header comment in sqlite_xmake.lua.
includes("sqlite_xmake.lua")
