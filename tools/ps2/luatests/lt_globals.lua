-- Lua equivalence: the whole global namespace (every function, constant and library table the scripts can see), sorted.
local function P(...)
	local t = {...}
	local s = {}
	for i = 1, select("#", ...) do s[#s+1] = tostring(t[i]) end
	print("LQ " .. table.concat(s, " "))
end
local names = {}
for k in pairs(_G) do names[#names+1] = tostring(k) end
table.sort(names)
P("nglobals", #names)
local line, n = {}, 0
for _, k in ipairs(names) do
	local v = rawget(_G, k)
	local t = type(v)
	local item = k .. ":" .. t
	if t == "number" or t == "boolean" or t == "string" then item = item .. "=" .. tostring(v) end
	if t == "table" and k ~= "_G" then
		local sub = {}
		for k2 in pairs(v) do sub[#sub+1] = tostring(k2) end
		table.sort(sub)
		item = item .. "{" .. #sub .. ":" .. table.concat(sub, ",") .. "}"
	end
	line[#line+1] = item
	n = n + 1
	if n == 20 then P("g", table.concat(line, " ")) line = {} n = 0 end
end
if #line > 0 then P("g", table.concat(line, " ")) end
-- metatable fields of the userdata globals
for _, k in ipairs(names) do
	local v = rawget(_G, k)
	if type(v) == "userdata" then
		local mt = getmetatable(v)
		local keys = {}
		if mt then for k2 in pairs(mt) do keys[#keys+1] = tostring(k2) end end
		table.sort(keys)
		P("ud", k, #keys, table.concat(keys, ","))
	end
end
P("DONE_GLOBALS")
