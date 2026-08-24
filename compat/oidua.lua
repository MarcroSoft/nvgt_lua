-- Oidua's array and dictionary, for running Oidua code on nvgt_lua.
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
--   oidua.install()   -- defines array and dictionary as globals
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

-- ---- installation ----

function M.install(target)
	target = target or _G
	target.array = M.array
	target.dictionary = M.dictionary
	return target
end

return M
