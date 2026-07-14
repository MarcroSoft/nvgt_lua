/* nvgt_lua.h - Lua scripting plugin for NVGT
 * Embeds Lua 5.4 and exposes NVGT's entire registered AngelScript API to Lua through runtime reflection.
 *
 * NVGT - NonVisual Gaming Toolkit
 * Copyright (c) 2022-2025 Sam Tupy
 * https://nvgt.dev
 * This software is provided "as-is", without any express or implied warranty. In no event will the authors be held liable for any damages arising from the use of this software.
 * Permission is granted to anyone to use this software for any purpose, including commercial applications, and to alter it and redistribute it freely, subject to the following restrictions:
 * 1. The origin of this software must not be misrepresented; you must not claim that you wrote the original software. If you use this software in a product, an acknowledgment in the product documentation would be appreciated but is not required.
 * 2. Altered source versions must be plainly marked as such, and must not be misrepresented as being the original software.
 * 3. This notice may not be removed or altered from any source distribution.
 */

#pragma once

#include <string>
#include <angelscript.h>

// Lua is deliberately compiled as C++ (see lua54.cpp) so lua_error unwinds with exceptions rather than longjmp;
// therefore the headers must be included without extern "C".
#include "lua54/lua.h"
#include "lua54/lauxlib.h"
#include "lua54/lualib.h"

class nvgt_lua_bridge;

// A Lua interpreter instance, exposed to AngelScript as the lua_state class. Not thread safe; use one per thread.
class lua_state {
	int refcount;
public:
	lua_State* L;
	asIScriptEngine* engine;
	nvgt_lua_bridge* bridge; // Created by expose_nvgt, owned by this object.
	std::string last_error;
	lua_state(asIScriptEngine* engine);
	~lua_state();
	void add_ref();
	void release();
	void open_libraries();
	void expose_nvgt(bool as_globals);
	bool exec(const std::string& code, const std::string& chunkname);
	bool exec_file(const std::string& filename);
	bool call(const std::string& function_name);
	std::string get_last_error() const { return last_error; }
	void set_global_number(const std::string& name, double value);
	void set_global_string(const std::string& name, const std::string& value);
	void set_global_bool(const std::string& name, bool value);
	double get_global_number(const std::string& name);
	std::string get_global_string(const std::string& name);
	bool get_global_bool(const std::string& name);
};

// Installs the nvgt table (and optionally global fallback) into the given lua state. Implemented in nvgt_lua_bridge.cpp.
nvgt_lua_bridge* nvgt_lua_bridge_create(lua_State* L, asIScriptEngine* engine, bool as_globals);
void nvgt_lua_bridge_destroy(nvgt_lua_bridge* bridge);
