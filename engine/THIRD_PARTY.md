# Native engine dependencies

Lua 5.1.5 is downloaded from [the official Lua archive](https://www.lua.org/ftp/lua-5.1.5.tar.gz), with SHA-256 `2640fc56a795f29d28ef15e13c34a47e223960b0240e8cb0a82d9b0738695333` checked by CMake. Its source is compiled for Windows x64, using Lua's supported C++ exception mode. No game-provided Lua executable or machine code is used.

Lua 5.1.5 is copyright (C) 1994-2012 Lua.org, PUC-Rio, distributed under the MIT license. The complete upstream license is retained in the downloaded source's `COPYRIGHT` and copied to the engine build directory as `Lua-5.1.5-COPYRIGHT.txt`. Include that notice when distributing the native library or executable. See [Lua's license](https://www.lua.org/license.html).
