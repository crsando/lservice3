local service = require "lservice3" .input(...)
local inspect = require "inspect"

local uv = service.uv

local S = {}

local function string_split(s, sep)
    sep = sep or " "
    local result = {}
    local start_pos = 1
    while true do
        local pos = string.find(s, sep, start_pos, true)
        if not pos then
            table.insert(result, string.sub(s, start_pos))
            break
        end
        table.insert(result, string.sub(s, start_pos, pos - 1))
        start_pos = pos + #sep
    end
    return result
end

local function on_line(line, client)
    do
        print(client, line)
    end
    local rst = string_split(line, " ")
    local cmd = assert(rst[1])

    local body = {}
    for i = 2, #rst do 
        body[i-1] = rst[i]
    end

    service.send(service.get_id(), cmd, unpack(body))
end

local function listen(host, port)
    local server = service.uv.new_tcp()
    host = host or "0.0.0.0"
    port = port or 19702
    assert(server:bind(host, port))

    --[[
    local function on_line(line, client)
        print(client, line)
        if string.match(line, "^quit") then 
            service.loopback("quit")
            service.send(0, "quit")
        end
    end
    ]]

    server:listen(128, function(err)
        assert(not err, err)
        local client = uv.new_tcp()
        server:accept(client)
        local buffer = ""
        client:read_start(function(read_err, chunk)
            if read_err then
                client:read_stop()
                client:close()
                return
            end
            -- chunk == nil 表示对端关闭连接
            if not chunk then
                client:read_stop()
                client:close()
                return
            end
            buffer = buffer .. chunk
            while true do
                local pos = buffer:find("\n", 1, true)
                if not pos then
                    break
                end
                local line = buffer:sub(1, pos - 1)
                buffer = buffer:sub(pos + 1)
                -- 兼容 Windows 风格的 \r\n
                if line:sub(-1) == "\r" then
                    line = line:sub(1, -2)
                end
                on_line(line, client)
            end
        end) -- end read_start
    end) -- end listen
  print(("listening on %s:%d"):format(host, port))
  return 1
end

listen()

function S.hello()
    print("say hello")
end

function S.quit()
    service.quit()
end

return service.dispatch(S)