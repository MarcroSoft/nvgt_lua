-- Oidua's API on nvgt_lua: its array and dictionary, and its names for what
-- NVGT already has.
--
-- Oidua gives a script two container objects carried over from BGT: array and
-- dictionary. NVGT has containers by those names, but they are AngelScript
-- templates - typed, constructed with a subtype, and with no way to hold nil -
-- so nvgt.array is not a stand-in. Calling Oidua's array() through the bridge
-- fails outright:
--
--   no matching overload for $beh3; candidates: T[]@ array(int&in) ...
--
-- They are reimplemented here in plain Lua, following Oidua's documented
-- behaviour rather than AngelScript's. Drop this file next to your game's Lua
-- sources, where nvgt_lua's require can find it:
--
--   local oidua = require("oidua")
--   oidua.install()   -- defines array, dictionary and Oidua's names as globals
--
-- Arrays count from 0, as they do in Oidua and BGT, and an index outside the
-- array raises rather than reading back nil. Oidua's own tests/colltest.ois is
-- the contract: 87 of its 89 checks pass here, the two failures being
-- type(a) == "userdata", which no pure Lua implementation can satisfy.

local M = {}

-- Element storage lives beside the object rather than in it, so that every key
-- the script touches reaches __index and __newindex and can be checked.
local arrays = setmetatable({}, {__mode = "k"})
local dicts = setmetatable({}, {__mode = "k"})

-- ---- array ----

local array_methods = {}

local function slot(state, position)
	return position + 1 -- logical 0 is storage 1; the rest of the file goes through here
end

local function check_range(state, position, what)
	if math.type(position) ~= "integer" then
		error("Array index out of range. Elements run from 0 to length minus one.", 3)
	end
	if position < 0 or position >= state.n then
		error("Array index out of range. Elements run from 0 to length minus one.", 3)
	end
end

function array_methods.length(self)
	return arrays[self].n
end

function array_methods.insert_last(self, value)
	local state = arrays[self]
	state.n = state.n + 1
	state[slot(state, state.n - 1)] = value
end

function array_methods.remove_last(self)
	local state = arrays[self]
	if state.n == 0 then return end -- harmless, as in Oidua
	state[slot(state, state.n - 1)] = nil
	state.n = state.n - 1
end

function array_methods.insert_at(self, position, value)
	local state = arrays[self]
	if math.type(position) ~= "integer" or position < 0 or position > state.n then
		error("Array index out of range. insert_at accepts 0 up to the array's length.", 2)
	end
	for i = state.n - 1, position, -1 do
		state[slot(state, i + 1)] = state[slot(state, i)]
	end
	state[slot(state, position)] = value
	state.n = state.n + 1
end

function array_methods.remove_at(self, position)
	local state = arrays[self]
	check_range(state, position)
	for i = position, state.n - 2 do
		state[slot(state, i)] = state[slot(state, i + 1)]
	end
	state[slot(state, state.n - 1)] = nil
	state.n = state.n - 1
end

function array_methods.resize(self, length)
	local state = arrays[self]
	if math.type(length) ~= "integer" or length < 0 then
		error("An array cannot have a negative length.", 2)
	end
	for i = length, state.n - 1 do -- dropped elements, if it is shrinking
		state[slot(state, i)] = nil
	end
	state.n = length -- growing needs nothing: the new slots are already nil
end

function array_methods.find(self, ...)
	local state = arrays[self]
	local count = select("#", ...)
	local start, value
	if count >= 2 then
		start, value = select(1, ...), select(2, ...)
	else
		start, value = 0, select(1, ...)
	end
	if start < 0 then start = 0 end
	for i = start, state.n - 1 do
		if state[slot(state, i)] == value then return i end
	end
	return -1
end

function array_methods.reverse(self)
	local state = arrays[self]
	local low, high = 0, state.n - 1
	while low < high do
		state[slot(state, low)], state[slot(state, high)] = state[slot(state, high)], state[slot(state, low)]
		low, high = low + 1, high - 1
	end
end

-- Both sorts work on a copied run so that table.sort never meets the nil in a
-- part of the array it was not asked about.
local function sort_run(self, start, count, compare)
	local state = arrays[self]
	if start == nil then start, count = 0, state.n end
	if count == nil then count = state.n - start end
	if start < 0 or count < 0 or start + count > state.n then
		error("Array index out of range. The run to sort must lie within the array.", 3)
	end
	local run = {}
	for i = 0, count - 1 do
		run[i + 1] = state[slot(state, start + i)]
	end
	table.sort(run, compare)
	for i = 0, count - 1 do
		state[slot(state, start + i)] = run[i + 1]
	end
end

function array_methods.sort_ascending(self, start, count)
	sort_run(self, start, count)
end

function array_methods.sort_descending(self, start, count)
	sort_run(self, start, count, function(a, b) return b < a end)
end

local array_mt = {
	__name = "array",
	__index = function(self, key)
		if type(key) == "number" then
			local state = arrays[self]
			check_range(state, key)
			return state[slot(state, key)]
		end
		return array_methods[key]
	end,
	__newindex = function(self, key, value)
		if type(key) ~= "number" then
			error("An array holds elements by number; use resize or insert_last to add one.", 2)
		end
		local state = arrays[self]
		check_range(state, key)
		state[slot(state, key)] = value
	end,
	__len = function(self) return arrays[self].n end,
	__metatable = "array",
}

function M.array(length)
	if length ~= nil and (math.type(length) ~= "integer" or length < 0) then
		error("An array cannot have a negative length.", 2)
	end
	local self = setmetatable({}, array_mt)
	arrays[self] = {n = length or 0}
	return self
end

-- ---- dictionary ----

-- Lua turns a number used as a table key into a string everywhere else, so a
-- numeric key has to mean the same thing to set, get, exists and delete.
local function dict_key(key, level)
	if key == nil then
		error("A dictionary key cannot be nil.", level + 1)
	end
	if type(key) == "number" then return tostring(key) end
	return key
end

local dict_methods = {}

function dict_methods.set(self, key, value)
	dicts[self][dict_key(key, 2)] = value
end

function dict_methods.get(self, key)
	local value = dicts[self][dict_key(key, 2)]
	if value == nil then return nil, false end
	return value, true
end

function dict_methods.exists(self, key)
	return dicts[self][dict_key(key, 2)] ~= nil
end

function dict_methods.delete(self, key)
	local k = dict_key(key, 2)
	local store = dicts[self]
	if store[k] == nil then return false end
	store[k] = nil
	return true
end

function dict_methods.delete_all(self)
	dicts[self] = {}
	return true
end

function dict_methods.get_keys(self)
	local keys = {}
	for k in pairs(dicts[self]) do keys[#keys + 1] = k end
	return keys
end

local dict_mt = {
	__name = "dictionary",
	__index = function(self, key) return dict_methods[key] end,
	__newindex = function()
		error("A dictionary holds values under keys; use set to store one.", 2)
	end,
	__len = function(self)
		local count = 0
		for _ in pairs(dicts[self]) do count = count + 1 end
		return count
	end,
	__metatable = "dictionary",
}

function M.dictionary()
	local self = setmetatable({}, dict_mt)
	dicts[self] = {}
	return self
end

-- ---- names ----
--
-- Oidua's API is BGT's, and NVGT renamed a good deal of it. Everything below
-- gives Oidua's name, arguments and return value on top of what NVGT already
-- has; the NVGT originals stay reachable through the nvgt table. Oidua's lists
-- are plain Lua tables counting from 1, so the functions returning one are
-- wrapped to give that too.
--
-- Built when installed rather than when required, so that requiring this file
-- without expose_nvgt still gives array and dictionary on their own.
local function build_api()
	local nvgt = nvgt
	local api = {}

	api.show_game_window = function(title) return nvgt.show_window(title) ~= nil end
	api.is_game_window_active = nvgt.is_window_active
	api.clipboard_copy_text = nvgt.clipboard_set_text
	api.clipboard_read_text = nvgt.clipboard_get_text
	-- Not the id Oidua would produce for the same machine.
	api.generate_computer_id = function() return nvgt.generate_system_fingerprint() end

	-- Screen readers. Oidua queues unless asked to interrupt, and accepts BGT's
	-- reader number in front of the text and ignores it.
	api.screen_reader_name = nvgt.screen_reader_detect
	api.screen_reader_stop_speech = nvgt.screen_reader_silence
	api.screen_reader_speak = function(a, b)
		if type(a) == "number" then return nvgt.screen_reader_speak(b, false) end
		return nvgt.screen_reader_speak(a, b == true)
	end
	api.screen_reader_speak_interrupt = function(text) return nvgt.screen_reader_speak(text, true) end
	local reader_names = {[1] = "jaws", [2] = "window", [3] = "system access", [4] = "nvda"}
	api.screen_reader_is_running = function(reader)
		local name = nvgt.screen_reader_detect():lower()
		if name == "" then return false end
		if reader == nil then return true end
		local wanted = reader_names[reader]
		return wanted ~= nil and name:find(wanted, 1, true) ~= nil
	end
	api.JAWS, api.WINDOW_EYES, api.SYSTEM_ACCESS, api.NVDA = 1, 2, 3, 4

	-- Hashing and text.
	api.string_hash = function(data, algorithm, binary)
		if algorithm == 1 then return nvgt.string_hash_sha256(data, binary == true) end
		if algorithm == 2 then return nvgt.string_hash_sha512(data, binary == true) end
		error("string_hash: algorithm must be 1 (SHA-256) or 2 (SHA-512).", 2)
	end
	api.file_hash = function(filename, algorithm, binary)
		local f = io.open(filename, "rb")
		if not f then return "" end
		local data = f:read("a")
		f:close()
		return api.string_hash(data, algorithm, binary)
	end
	api.number_to_hex_string = function(n)
		if n < 0 then return "" end
		return string.format("%x", math.floor(n))
	end
	-- NVGT raises on a stray percent; Oidua leaves it alone.
	api.url_decode = function(text)
		text = text:gsub("+", " ")
		return (text:gsub("%%(%x%x)", function(hex) return string.char(tonumber(hex, 16)) end))
	end
	-- The same round trip within one engine, but not the bytes Oidua writes.
	api.string_compress = function(data) return nvgt.string_deflate(data) end
	-- Data that is not compressed gives an empty string, where NVGT raises.
	api.string_decompress = function(data)
		local ok, result = pcall(nvgt.string_inflate, data)
		return ok and result or ""
	end

	-- Lists come back as Lua tables counting from 1.
	api.find_files = function(pattern) return nvgt.totable(nvgt.find_files(pattern)) end
	api.find_directories = function(pattern) return nvgt.totable(nvgt.find_directories(pattern)) end
	api.keys_pressed = function() return nvgt.totable(nvgt.keys_pressed()) end
	api.keys_down = function() return nvgt.totable(nvgt.keys_down()) end
	api.keys_released = function() return nvgt.totable(nvgt.keys_released()) end
	-- Without focus the keyboard is not read, so no key is up either.
	api.key_up = function(key)
		local up = nvgt.key_up(key) -- first, so a missing or wrong argument still raises
		return up and nvgt.is_window_active()
	end
	local all_keys
	api.keys_up = function()
		if not all_keys then
			local seen = {}
			all_keys = {}
			for _, name in ipairs(nvgt.api_names()) do
				if name:match("^KEY_") then
					local code = nvgt[name]
					if math.type(code) == "integer" and code > 0 and not seen[code] then
						seen[code] = true
						all_keys[#all_keys + 1] = code
					end
				end
			end
			table.sort(all_keys)
		end
		if not nvgt.is_window_active() then return {} end -- without focus no key is known to be anywhere
		local down = {}
		for _, code in ipairs(api.keys_down()) do down[code] = true end
		local up = {}
		for _, code in ipairs(all_keys) do
			if not down[code] then up[#up + 1] = code end
		end
		return up
	end

	-- Sound devices, counted from 1 as list_sound_devices hands them out.
	api.list_sound_devices = function() return nvgt.totable(nvgt.sound_output_devices) end
	api.open_sound_device = function(index)
		if math.type(index) ~= "integer" or index < 1 or index > #api.list_sound_devices() then return false end
		nvgt.sound_output_device = index - 1
		return true
	end

	-- The mouse. Oidua numbers buttons from 0 in BGT's order; NVGT uses SDL's.
	api.MOUSE_LEFT, api.MOUSE_RIGHT, api.MOUSE_MIDDLE = 0, 1, 2
	api.MOUSE_BUTTON4, api.MOUSE_BUTTON5, api.MOUSE_BUTTON6, api.MOUSE_BUTTON7, api.MOUSE_BUTTON8 = 3, 4, 5, 6, 7
	local sdl_button = {[0] = 1, [1] = 3, [2] = 2, [3] = 4, [4] = 5, [5] = 6, [6] = 7, [7] = 8}
	local function button_of(button)
		if math.type(button) ~= "integer" then error("A mouse button is a number, such as MOUSE_LEFT.", 3) end
		return sdl_button[button]
	end
	api.mouse_down = function(button) local b = button_of(button) return b ~= nil and nvgt.mouse_down(b) end
	api.mouse_pressed = function(button) local b = button_of(button) return b ~= nil and nvgt.mouse_pressed(b) end
	api.mouse_update = function() nvgt.mouse_update() return true end

	-- Network events, which NVGT spells in lower case.
	api.EVENT_NONE, api.EVENT_CONNECT = nvgt.event_none, nvgt.event_connect
	api.EVENT_RECEIVE, api.EVENT_DISCONNECT = nvgt.event_receive, nvgt.event_disconnect

	-- Folders, without the trailing separator NVGT leaves on. NVGT has no
	-- desktop or documents folder; those are taken from the user profile.
	local function folder(path) return (path:gsub("[\\/]+$", "")) end
	api.DIRECTORY_APPDATA = folder(nvgt.DIRECTORY_APPDATA)
	api.DIRECTORY_COMMON_APPDATA = folder(nvgt.DIRECTORY_COMMON_APPDATA)
	api.DIRECTORY_LOCAL_APPDATA = folder(nvgt.DIRECTORY_LOCAL_APPDATA)
	api.DIRECTORY_TEMP = folder(nvgt.DIRECTORY_TEMP)
	local profile = os.getenv("USERPROFILE") or os.getenv("HOME") or ""
	api.DIRECTORY_DESKTOP = profile .. "\\Desktop"
	api.DIRECTORY_DESKTOP_VIRTUAL = api.DIRECTORY_DESKTOP
	api.DIRECTORY_MY_DOCUMENTS = profile .. "\\Documents"

	-- Oidua's run takes one to three arguments; NVGT's wants all four.
	api.run = function(filename, arguments, wait)
		if type(filename) == "table" then return nvgt.run(filename, arguments, wait) end
		return nvgt.run(filename, arguments or "", wait == true, false)
	end
	-- vector(x, y) leaves z at 0, as in Oidua.
	api.vector = function(x, y, z)
		if x == nil then return nvgt.vector() end
		return nvgt.vector(x, y or 0, z or 0)
	end

	-- BGT's key names, as Oidua spells them.
	local keys = {
		KEY_LBRACKET = "KEY_LEFTBRACKET", KEY_RBRACKET = "KEY_RIGHTBRACKET", KEY_CAPITAL = "KEY_CAPSLOCK",
		KEY_LCONTROL = "KEY_LCTRL", KEY_RCONTROL = "KEY_RCTRL", KEY_LMENU = "KEY_LALT", KEY_RMENU = "KEY_RALT",
		KEY_LWIN = "KEY_LGUI", KEY_RWIN = "KEY_RGUI", KEY_APPS = "KEY_APPLICATION", KEY_NUMLOCK = "KEY_NUMLOCKCLEAR",
		KEY_SCROLL = "KEY_SCROLLLOCK", KEY_SYSRQ = "KEY_PRINTSCREEN", KEY_PRIOR = "KEY_PAGEUP", KEY_NEXT = "KEY_PAGEDOWN",
		KEY_DECIMAL = "KEY_NUMPAD_PERIOD", KEY_ADD = "KEY_NUMPAD_PLUS", KEY_SUBTRACT = "KEY_NUMPAD_MINUS",
		KEY_MULTIPLY = "KEY_NUMPAD_MULTIPLY", KEY_DIVIDE = "KEY_NUMPAD_DIVIDE", KEY_NUMPADENTER = "KEY_NUMPAD_ENTER",
		KEY_NUMPADCOMMA = "KEY_NUMPAD_COMMA", KEY_NUMPADEQUALS = "KEY_NUMPAD_EQUALS", KEY_PLAYPAUSE = "KEY_MEDIA_PLAY",
		KEY_MEDIASTOP = "KEY_MEDIA_STOP", KEY_NEXTTRACK = "KEY_MEDIA_NEXT_TRACK", KEY_PREVTRACK = "KEY_MEDIA_PREVIOUS_TRACK",
		KEY_MEDIASELECT = "KEY_MEDIA_SELECT", KEY_WEBHOME = "KEY_AC_HOME", KEY_WEBSEARCH = "KEY_AC_SEARCH",
		KEY_WEBFAVORITES = "KEY_AC_BOOKMARKS", KEY_WEBREFRESH = "KEY_AC_REFRESH", KEY_WEBSTOP = "KEY_AC_STOP",
		KEY_WEBFORWARD = "KEY_AC_FORWARD", KEY_WEBBACK = "KEY_AC_BACK", KEY_DASH = "KEY_MINUS", KEY_OEM_102 = "KEY_NONUSBACKSLASH",
	}
	for i = 0, 9 do keys["KEY_NUMPAD" .. i] = "KEY_NUMPAD_" .. i end
	for bgt, sdl in pairs(keys) do api[bgt] = nvgt[sdl] end

	return api
end

-- ---- installation ----

function M.install(target)
	target = target or _G
	target.array = M.array
	target.dictionary = M.dictionary
	if nvgt then
		for name, value in pairs(build_api()) do target[name] = value end
	end
	return target
end

return M
