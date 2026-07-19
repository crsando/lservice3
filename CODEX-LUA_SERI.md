# `lua-seri` 序列化实现笔记

## 范围与入口

- 实现位于 `src/lua-seri.c`，公开的 Lua C API 声明位于 `src/lua-seri.h`。
- `luaseri_pack(lua_State *L)` 将 Lua 栈上的**全部参数**编码后返回两个 Lua 值：`lightuserdata`（C 堆缓冲区地址）与长度。
- `luaseri_unpack(lua_State *L)` 从该地址解码，但不释放缓冲区；`luaseri_unpack_remove(lua_State *L)` 通过保护调用解码并在随后 `free` 缓冲区。这与项目消息队列的堆缓冲区所有权模型匹配。
- `src/lservice.c` 将 `pack`、`unpack`、`unpack_remove` 注册到 `lservice3` 模块；`src/lua-seri-lib.c` 也可将前三者注册为独立的 `seri` 模块。

本项目使用 LuaJIT 2.1 / Lua 5.1 兼容 API；源码注释中的若干兼容性修改（例如 `lua_objlen`）也反映了这一点。

## 二进制格式

### 总体布局

`seri_pack` 先在由 128 字节链表块组成的写缓冲区中生成有效负载，最终分配连续内存，布局为：

```text
[ 4 字节 native int：payload 长度 ][ payload 字节流 ]
```

每个值以一个标签字节开始：

```text
bits 0..2 : 基础类型（0..7）
bits 3..7 : cookie / 子类型 / 短长度
```

该布局直接写入本机的整数、`double` 和指针字节序，因而不是跨端序、跨 ABI、跨指针宽度的稳定线格式；长度前缀也不是网络字节序。

### 标签类型

基础类型由 `TYPE_*` 常量定义：

| 类型 | 编码内容 |
| --- | --- |
| `TYPE_BOOLEAN` | `nil`、`false`、`true` 全部只占标签字节 |
| `TYPE_NUMBER` | 预留紧凑整数和 `double` 子类型；当前 LuaJIT 路径把所有 Lua number 写成 8 字节 `double` |
| `TYPE_USERDATA` | 裸指针，子类型为普通指针或 light C function |
| `TYPE_SHORT_STRING` | cookie 直接保存长度（小于 32） |
| `TYPE_LONG_STRING` | cookie 指示后跟 2 或 4 字节长度，再跟字符串字节 |
| `TYPE_TABLE` / `TYPE_TABLE_MARK` | 表的数组区长度（或扩展长度）和键值对流；`MARK` 表示该表要登记为可复用对象 |
| `TYPE_REF` | 指向祖先表的短引用，或指向已登记表的扩展对象 ID |

## 编码流程

1. `luaseri_pack` 调用 `seri_pack(L, 0, &sz)`；`pack_from` 遍历当前 Lua 栈的所有参数。
2. `pack_one` 以 `lua_type` 分派：`nil`、布尔、数字、字符串、`lightuserdata`、无 upvalue 的 C 函数和 table 可处理；其他类型进入默认分支报错。
3. 写缓冲区由 `write_block` 管理。`wb_push` 将字节追加到 128 字节 `block` 链；`seri` 再将它们拷贝为带长度头的连续 `malloc` 内存。
4. table 先写数组部分：`lua_objlen` 给出数组长度，顺序编码整数键 `1..n` 的值。随后以 `lua_next` 编码其余键值对，并以一个 `nil` 标签作为哈希部分结束符。
5. 若表具有 `__pairs` 元字段，`wb_table_metapairs` 调用该元方法并按迭代结果写键值对；此时表头的数组长度为 0。
6. 遇到 table 时会先进行引用判断：
   - 当前递归祖先表命中时，写入短 `TYPE_REF`，用于直接或间接环；
   - 已出现但不在当前祖先链上的表命中时，写入扩展对象 ID，并把第一次出现时的 `TYPE_TABLE` 标签回写为 `TYPE_TABLE_MARK`；
   - 第一次且未重复的表不需要全局对象表登记。

`MAX_DEPTH` 为 31，`MAX_REFERENCE` 为 32。前 32 个候选引用保存在 C 数组中；超过后，编码器临时使用 Lua 表建立 `pointer -> id` 和 `id -> 标签地址` 映射。

## 解码流程

1. `seri_unpack` 读出前 4 字节长度，初始化 `read_block`，并逐个读取标签直到有效负载耗尽。
2. `push_value` 根据基础类型及 cookie 将一个 Lua 值压栈。字符串通过 `lua_pushlstring` 创建；表由 `unpack_table` 递归构建。
3. `unpack_table` 先建表，再读数组区，之后不断读取“键、值”对，读到 `nil` 键即结束。对于 `TYPE_TABLE_MARK`，新表会以递增 ID 保存到解码侧引用表。
4. `TYPE_REF` 的短形式从当前祖先栈取表；扩展形式按对象 ID 从引用表取回同一个表，从而保持共享引用关系。

## `lightuserdata`：是否支持？

**支持。** 这是显式实现的功能，而非偶然行为：

- 编码时，`pack_one` 的 `LUA_TLIGHTUSERDATA` 分支调用 `wb_pointer(..., TYPE_USERDATA_POINTER)`。
- `wb_pointer` 写入 `TYPE_USERDATA` 标签、`TYPE_USERDATA_POINTER` 子类型和 `sizeof(void *)` 个原始指针字节。
- 解码时，`push_value` 对相同子类型调用 `get_pointer`，再调用 `lua_pushlightuserdata`。

也就是说，往返后得到的是数值相同的**地址值**，而不是原指针所指内存的一份副本。它不会：

- 拷贝、检查或延长指针所指对象的生命周期；
- 重建 C 对象、元数据或资源所有权；
- 在不同进程、重启后的进程、不同地址空间，或不兼容的 ABI 之间产生有效指针。

在本项目的典型模型中，多个 LuaJIT VM 位于同一进程的不同线程内，因此同一进程内、对象仍存活、并且接收方确实理解该指针类型时，地址通常仍可使用。但这仍不是线程安全或所有权安全的保证；发送方若提前释放对象，接收方会持有悬空指针。C function 的序列化也同样只是保存函数地址，且仅允许没有 upvalue 的 light C function。

## 是否需要修改来“支持”它？

不需要：项目当前已经支持 `lightuserdata` 的“原样传递指针地址”语义。

如果“支持”的含义是**跨进程、持久化、跨重启，或复制指向的数据**，则不能靠把现有裸地址编码保留下来实现，必须定义对象级协议。可行方向包括：

1. 为可传输对象分配稳定的整数句柄，并在接收侧用注册表 / 资源管理器解析句柄；需要处理引用计数、释放、权限与失效。
2. 为明确的对象类型增加序列化器 / 反序列化器，编码版本号和实际字段或字节内容，而非 `void *`。
3. 对未知 `lightuserdata` 明确拒绝，或只允许白名单类型，避免把不受控指针经消息边界传播。

这类改动应增加新的、显式的标签子类型或由上层把对象先转换成普通 Lua table/string；不要把任意地址解引用并复制，因为 C 无法从 `void *` 通用地获知对象大小、布局、所有权或析构规则。

## 额外实现注意事项

- 此格式含本机表示，且没有版本字段；将序列化数据存盘或发送给不同架构的进程并不安全。
- `luaseri_unpack` 仅接收指针、不使用 Lua 侧传入的长度；它信任缓冲区的前 4 字节。仅应对受信任、由本模块分配且仍然存活的缓冲区使用。
- 普通 full userdata、thread、Lua 闭包（以及含 upvalue 的 C 闭包）不受支持，`pack_one` 会报 `Unsupport type ... to serialize` 或 C 函数专用错误。
- 在超过 `MAX_REFERENCE` 的扩展引用解码路径中，`unpack_ref` 取回对象后对 `LUA_TTABLE` 的检查看起来与预期相反：成功取到表时会报 `Invalid ref object id`。这会影响大规模共享表图的扩展引用，而不影响本问题中的 `lightuserdata` 路径；如后续要修改序列化器，建议为该边界条件补测试并先核实 / 修正该判断。
