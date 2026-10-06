/* lua.cpp - Lua scripting plugin for NVGT
 * Registers the lua_state class with AngelScript. A lua_state embeds a Lua 5.5 interpreter which can expose NVGT's
 * entire registered API to Lua code through the reflection bridge in nvgt_lua_bridge.cpp, allowing games to be written
 * mostly or entirely in Lua with a small AngelScript loader.
 *
 * Plugin for NVGT - NonVisual Gaming Toolkit (https://nvgt.dev)
 * Copyright (c) 2026 MarcroSoft
 * This software is provided "as-is", without any express or implied warranty. In no event will the authors be held liable for any damages arising from the use of this software.
 * Permission is granted to anyone to use this software for any purpose, including commercial applications, and to alter it and redistribute it freely, subject to the following restrictions:
 * 1. The origin of this software must not be misrepresented; you must not claim that you wrote the original software. If you use this software in a product, an acknowledgment in the product documentation would be appreciated but is not required.
 * 2. Altered source versions must be plainly marked as such, and must not be misrepresented as being the original software.
 * 3. This notice may not be removed or altered from any source distribution.
 */

#include "../../src/nvgt_plugin.h"
#include "nvgt_lua.h"
#include <cstring>

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
	if (!L) return;
	luaL_openlibs(L);
	// Keep require game-local: package.preload plus the working directory. The stock search path probes
	// machine-wide Lua installs relative to the host executable, which never apply to an NVGT game and
	// just bloat "module not found" errors. C module loading is disabled entirely; binary modules built
	// against a standalone Lua would not be ABI compatible with the interpreter embedded here anyway.
	// .ois files are searched as well as .lua ones so that games can ship their Lua modules under an
	// extension of their own; they are loaded as ordinary Lua chunks (source or precompiled).
	luaL_dostring(L, "package.path = './?.lua;./?.ois;./?/init.lua;./?/init.ois' package.cpath = '' package.searchers[4] = nil package.searchers[3] = nil");
}

void lua_state::expose_nvgt(bool as_globals) {
	if (!L || bridge) return;
	bridge = nvgt_lua_bridge_create(L, engine, as_globals);
}

bool lua_state::exec(const std::string& code, const std::string& chunkname) {
	if (!L) return false;
	last_error = "";
	last_error_code = LUA_OK;
	int r = luaL_loadbuffer(L, code.data(), code.size(), chunkname.empty() ? "=nvgt" : chunkname.c_str());
	if (r == LUA_OK) r = lua_pcall(L, 0, 0, 0);
	if (r != LUA_OK) {
		const char* msg = lua_tostring(L, -1);
		last_error = msg ? msg : "unknown lua error";
		last_error_code = r;
		lua_pop(L, 1);
		return false;
	}
	return true;
}

bool lua_state::exec_file(const std::string& filename) {
	if (!L) return false;
	last_error = "";
	last_error_code = LUA_OK;
	int r;
	if (pack_loadfile_ref != LUA_NOREF) {
		// Load through the pack-aware loadfile installed by set_pack, which falls back to the disk.
		lua_rawgeti(L, LUA_REGISTRYINDEX, pack_loadfile_ref);
		lua_pushlstring(L, filename.data(), filename.size());
		r = lua_pcall(L, 1, 2, 0);
		if (r == LUA_OK) {
			if (lua_isfunction(L, -2)) lua_pop(L, 1);
			else {
				const char* msg = lua_tostring(L, -1);
				r = msg && strncmp(msg, "cannot open", 11) == 0 ? LUA_ERRFILE : LUA_ERRSYNTAX;
				lua_remove(L, -2);
			}
		}
	} else r = luaL_loadfile(L, filename.c_str());
	if (r == LUA_OK) r = lua_pcall(L, 0, 0, 0);
	if (r != LUA_OK) {
		const char* msg = lua_tostring(L, -1);
		last_error = msg ? msg : "unknown lua error";
		last_error_code = r;
		lua_pop(L, 1);
		return false;
	}
	return true;
}

// Installed once per lua_state by the first set_pack. Everything consults package.pack at call time, so later
// set_pack calls only swap that field. Returns the pack-aware loadfile, which exec_file uses as well.
static const char* pack_hooks_lua = R"LUA(
local package, load, loadfile_disk, select, error, concat = package, load, loadfile, select, error, table.concat
local function normalize(name)
	name = name:gsub("\\", "/"):gsub("//+", "/")
	while name:sub(1, 2) == "./" do name = name:sub(3) end
	return name
end
local function read(name)
	local pack = package.pack
	if not pack then return nil end
	name = normalize(name)
	if not pack:file_exists(name) then return nil end
	return pack:get_file(name):read(), name
end
local function pack_loadfile(filename, mode, ...)
	if filename ~= nil then
		local src, name = read(filename)
		if src then
			-- an explicit nil env would count as given, so only pass env through when the caller did
			if select("#", ...) > 0 then return load(src, "@" .. name, mode or "bt", (...)) end
			return load(src, "@" .. name, mode or "bt")
		end
	end
	return loadfile_disk(filename, mode, ...)
end
loadfile = pack_loadfile
function dofile(filename)
	local f, err = pack_loadfile(filename)
	if not f then error(err, 2) end
	return f()
end
-- searched right after package.preload, so a module in the pack wins over one on disk
table.insert(package.searchers, 2, function(modname)
	if not package.pack then return nil end
	local base = modname:gsub("%.", "/")
	local tried = {}
	for template in package.path:gmatch("[^;]+") do
		local src, name = read((template:gsub("%?", base)))
		if src then
			local f, err = load(src, "@" .. name)
			if not f then error(("error loading module '%s' from pack file '%s':\n\t%s"):format(modname, name, err), 2) end
			return f, name
		end
		tried[#tried + 1] = "no file '" .. normalize((template:gsub("%?", base))) .. "' in pack"
	end
	return concat(tried, "\n\t")
end)
return pack_loadfile
)LUA";

bool lua_state::set_pack(void* pack) {
	if (!L) return false;
	last_error = "";
	last_error_code = LUA_OK;
	if (!bridge) {
		last_error = "expose_nvgt must be called before set_pack";
		return false;
	}
	lua_getglobal(L, "package");
	bool have_package = lua_istable(L, -1);
	lua_pop(L, 1);
	if (!have_package) {
		last_error = "open_libraries must be called before set_pack";
		return false;
	}
	if (pack_loadfile_ref == LUA_NOREF) {
		int r = luaL_loadbuffer(L, pack_hooks_lua, strlen(pack_hooks_lua), "=nvgt_pack");
		if (r == LUA_OK) r = lua_pcall(L, 0, 1, 0);
		if (r != LUA_OK) {
			const char* msg = lua_tostring(L, -1);
			last_error = msg ? msg : "unknown lua error";
			last_error_code = r;
			lua_pop(L, 1);
			return false;
		}
		pack_loadfile_ref = luaL_ref(L, LUA_REGISTRYINDEX);
	}
	// Marshal the handle through the bridge into a temporary global, then move it to package.pack.
	if (!nvgt_lua_bridge_set_global(L, bridge, "__nvgt_pack", &pack, engine->GetTypeIdByDecl("pack_file@"), last_error)) return false;
	lua_getglobal(L, "package");
	lua_getglobal(L, "__nvgt_pack");
	lua_setfield(L, -2, "pack");
	lua_pop(L, 1);
	lua_pushnil(L);
	lua_setglobal(L, "__nvgt_pack");
	return true;
}

bool lua_state::call(const std::string& function_name) {
	if (!L) return false;
	last_error = "";
	last_error_code = LUA_OK;
	lua_getglobal(L, function_name.c_str());
	if (!lua_isfunction(L, -1)) {
		lua_pop(L, 1);
		last_error = "no such function " + function_name;
		last_error_code = LUA_ERRRUN;
		return false;
	}
	int r = lua_pcall(L, 0, 0, 0);
	if (r != LUA_OK) {
		const char* msg = lua_tostring(L, -1);
		last_error = msg ? msg : "unknown lua error";
		last_error_code = r;
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

bool lua_state::set_global(const std::string& name, void* ref, int type_id) {
	last_error = "";
	if (!L) return false;
	if (!bridge) {
		last_error = "expose_nvgt must be called before set_global";
		return false;
	}
	return nvgt_lua_bridge_set_global(L, bridge, name, ref, type_id, last_error);
}

bool lua_state::get_global(const std::string& name, void* ref, int type_id) {
	last_error = "";
	if (!L) return false;
	if (!bridge) {
		last_error = "expose_nvgt must be called before get_global";
		return false;
	}
	return nvgt_lua_bridge_get_global(L, bridge, name, ref, type_id, last_error);
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
	engine->RegisterEnum("lua_status");
	engine->RegisterEnumValue("lua_status", "LUA_OK", LUA_OK);
	engine->RegisterEnumValue("lua_status", "LUA_ERRRUN", LUA_ERRRUN);
	engine->RegisterEnumValue("lua_status", "LUA_ERRSYNTAX", LUA_ERRSYNTAX);
	engine->RegisterEnumValue("lua_status", "LUA_ERRMEM", LUA_ERRMEM);
	engine->RegisterEnumValue("lua_status", "LUA_ERRERR", LUA_ERRERR);
	engine->RegisterEnumValue("lua_status", "LUA_ERRFILE", LUA_ERRFILE);
	engine->RegisterObjectType("lua_state", 0, asOBJ_REF);
	engine->RegisterObjectBehaviour("lua_state", asBEHAVE_FACTORY, "lua_state@ f()", asFUNCTION(lua_state_factory), asCALL_CDECL);
	engine->RegisterObjectBehaviour("lua_state", asBEHAVE_ADDREF, "void f()", asMETHOD(lua_state, add_ref), asCALL_THISCALL);
	engine->RegisterObjectBehaviour("lua_state", asBEHAVE_RELEASE, "void f()", asMETHOD(lua_state, release), asCALL_THISCALL);
	engine->RegisterObjectMethod("lua_state", "void open_libraries()", asMETHOD(lua_state, open_libraries), asCALL_THISCALL);
	engine->RegisterObjectMethod("lua_state", "void expose_nvgt(bool as_globals = true)", asMETHOD(lua_state, expose_nvgt), asCALL_THISCALL);
	engine->RegisterObjectMethod("lua_state", "bool exec(const string&in code, const string&in chunkname = \"\")", asMETHOD(lua_state, exec), asCALL_THISCALL);
	engine->RegisterObjectMethod("lua_state", "bool exec_file(const string&in filename)", asMETHOD(lua_state, exec_file), asCALL_THISCALL);
	engine->RegisterObjectMethod("lua_state", "bool call(const string&in function_name)", asMETHOD(lua_state, call), asCALL_THISCALL);
	engine->RegisterObjectMethod("lua_state", "bool set_pack(pack_file@+ pack)", asMETHOD(lua_state, set_pack), asCALL_THISCALL);
	engine->RegisterObjectMethod("lua_state", "string get_last_error() const property", asMETHOD(lua_state, get_last_error), asCALL_THISCALL);
	engine->RegisterObjectMethod("lua_state", "lua_status get_last_error_code() const property", asMETHOD(lua_state, get_last_error_code), asCALL_THISCALL);
	engine->RegisterObjectMethod("lua_state", "void set_global_number(const string&in name, double value)", asMETHOD(lua_state, set_global_number), asCALL_THISCALL);
	engine->RegisterObjectMethod("lua_state", "void set_global_string(const string&in name, const string&in value)", asMETHOD(lua_state, set_global_string), asCALL_THISCALL);
	engine->RegisterObjectMethod("lua_state", "void set_global_bool(const string&in name, bool value)", asMETHOD(lua_state, set_global_bool), asCALL_THISCALL);
	engine->RegisterObjectMethod("lua_state", "double get_global_number(const string&in name)", asMETHOD(lua_state, get_global_number), asCALL_THISCALL);
	engine->RegisterObjectMethod("lua_state", "string get_global_string(const string&in name)", asMETHOD(lua_state, get_global_string), asCALL_THISCALL);
	engine->RegisterObjectMethod("lua_state", "bool get_global_bool(const string&in name)", asMETHOD(lua_state, get_global_bool), asCALL_THISCALL);
	engine->RegisterObjectMethod("lua_state", "bool set_global(const string&in name, const ?&in value)", asMETHOD(lua_state, set_global), asCALL_THISCALL);
	engine->RegisterObjectMethod("lua_state", "bool get_global(const string&in name, ?&out value)", asMETHOD(lua_state, get_global), asCALL_THISCALL);
	engine->RegisterGlobalFunction("string lua_version()", asFUNCTION(lua_version_string), asCALL_CDECL);
	return true;
}
