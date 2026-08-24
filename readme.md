# lua plugin
This plugin embeds the Lua 5.5 scripting language into NVGT, letting you write most or all of your game in Lua while a small AngelScript loader boots it. Instead of hand-binding each function, the plugin walks the AngelScript engine's registration tables at runtime and exposes everything it finds — global functions, enums, global properties and object types with their methods, properties and operators — so nearly the entire NVGT API is callable from Lua automatically, including anything registered by other loaded plugins.

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
* `void open_libraries()`: opens Lua's standard libraries. `require` is game-local: it searches `package.preload` and the working directory (`./?.lua`, `./?.ois`, `./?/init.lua`, `./?/init.ois`) only — no machine-wide Lua installs, and C module loading is disabled (binary modules built against a standalone Lua wouldn't be ABI compatible with the embedded interpreter). `.ois` files are searched alongside `.lua` ones and loaded as ordinary Lua chunks, so a module can be named `foo.ois` just as well as `foo.lua`. Extend `package.path` from Lua if you need more.
* `void expose_nvgt(bool as_globals = true)`: installs the NVGT bridge.
* `bool exec(const string&in code, const string&in chunkname = "")`: run a string of Lua code.
* `bool exec_file(const string&in filename)`: run a Lua file (path is relative to the working directory).
* `bool call(const string&in function_name)`: call a global Lua function without arguments.
* `string last_error`: message of the last failed exec/exec_file/call.
* `lua_status last_error_code`: Lua status of the last failed exec/exec_file/call — LUA_OK, LUA_ERRRUN, LUA_ERRSYNTAX, LUA_ERRMEM, LUA_ERRERR or LUA_ERRFILE. A syntax or file error means the code never started running; a runtime error means it failed partway through, so globals may have been modified. call reports a missing function as LUA_ERRRUN.
* `set_global_number/set_global_string/set_global_bool(const string&in name, value)`: set a Lua global.
* `get_global_number/get_global_string/get_global_bool(const string&in name)`: read a Lua global.
* `bool set_global(const string&in name, const ?&in value)`: store any NVGT value in a Lua global — numbers, strings, enums, objects and handles. Requires expose_nvgt. Reference types are shared with Lua; value types are copied. Function handles (funcdefs) become directly callable from Lua — `handler@ h = @my_func; L.set_global("boom", @h);` lets Lua call `boom(2, 3)`, and delegates like `handler(obj.method)` work too.
* `bool get_global(const string&in name, ?&out value)`: read a Lua global into an AngelScript variable of matching type, e.g. `sound@ s; L.get_global("music", @s);`. Returns false and sets last_error on a type mismatch; a nil global reads back as a null handle. A plain Lua table global reads directly into an `array<T>@` (element type taken from the handle) or `dictionary@`, converting the same way tables do when passed to functions.
* Global function `string lua_version()` returns the embedded Lua release.

## conversions and helpers
* Numbers, strings, booleans and enums convert automatically in both directions; enum constants are available by name, e.g. `nvgt.KEY_RETURN`.
* Wrapped NVGT containers are 0-based from Lua, as they are in AngelScript: `arr[0]` is the first element, and `for i = 0, #arr - 1 do` walks the whole thing. This keeps `[]` agreeing with the methods and functions that take or return an index — `insert_at`, `remove_at`, `find`, `sound_output_device`, `tts_voice:set_voice` — all of which count from 0 and are handed through unchanged. Reading or assigning out of range raises the container's own exception rather than reading back `nil`, so `ipairs` does not walk an NVGT array; convert with `nvgt.totable(arr)` first if you want a plain 1-based Lua table. `arr[i] = v` assigns through the element reference (primitives, enums and strings); NVGT arrays don't grow on assignment, use `arr.insert_last(v)`.
* NVGT objects appear in Lua as userdata: call methods with `:`, access properties and fields with `.`, index with `[]` (0-based, like AngelScript), and use `#`, `+`, `-`, `*`, `/`, `%`, unary minus and comparisons where the type implements the matching AngelScript operator methods.
* Construct objects by calling the type: `nvgt.sound()`, `nvgt.vector(1, 2, 3)`.
* `nvgt.toarray(table, "int")` converts a Lua sequence to an NVGT array (element type deduced from the first element when omitted); `nvgt.totable(array)` converts back; `nvgt.todict(table)` builds a dictionary from a string-keyed table. Plain Lua tables are also converted automatically when passed where a function expects an array or dictionary.
* `nvgt.api_names()` returns an array of every name the bridge can resolve (functions, types, enum constants and global properties). The nvgt table fills lazily so pairs() cannot enumerate it; use this to whitelist NVGT's API in tools like luacheck.
* Functions with trailing `&out` parameters return those as extra Lua return values.
* Sound and mixer `pan` and `volume` behave exactly as they do in AngelScript, with no scale translation: on the default engine (which sets `PERCENTAGE_ATTRIBUTES`) both are BGT-style db. If you would rather work in raw linear values, convert explicitly with the `pan_linear_to_db` / `pan_db_to_linear` and `volume_linear_to_db` / `volume_db_to_linear` global functions, e.g. `snd.pan = pan_linear_to_db(0.5)` to place a sound halfway to the right, or `local amp = volume_db_to_linear(snd.volume)`.
* Objects cross the boundary in both directions through `set_global`/`get_global`: a sound created in Lua can be fetched into a `sound@` in AngelScript and vice versa, with both sides holding references to the same object.

## limitations
* Lua functions cannot yet be passed where NVGT expects a callback (funcdef); such parameters are rejected with a clear error.
* A lua_state is not thread safe; create one per thread if needed.
* Functions and classes declared in .nvgt files (including standard includes like speech.nvgt) are not visible to Lua, only what is registered with the engine.
* Reading dictionary values from Lua requires going through functions that return concrete types; `nvgt.totable` only accepts arrays.

## building
The plugin is developed and distributed independently of NVGT, but builds inside NVGT's build system. Clone this repository to `plugin/lua` inside a checkout of the [NVGT source tree](https://github.com/MarcroSoft/nvgt), then from the NVGT root run:

```bash
scons plugins=lua release/lib/lua.dll
```

replacing the target with `release/lib/liblua.so` on Linux or `release/lib/liblua.dylib` on macOS. For Android, set `ANDROID_NDK_HOME` and run `scons target=android plugins=lua release/lib_android/lua.so`; for iOS (which cannot load shared plugins) build the static library into the stubs with `scons target=ios plugins=lua static_lua_plugin=1`. Building all of NVGT with a plain `scons` also builds the plugin when it is present at plugin/lua. The .github/workflows/build.yml workflow in this repository builds the shared plugin for Windows, Linux, macOS and Android.

The Lua 5.5 sources are vendored under lua55 and compiled into the plugin as a single C++ translation unit (lua55.cpp), so lua errors unwind safely through the bridge with C++ exceptions and no external dependency is needed. Lua is distributed under the MIT license, see license.md.
