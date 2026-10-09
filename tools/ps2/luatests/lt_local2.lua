print("LQ local2 loaded " .. tostring(leveltime))
addHook("ThinkFrame", function() if leveltime == 20 then print("LQ local2 think " .. leveltime) end end)
rawset(_G, "LQ_LOCAL2", 1)
