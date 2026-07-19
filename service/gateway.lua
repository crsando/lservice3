local service = require "lservice3" .input(...)
local inspect = require "inspect"

local S = {}

local uv = service.uv

local cjson = require('cjson')

local HOST = '0.0.0.0'
local PORT = 8432 -- tifa 用手机输入法九宫格对应的数字

--[[

约定：

输入的消息

```
{
    "type" : "request",
    "actor": "gateway",
    "method" : "ping",
    "args": ["pong", 1] 
}
```

输入消息 -> handle_message -> service.send(actor, method, args)

]]

--------------------------------------------------------------
-- 业务处理：收到一个完整的 JSON 对象后在这里处理
--------------------------------------------------------------
local function handle_message(client, peer, line)
    local ok, msg = pcall(cjson.decode, line)
    print(string.format('[消息] %s:%d => %s',
        peer.ip, peer.port, inspect(msg)))

    if (msg.type == "request") and (type(msg.method) == "string")  then 
        service.send(service.get_id(), "handler", client, peer, msg)
    end
end

function S.request_handler(client, peer, msg)
    local actor = msg.actor or service.get_id() -- default to self
    local method = msg.method or "dummy"
    local resp = service.call(actor, method, unpack(msg.args or {}))
    send_json(client, "msg handled")
end

--------------------------------------------------------------
-- 发送 JSON 响应（一行一条，\n 结尾）
--------------------------------------------------------------
local function send_json(client, tbl)
    local ok, data = pcall(cjson.encode, tbl)
    if ok then
        client:write(data .. '\n')
    end
end

local client_list = {}

local function accept_client(server)
    local client = uv.new_tcp()
    server:accept(client)
    local peer = client:getpeername()
    print(string.format('[连接] %s:%d', peer.ip, peer.port))

    client_list[peer] = client

    return client, peer
end

local function close_client(client)
    local peer = client:getpeername()
    client_list[peer] = nil
    client:close()
end

--------------------------------------------------------------
-- TCP Server
--------------------------------------------------------------

local function init_server()
    print(string.format('Server listening on %s:%d ...', HOST, PORT))
    print('Protocol: one JSON object per line (\\n delimited)')
    --
    -- 每次读取一行，这里只接收，不处理，也不回复
    --
    local server = uv.new_tcp()
    server:bind(HOST, PORT)

    server:listen(128, function(err)
        assert(not err, err)

        local client, peer = accept_client(server)
        print(type(client), inspect(client))
        print(type(peer), inspect(peer))

        -- 每个连接维护一个 buffer，解决粘包/拆包
        local buf = ''

        client:read_start(function(err, data)
            if err then
                print('[错误] ' .. err)
                close_client(client)
                return
            end

            if not data then
                print(string.format('[断开] %s:%d', peer.ip, peer.port))
                close_client(client)
                return
            end

            -- 把新数据追加到 buffer
            buf = buf .. data

            -- 按 \n 切分，每行是一条完整 JSON
            while true do
                local pos = buf:find('\n')
                if not pos then break end

                local line = buf:sub(1, pos - 1):gsub('\r$', '')
                buf = buf:sub(pos + 1)

                if #line > 0 then
                    -- 解析 JSON
                    handle_message(client, peer, line)
                end
            end
        end)
    end) -- end server:listen()
end

function S.echo(...)
    print("gateway echo: ", ...)
end


function S.check(id)
    print("user check", id)
end

function S.quit()
    print("gateway is quitting")
    service.quit()
end

init_server()

return service.dispatch(S)