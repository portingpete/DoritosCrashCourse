# Upstream Lua is a native PC library. Its source archive and license are
# independent of the game assets and are pinned by the upstream SHA-256.
include(FetchContent)
FetchContent_Declare(doritos_lua51
    URL https://www.lua.org/ftp/lua-5.1.5.tar.gz
    URL_HASH SHA256=2640fc56a795f29d28ef15e13c34a47e223960b0240e8cb0a82d9b0738695333
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(doritos_lua51)
set(lua_sources lapi lcode ldebug ldo ldump lfunc lgc llex lmem lobject
    lopcodes lparser lstate lstring ltable ltm lundump lvm lzio
    lauxlib lbaselib lmathlib lstrlib ltablib)
set(lua_files)
foreach(name IN LISTS lua_sources)
    list(APPEND lua_files "${doritos_lua51_SOURCE_DIR}/src/${name}.c")
endforeach()
# Lua's supported C++ build uses exceptions, so native callback temporaries
# unwind normally on script errors instead of being skipped by longjmp.
set_source_files_properties(${lua_files} PROPERTIES LANGUAGE CXX)
add_library(DoritosLua51 STATIC ${lua_files})
target_include_directories(DoritosLua51 PUBLIC "${doritos_lua51_SOURCE_DIR}/src")
target_compile_definitions(DoritosLua51 PRIVATE _CRT_SECURE_NO_WARNINGS)
configure_file("${doritos_lua51_SOURCE_DIR}/COPYRIGHT"
    "${CMAKE_CURRENT_BINARY_DIR}/Lua-5.1.5-COPYRIGHT.txt" COPYONLY)
