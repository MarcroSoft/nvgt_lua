/* nvgt_lua_bridge.cpp - reflection bridge exposing NVGT's registered AngelScript API to Lua
 * Rather than hand-binding thousands of functions, this walks the AngelScript engine's registration tables at runtime
 * and dispatches calls through asIScriptContext, so anything registered with the engine (functions, enums, global
 * properties, object types with their methods and property accessors) becomes callable from Lua automatically.
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

#define NVGT_PLUGIN_INCLUDE
#include "../../src/nvgt_plugin.h"
#include <cstring>
#include <cstdlib>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>
#include "nvgt_lua.h"

static const char* NVGT_OBJECT_MT = "nvgt_object";

// A group of same-named function overloads.
struct func_group {
	std::vector<asIScriptFunction*> overloads;
};

struct global_prop {
	void* address;
	int type_id;
	bool is_const;
};

struct type_cache {
	std::unordered_map<std::string, func_group> methods;
	std::unordered_map<std::string, asUINT> fields; // name -> property index on the asITypeInfo
	bool built = false;
};

class nvgt_lua_bridge {
public:
	asIScriptEngine* engine;
	int string_tid;
	std::unordered_map<std::string, func_group> global_funcs;
	std::unordered_map<std::string, asITypeInfo*> types;
	std::unordered_map<std::string, int> enums;
	std::unordered_map<std::string, global_prop> global_props;
	std::unordered_map<asITypeInfo*, type_cache> type_caches;
	nvgt_lua_bridge(asIScriptEngine* e) : engine(e) {
		string_tid = engine->GetTypeIdByDecl("string");
		for (asUINT i = 0; i < engine->GetGlobalFunctionCount(); i++) {
			asIScriptFunction* f = engine->GetGlobalFunctionByIndex(i);
			if (!f || f->GetNamespace() && strlen(f->GetNamespace())) continue;
			global_funcs[f->GetName()].overloads.push_back(f);
		}
		for (asUINT i = 0; i < engine->GetObjectTypeCount(); i++) {
			asITypeInfo* ti = engine->GetObjectTypeByIndex(i);
			if (!ti || ti->GetNamespace() && strlen(ti->GetNamespace())) continue;
			types[ti->GetName()] = ti;
		}
		for (asUINT i = 0; i < engine->GetEnumCount(); i++) {
			asITypeInfo* ti = engine->GetEnumByIndex(i);
			if (!ti) continue;
			for (asUINT j = 0; j < ti->GetEnumValueCount(); j++) {
				asINT64 value;
				const char* name = ti->GetEnumValueByIndex(j, &value);
				if (name) enums[name] = (int)value;
			}
		}
		for (asUINT i = 0; i < engine->GetGlobalPropertyCount(); i++) {
			const char* name; const char* ns; int tid; bool is_const; void* addr;
			if (engine->GetGlobalPropertyByIndex(i, &name, &ns, &tid, &is_const, nullptr, &addr) < 0) continue;
			if (ns && strlen(ns)) continue;
			global_props[name] = {addr, tid, is_const};
		}
	}
	type_cache& get_type_cache(asITypeInfo* ti) {
		type_cache& tc = type_caches[ti];
		if (tc.built) return tc;
		for (asUINT i = 0; i < ti->GetMethodCount(); i++) {
			asIScriptFunction* m = ti->GetMethodByIndex(i);
			if (m) tc.methods[m->GetName()].overloads.push_back(m);
		}
		for (asUINT i = 0; i < ti->GetPropertyCount(); i++) {
			const char* name;
			if (ti->GetProperty(i, &name) >= 0 && name) tc.fields[name] = i;
		}
		tc.built = true;
		return tc;
	}
};

// Userdata wrapping an AngelScript object (registered type or script class instance).
struct as_object {
	void* ptr;
	asITypeInfo* ti;
	bool owned; // If true, __gc releases the object.
};

static int base_tid(int tid) { return tid & ~(asTYPEID_OBJHANDLE | asTYPEID_HANDLETOCONST); }
// Variable type parameters (?) report a type id of -1 from asIScriptFunction::GetParam.
static bool tid_is_vartype(int tid) { return tid < 0; }

static nvgt_lua_bridge* get_bridge(lua_State* L) {
	lua_getfield(L, LUA_REGISTRYINDEX, "nvgt_bridge");
	nvgt_lua_bridge* b = (nvgt_lua_bridge*)lua_touserdata(L, -1);
	lua_pop(L, 1);
	return b;
}

static as_object* check_as_object(lua_State* L, int idx) {
	return (as_object*)luaL_testudata(L, idx, NVGT_OBJECT_MT);
}

static as_object* push_as_object(lua_State* L, void* ptr, asITypeInfo* ti, bool owned) {
	as_object* ud = (as_object*)lua_newuserdatauv(L, sizeof(as_object), 0);
	ud->ptr = ptr;
	ud->ti = ti;
	ud->owned = owned;
	luaL_setmetatable(L, NVGT_OBJECT_MT);
	return ud;
}

// Classification of a lua value for overload scoring.
enum lua_kind { LK_NIL, LK_BOOL, LK_NUM, LK_STR, LK_TABLE, LK_OBJ, LK_FUNC, LK_OTHER };
static lua_kind classify(lua_State* L, int idx) {
	switch (lua_type(L, idx)) {
		case LUA_TNIL: return LK_NIL;
		case LUA_TBOOLEAN: return LK_BOOL;
		case LUA_TNUMBER: return LK_NUM;
		case LUA_TSTRING: return LK_STR;
		case LUA_TTABLE: return LK_TABLE;
		case LUA_TFUNCTION: return LK_FUNC;
		case LUA_TUSERDATA: return check_as_object(L, idx) ? LK_OBJ : LK_OTHER;
		default: return LK_OTHER;
	}
}

static bool tid_is_primitive(int tid) { return tid >= asTYPEID_BOOL && tid <= asTYPEID_DOUBLE; }
static bool tid_is_integral(int tid) { return tid >= asTYPEID_INT8 && tid <= asTYPEID_UINT64; }

static bool tid_is_enum(asIScriptEngine* engine, int tid) {
	if (tid & asTYPEID_MASK_OBJECT || tid <= asTYPEID_DOUBLE) return false;
	asITypeInfo* ti = engine->GetTypeInfoById(tid);
	return ti && (ti->GetFlags() & asOBJ_ENUM);
}

// Parsed default argument. AngelScript evaluates default args at compile time so when calling through a context we
// must supply every argument ourselves; we handle the simple literals which cover nearly all of NVGT's API.
struct default_val {
	enum { DV_NONE, DV_NUM, DV_BOOL, DV_STR, DV_NULL, DV_PROPCALL } kind = DV_NONE;
	double num = 0;
	bool boolean = false;
	std::string str;
	asIScriptFunction* getter = nullptr; // DV_PROPCALL: virtual global property accessor to evaluate at call time
};

static default_val parse_default(nvgt_lua_bridge* b, const char* text) {
	default_val dv;
	if (!text) return dv;
	std::string s(text);
	// Trim whitespace.
	size_t start = s.find_first_not_of(" \t");
	size_t end = s.find_last_not_of(" \t");
	if (start == std::string::npos) return dv;
	s = s.substr(start, end - start + 1);
	if (s == "true") { dv.kind = default_val::DV_BOOL; dv.boolean = true; return dv; }
	if (s == "false") { dv.kind = default_val::DV_BOOL; dv.boolean = false; return dv; }
	if (s == "null") { dv.kind = default_val::DV_NULL; return dv; }
	if (s.size() >= 2 && (s[0] == '"' && s.back() == '"' || s[0] == '\'' && s.back() == '\'')) {
		dv.kind = default_val::DV_STR;
		dv.str = s.substr(1, s.size() - 2);
		return dv;
	}
	char* endptr = nullptr;
	double v = strtod(s.c_str(), &endptr);
	if (endptr && *endptr == 0) { dv.kind = default_val::DV_NUM; dv.num = v; return dv; }
	// Might be an enum constant or global const like SCREEN_READER_AUTO.
	auto it = b->enums.find(s);
	if (it != b->enums.end()) { dv.kind = default_val::DV_NUM; dv.num = it->second; return dv; }
	auto pit = b->global_props.find(s);
	if (pit != b->global_props.end() && tid_is_primitive(pit->second.type_id)) {
		dv.kind = default_val::DV_NUM;
		int tid = pit->second.type_id;
		void* a = pit->second.address;
		if (tid == asTYPEID_FLOAT) dv.num = *(float*)a;
		else if (tid == asTYPEID_DOUBLE) dv.num = *(double*)a;
		else if (tid == asTYPEID_INT64) dv.num = (double)*(int64_t*)a;
		else if (tid == asTYPEID_UINT64) dv.num = (double)*(uint64_t*)a;
		else if (tid == asTYPEID_BOOL) { dv.kind = default_val::DV_BOOL; dv.boolean = *(char*)a != 0; }
		else dv.num = *(int*)a;
		return dv;
	}
	// Virtual global properties like sound_default_pack resolve to a get_ accessor returning an object handle.
	auto git = b->global_funcs.find("get_" + s);
	if (git != b->global_funcs.end()) {
		for (asIScriptFunction* f : git->second.overloads) {
			int rtid = f->GetReturnTypeId();
			if (f->GetParamCount() == 0 && rtid & asTYPEID_MASK_OBJECT) {
				dv.kind = default_val::DV_PROPCALL;
				dv.getter = f;
				return dv;
			}
		}
	}
	return dv;
}

// Score how well the lua arguments starting at first_arg match a candidate; -1 means incompatible and fills why if given.
static int score_candidate(lua_State* L, nvgt_lua_bridge* b, asIScriptFunction* f, int first_arg, int nargs, std::string* why = nullptr) {
	asUINT pc = f->GetParamCount();
	auto reject = [&](asUINT param, const char* reason) {
		if (why) {
			int tid = 0; asDWORD fl = 0;
			f->GetParam(param, &tid, &fl);
			*why = "parameter " + std::to_string(param + 1) + ": " + reason + " (typeid " + std::to_string(tid) + ", flags " + std::to_string(fl) + ")";
		}
		return -1;
	};
	if ((int)pc < nargs) { if (why) *why = "too many arguments"; return -1; }
	int score = 0;
	for (asUINT i = 0; i < pc; i++) {
		int tid; asDWORD flags; const char* defarg = nullptr;
		f->GetParam(i, &tid, &flags, nullptr, &defarg);
		if ((int)i >= nargs) {
			if (flags & asTM_OUTREF || tid_is_vartype(tid)) continue; // out params and trailing variable type params need no lua argument
			if (!defarg) return reject(i, "missing argument without a default");
			default_val dv = parse_default(b, defarg);
			if (dv.kind == default_val::DV_NONE) return reject(i, (std::string("cannot evaluate default value '") + defarg + "'").c_str());
			continue;
		}
		if (flags & asTM_OUTREF && !(flags & asTM_INREF)) return reject(i, "output parameters cannot take a value; leave them off to receive extra return values");
		lua_kind k = classify(L, first_arg + i);
		asITypeInfo* pti = b->engine->GetTypeInfoById(tid);
		if (tid_is_vartype(tid)) score += 1; // ?& variable type accepts anything
		else if (tid == asTYPEID_BOOL) { if (k == LK_BOOL) score += 2; else return reject(i, "expected boolean"); }
		else if (tid_is_primitive(tid) || tid_is_enum(b->engine, tid)) { if (k == LK_NUM) score += 2; else return reject(i, "expected number"); }
		else if (base_tid(tid) == b->string_tid) { if (k == LK_STR) score += 2; else if (k == LK_NUM) score += 1; else return reject(i, "expected string"); }
		else if (pti && pti->GetFuncdefSignature()) return reject(i, "callbacks cannot be passed from lua yet");
		else if (tid & asTYPEID_MASK_OBJECT) {
			if (k == LK_OBJ) {
				as_object* ud = check_as_object(L, first_arg + i);
				int arg_tid = ud->ti->GetTypeId();
				if (base_tid(arg_tid) == base_tid(tid)) score += 3;
				else if (pti && ud->ti->DerivesFrom(pti)) score += 2;
				else if (pti && ud->ti->Implements(pti)) score += 2;
				else return reject(i, "object type mismatch");
			}
			else if (k == LK_NIL && tid & asTYPEID_OBJHANDLE) score += 1;
			else if (k == LK_TABLE && pti && (strcmp(pti->GetName(), "array") == 0 || strcmp(pti->GetName(), "dictionary") == 0)) score += 1;
			else return reject(i, "expected object");
		}
		else return reject(i, "unsupported parameter type");
	}
	return score;
}

// Storage that must stay alive for the duration of a context execution.
struct arg_scratch {
	asIScriptEngine* engine = nullptr;
	std::deque<std::string> strings;
	std::deque<double> doubles;
	struct out_slot { int param; int tid; asDWORD flags; char data[16]; };
	std::deque<out_slot> outs;
	std::deque<std::string*> out_strings;
	std::vector<std::pair<void*, asITypeInfo*>> temp_objects; // released after the call
	~arg_scratch() {
		for (auto& o : temp_objects) engine->ReleaseScriptObject(o.first, o.second);
		for (std::string* s : out_strings) delete s;
	}
};

// Returns a script context to the pool on scope exit, even when luaL_error unwinds through us with a C++ exception
// (lua is compiled as C++ for exactly that reason).
struct ctx_guard {
	asIScriptEngine* engine;
	asIScriptContext* ctx;
	~ctx_guard() { if (ctx) { ctx->Unprepare(); engine->ReturnContext(ctx); } }
};

static bool lua_to_array(lua_State* L, nvgt_lua_bridge* b, int idx, asITypeInfo* ti, void** out, asITypeInfo** used_ti = nullptr, std::string* err = nullptr);
static bool lua_to_dict(lua_State* L, nvgt_lua_bridge* b, int idx, void** out);
static void push_as_value(lua_State* L, nvgt_lua_bridge* b, void* addr, int tid);

// Set a single argument on the context from the lua stack. Returns false and fills err on failure.
static bool set_context_arg(lua_State* L, nvgt_lua_bridge* b, asIScriptContext* ctx, asIScriptFunction* f, asUINT i, int lua_idx, arg_scratch& scratch, std::string& err) {
	int tid; asDWORD flags; const char* defarg;
	f->GetParam(i, &tid, &flags, nullptr, &defarg);
	asIScriptEngine* engine = b->engine;
	bool has_lua_arg = lua_idx > 0 && !lua_isnone(L, lua_idx);
	// Pure out params get scratch storage regardless of any lua value.
	if (flags & asTM_OUTREF && !(flags & asTM_INREF)) {
		if (base_tid(tid) == b->string_tid) {
			std::string* s = new std::string();
			scratch.out_strings.push_back(s);
			arg_scratch::out_slot slot; slot.param = i; slot.tid = tid; slot.flags = flags;
			*(void**)slot.data = s;
			scratch.outs.push_back(slot);
			ctx->SetArgAddress(i, s);
			return true;
		}
		scratch.outs.push_back({(int)i, tid, flags, {0}});
		ctx->SetArgAddress(i, scratch.outs.back().data);
		return true;
	}
	default_val dv;
	if (!has_lua_arg) {
		dv = parse_default(b, defarg);
		if (dv.kind == default_val::DV_NONE) { err = "missing argument " + std::to_string(i + 1) + " to " + f->GetDeclaration(); return false; }
	}
	if (tid_is_vartype(tid)) { // ?&in variable argument
		if (!has_lua_arg) return true; // leave unset; generic functions see a void argument, which variadic-style functions treat as end of input
		switch (classify(L, lua_idx)) {
			case LK_BOOL: { scratch.doubles.push_back(lua_toboolean(L, lua_idx)); ctx->SetArgVarType(i, &scratch.doubles.back(), asTYPEID_DOUBLE); break; }
			case LK_NUM: { scratch.doubles.push_back(lua_tonumber(L, lua_idx)); ctx->SetArgVarType(i, &scratch.doubles.back(), asTYPEID_DOUBLE); break; }
			case LK_STR: { scratch.strings.emplace_back(lua_tostring(L, lua_idx)); ctx->SetArgVarType(i, &scratch.strings.back(), b->string_tid); break; }
			case LK_OBJ: { as_object* ud = check_as_object(L, lua_idx); ctx->SetArgVarType(i, ud->ti->GetFlags() & asOBJ_REF ? (void*)&ud->ptr : ud->ptr, ud->ti->GetTypeId() | (ud->ti->GetFlags() & asOBJ_REF ? asTYPEID_OBJHANDLE : 0)); break; }
			default: err = "unsupported lua value for variable type parameter"; return false;
		}
		return true;
	}
	if (tid == asTYPEID_BOOL) {
		bool v = has_lua_arg ? lua_toboolean(L, lua_idx) != 0 : dv.boolean;
		if (flags & asTM_INREF) { scratch.doubles.push_back(0); *(char*)&scratch.doubles.back() = v; ctx->SetArgAddress(i, &scratch.doubles.back()); }
		else ctx->SetArgByte(i, v);
		return true;
	}
	if (tid_is_primitive(tid) || tid_is_enum(engine, tid)) {
		double num = has_lua_arg ? lua_tonumber(L, lua_idx) : dv.num;
		if (flags & (asTM_INREF | asTM_OUTREF)) { // in or inout reference to a primitive
			scratch.outs.push_back({(int)i, tid, flags, {0}});
			char* data = scratch.outs.back().data;
			if (tid == asTYPEID_FLOAT) *(float*)data = (float)num;
			else if (tid == asTYPEID_DOUBLE) *(double*)data = num;
			else if (tid == asTYPEID_INT64 || tid == asTYPEID_UINT64) *(int64_t*)data = (int64_t)num;
			else if (tid == asTYPEID_INT8 || tid == asTYPEID_UINT8) *(char*)data = (char)(int64_t)num;
			else if (tid == asTYPEID_INT16 || tid == asTYPEID_UINT16) *(short*)data = (short)(int64_t)num;
			else *(int*)data = (int)(int64_t)num;
			if (!(flags & asTM_OUTREF)) scratch.outs.back().flags = 0; // pure in-ref, don't report back
			ctx->SetArgAddress(i, data);
			return true;
		}
		if (tid == asTYPEID_FLOAT) ctx->SetArgFloat(i, (float)num);
		else if (tid == asTYPEID_DOUBLE) ctx->SetArgDouble(i, num);
		else if (tid == asTYPEID_INT64 || tid == asTYPEID_UINT64) ctx->SetArgQWord(i, (asQWORD)(int64_t)num);
		else if (tid == asTYPEID_INT8 || tid == asTYPEID_UINT8) ctx->SetArgByte(i, (asBYTE)(int64_t)num);
		else if (tid == asTYPEID_INT16 || tid == asTYPEID_UINT16) ctx->SetArgWord(i, (asWORD)(int64_t)num);
		else ctx->SetArgDWord(i, (asDWORD)(int64_t)num);
		return true;
	}
	if (base_tid(tid) == b->string_tid) {
		if (has_lua_arg) {
			size_t len;
			const char* sv = luaL_tolstring(L, lua_idx, &len); // handles numbers too, pushes the result
			scratch.strings.emplace_back(sv, len);
			lua_pop(L, 1);
		}
		else scratch.strings.emplace_back(dv.kind == default_val::DV_STR ? dv.str : "");
		std::string* s = &scratch.strings.back();
		if (flags & (asTM_INREF | asTM_OUTREF)) ctx->SetArgAddress(i, s);
		else ctx->SetArgObject(i, s);
		return true;
	}
	asITypeInfo* pti = engine->GetTypeInfoById(tid);
	if (pti && pti->GetFuncdefSignature()) { err = std::string("passing Lua functions as callbacks is not supported yet (parameter ") + std::to_string(i + 1) + " of " + f->GetDeclaration() + ")"; return false; }
	if (tid & asTYPEID_MASK_OBJECT) {
		if (!has_lua_arg && dv.kind == default_val::DV_PROPCALL) {
			// Evaluate the property accessor now and borrow its result for the call.
			asIScriptContext* c2 = engine->RequestContext();
			if (!c2) { err = "could not acquire script context for default argument"; return false; }
			ctx_guard guard{engine, c2};
			c2->Prepare(dv.getter);
			if (c2->Execute() != asEXECUTION_FINISHED) { err = std::string("failed to evaluate default argument ") + defarg; return false; }
			void* obj = c2->GetReturnObject();
			if (obj) {
				asITypeInfo* rti = engine->GetTypeInfoById(dv.getter->GetReturnTypeId());
				engine->AddRefScriptObject(obj, rti);
				scratch.temp_objects.push_back({obj, rti});
			}
			ctx->SetArgAddress(i, obj);
			return true;
		}
		if (!has_lua_arg || lua_isnil(L, lua_idx)) {
			if (tid & asTYPEID_OBJHANDLE) { ctx->SetArgAddress(i, nullptr); return true; }
			err = "nil passed for non-handle object parameter " + std::to_string(i + 1) + " of " + f->GetDeclaration();
			return false;
		}
		if (lua_istable(L, lua_idx) && pti) {
			void* container = nullptr;
			if (strcmp(pti->GetName(), "array") == 0 && !lua_to_array(L, b, lua_idx, pti, &container)) { err = "cannot convert table to " + std::string(engine->GetTypeDeclaration(tid, true)); return false; }
			if (strcmp(pti->GetName(), "dictionary") == 0 && !lua_to_dict(L, b, lua_idx, &container)) { err = "cannot convert table to dictionary"; return false; }
			if (container) {
				scratch.temp_objects.push_back({container, pti});
				if (tid & asTYPEID_OBJHANDLE) ctx->SetArgAddress(i, container);
				else ctx->SetArgAddress(i, container); // &in reference to the container
				return true;
			}
		}
		as_object* ud = check_as_object(L, lua_idx);
		if (!ud) { err = "expected " + std::string(engine->GetTypeDeclaration(base_tid(tid), true)) + " for parameter " + std::to_string(i + 1) + " of " + f->GetDeclaration(); return false; }
		if (tid & asTYPEID_OBJHANDLE) ctx->SetArgAddress(i, ud->ptr); // borrowed reference, lua userdata keeps it alive during the call
		else if (flags & (asTM_INREF | asTM_OUTREF)) ctx->SetArgAddress(i, ud->ptr);
		else ctx->SetArgObject(i, ud->ptr); // by value, the context copies
		return true;
	}
	err = "unsupported parameter type in " + std::string(f->GetDeclaration());
	return false;
}

// Push the result (and out params) of an executed context onto the lua stack. Returns the number of lua return values.
static int push_context_results(lua_State* L, nvgt_lua_bridge* b, asIScriptContext* ctx, asIScriptFunction* f, arg_scratch& scratch) {
	asIScriptEngine* engine = b->engine;
	int pushed = 0;
	asDWORD rflags = 0;
	int rtid = f->GetReturnTypeId(&rflags);
	if (rtid != asTYPEID_VOID) {
		if (tid_is_primitive(rtid) || tid_is_enum(engine, rtid)) {
			if (rflags & (asTM_INREF | asTM_OUTREF)) push_as_value(L, b, ctx->GetReturnAddress(), rtid);
			else if (rtid == asTYPEID_BOOL) lua_pushboolean(L, ctx->GetReturnByte());
			else if (rtid == asTYPEID_FLOAT) lua_pushnumber(L, ctx->GetReturnFloat());
			else if (rtid == asTYPEID_DOUBLE) lua_pushnumber(L, ctx->GetReturnDouble());
			else if (rtid == asTYPEID_INT64) lua_pushinteger(L, (lua_Integer)ctx->GetReturnQWord());
			else if (rtid == asTYPEID_UINT64) lua_pushinteger(L, (lua_Integer)ctx->GetReturnQWord());
			else if (rtid == asTYPEID_INT8) lua_pushinteger(L, (int8_t)ctx->GetReturnByte());
			else if (rtid == asTYPEID_UINT8) lua_pushinteger(L, ctx->GetReturnByte());
			else if (rtid == asTYPEID_INT16) lua_pushinteger(L, (int16_t)ctx->GetReturnWord());
			else if (rtid == asTYPEID_UINT16) lua_pushinteger(L, ctx->GetReturnWord());
			else if (rtid == asTYPEID_UINT32) lua_pushinteger(L, ctx->GetReturnDWord());
			else lua_pushinteger(L, (int32_t)ctx->GetReturnDWord());
			pushed++;
		}
		else if (base_tid(rtid) == b->string_tid) {
			std::string* s = (std::string*)(rflags & (asTM_INREF | asTM_OUTREF) ? ctx->GetReturnAddress() : ctx->GetReturnObject());
			if (s) lua_pushlstring(L, s->data(), s->size()); else lua_pushnil(L);
			pushed++;
		}
		else if (rtid & asTYPEID_MASK_OBJECT) {
			void* obj = rflags & (asTM_INREF | asTM_OUTREF) ? ctx->GetReturnAddress() : ctx->GetReturnObject();
			asITypeInfo* ti = engine->GetTypeInfoById(rtid);
			if (!obj || !ti) lua_pushnil(L);
			else if (rtid & asTYPEID_OBJHANDLE || ti->GetFlags() & asOBJ_REF) {
				engine->AddRefScriptObject(obj, ti);
				push_as_object(L, obj, ti, true);
			}
			else push_as_object(L, engine->CreateScriptObjectCopy(obj, ti), ti, true); // value type, the context's copy dies on reuse
			pushed++;
		}
		else { lua_pushnil(L); pushed++; }
	}
	// Out params become extra return values.
	for (arg_scratch::out_slot& slot : scratch.outs) {
		if (!(slot.flags & asTM_OUTREF)) continue;
		if (base_tid(slot.tid) == b->string_tid) {
			std::string* s = *(std::string**)slot.data;
			lua_pushlstring(L, s->data(), s->size());
		}
		else push_as_value(L, b, slot.data, slot.tid);
		pushed++;
	}
	return pushed;
}

// Push a primitive/enum value residing at addr.
static void push_as_value(lua_State* L, nvgt_lua_bridge* b, void* addr, int tid) {
	if (!addr) { lua_pushnil(L); return; }
	if (tid == asTYPEID_BOOL) lua_pushboolean(L, *(char*)addr != 0);
	else if (tid == asTYPEID_FLOAT) lua_pushnumber(L, *(float*)addr);
	else if (tid == asTYPEID_DOUBLE) lua_pushnumber(L, *(double*)addr);
	else if (tid == asTYPEID_INT64) lua_pushinteger(L, *(int64_t*)addr);
	else if (tid == asTYPEID_UINT64) lua_pushinteger(L, (lua_Integer)*(uint64_t*)addr);
	else if (tid == asTYPEID_INT8) lua_pushinteger(L, *(int8_t*)addr);
	else if (tid == asTYPEID_UINT8) lua_pushinteger(L, *(uint8_t*)addr);
	else if (tid == asTYPEID_INT16) lua_pushinteger(L, *(int16_t*)addr);
	else if (tid == asTYPEID_UINT16) lua_pushinteger(L, *(uint16_t*)addr);
	else if (tid == asTYPEID_UINT32) lua_pushinteger(L, *(uint32_t*)addr);
	else lua_pushinteger(L, *(int32_t*)addr); // int32 and enums
}

// The core dispatcher: select an overload, marshal arguments, execute, marshal results.
// this_obj is null for global functions and factories take no object either.
static int dispatch(lua_State* L, nvgt_lua_bridge* b, func_group* fg, void* this_obj, int first_arg) {
	int nargs = lua_gettop(L) - first_arg + 1;
	if (nargs < 0) nargs = 0;
	asIScriptFunction* best = nullptr;
	int best_score = -1;
	for (asIScriptFunction* f : fg->overloads) {
		int s = score_candidate(L, b, f, first_arg, nargs);
		if (s > best_score) { best_score = s; best = f; }
	}
	if (!best) {
		std::string err = "no matching overload";
		if (!fg->overloads.empty()) err += std::string(" for ") + fg->overloads[0]->GetName() + "; candidates:";
		for (asIScriptFunction* f : fg->overloads) {
			std::string why;
			score_candidate(L, b, f, first_arg, nargs, &why);
			err += std::string("\n  ") + f->GetDeclaration() + " -- " + why;
		}
		return luaL_error(L, "%s", err.c_str());
	}
	asIScriptEngine* engine = b->engine;
	asIScriptContext* ctx = engine->RequestContext();
	if (!ctx) return luaL_error(L, "could not acquire script context");
	ctx_guard guard{engine, ctx};
	arg_scratch scratch;
	scratch.engine = engine;
	std::string err;
	int r = ctx->Prepare(best);
	if (r < 0) return luaL_error(L, "failed to prepare context for %s", best->GetDeclaration());
	if (this_obj) ctx->SetObject(this_obj);
	for (asUINT i = 0; i < best->GetParamCount(); i++) {
		int lua_idx = (int)i < nargs ? first_arg + i : 0;
		if (!set_context_arg(L, b, ctx, best, i, lua_idx, scratch, err)) return luaL_error(L, "%s", err.c_str());
	}
	r = ctx->Execute();
	if (r != asEXECUTION_FINISHED) {
		std::string msg;
		if (r == asEXECUTION_EXCEPTION) {
			msg = ctx->GetExceptionString();
			asIScriptFunction* ef = ctx->GetExceptionFunction();
			if (ef) msg += std::string(" (in ") + ef->GetDeclaration() + ")";
		}
		else msg = "script execution did not finish normally";
		return luaL_error(L, "%s", msg.c_str());
	}
	return push_context_results(L, b, ctx, best, scratch);
}

// Convert a lua table (sequence) to a script array by creating one through the engine and filling it with its own
// insertLast method, so no compiled-in array implementation is needed. ti is the template instance if known.
static bool lua_to_array(lua_State* L, nvgt_lua_bridge* b, int idx, asITypeInfo* ti, void** out, asITypeInfo** used_ti, std::string* err) {
	asIScriptEngine* engine = b->engine;
	idx = lua_absindex(L, idx);
	lua_Integer n = luaL_len(L, idx);
	if (!ti) { // deduce the element type from the first element
		if (n < 1) ti = engine->GetTypeInfoByDecl("array<string>");
		else {
			lua_geti(L, idx, 1);
			lua_kind k = classify(L, -1);
			as_object* ud = k == LK_OBJ ? check_as_object(L, -1) : nullptr;
			lua_pop(L, 1);
			if (k == LK_NUM) ti = engine->GetTypeInfoByDecl("array<double>");
			else if (k == LK_BOOL) ti = engine->GetTypeInfoByDecl("array<bool>");
			else if (k == LK_STR) ti = engine->GetTypeInfoByDecl("array<string>");
			else if (ud) {
				std::string decl = std::string("array<") + ud->ti->GetName() + (ud->ti->GetFlags() & asOBJ_REF ? "@>" : ">");
				ti = engine->GetTypeInfoByDecl(decl.c_str());
			}
			else { if (err) *err = "cannot deduce array element type from the table's first element"; return false; }
		}
		if (!ti) { if (err) *err = "array type lookup failed"; return false; }
	}
	void* arr = engine->CreateScriptObject(ti);
	if (!arr) { if (err) *err = std::string("CreateScriptObject failed for ") + ti->GetName(); return false; }
	type_cache& tc = b->get_type_cache(ti);
	auto insert = tc.methods.find("insert_last");
	if (insert == tc.methods.end()) insert = tc.methods.find("insertLast");
	if (insert == tc.methods.end()) { engine->ReleaseScriptObject(arr, ti); if (err) *err = std::string("no insert_last method on ") + ti->GetName(); return false; }
	for (lua_Integer i = 1; i <= n; i++) {
		lua_geti(L, idx, i);
		dispatch(L, b, &insert->second, arr, lua_gettop(L)); // raises a lua error on element type mismatch, leaking arr; accepted for this failure path
		lua_pop(L, 1);
	}
	*out = arr;
	if (used_ti) *used_ti = ti;
	return true;
}

// Convert a lua table with string keys to a script dictionary through its set(string, ?&in) method.
static bool lua_to_dict(lua_State* L, nvgt_lua_bridge* b, int idx, void** out) {
	asIScriptEngine* engine = b->engine;
	asITypeInfo* ti = engine->GetTypeInfoByDecl("dictionary");
	if (!ti) return false;
	idx = lua_absindex(L, idx);
	void* dict = engine->CreateScriptObject(ti);
	if (!dict) return false;
	type_cache& tc = b->get_type_cache(ti);
	auto set = tc.methods.find("set");
	if (set == tc.methods.end()) { engine->ReleaseScriptObject(dict, ti); return false; }
	lua_pushnil(L);
	while (lua_next(L, idx)) {
		if (lua_type(L, -2) != LUA_TSTRING) { lua_pop(L, 2); engine->ReleaseScriptObject(dict, ti); return false; }
		lua_pushvalue(L, -2); // key copy
		lua_pushvalue(L, -2); // value copy
		dispatch(L, b, &set->second, dict, lua_gettop(L) - 1);
		lua_pop(L, 3); // both copies and the original value; key stays for lua_next
	}
	*out = dict;
	return true;
}

// lua closures
static int global_func_call(lua_State* L) {
	nvgt_lua_bridge* b = (nvgt_lua_bridge*)lua_touserdata(L, lua_upvalueindex(1));
	func_group* fg = (func_group*)lua_touserdata(L, lua_upvalueindex(2));
	return dispatch(L, b, fg, nullptr, 1);
}

static int method_call(lua_State* L) {
	nvgt_lua_bridge* b = (nvgt_lua_bridge*)lua_touserdata(L, lua_upvalueindex(1));
	func_group* fg = (func_group*)lua_touserdata(L, lua_upvalueindex(2));
	as_object* ud = check_as_object(L, 1);
	if (!ud) return luaL_error(L, "method must be called with ':' on an nvgt object");
	return dispatch(L, b, fg, ud->ptr, 2);
}

// Constructing objects: type tables are callable, nvgt.sound() etc.
static int type_construct(lua_State* L) {
	nvgt_lua_bridge* b = (nvgt_lua_bridge*)lua_touserdata(L, lua_upvalueindex(1));
	asITypeInfo* ti = (asITypeInfo*)lua_touserdata(L, lua_upvalueindex(2));
	asIScriptEngine* engine = b->engine;
	int first_arg = 2; // arg 1 is the type table itself (__call)
	if (ti->GetFlags() & asOBJ_REF) {
		func_group fg;
		for (asUINT i = 0; i < ti->GetFactoryCount(); i++) fg.overloads.push_back(ti->GetFactoryByIndex(i));
		if (fg.overloads.empty()) return luaL_error(L, "type %s cannot be constructed", ti->GetName());
		// dispatch AddRefs the returned handle for the lua wrapper and the context drops the factory's own reference on unprepare, leaving exactly one reference owned by lua.
		return dispatch(L, b, &fg, nullptr, first_arg);
	}
	if (ti->GetFlags() & asOBJ_VALUE) {
		int nargs = lua_gettop(L) - first_arg + 1;
		if (nargs < 0) nargs = 0;
		if (nargs == 0) {
			void* obj = engine->CreateScriptObject(ti);
			if (!obj) return luaL_error(L, "failed to construct %s", ti->GetName());
			push_as_object(L, obj, ti, true);
			return 1;
		}
		// Parameterized construction: find a matching constructor behaviour and run it on raw memory.
		func_group fg;
		for (asUINT i = 0; i < ti->GetBehaviourCount(); i++) {
			asEBehaviours beh;
			asIScriptFunction* bf = ti->GetBehaviourByIndex(i, &beh);
			if (beh == asBEHAVE_CONSTRUCT) fg.overloads.push_back(bf);
		}
		if (fg.overloads.empty()) return luaL_error(L, "type %s has no matching constructor", ti->GetName());
		void* mem = asAllocMem(ti->GetSize());
		if (!mem) return luaL_error(L, "out of memory");
		memset(mem, 0, ti->GetSize());
		// Run the matching constructor behaviour on the raw memory; constructors return void.
		{
			asIScriptFunction* best = nullptr;
			int best_score = -1;
			for (asIScriptFunction* f : fg.overloads) {
				int s = score_candidate(L, b, f, first_arg, nargs);
				if (s > best_score) { best_score = s; best = f; }
			}
			if (!best) { asFreeMem(mem); return luaL_error(L, "no matching constructor for %s", ti->GetName()); }
			asIScriptContext* ctx = engine->RequestContext();
			if (!ctx) { asFreeMem(mem); return luaL_error(L, "could not acquire script context"); }
			ctx_guard guard{engine, ctx};
			arg_scratch scratch;
			scratch.engine = engine;
			std::string err;
			ctx->Prepare(best);
			ctx->SetObject(mem);
			for (asUINT i = 0; i < best->GetParamCount(); i++) {
				int lua_idx = (int)i < nargs ? first_arg + i : 0;
				if (!set_context_arg(L, b, ctx, best, i, lua_idx, scratch, err)) {
					asFreeMem(mem);
					return luaL_error(L, "%s", err.c_str());
				}
			}
			int r = ctx->Execute();
			if (r != asEXECUTION_FINISHED) { asFreeMem(mem); return luaL_error(L, "constructor for %s failed", ti->GetName()); }
			push_as_object(L, mem, ti, true);
		}
		return 1;
	}
	return luaL_error(L, "type %s cannot be constructed from Lua", ti->GetName());
}

static void push_func_closure(lua_State* L, nvgt_lua_bridge* b, func_group* fg, lua_CFunction fn) {
	lua_pushlightuserdata(L, b);
	lua_pushlightuserdata(L, fg);
	lua_pushcclosure(L, fn, 2);
}

// Read/write an exposed member field directly through its offset. Returns pushed count or -1 if unsupported.
static int push_field(lua_State* L, nvgt_lua_bridge* b, as_object* ud, asUINT prop_index) {
	const char* name; int tid; int offset; bool is_ref; int comp_offset; bool comp_indirect;
	if (ud->ti->GetProperty(prop_index, &name, &tid, nullptr, nullptr, &offset, &is_ref, nullptr, &comp_offset, &comp_indirect) < 0) return -1;
	if (comp_offset || comp_indirect || is_ref) return -1;
	char* addr = (char*)ud->ptr + offset;
	if (tid_is_primitive(tid) || tid_is_enum(b->engine, tid)) { push_as_value(L, b, addr, tid); return 1; }
	if (base_tid(tid) == b->string_tid) { std::string* s = (std::string*)addr; lua_pushlstring(L, s->data(), s->size()); return 1; }
	if (tid & asTYPEID_OBJHANDLE) {
		void* obj = *(void**)addr;
		asITypeInfo* fti = b->engine->GetTypeInfoById(tid);
		if (!obj || !fti) { lua_pushnil(L); return 1; }
		b->engine->AddRefScriptObject(obj, fti);
		push_as_object(L, obj, fti, true);
		return 1;
	}
	if (tid & asTYPEID_MASK_OBJECT) {
		asITypeInfo* fti = b->engine->GetTypeInfoById(tid);
		if (!fti) return -1;
		if (fti->GetFlags() & asOBJ_REF) { b->engine->AddRefScriptObject(addr, fti); push_as_object(L, addr, fti, true); }
		else push_as_object(L, b->engine->CreateScriptObjectCopy(addr, fti), fti, true);
		return 1;
	}
	return -1;
}

static bool write_field(lua_State* L, nvgt_lua_bridge* b, as_object* ud, asUINT prop_index, int value_idx) {
	const char* name; int tid; int offset; bool is_ref; int comp_offset; bool comp_indirect;
	if (ud->ti->GetProperty(prop_index, &name, &tid, nullptr, nullptr, &offset, &is_ref, nullptr, &comp_offset, &comp_indirect) < 0) return false;
	if (comp_offset || comp_indirect || is_ref) return false;
	char* addr = (char*)ud->ptr + offset;
	if (tid == asTYPEID_BOOL) { *(char*)addr = lua_toboolean(L, value_idx); return true; }
	if (tid_is_primitive(tid) || tid_is_enum(b->engine, tid)) {
		double num = lua_tonumber(L, value_idx);
		if (tid == asTYPEID_FLOAT) *(float*)addr = (float)num;
		else if (tid == asTYPEID_DOUBLE) *(double*)addr = num;
		else if (tid == asTYPEID_INT64 || tid == asTYPEID_UINT64) *(int64_t*)addr = (int64_t)num;
		else if (tid == asTYPEID_INT8 || tid == asTYPEID_UINT8) *(char*)addr = (char)(int64_t)num;
		else if (tid == asTYPEID_INT16 || tid == asTYPEID_UINT16) *(short*)addr = (short)(int64_t)num;
		else *(int*)addr = (int)(int64_t)num;
		return true;
	}
	if (base_tid(tid) == b->string_tid && lua_isstring(L, value_idx)) {
		size_t len;
		const char* sv = lua_tolstring(L, value_idx, &len);
		*(std::string*)addr = std::string(sv, len);
		return true;
	}
	return false;
}

// __index for nvgt objects: methods, get_X property accessors, direct fields, numeric opIndex.
static int obj_index(lua_State* L) {
	as_object* ud = check_as_object(L, 1);
	nvgt_lua_bridge* b = get_bridge(L);
	if (!ud || !b) return luaL_error(L, "invalid nvgt object");
	type_cache& tc = b->get_type_cache(ud->ti);
	if (lua_type(L, 2) == LUA_TNUMBER) {
		auto it = tc.methods.find("opIndex");
		if (it == tc.methods.end()) return luaL_error(L, "%s does not support indexing", ud->ti->GetName());
		return dispatch(L, b, &it->second, ud->ptr, 2);
	}
	const char* key = luaL_checkstring(L, 2);
	auto it = tc.methods.find(key);
	if (it != tc.methods.end()) {
		push_func_closure(L, b, &it->second, method_call);
		return 1;
	}
	auto git = tc.methods.find(std::string("get_") + key);
	if (git != tc.methods.end()) {
		lua_settop(L, 1); // getter takes no arguments
		return dispatch(L, b, &git->second, ud->ptr, 3);
	}
	auto fit = tc.fields.find(key);
	if (fit != tc.fields.end()) {
		int n = push_field(L, b, ud, fit->second);
		if (n >= 0) return n;
	}
	lua_pushnil(L);
	return 1;
}

static int obj_newindex(lua_State* L) {
	as_object* ud = check_as_object(L, 1);
	nvgt_lua_bridge* b = get_bridge(L);
	if (!ud || !b) return luaL_error(L, "invalid nvgt object");
	type_cache& tc = b->get_type_cache(ud->ti);
	const char* key = luaL_checkstring(L, 2);
	auto sit = tc.methods.find(std::string("set_") + key);
	if (sit != tc.methods.end()) {
		dispatch(L, b, &sit->second, ud->ptr, 3);
		return 0;
	}
	auto fit = tc.fields.find(key);
	if (fit != tc.fields.end() && write_field(L, b, ud, fit->second, 3)) return 0;
	return luaL_error(L, "cannot set '%s' on %s", key, ud->ti->GetName());
}

static int obj_gc(lua_State* L) {
	as_object* ud = check_as_object(L, 1);
	nvgt_lua_bridge* b = get_bridge(L);
	if (ud && ud->owned && ud->ptr && b) b->engine->ReleaseScriptObject(ud->ptr, ud->ti);
	if (ud) ud->ptr = nullptr;
	return 0;
}

static int obj_tostring(lua_State* L) {
	as_object* ud = check_as_object(L, 1);
	if (!ud) return 0;
	lua_pushfstring(L, "%s: %p", ud->ti->GetName(), ud->ptr);
	return 1;
}

// Binary operator metamethods routed to AngelScript's operator methods.
static int obj_binop(lua_State* L, const char* method, const char* method_r) {
	nvgt_lua_bridge* b = get_bridge(L);
	as_object* lhs = check_as_object(L, 1);
	if (lhs) {
		type_cache& tc = b->get_type_cache(lhs->ti);
		auto it = tc.methods.find(method);
		if (it != tc.methods.end()) {
			lua_remove(L, 1); // args start at the rhs
			return dispatch(L, b, &it->second, lhs->ptr, 1);
		}
	}
	as_object* rhs = check_as_object(L, 2);
	if (rhs && method_r) {
		type_cache& tc = b->get_type_cache(rhs->ti);
		auto it = tc.methods.find(method_r);
		if (it != tc.methods.end()) {
			lua_remove(L, 2);
			return dispatch(L, b, &it->second, rhs->ptr, 1);
		}
	}
	return luaL_error(L, "operator %s not supported between these values", method);
}

static int obj_add(lua_State* L) { return obj_binop(L, "opAdd", "opAdd_r"); }
static int obj_sub(lua_State* L) { return obj_binop(L, "opSub", "opSub_r"); }
static int obj_mul(lua_State* L) { return obj_binop(L, "opMul", "opMul_r"); }
static int obj_div(lua_State* L) { return obj_binop(L, "opDiv", "opDiv_r"); }
static int obj_mod(lua_State* L) { return obj_binop(L, "opMod", "opMod_r"); }

static int obj_unm(lua_State* L) {
	nvgt_lua_bridge* b = get_bridge(L);
	as_object* ud = check_as_object(L, 1);
	if (!ud) return luaL_error(L, "invalid operand");
	type_cache& tc = b->get_type_cache(ud->ti);
	auto it = tc.methods.find("opNeg");
	if (it == tc.methods.end()) return luaL_error(L, "%s does not support unary minus", ud->ti->GetName());
	lua_settop(L, 1);
	return dispatch(L, b, &it->second, ud->ptr, 2);
}

static int obj_eq(lua_State* L) {
	nvgt_lua_bridge* b = get_bridge(L);
	as_object* lhs = check_as_object(L, 1);
	as_object* rhs = check_as_object(L, 2);
	if (!lhs || !rhs) { lua_pushboolean(L, false); return 1; }
	if (lhs->ptr == rhs->ptr) { lua_pushboolean(L, true); return 1; }
	type_cache& tc = b->get_type_cache(lhs->ti);
	auto it = tc.methods.find("opEquals");
	if (it != tc.methods.end()) {
		lua_remove(L, 1);
		return dispatch(L, b, &it->second, lhs->ptr, 1);
	}
	it = tc.methods.find("opCmp");
	if (it != tc.methods.end()) {
		lua_remove(L, 1);
		int n = dispatch(L, b, &it->second, lhs->ptr, 1);
		if (n == 1) { lua_pushboolean(L, lua_tointeger(L, -1) == 0); return 1; }
	}
	lua_pushboolean(L, false);
	return 1;
}

static int obj_cmp(lua_State* L, bool less_equal) {
	nvgt_lua_bridge* b = get_bridge(L);
	as_object* lhs = check_as_object(L, 1);
	if (!lhs) return luaL_error(L, "invalid comparison operand");
	type_cache& tc = b->get_type_cache(lhs->ti);
	auto it = tc.methods.find("opCmp");
	if (it == tc.methods.end()) return luaL_error(L, "%s does not support comparison", lhs->ti->GetName());
	lua_remove(L, 1);
	int n = dispatch(L, b, &it->second, lhs->ptr, 1);
	if (n != 1) return luaL_error(L, "opCmp failed");
	lua_Integer r = lua_tointeger(L, -1);
	lua_pushboolean(L, less_equal ? r <= 0 : r < 0);
	return 1;
}

static int obj_lt(lua_State* L) { return obj_cmp(L, false); }
static int obj_le(lua_State* L) { return obj_cmp(L, true); }

static int obj_len(lua_State* L) {
	nvgt_lua_bridge* b = get_bridge(L);
	as_object* ud = check_as_object(L, 1);
	if (!ud) return luaL_error(L, "invalid operand");
	type_cache& tc = b->get_type_cache(ud->ti);
	auto it = tc.methods.find("length");
	if (it == tc.methods.end()) it = tc.methods.find("get_length");
	if (it == tc.methods.end()) return luaL_error(L, "%s has no length", ud->ti->GetName());
	lua_settop(L, 1);
	return dispatch(L, b, &it->second, ud->ptr, 2);
}

// __index for the nvgt table: functions, types, enums, global properties. Immutable hits are cached in the table.
static int nvgt_index(lua_State* L) {
	nvgt_lua_bridge* b = (nvgt_lua_bridge*)lua_touserdata(L, lua_upvalueindex(1));
	const char* key = luaL_checkstring(L, 2);
	auto fit = b->global_funcs.find(key);
	if (fit != b->global_funcs.end()) {
		push_func_closure(L, b, &fit->second, global_func_call);
		lua_pushvalue(L, 2);
		lua_pushvalue(L, -2);
		lua_rawset(L, 1); // cache
		return 1;
	}
	auto tit = b->types.find(key);
	if (tit != b->types.end()) {
		lua_newtable(L); // callable type table
		lua_newtable(L); // its metatable
		lua_pushlightuserdata(L, b);
		lua_pushlightuserdata(L, tit->second);
		lua_pushcclosure(L, type_construct, 2);
		lua_setfield(L, -2, "__call");
		lua_setmetatable(L, -2);
		lua_pushvalue(L, 2);
		lua_pushvalue(L, -2);
		lua_rawset(L, 1); // cache
		return 1;
	}
	auto eit = b->enums.find(key);
	if (eit != b->enums.end()) {
		lua_pushinteger(L, eit->second);
		lua_pushvalue(L, 2);
		lua_pushvalue(L, -2);
		lua_rawset(L, 1); // cache
		return 1;
	}
	auto pit = b->global_props.find(key);
	if (pit != b->global_props.end()) {
		global_prop& gp = pit->second;
		if (tid_is_primitive(gp.type_id) || tid_is_enum(b->engine, gp.type_id)) { push_as_value(L, b, gp.address, gp.type_id); return 1; }
		if (base_tid(gp.type_id) == b->string_tid) { std::string* s = (std::string*)gp.address; lua_pushlstring(L, s->data(), s->size()); return 1; }
		if (gp.type_id & asTYPEID_OBJHANDLE) {
			void* obj = *(void**)gp.address;
			asITypeInfo* ti = b->engine->GetTypeInfoById(gp.type_id);
			if (!obj || !ti) { lua_pushnil(L); return 1; }
			b->engine->AddRefScriptObject(obj, ti);
			push_as_object(L, obj, ti, true);
			return 1;
		}
		if (gp.type_id & asTYPEID_MASK_OBJECT) {
			asITypeInfo* ti = b->engine->GetTypeInfoById(gp.type_id);
			if (ti && ti->GetFlags() & asOBJ_REF) { b->engine->AddRefScriptObject(gp.address, ti); push_as_object(L, gp.address, ti, true); return 1; }
			if (ti) { push_as_object(L, b->engine->CreateScriptObjectCopy(gp.address, ti), ti, true); return 1; }
		}
		lua_pushnil(L);
		return 1;
	}
	lua_pushnil(L);
	return 1;
}

static int nvgt_newindex(lua_State* L) {
	nvgt_lua_bridge* b = (nvgt_lua_bridge*)lua_touserdata(L, lua_upvalueindex(1));
	const char* key = luaL_checkstring(L, 2);
	auto pit = b->global_props.find(key);
	if (pit == b->global_props.end() || pit->second.is_const) return luaL_error(L, "cannot assign '%s'; only writable nvgt global properties may be assigned", key);
	global_prop& gp = pit->second;
	void* addr = gp.address;
	int tid = gp.type_id;
	if (tid == asTYPEID_BOOL) { *(char*)addr = lua_toboolean(L, 3); return 0; }
	if (tid_is_primitive(tid) || tid_is_enum(b->engine, tid)) {
		double num = lua_tonumber(L, 3);
		if (tid == asTYPEID_FLOAT) *(float*)addr = (float)num;
		else if (tid == asTYPEID_DOUBLE) *(double*)addr = num;
		else if (tid == asTYPEID_INT64 || tid == asTYPEID_UINT64) *(int64_t*)addr = (int64_t)num;
		else if (tid == asTYPEID_INT8 || tid == asTYPEID_UINT8) *(char*)addr = (char)(int64_t)num;
		else if (tid == asTYPEID_INT16 || tid == asTYPEID_UINT16) *(short*)addr = (short)(int64_t)num;
		else *(int*)addr = (int)(int64_t)num;
		return 0;
	}
	if (base_tid(tid) == b->string_tid && lua_isstring(L, 3)) {
		size_t len;
		const char* sv = lua_tolstring(L, 3, &len);
		*(std::string*)addr = std::string(sv, len);
		return 0;
	}
	return luaL_error(L, "unsupported assignment to nvgt global '%s'", key);
}

// nvgt.toarray(table, "element declaration") -> array object
static int nvgt_toarray(lua_State* L) {
	nvgt_lua_bridge* b = (nvgt_lua_bridge*)lua_touserdata(L, lua_upvalueindex(1));
	luaL_checktype(L, 1, LUA_TTABLE);
	asITypeInfo* ti = nullptr;
	if (lua_isstring(L, 2)) {
		std::string decl = std::string("array<") + lua_tostring(L, 2) + ">";
		ti = b->engine->GetTypeInfoByDecl(decl.c_str());
		if (!ti) return luaL_error(L, "unknown array element type '%s'", lua_tostring(L, 2));
	}
	void* arr = nullptr;
	asITypeInfo* used = nullptr;
	std::string err;
	if (!lua_to_array(L, b, 1, ti, &arr, &used, &err)) return luaL_error(L, "cannot convert table to array: %s", err.c_str());
	push_as_object(L, arr, used, true);
	return 1;
}

// nvgt.totable(array) -> lua table, reading elements through the array's own length and opIndex methods.
static int nvgt_totable(lua_State* L) {
	nvgt_lua_bridge* b = (nvgt_lua_bridge*)lua_touserdata(L, lua_upvalueindex(1));
	as_object* ud = check_as_object(L, 1);
	if (!ud || strcmp(ud->ti->GetName(), "array") != 0) return luaL_error(L, "expected an nvgt array (for dictionaries, use get_keys and index individually)");
	type_cache& tc = b->get_type_cache(ud->ti);
	auto len_m = tc.methods.find("length");
	auto idx_m = tc.methods.find("opIndex");
	if (len_m == tc.methods.end() || idx_m == tc.methods.end()) return luaL_error(L, "array type lacks length/opIndex");
	lua_settop(L, 1);
	dispatch(L, b, &len_m->second, ud->ptr, 2);
	lua_Integer n = lua_tointeger(L, -1);
	lua_pop(L, 1);
	lua_createtable(L, (int)n, 0);
	for (lua_Integer i = 0; i < n; i++) {
		lua_pushinteger(L, i);
		dispatch(L, b, &idx_m->second, ud->ptr, lua_gettop(L)); // pushes the element
		lua_remove(L, -2); // the index argument
		lua_rawseti(L, -2, i + 1);
	}
	return 1;
}

// nvgt.todict(table) -> dictionary object
static int nvgt_todict(lua_State* L) {
	nvgt_lua_bridge* b = (nvgt_lua_bridge*)lua_touserdata(L, lua_upvalueindex(1));
	luaL_checktype(L, 1, LUA_TTABLE);
	void* dict = nullptr;
	if (!lua_to_dict(L, b, 1, &dict)) return luaL_error(L, "cannot convert table to dictionary; keys must be strings");
	push_as_object(L, dict, b->engine->GetTypeInfoByDecl("dictionary"), true);
	return 1;
}

nvgt_lua_bridge* nvgt_lua_bridge_create(lua_State* L, asIScriptEngine* engine, bool as_globals) {
	nvgt_lua_bridge* b = new nvgt_lua_bridge(engine);
	lua_pushlightuserdata(L, b);
	lua_setfield(L, LUA_REGISTRYINDEX, "nvgt_bridge");
	// Metatable for wrapped objects.
	if (luaL_newmetatable(L, NVGT_OBJECT_MT)) {
		lua_pushcfunction(L, obj_index); lua_setfield(L, -2, "__index");
		lua_pushcfunction(L, obj_newindex); lua_setfield(L, -2, "__newindex");
		lua_pushcfunction(L, obj_gc); lua_setfield(L, -2, "__gc");
		lua_pushcfunction(L, obj_tostring); lua_setfield(L, -2, "__tostring");
		lua_pushcfunction(L, obj_add); lua_setfield(L, -2, "__add");
		lua_pushcfunction(L, obj_sub); lua_setfield(L, -2, "__sub");
		lua_pushcfunction(L, obj_mul); lua_setfield(L, -2, "__mul");
		lua_pushcfunction(L, obj_div); lua_setfield(L, -2, "__div");
		lua_pushcfunction(L, obj_mod); lua_setfield(L, -2, "__mod");
		lua_pushcfunction(L, obj_unm); lua_setfield(L, -2, "__unm");
		lua_pushcfunction(L, obj_eq); lua_setfield(L, -2, "__eq");
		lua_pushcfunction(L, obj_lt); lua_setfield(L, -2, "__lt");
		lua_pushcfunction(L, obj_le); lua_setfield(L, -2, "__le");
		lua_pushcfunction(L, obj_len); lua_setfield(L, -2, "__len");
	}
	lua_pop(L, 1);
	// The nvgt table.
	lua_newtable(L);
	lua_newtable(L); // metatable
	lua_pushlightuserdata(L, b);
	lua_pushcclosure(L, nvgt_index, 1);
	lua_setfield(L, -2, "__index");
	lua_pushlightuserdata(L, b);
	lua_pushcclosure(L, nvgt_newindex, 1);
	lua_setfield(L, -2, "__newindex");
	lua_setmetatable(L, -2);
	// Conversion helpers live directly in the table; rawset so the metatable's __newindex guard is bypassed.
	lua_pushstring(L, "toarray");
	lua_pushlightuserdata(L, b);
	lua_pushcclosure(L, nvgt_toarray, 1);
	lua_rawset(L, -3);
	lua_pushstring(L, "totable");
	lua_pushlightuserdata(L, b);
	lua_pushcclosure(L, nvgt_totable, 1);
	lua_rawset(L, -3);
	lua_pushstring(L, "todict");
	lua_pushlightuserdata(L, b);
	lua_pushcclosure(L, nvgt_todict, 1);
	lua_rawset(L, -3);
	if (as_globals) {
		// Fall back to the nvgt table for unknown globals, so speak("hi") works directly. Lua's own globals win.
		lua_pushglobaltable(L);
		lua_newtable(L);
		lua_pushvalue(L, -3); // the nvgt table
		lua_setfield(L, -2, "__index");
		lua_setmetatable(L, -2);
		lua_pop(L, 1);
	}
	lua_setglobal(L, "nvgt");
	return b;
}

void nvgt_lua_bridge_destroy(nvgt_lua_bridge* bridge) {
	delete bridge;
}
