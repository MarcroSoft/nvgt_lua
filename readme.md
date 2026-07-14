# lua plugin
This plugin embeds the Lua 5.4 scripting language into NVGT, letting you write most or all of your game in Lua while a small AngelScript loader boots it. Instead of hand-binding each function, the plugin walks the AngelScript engine's registration tables at runtime and exposes everything it finds — global functions, enums, global properties and object types with their methods, properties and operators — so nearly the entire NVGT API is callable from Lua automatically, including anything registered by other loaded plugins.

## usage
```angelscript
#pragma plugin lua

void main() {
	lua_state@ L = lua_state();
	L.open_libraries(); // standard Lua libraries
	L.expose_nvgt(); // expose NVGT's API; pass false to only create the nvgt table without the global fallback
	if (!L.exec_file("game.lua")) alert("error", L.last_error);
}
```

On the Lua side, NVGT's API lives in the `nvgt` table. When `expose_nvgt(true)` is used (the default), unknown globals also fall back to it, so both `nvgt.screen_reader_speak("hello")` and `screen_reader_speak("hello")` work. Lua's own globals always win over NVGT names, so Lua's `print`, `string` and `math` remain untouched; reach the NVGT equivalents through `nvgt.print`, `nvgt.string` and so on.

```lua
screen_reader_speak("Hello there!")
local s = nvgt.sound()
s:load("music.ogg")
s:play_looped()
local v = nvgt.vector(1, 2, 3) + nvgt.vector(4, 5, 6)
println(v:length())
wait(50)
```

Note that only what is registered with the engine itself is visible. Functions and classes written in AngelScript includes such as speech.nvgt (`speak` and friends) live in the compiled script module, not the engine, so they cannot be called from Lua; use the underlying registered functions like `screen_reader_speak` and `tts_voice` instead.

## lua_state members
* `void open_libraries()`: opens Lua's standard libraries.
* `void expose_nvgt(bool as_globals = true)`: installs the NVGT bridge.
* `bool exec(const string&in code, const string&in chunkname = "")`: run a string of Lua code.
* `bool exec_file(const string&in filename)`: run a Lua file (path is relative to the working directory).
* `bool call(const string&in function_name)`: call a global Lua function without arguments.
* `string last_error`: message of the last failed exec/exec_file/call.
* `set_global_number/set_global_string/set_global_bool(const string&in name, value)`: set a Lua global.
* `get_global_number/get_global_string/get_global_bool(const string&in name)`: read a Lua global.
* Global function `string lua_version()` returns the embedded Lua release.

## conversions and helpers
* Numbers, strings, booleans and enums convert automatically in both directions; enum constants are available by name, e.g. `nvgt.KEY_RETURN`.
* NVGT objects appear in Lua as userdata: call methods with `:`, access properties and fields with `.`, index with `[]` (0-based, as in AngelScript), and use `#`, `+`, `-`, `*`, `/`, `%`, unary minus and comparisons where the type implements the matching AngelScript operator methods.
* Construct objects by calling the type: `nvgt.sound()`, `nvgt.vector(1, 2, 3)`.
* `nvgt.toarray(table, "int")` converts a Lua sequence to an NVGT array (element type deduced from the first element when omitted); `nvgt.totable(array)` converts back; `nvgt.todict(table)` builds a dictionary from a string-keyed table. Plain Lua tables are also converted automatically when passed where a function expects an array or dictionary.
* Functions with trailing `&out` parameters return those as extra Lua return values.

## limitations
* Lua functions cannot yet be passed where NVGT expects a callback (funcdef); such parameters are rejected with a clear error.
* A lua_state is not thread safe; create one per thread if needed.
* Functions and classes declared in .nvgt files (including standard includes like speech.nvgt) are not visible to Lua, only what is registered with the engine.
* Reading dictionary values from Lua requires going through functions that return concrete types; `nvgt.totable` only accepts arrays.

## building
The Lua 5.4 sources are vendored under lua54 and compiled into the plugin as a single C++ translation unit (lua54.cpp), so lua errors unwind safely through the bridge with C++ exceptions and no external dependency is needed. Lua is distributed under the MIT license, see doc/OSL/MIT/lua.txt.
