-- Platform-dependent results of the unchanged upstream Lua (not compared by lua_equiv.py, printed with "LP " for the report): the same source gives other
-- numbers on a 64-bit Linux build (long = 8 bytes) and on Windows / the PS2 (long = 4 bytes).
print("LP strtol_overflow " .. tostring(tonumber("9999999999")))
print("LP format_x_minus1 " .. string.format("%x", -1))
print("LP format_u_minus1 " .. string.format("%u", -1))
print("LP format_d_big " .. string.format("%d", 2147483647 + 1))
local function print_lp(name, ...)
	local r = {...}
	for i = 1, select("#", ...) do r[i] = tostring(r[i]) end
	print("LP " .. name .. " " .. table.concat(r, " "))
end
-- number literals and strings beyond 32 bits (lexer and tonumber): the PC reads them with a 64-bit long
print_lp("lit32", 2147483647, 2147483648, -2147483648, 2147483649, 4294967295, 4294967296, 9999999999, 99999999999999999999)
print_lp("lithex", 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0x100000000, 0x1FFFFFFFF, 0xDEADBEEF)
print_lp("tonum32", tonumber("2147483647"), tonumber("2147483648"), tonumber("-2147483648"), tonumber("-2147483649"), tonumber("4294967295"), tonumber("4294967297"), tonumber("99999999999999999999"))
print_lp("tonumhex", tonumber("0xFFFFFFFF"), tonumber("0x100000000"), tonumber("0x1FFFFFFFF"), tonumber("-0x10"), tonumber("0x"))
print_lp("strarith", "10" + 5, "0x10" + 1, "2147483648" + 0, " 12 " * 2, pcall(function() return "abc" + 1 end))
print_lp("wrap", 2147483647 + 1, -2147483647 - 2, 65536 * 65536, 46341 * 46341, -2147483647 - 1 - 1)
print("LP DONE_PLAT")
