-- demo.lua: runs inside lua54.exe (+ lua54.dll) under the Nebula x64 emulator
local function fib(n) if n < 2 then return n end return fib(n - 1) + fib(n - 2) end
print("Lua", _VERSION, "fib(25) =", fib(25))
local t = {} for i = 1, 10 do t[#t + 1] = i * i end
print("squares:", table.concat(t, ","), string.format("pi=%.4f year=%s", math.pi, os.date("%Y")))
local f = assert(io.open("lua_out.txt", "w")) f:write("written from Lua\n") f:close()
for line in io.lines("lua_out.txt") do print("read back:", line) end
