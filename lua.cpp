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
#include <filesystem>
#include <fstream>
#include <iterator>

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

// Run protected from open_libraries.
static int restrict_package_search(lua_State* L) {
	if (lua_getglobal(L, "package") != LUA_TTABLE) return 0;
	lua_pushliteral(L, "./?.lua;./?.ois;./?/init.lua;./?/init.ois");
	lua_setfield(L, -2, "path");
	lua_pushliteral(L, "");
	lua_setfield(L, -2, "cpath");
	if (lua_getfield(L, -1, "searchers") == LUA_TTABLE) {
		lua_pushnil(L);
		lua_seti(L, -2, 4);
		lua_pushnil(L);
		lua_seti(L, -2, 3);
	}
	return 0;
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
	// Done through the C API rather than as a chunk of Lua, which a compiled game's Lua could not parse.
	lua_pushcfunction(L, restrict_package_search);
	if (lua_pcall(L, 0, 0, 0) != LUA_OK) lua_pop(L, 1);
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

// The pack hooks installed by the first set_pack. They are C rather than a chunk of Lua, which a compiled game's Lua
// could not parse. Everything consults package.pack at call time, so later set_pack calls only swap that field.

// A pack path as pack_file stores it: forward slashes, no doubled ones and no leading ./
static std::string pack_normalize(const char* name, size_t len) {
	std::string out;
	out.reserve(len);
	for (size_t i = 0; i < len; i++) {
		char c = name[i] == '\\' ? '/' : name[i];
		if (c == '/' && !out.empty() && out.back() == '/') continue;
		out += c;
	}
	size_t skip = 0;
	while (out.compare(skip, 2, "./") == 0) skip += 2;
	return out.substr(skip);
}

// Looks name up in package.pack, with the package table at package_index. On success pushes the file's contents and
// its normalized name and returns true; otherwise leaves the stack as it was.
static bool pack_read(lua_State* L, int package_index, const char* name, size_t len) {
	lua_getfield(L, package_index, "pack");
	if (!lua_toboolean(L, -1)) {
		lua_pop(L, 1);
		return false;
	}
	int pack = lua_gettop(L);
	std::string normalized = pack_normalize(name, len);
	lua_getfield(L, pack, "file_exists");
	lua_pushvalue(L, pack);
	lua_pushlstring(L, normalized.data(), normalized.size());
	lua_call(L, 2, 1);
	bool exists = lua_toboolean(L, -1);
	lua_pop(L, 1);
	if (!exists) {
		lua_pop(L, 1);
		return false;
	}
	lua_getfield(L, pack, "get_file");
	lua_pushvalue(L, pack);
	lua_pushlstring(L, normalized.data(), normalized.size());
	lua_call(L, 2, 1); // the file
	lua_getfield(L, -1, "read");
	lua_insert(L, -2);
	lua_call(L, 1, 1); // its contents
	if (!lua_isstring(L, -1)) luaL_error(L, "could not read '%s' from the pack", normalized.c_str());
	lua_remove(L, pack);
	lua_pushlstring(L, normalized.data(), normalized.size());
	return true;
}

// Replaces loadfile: loadfile(filename [, mode [, env]]) loads from the pack when it has the file, else from the disk.
// Upvalues: the package table, the original loadfile.
static int pack_loadfile(lua_State* L) {
	int nargs = lua_gettop(L);
	if (!lua_isnoneornil(L, 1)) {
		size_t len;
		const char* filename = luaL_checklstring(L, 1, &len);
		const char* mode = luaL_optstring(L, 2, "bt"); // before pack_read pushes onto the stack
		if (pack_read(L, lua_upvalueindex(1), filename, len)) {
			size_t source_len;
			const char* source = lua_tolstring(L, -2, &source_len);
			std::string chunkname = std::string("@") + lua_tostring(L, -1);
			if (luaL_loadbufferx(L, source, source_len, chunkname.c_str(), mode) != LUA_OK) {
				luaL_pushfail(L);
				lua_insert(L, -2);
				return 2; // fail, message
			}
			if (nargs >= 3) { // an explicit nil env counts as given, as it does for load
				lua_pushvalue(L, 3);
				if (!lua_setupvalue(L, -2, 1)) lua_pop(L, 1);
			}
			return 1;
		}
	}
	lua_pushvalue(L, lua_upvalueindex(2));
	lua_insert(L, 1);
	lua_call(L, nargs, LUA_MULTRET);
	return lua_gettop(L);
}

static int pack_dofile_continue(lua_State* L, int status, lua_KContext ctx) {
	(void)status;
	(void)ctx;
	return lua_gettop(L) - 1;
}

// Replaces dofile so it goes through pack_loadfile. Upvalue: pack_loadfile.
static int pack_dofile(lua_State* L) {
	lua_settop(L, 1);
	lua_pushvalue(L, lua_upvalueindex(1));
	lua_pushvalue(L, 1);
	lua_call(L, 1, 2);
	if (!lua_toboolean(L, -2)) {
		if (lua_type(L, -1) == LUA_TSTRING) { // as error(message, 2) would: where dofile was called from
			luaL_where(L, 1);
			lua_insert(L, -2);
			lua_concat(L, 2);
		}
		return lua_error(L);
	}
	lua_pop(L, 1);
	lua_callk(L, 0, LUA_MULTRET, 0, pack_dofile_continue); // continuation keeps yields across dofile working
	return pack_dofile_continue(L, LUA_OK, 0);
}

// The package.searchers entry that finds modules in the pack, right after package.preload so a module in the pack wins
// over one on the disk. It resolves modname through package.path like the stock Lua searcher. Upvalue: package table.
static int pack_searcher(lua_State* L) {
	const char* modname = luaL_checkstring(L, 1);
	lua_getfield(L, lua_upvalueindex(1), "pack");
	bool have_pack = lua_toboolean(L, -1);
	lua_pop(L, 1);
	if (!have_pack) return 0;
	std::string base = modname;
	for (char& c : base) if (c == '.') c = '/';
	lua_getfield(L, lua_upvalueindex(1), "path");
	const char* path = lua_tostring(L, -1);
	if (!path) return luaL_error(L, "'package.path' must be a string");
	std::string templates = path;
	lua_pop(L, 1);
	std::string tried;
	for (size_t pos = 0; pos <= templates.size();) {
		size_t sep = templates.find(';', pos);
		if (sep == std::string::npos) sep = templates.size();
		std::string filename;
		for (size_t i = pos; i < sep; i++) {
			if (templates[i] == '?') filename += base;
			else filename += templates[i];
		}
		pos = sep + 1;
		if (filename.empty()) continue;
		if (pack_read(L, lua_upvalueindex(1), filename.data(), filename.size())) {
			size_t source_len;
			const char* source = lua_tolstring(L, -2, &source_len);
			std::string name = lua_tostring(L, -1);
			std::string chunkname = "@" + name;
			if (luaL_loadbufferx(L, source, source_len, chunkname.c_str(), nullptr) != LUA_OK) {
				lua_pushfstring(L, "error loading module '%s' from pack file '%s':\n\t%s", modname, name.c_str(), lua_tostring(L, -1));
				return lua_error(L);
			}
			lua_pushlstring(L, name.data(), name.size());
			return 2; // loader, file name
		}
		if (!tried.empty()) tried += "\n\t";
		tried += "no file '" + pack_normalize(filename.data(), filename.size()) + "' in pack";
	}
	lua_pushlstring(L, tried.data(), tried.size());
	return 1;
}

// Run protected by the first set_pack. Installs the hooks and returns pack_loadfile, which exec_file uses as well.
static int install_pack_hooks(lua_State* L) {
	lua_getglobal(L, "package");
	int package = lua_gettop(L);
	lua_pushvalue(L, package);
	lua_getglobal(L, "loadfile");
	lua_pushcclosure(L, pack_loadfile, 2);
	lua_pushvalue(L, -1);
	lua_setglobal(L, "loadfile");
	lua_pushvalue(L, -1);
	lua_pushcclosure(L, pack_dofile, 1);
	lua_setglobal(L, "dofile");
	// table.insert(package.searchers, 2, pack_searcher)
	if (lua_getfield(L, package, "searchers") != LUA_TTABLE) return luaL_error(L, "'package.searchers' must be a table");
	for (lua_Integer i = luaL_len(L, -1); i >= 2; i--) {
		lua_geti(L, -1, i);
		lua_seti(L, -2, i + 1);
	}
	lua_pushvalue(L, package);
	lua_pushcclosure(L, pack_searcher, 1);
	lua_seti(L, -2, 2);
	lua_pop(L, 1);
	return 1; // pack_loadfile
}

// The parameter is a variable type (?&in) rather than pack_file@: compiled games load plugins before
// NVGT registers its own API, so naming pack_file in the declaration would fail to register there.
// A pack_file object or handle is accepted, and null switches back to the disk.
bool lua_state::set_pack(void* ref, int type_id) {
	if (!L) return false;
	last_error = "";
	last_error_code = LUA_OK;
	bool clear = !ref || type_id == asTYPEID_VOID || ((type_id & asTYPEID_OBJHANDLE) && !*(void**)ref);
	if (!clear) {
		asITypeInfo* ti = (type_id & asTYPEID_MASK_OBJECT) ? engine->GetTypeInfoById(type_id) : nullptr;
		if (!ti || strcmp(ti->GetName(), "pack_file") != 0) {
			last_error = "set_pack expects a pack_file";
			return false;
		}
	}
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
		lua_pushcfunction(L, install_pack_hooks);
		int r = lua_pcall(L, 0, 1, 0);
		if (r != LUA_OK) {
			const char* msg = lua_tostring(L, -1);
			last_error = msg ? msg : "unknown lua error";
			last_error_code = r;
			lua_pop(L, 1);
			return false;
		}
		pack_loadfile_ref = luaL_ref(L, LUA_REGISTRYINDEX);
	}
	// Marshal the pack through the bridge into a temporary global, then move it to package.pack.
	if (clear) lua_pushnil(L);
	else {
		if (!nvgt_lua_bridge_set_global(L, bridge, "__nvgt_pack", ref, type_id, last_error)) return false;
		lua_getglobal(L, "__nvgt_pack");
	}
	lua_getglobal(L, "package");
	lua_insert(L, -2);
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

// Defined in lua55.cpp: false in the bytecode-only Lua that NVGT's stubs link when the plugin is linked statically.
bool nvgt_lua_has_compiler();

static void raise_script_exception(const std::string& message) {
	asIScriptContext* ctx = asGetActiveContext();
	if (ctx) ctx->SetException(message.c_str());
}

static int compile_writer(lua_State* L, const void* p, size_t size, void* ud) {
	(void)L;
	static_cast<std::string*>(ud)->append(static_cast<const char*>(p), size);
	return 0;
}

// Compiles a chunk to bytecode in a throwaway lua_State. On failure raises a script exception and returns false.
static bool compile_chunk(const std::string& source, const std::string& chunkname, bool strip, std::string& bytecode, const char* caller) {
	if (!nvgt_lua_has_compiler()) {
		raise_script_exception(std::string(caller) + " is not available in compiled games, whose Lua can only load precompiled code; compile the Lua code with nvgt beforehand");
		return false;
	}
	lua_State* L = luaL_newstate();
	if (!L) {
		raise_script_exception(std::string(caller) + ": not enough memory");
		return false;
	}
	int r = luaL_loadbufferx(L, source.data(), source.size(), chunkname.c_str(), nullptr);
	if (r != LUA_OK) {
		const char* msg = lua_tostring(L, -1);
		std::string error = msg ? msg : "unknown lua error";
		lua_close(L);
		raise_script_exception(error);
		return false;
	}
	bytecode.clear();
	r = lua_dump(L, compile_writer, &bytecode, strip ? 1 : 0);
	lua_close(L);
	if (r != 0) {
		raise_script_exception(std::string(caller) + ": could not write the bytecode");
		return false;
	}
	return true;
}

static std::string lua_compile(const std::string& source, const std::string& chunkname, bool strip) {
	std::string bytecode;
	if (!compile_chunk(source, chunkname.empty() ? "=nvgt" : chunkname, strip, bytecode, "lua_compile")) return "";
	return bytecode;
}

// Paths are UTF-8, as everywhere in NVGT.
static void lua_compile_file(const std::string& input, const std::string& output, bool strip) {
	std::string bytecode;
	if (!nvgt_lua_has_compiler()) { // say so before complaining about a missing input file
		compile_chunk("", "", strip, bytecode, "lua_compile_file");
		return;
	}
	std::ifstream in(std::filesystem::path(std::u8string(input.begin(), input.end())), std::ios::binary);
	if (!in) {
		raise_script_exception("lua_compile_file: cannot open " + input);
		return;
	}
	std::string source((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	in.close();
	if (!compile_chunk(source, "@" + input, strip, bytecode, "lua_compile_file")) return;
	std::ofstream out(std::filesystem::path(std::u8string(output.begin(), output.end())), std::ios::binary | std::ios::trunc);
	if (!out || !out.write(bytecode.data(), bytecode.size())) raise_script_exception("lua_compile_file: cannot write " + output);
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
	engine->RegisterObjectMethod("lua_state", "bool set_pack(const ?&in pack)", asMETHOD(lua_state, set_pack), asCALL_THISCALL);
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
	engine->RegisterGlobalFunction("string lua_compile(const string&in source, const string&in chunkname = \"\", bool strip = false)", asFUNCTION(lua_compile), asCALL_CDECL);
	engine->RegisterGlobalFunction("void lua_compile_file(const string&in input, const string&in output, bool strip = false)", asFUNCTION(lua_compile_file), asCALL_CDECL);
	return true;
}
