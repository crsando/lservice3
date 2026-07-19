#include "service.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "uv.h"

#include "log.h"
#include <assert.h>
#include <stdlib.h>
#include "message.h"

#include "lua.h"
#include "loadluv.h"

#define _SERVICE_MQ_DEF_SIZE_ 1024

// 每次service收到消息的时候，由此进入
void service_async_cb(uv_async_t * handle) {
    service_t * s = NULL;
    int service_ref = 0;
    s = (service_t *)(handle->data);
    if (s == NULL) {
        log_debug("handle data error, need service_t pointer");
        return;
    }

    service_ref = s->func_ref;
    if (service_ref == LUA_NOREF || service_ref == LUA_REFNIL) {
        log_debug("service function not loaded, do nothing\n");
        return;
    }

    // 从 registry 取出之前保存的 function，压到栈顶
    lua_rawgeti(s->L, LUA_REGISTRYINDEX, s->func_ref);

    // 压入参数
    lua_pushstring(s->L, "msg");

    // 调用：1 个参数，1 个返回值
    if (lua_pcall(s->L, 1, 1, 0) != LUA_OK) {
        fprintf(stderr, "lua_pcall error: %s\n", lua_tostring(s->L, -1));
        lua_pop(s->L, 1);
    }
}


//

service_pool_t * service_pool_new() {
    service_pool_t * pool = NULL;
    pool = (service_pool_t *)malloc(sizeof(service_pool_t));
    memset(pool, 0, sizeof(service_pool_t));
    pool->id = 0;
	pthread_mutex_init(&pool->lock, NULL);
    return pool;
}

service_t * service_pool_get_service(service_pool_t * pool, service_id id) {
    assert(id >= 0 && id < MAX_SERVICES);
    return pool->services[id];
}

service_t * service_pool_lookup_service(service_pool_t * pool, const char * name) {
    service_t * s = NULL;
    int i = 0;
    while(i < pool->id) {
        s = pool->services[i];
        if(s && (strcmp(s->name, name) == 0)) {
            return s;
        }
        i++;
    }
    return NULL;
}

// service_t * service_pool_query_service(service_pool_t * pool, const char * key) {
//     service_t * s = NULL;
//     pthread_mutex_lock(&pool->lock);
//     registry_t * r = registry_get(&pool->services, key);
//     s = (service_t*)( r ? r->ptr : NULL );
//     pthread_mutex_unlock(&pool->lock);
//     return s;
// }

void * service_pool_registry(service_pool_t * pool, const char * key, void * ptr) {
    if(ptr) {
        pthread_mutex_lock(&pool->lock);
        registry_put(&pool->variables, key, ptr);
        pthread_mutex_unlock(&pool->lock);
        return ptr;
    }
    else {
        void * data = NULL;
        // get registry
        log_debug("lock pool->lock");
        pthread_mutex_lock(&pool->lock);
        if(! pool->variables ) {
            // log_debug("pool->variables == NULL");
            data = NULL;
        }
        else {
            registry_t * r = registry_get(&pool->variables, key);
            data = ( r ? r->ptr : NULL );
        }
        pthread_mutex_unlock(&pool->lock);
        return data;
    }
}

int service_join(service_t * s) {
    void * ret = NULL;
    int err = pthread_join(s->thread, &ret);
    return err;
}


/*
 * 读取 @filename 指向的文件，并 luaL_loadbuffer。
 *
 * 参数:
 *   L          Lua state
 *   at_name    带 @ 的文件名，例如 "@scripts/foo.lua"
 *
 * 返回:
 *   LUA_OK / 0:
 *      成功。栈顶是编译好的 Lua function。
 *
 *   非 0:
 *      失败。栈顶是错误信息字符串。
 *
 * 注意:
 *   - at_name 必须以 '@' 开头。
 *   - 实际读取文件时会去掉 '@'。
 *   - luaL_loadbuffer 的 chunk name 仍然使用原始 at_name。
 */
int lua_loadfile_as_buffer(lua_State *L, const char *at_name) {
    const char *real_path;
    FILE *fp = NULL;
    char *buf = NULL;
    long file_size;
    size_t read_size;
    int status;

    if (at_name == NULL) {
        lua_pushliteral(L, "lua_loadfile_as_buffer: filename is NULL");
        return LUA_ERRFILE;
    }

    if (at_name[0] != '@') {
        lua_pushfstring(
            L,
            "lua_loadfile_as_buffer: filename must start with '@', got '%s'",
            at_name
        );
        return LUA_ERRFILE;
    }

    real_path = at_name + 1;

    if (real_path[0] == '\0') {
        lua_pushliteral(L, "lua_loadfile_as_buffer: empty filename after '@'");
        return LUA_ERRFILE;
    }

    fp = fopen(real_path, "rb");
    if (fp == NULL) {
        lua_pushfstring(L, "cannot open file '%s'", real_path);
        return LUA_ERRFILE;
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        lua_pushfstring(L, "cannot seek file '%s'", real_path);
        return LUA_ERRFILE;
    }

    file_size = ftell(fp);
    if (file_size < 0) {
        fclose(fp);
        lua_pushfstring(L, "cannot tell file size '%s'", real_path);
        return LUA_ERRFILE;
    }

    if (fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        lua_pushfstring(L, "cannot rewind file '%s'", real_path);
        return LUA_ERRFILE;
    }

    /*
     * malloc(0) 的行为在不同实现上不完全一致。
     * 空文件也给它分配 1 字节，方便处理。
     */
    buf = (char *)malloc((size_t)file_size > 0 ? (size_t)file_size : 1);
    if (buf == NULL) {
        fclose(fp);
        lua_pushfstring(L, "out of memory while reading '%s'", real_path);
        return LUA_ERRMEM;
    }

    read_size = fread(buf, 1, (size_t)file_size, fp);
    if (read_size != (size_t)file_size) {
        free(buf);
        fclose(fp);
        lua_pushfstring(L, "failed to read file '%s'", real_path);
        return LUA_ERRFILE;
    }

    fclose(fp);
    fp = NULL;

    /*
     * 关键点:
     *   - buffer 用真实文件内容
     *   - size 用真实长度，不用 strlen
     *   - name 用带 @ 的 at_name
     *
     * 这样报错时会显示:
     *   scripts/foo.lua:行号: 错误
     */
    status = luaL_loadbuffer(L, buf, (size_t)file_size, at_name);

    free(buf);
    buf = NULL;

    /*
     * status == 0:
     *   栈顶是编译好的 function
     *
     * status != 0:
     *   栈顶是 luaL_loadbuffer 放上来的错误信息
     */
    return status;
}

int service_init_lua(service_t * s) {
    lua_State * L;
	L = luaL_newstate();
    s->L = L;

    if(!L) {
		log_error("THREAD FATAL ERROR: could not create lua state");
		return -1;
	}

    if(s->source == NULL) {
        log_error("no lua source provided");
        return -1;
    }

	luaL_openlibs(L);



    // 加载 对应的lua source file
    if(s->source[0] == '@') {
        if(lua_loadfile_as_buffer(L, s->source)) { 
            log_error("FATAL THREAD PANIC: (loadbuffe) %s", lua_tolstring(L, -1, NULL));
            lua_close(L);
            return -1; 
        }
    }
    else {
        if(luaL_loadstring(L, s->source)) { 
            log_error("FATAL THREAD PANIC: (loadstring) %s", lua_tolstring(L, -1, NULL));
            lua_close(L);
            return -1; 
        }
    }

    int n_args = 0;
    // push pointer to self
    lua_pushlightuserdata(L, s);
    n_args ++;

    // push lightuserdata (config)
    if(s->config) {
        lua_pushlightuserdata(L, s->config);
        n_args ++;
    }

    // load luv
    if(service_load_luv(L, s->loop) < 0) {
        log_error("FATAL ERROR: (service_load_luv failed)");
        lua_close(L);
        return -1;
    }

    // run the lua code
    // 2 input expect (service, config), no output
	if(lua_pcall(L, n_args, 1, 0)) {
		log_error("FATAL THREAD PANIC: (pcall) %s", lua_tolstring(L, -1, NULL));
		lua_close(L);
		return -1;
	}

    // 理想情况下，上述运行的lua代码最后会返回一个函数handler（通过service.dispatch组装成一个函数）
    // 我们要保存这个函数，在每次service中断（收到某个消息）的时候调用
    if (!lua_isfunction(L, -1)) {
        log_debug("lua service file must return a function\n");
        lua_close(L);
        return -1;
    }

    s->func_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    // 跑uv的主循环
    uv_run(s->loop, UV_RUN_DEFAULT);

    return 0; // no error
}

service_t * service_new(service_pool_t * pool, const char * name, const char * source, void * config) {
    int err = 0;
    service_t * s;

    s = (service_t *)malloc(sizeof(service_t));
    memset(s, 0, sizeof(service_t));

    if(pool) {
        pthread_mutex_lock(&pool->lock);
        s->pool = pool;
        s->id = pool->id ++; // assign service_id

        // log_info("service_new, assign id %d", s->id);

        if(s->id > MAX_SERVICES)
            err = 1; // too many services
        else 
            pool->services[s->id] = s; // add service pointer to the list

        pthread_mutex_unlock(&pool->lock);
    }

    if(err) {
        free(s);
        return NULL;
    }

    if(name && strlen(name) > 1) {
        strlcpy(s->name, name, MAX_SERVICE_NAME_LEN);
    }
    else {
        memset(s->name, 0, sizeof(s->name));
    }

    assert(source != NULL);
    s->source = (char *)malloc(sizeof(char) * (strlen(source) + 1));
    strcpy(s->source, source);
    s->config = config;

    s->q = queue_new_ptr(_SERVICE_MQ_DEF_SIZE_);

    // init libuv main loop
    s->loop = (uv_loop_t *)malloc(sizeof(uv_loop_t));
    uv_loop_init(s->loop);

    // init async_handler
    s->async_handler = (uv_async_t *)malloc(sizeof(uv_async_t));
    uv_async_init(s->loop, s->async_handler, service_async_cb);
    s->async_handler->data = (void *)s;

    return s;
}

// entry fro pthread_create
void * service_routine_wrap(void * arg) {
    service_t * s = (service_t *)arg;
    service_init_lua(s);
    log_debug("service_routine_wrap : service (%d) exited", s->id);

    return NULL;
}

int service_start(service_t * s) {
    pthread_t th;
    int ret = pthread_create(&th, NULL, service_routine_wrap, s);
    s->thread = th;
    return ret;
}

int service_send(service_t * s, message_t * msg) {
    queue_push_ptr(s->q, msg);
    uv_async_send(s->async_handler);
    return 1;
}

message_t * service_recv(service_t * s, bool blocking) {
    // log_debug("service_recv begin");
    message_t * msg = NULL;
    if ( queue_length(s->q) > 0)
        msg = queue_pop_ptr(s->q);

    if( (!msg) && (blocking) ) {
        msg = queue_pop_ptr(s->q);
    }

    return msg;
}

void walk_cb(uv_handle_t* handle, void* arg) {
    if (!uv_is_closing(handle)) {
        uv_close(handle, NULL);
    }
}

int service_stop(service_t * s) {
    uv_stop(s->loop);
    uv_walk(s->loop, walk_cb, NULL);
    uv_run(s->loop, UV_RUN_DEFAULT);
    uv_loop_close(s->loop);
    return 1;
}

int service_free(service_t * s) {
    lua_close(s->L);
    queue_delete(s->q);

    free(s->loop);
    return 1;
}

//
// socket related stuffs
//