-- Everything registered in NVGT's AngelScript engine is reachable from here, either through the nvgt table or, when
-- expose_nvgt(true) was used, directly as globals. Lua's own globals always win over NVGT ones.
println("Hello from " .. _VERSION .. " inside NVGT!")
println("engine ticks: " .. tostring(nvgt.ticks()))

-- NVGT object types work, including constructors, fields, methods and operators.
local a = nvgt.vector(1, 2, 3)
local b = nvgt.vector(4, 5, 6)
local c = a + b
println(string.format("vector sum: %g %g %g, length %g", c.x, c.y, c.z, c:length()))

-- Arrays and dictionaries convert to and from lua tables.
local names = nvgt.toarray({"jazz", "blues", "rock"}, "string")
println("joined: " .. nvgt.join(names, ", "))
println("first element: " .. names[0]) -- AngelScript arrays are 0-based.

-- Globals defined here are visible to the hosting AngelScript through the lua_state object.
score = 1422
function farewell()
	println("farewell, " .. tostring(player_name))
end
