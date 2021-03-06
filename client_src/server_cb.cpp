#include "fwd.h"
#include "login.pb.h"

#include "msg_protobuf.h"
#include "cmd.h"
#include "logic_thread.h"
#include "net.h"
#include "log.h"

typedef void (*cb)(conn *, unsigned char *, size_t);
static cb cbs[SC_END - SC_BEGIN];

static void login_reply_cb(conn *c, unsigned char *msg, size_t sz)
{
    login::login_reply lr;
    msg_body<login::login_reply>(msg, sz, &lr);
    mdebug("login_reply_cb err:%d", lr.err());

    login::login_request r;
    r.set_account("abc");
    r.set_passwd("xxx");
    conn_write<login::login_request>(c, cl_login_request, &r);
}

static void connect_reply_cb(conn *c, unsigned char *msg, size_t sz)
{
    login::connect_reply cr;
    msg_body<login::connect_reply>(msg, sz, &cr);
    mdebug("connect_reply_cb err:%d", cr.err());
}

static void logic_server_rpc_cb(conn *c, unsigned char *msg, size_t sz)
{
    msg_head h;
    if (0 != message_head(msg, sz, &h)) {
        merror("message_head failed!");
        return;
    }

    mdebug("server -> client cmd:%d len:%d flags:%d", h.cmd, h.len, h.flags);
    if (h.cmd > SC_BEGIN && h.cmd < SC_END) {
        if (cbs[h.cmd - SC_BEGIN])
            (*(cbs[h.cmd - SC_BEGIN]))(c, msg, sz);
    } else {
        merror("server -> client invalid cmd:%d len:%d flags:%d", h.cmd, h.len, h.flags);
        conn_decref(c);
    }
}

static void logic_server_connect_cb(conn *c, int ok)
{
    mdebug("logic_server_connect_cb ok:%d", ok);
    login::login_request lr;
    lr.set_account("abc");
    lr.set_passwd("xxx");
    conn_write<login::login_request>(c, cl_login_request, &lr);
}

static void logic_server_disconnect_cb(conn *c)
{
    mdebug("logic_server_disconnect_cb");
}

static void server_rpc_cb(conn *c, unsigned char *msg, size_t sz)
{
    msg_head h;
    if (0 != message_head(msg, sz, &h)) {
        return;
    }
    logic_thread_add_rpc_event(&logic, c, msg, sz);
}

static void server_connect_cb(conn *c, int ok)
{
    logic_thread_add_connect_event(&logic, c, ok);
}

static void server_disconnect_cb(conn *c)
{
    logic_thread_add_disconnect_event(&logic, c);
}

void server_cb_init(user_callback *cb, user_callback *cb2)
{
    cb->rpc = server_rpc_cb;
    cb->connect = server_connect_cb;
    cb->disconnect = server_disconnect_cb;
    cb2->rpc = logic_server_rpc_cb;
    cb2->connect = logic_server_connect_cb;
    cb2->disconnect = logic_server_disconnect_cb;
    memset(cbs, 0, sizeof(cb) * (SC_END - SC_BEGIN));
    cbs[lc_login_reply - SC_BEGIN] = login_reply_cb;
    cbs[gc_connect_reply - SC_BEGIN] = connect_reply_cb;
}
