#include "net.h"
#include "log.h"

#include <string.h>
#include <stdlib.h>
#include <arpa/inet.h>

connector *connector_new(struct sockaddr *sa, int socklen,
        rpc_cb_func rpc,
        connect_cb_func connect,
        disconnect_cb_func disconnect)
{
    connector *cr = (connector *)malloc(sizeof(connector));
    if (NULL == cr) {
        mfatal("connector alloc failed!");
        return NULL;
    }

    cr->sa = (struct sockaddr *)malloc(socklen);
    if (NULL == cr->sa) {
        mfatal("sockaddr alloc failed!");
        free(cr);
        return NULL;
    }

    cr->cb.type = 'c';
    cr->cb.rpc = rpc;
    cr->cb.connect = connect;
    cr->cb.disconnect = disconnect;

    pthread_mutex_init(&cr->lock, NULL);

    cr->state = STATE_NOT_CONNECTED;
    cr->c = NULL;
    memcpy(cr->sa, sa, socklen);
    cr->socklen = socklen;
    snprintf(cr->addrtext, 32, "%s:%d",
            inet_ntoa(((struct sockaddr_in *)(cr->sa))->sin_addr),
            ntohs(((struct sockaddr_in *)(cr->sa))->sin_port));
    cr->timer = NULL;
    dispatch_conn_new(-1, 't', cr);
    return cr;
}

void connector_free(connector *cr)
{
    free(cr->sa);
    free(cr);
}

int connector_write(connector *cr, unsigned char *msg, size_t sz)
{
    int ret = -1;
    pthread_mutex_lock(&cr->lock);
    if (cr->state == STATE_CONNECTED) {
        ret = conn_write(cr->c, msg, sz);
    }
    pthread_mutex_unlock(&cr->lock);
    return ret;
}