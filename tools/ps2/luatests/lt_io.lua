-- OPT14: the io / os libraries a mod uses for its settings and clocks. Files live in luafiles/ of the home folder ("client/" for clients). Values that are clocks (os.time, os.clock) are
-- only checked for plausibility; everything else must be the PC's.
local function P(...)
	local t = {...}
	local s = {}
	for i = 1, select("#", ...) do s[#s+1] = tostring(t[i]) end
	print("LQ " .. table.concat(s, " "))
end
local function E(f, ...) local r = {pcall(f, ...)} if r[1] then table.remove(r, 1) for i = 1, #r do local v = r[i] r[i] = (type(v) == "userdata") and "userdata" or tostring(v) end return table.concat(r, ",") end return "E:" .. (tostring(r[2]):gsub("^.-:%d+: ", "")) end

-- writing and reading back
local name = "client/lt_io.dat"
local f = io.openlocal(name, "w")
P("open_w", f ~= nil, type(f))
if f then
	P("write", E(function() return f:write("hello\n", "line two\n", 12345, "\n", "last line without newline") end))
	P("close", E(function() return f:close() end))
end
f = io.openlocal(name, "r")
P("open_r", f ~= nil)
if f then
	P("read_line", E(function() return f:read("*l") end))
	P("read_line2", E(function() return f:read("*l") end))
	P("read_num", E(function() return f:read("*n") end))
	P("read_rest", E(function() return f:read("*a") end))
	P("read_eof", E(function() return f:read("*l") end))
	P("seek", E(function() return f:seek("set", 0) end), E(function() return f:seek("cur", 3) end), E(function() return f:seek("end") end))
	f:seek("set", 0)
	local n = 0
	for line in f:lines() do n = n + 1 end
	P("lines", n)
	f:close()
end
-- appending, binary bytes, a long file
f = io.openlocal(name, "a")
if f then
	for i = 1, 200 do f:write(string.char(i % 256, 0, 255)) end
	f:close()
end
f = io.openlocal(name, "rb")
if f then
	local all = f:read("*a")
	P("size", #all, all:byte(#all), all:byte(#all - 1), all:byte(#all - 2))
	f:close()
end
-- a settings round trip the way mods do it
local cfg = {sensitivity = 7, name = "Lua Player", scale = 120, colors = {1, 2, 3}}
f = io.openlocal("client/lt_io_cfg.txt", "w")
if f then
	f:write(string.format("sensitivity=%d\nname=%s\nscale=%d\ncolors=%s\n", cfg.sensitivity, cfg.name, cfg.scale, table.concat(cfg.colors, ",")))
	f:close()
end
f = io.openlocal("client/lt_io_cfg.txt", "r")
if f then
	local back = {}
	for line in f:lines() do local k, v = line:match("^(%w+)=(.*)$") if k then back[k] = v end end
	f:close()
	P("cfg", back.sensitivity, back.name, back.scale, back.colors)
end
-- errors: names that are refused, the missing file
P("deny1", E(io.openlocal, "../x.dat", "w"))
P("deny2", E(io.openlocal, "/etc/passwd", "r"))
P("deny3", E(io.openlocal, "client/x.exe", "w"))
P("deny4", E(io.openlocal, "client\\x.dat", "w"))
P("missing", E(function() local g, err, code = io.openlocal("client/does_not_exist.dat", "r") return g, err end))
P("io_open_w", E(io.open, "x.dat", "w", function() end))
P("io_type", E(io.type, f), E(io.type, 5))
-- io.write / io.read / io.output exist?
P("iolib", type(io.write), type(io.read), type(io.lines), type(io.output), type(io.input), type(io.stdout), type(io.stderr), type(io.tmpfile))
-- os
local t = os.time()
P("os_time", type(t), t > 1500000000, t < 2147483647)
P("os_clock", type(os.clock()), os.clock() >= 0)
local d = os.date("*t")
P("os_date_t", type(d), type(d.year), d.year >= 2020, d.year < 2100, d.month >= 1 and d.month <= 12, d.day >= 1 and d.day <= 31, d.hour >= 0 and d.hour <= 23, d.min >= 0 and d.min <= 59, d.wday >= 1 and d.wday <= 7)
P("os_date_s", #os.date("%Y-%m-%d %H:%M:%S"), os.date("%Y", 86400 * 365), os.date("!%Y-%m-%d %H:%M:%S", 1000000000), os.date("!%A %B %d", 1000000000), os.date("!%j", 1000000000))
P("os_time_t", os.time({year = 2001, month = 9, day = 9, hour = 1, min = 46, sec = 40, isdst = false}) ~= nil)
P("os_difftime", os.difftime(100, 40), os.difftime(40, 100))
P("os_others", type(os.getenv), type(os.exit), type(os.remove), type(os.rename), type(os.tmpname), type(os.execute), type(os.setlocale))
-- the clock moves forward
local c0 = os.clock()
local x = 0
for i = 1, 200000 do x = x + i % 7 end
P("clock_moves", os.clock() >= c0)
P("DONE_IO")
