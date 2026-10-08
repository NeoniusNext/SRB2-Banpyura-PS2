-- Platform-dependent results of the unchanged upstream Lua (not compared by lua_equiv.py, printed with "LP " for the report): the same source gives other
-- numbers on a 64-bit Linux build (long = 8 bytes) and on Windows / the PS2 (long = 4 bytes).
print("LP strtol_overflow " .. tostring(tonumber("9999999999")))
print("LP format_x_minus1 " .. string.format("%x", -1))
print("LP format_u_minus1 " .. string.format("%u", -1))
print("LP format_d_big " .. string.format("%d", 2147483647 + 1))
print("LP DONE_PLAT")
