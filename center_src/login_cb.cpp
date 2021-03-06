#include "fwd.h"
#include "gate_info.h"
#include "login.pb.h"

#include "msg_protobuf.h"
#include "cmd.h"
#include "net.h"
#include "log.h"

#include <string.h>

typedef void (*cb)(conn *, unsigned char *, size_t);
static cb cbs[LE_END - LE_BEGIN];

static void user_login_request_cb(conn *c, unsigned char *msg, size_t sz)
{
    login::user_login_request ulr;
    if (0 > msg_body<login::user_login_request>(msg, sz, &ulr)) {
        merror("msg_body<login::user_user_login_request failed!");
        return;
    }

    struct gate_info *info = gate_info_mgr->get_best_gate_incref(ulr.uid());
    if (NULL == info) {
        minfo("get_best_gate_incref failed!");
        return;
    }

    const char *sk = "12345678";
    login::user_session_request usr;
    usr.set_tempid(ulr.tempid());
    usr.set_uid(ulr.uid());
    usr.set_sk(sk);
    conn_write<login::user_session_request>(info->c, eg_user_session_request, &usr);

    gate_info_decref(info);
}

static void login_rpc_cb(conn *c, unsigned char *msg, size_t sz)
{
    msg_head h;
    if (0 != message_head(msg, sz, &h)) {
        merror("message_head failed!");
        return;
    }
    mdebug("login -> center cmd:%d len:%d flags:%d", h.cmd, h.len, h.flags);

    if (h.cmd > LE_BEGIN && h.cmd < LE_END) {
        if (cbs[h.cmd - LE_BEGIN]) {
            (*(cbs[h.cmd - LE_BEGIN]))(c, msg, sz);
        }
    } else {
        merror("login -> center invalid cmd:%d len:%d flags:%d", h.cmd, h.len, h.flags);
        return;
    }
}

static void login_connect_cb(conn *c, int ok)
{
    mdebug("login_connect_cb");
    login::center_reg r;
    r.set_id(1);
    conn_write<login::center_reg>(c, el_center_reg, &r);
}

static void login_disconnect_cb(conn *c)
{
    mdebug("login_disconnect_cb");
}

void login_cb_init(user_callback *cb)
{
    cb->rpc = login_rpc_cb;
    cb->connect = login_connect_cb;
    cb->disconnect = login_disconnect_cb;
    memset(cbs, 0, sizeof(cb) * (LE_END - LE_BEGIN));
    cbs[le_user_login_request - LE_BEGIN] = user_login_request_cb;
}
