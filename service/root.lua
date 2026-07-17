local inspect = require "inspect"
local service = require "lservice3" .input(...)

local S = {}

function S.pong()
    print("root pong")
end

function S.boot()
    print("root is booting")
    service.spawn { name = "remote", source = "@service/remote.lua", config = {} }
    service.spawn { name = "user", source = "@service/user.lua", config = {} }
    service.spawn { name = "echo", source = "@service/echo.lua", config = {} }

    print("echo:", service.call("echo", "hi mountain"))

    local rsp = service.call("user", "ping", 0)

    print("get rsp", rsp)
    service.call("user", "quit")
    service.call(0, "quit")
end

function S.quit()
    print("root is quiting")
    service.quit()
end

return service.dispatch(S)