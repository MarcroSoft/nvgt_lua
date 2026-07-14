/* lua.cpp - Lua scripting plugin for NVGT
 * Registers the lua_state class with AngelScript. A lua_state embeds a Lua 5.4 interpreter which can expose NVGT's
 * entire registered API to Lua code through the reflection bridge in nvgt_lua_bridge.cpp, allowing games to be written
 * mostly or entirely in Lua with a small AngelScript loader.
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

#include "../../src/nvgt_plugin.h"
#include "nvgt_lua.h"

static asIScriptEngine* g_plugin_engine = nullptr;

lua_state::lua_state(asIScriptEngine* engine) : refcount(1), engine(engine), bridge(nullptr) {
	L = luaL_newstate();
}

lua_state::~lua_state() {
	if (L) lua_close(L); // Runs __gc handlers which may still need the bridge, so close before destroying it.
	if (bridge) nvgt_lua_bridge_destroy(bridge);
}

void lua_state::add_ref() { asAtomicInc(refcount); }
void lua_state::release() {
	if (asAtomicDec(refcount) < 1) delete this;
}

void lua_state::open_libraries() {
	if (L) luaL_openlibs(L);
}

void lua_state::expose_nvgt(bool as_globals) {
	if (!L || bridge) return;
	bridge = nvgt_lua_bridge_create(L, engine, as_globals);
}

bool lua_state::exec(const std::string& code, const std::string& chunkname) {
	if (!L) return false;
	last_error = "";
	int r = luaL_loadbuffer(L, code.data(), code.size(), chunkname.empty() ? "=nvgt" : chunkname.c_str());
	if (r == LUA_OK) r = lua_pcall(L, 0, 0, 0);
	if (r != LUA_OK) {
		const char* msg = lua_tostring(L, -1);
		last_error = msg ? msg : "unknown lua error";
		lua_pop(L, 1);
		return false;
	}
	return true;
}

bool lua_state::exec_file(const std::string& filename) {
	if (!L) return false;
	last_error = "";
	int r = luaL_loadfile(L, filename.c_str());
	if (r == LUA_OK) r = lua_pcall(L, 0, 0, 0);
	if (r != LUA_OK) {
		const char* msg = lua_tostring(L, -1);
		last_error = msg ? msg : "unknown lua error";
		lua_pop(L, 1);
		return false;
	}
	return true;
}

bool lua_state::call(const std::string& function_name) {
	if (!L) return false;
	last_error = "";
	lua_getglobal(L, function_name.c_str());
	if (!lua_isfunction(L, -1)) {
		lua_pop(L, 1);
		last_error = "no such function " + function_name;
		return false;
	}
	if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
		const char* msg = lua_tostring(L, -1);
		last_error = msg ? msg : "unknown lua error";
		lua_pop(L, 1);
		return false;
	}
	return true;
}

void lua_state::set_global_number(const std::string& name, double value) {
	if (!L) return;
	lua_pushnumber(L, value);
	lua_setglobal(L, name.c_str());
}

void lua_state::set_global_string(const std::string& name, const std::string& value) {
	if (!L) return;
	lua_pushlstring(L, value.data(), value.size());
	lua_setglobal(L, name.c_str());
}

void lua_state::set_global_bool(const std::string& name, bool value) {
	if (!L) return;
	lua_pushboolean(L, value);
	lua_setglobal(L, name.c_str());
}

double lua_state::get_global_number(const std::string& name) {
	if (!L) return 0;
	lua_getglobal(L, name.c_str());
	double v = lua_tonumber(L, -1);
	lua_pop(L, 1);
	return v;
}

std::string lua_state::get_global_string(const std::string& name) {
	if (!L) return "";
	lua_getglobal(L, name.c_str());
	size_t len = 0;
	const char* s = lua_tolstring(L, -1, &len);
	std::string v = s ? std::string(s, len) : "";
	lua_pop(L, 1);
	return v;
}

bool lua_state::get_global_bool(const std::string& name) {
	if (!L) return false;
	lua_getglobal(L, name.c_str());
	bool v = lua_toboolean(L, -1) != 0;
	lua_pop(L, 1);
	return v;
}

static lua_state* lua_state_factory() {
	return new lua_state(g_plugin_engine);
}

static std::string lua_version_string() {
	return LUA_RELEASE;
}

plugin_main(nvgt_plugin_shared* shared) {
	if (!prepare_plugin(shared)) return false;
	asIScriptEngine* engine = shared->script_engine;
	g_plugin_engine = engine;
	engine->RegisterObjectType("lua_state", 0, asOBJ_REF);
	engine->RegisterObjectBehaviour("lua_state", asBEHAVE_FACTORY, "lua_state@ f()", asFUNCTION(lua_state_factory), asCALL_CDECL);
	engine->RegisterObjectBehaviour("lua_state", asBEHAVE_ADDREF, "void f()", asMETHOD(lua_state, add_ref), asCALL_THISCALL);
	engine->RegisterObjectBehaviour("lua_state", asBEHAVE_RELEASE, "void f()", asMETHOD(lua_state, release), asCALL_THISCALL);
	engine->RegisterObjectMethod("lua_state", "void open_libraries()", asMETHOD(lua_state, open_libraries), asCALL_THISCALL);
	engine->RegisterObjectMethod("lua_state", "void expose_nvgt(bool as_globals = true)", asMETHOD(lua_state, expose_nvgt), asCALL_THISCALL);
	engine->RegisterObjectMethod("lua_state", "bool exec(const string&in code, const string&in chunkname = \"\")", asMETHOD(lua_state, exec), asCALL_THISCALL);
	engine->RegisterObjectMethod("lua_state", "bool exec_file(const string&in filename)", asMETHOD(lua_state, exec_file), asCALL_THISCALL);
	engine->RegisterObjectMethod("lua_state", "bool call(const string&in function_name)", asMETHOD(lua_state, call), asCALL_THISCALL);
	engine->RegisterObjectMethod("lua_state", "string get_last_error() const property", asMETHOD(lua_state, get_last_error), asCALL_THISCALL);
	engine->RegisterObjectMethod("lua_state", "void set_global_number(const string&in name, double value)", asMETHOD(lua_state, set_global_number), asCALL_THISCALL);
	engine->RegisterObjectMethod("lua_state", "void set_global_string(const string&in name, const string&in value)", asMETHOD(lua_state, set_global_string), asCALL_THISCALL);
	engine->RegisterObjectMethod("lua_state", "void set_global_bool(const string&in name, bool value)", asMETHOD(lua_state, set_global_bool), asCALL_THISCALL);
	engine->RegisterObjectMethod("lua_state", "double get_global_number(const string&in name)", asMETHOD(lua_state, get_global_number), asCALL_THISCALL);
	engine->RegisterObjectMethod("lua_state", "string get_global_string(const string&in name)", asMETHOD(lua_state, get_global_string), asCALL_THISCALL);
	engine->RegisterObjectMethod("lua_state", "bool get_global_bool(const string&in name)", asMETHOD(lua_state, get_global_bool), asCALL_THISCALL);
	engine->RegisterGlobalFunction("string lua_version()", asFUNCTION(lua_version_string), asCALL_CDECL);
	return true;
}
