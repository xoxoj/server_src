#include "net.h"
#include "log.h"

#include <strings.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

/******************************* connection queue ********************************/

typedef struct conn_queue_item CQ_ITEM;
struct conn_queue_item {
    int fd;
    int type;
    void *data;
    CQ_ITEM *next;
};

typedef struct conn_queue CQ;
struct conn_queue {
    CQ_ITEM *head;
    CQ_ITEM *tail;
    pthread_mutex_t lock;
};

static CQ_ITEM *cqi_freelist;
static pthread_mutex_t cqi_freelist_lock;

#define ITEMS_PER_ALLOC 16

static CQ_ITEM *cqi_new() {
    CQ_ITEM *item = NULL;
    pthread_mutex_lock(&cqi_freelist_lock);
    if (cqi_freelist) {
        item = cqi_freelist;
        cqi_freelist = item->next;
    }
    pthread_mutex_unlock(&cqi_freelist_lock);

    if (NULL == item) {
        int i;

        item = (CQ_ITEM *)malloc(sizeof(CQ_ITEM) * ITEMS_PER_ALLOC);
        if (NULL == item)
            return NULL;

        for (i = 2; i < ITEMS_PER_ALLOC; i++)
            item[i - 1].next = &item[i];

        pthread_mutex_lock(&cqi_freelist_lock);
        item[ITEMS_PER_ALLOC - 1].next = cqi_freelist;
        cqi_freelist = &item[1];
        pthread_mutex_unlock(&cqi_freelist_lock);
    }

    return item;
}

#undef ITEMS_PER_ALLOC

static void cqi_free(CQ_ITEM *item) {
    pthread_mutex_lock(&cqi_freelist_lock);
    item->next = cqi_freelist;
    cqi_freelist = item;
    pthread_mutex_unlock(&cqi_freelist_lock);
}

static void cq_init(CQ *cq) {
    pthread_mutex_init(&cq->lock, NULL);
    cq->head = NULL;
    cq->tail = NULL;
}

static void cq_push(CQ *cq, CQ_ITEM *item) {
    item->next = NULL;

    pthread_mutex_lock(&cq->lock);
    if (NULL == cq->tail)
        cq->head = item;
    else
        cq->tail->next = item;
    cq->tail = item;
    pthread_mutex_unlock(&cq->lock);
}

static CQ_ITEM *cq_pop(CQ *cq) {
    CQ_ITEM *item;

    pthread_mutex_lock(&cq->lock);
    item = cq->head;
    if (NULL != item) {
        cq->head = item->next;
        if (NULL == cq->head)
            cq->tail = NULL;
    }
    pthread_mutex_unlock(&cq->lock);

    return item;
}

/******************************* worker thread ********************************/

static LIBEVENT_DISPATCHER_THREAD dispatcher_thread;
static LIBEVENT_THREAD *threads;
static int num_threads;

void conn_read_cb(struct bufferevent *, void *);
void conn_write_cb(struct bufferevent *, void *);
void conn_event_cb(struct bufferevent *, short, void *);
void connecting_event_cb(struct bufferevent *, short, void *);

static void thread_libevent_process(int fd, short which, void *arg)
{
    LIBEVENT_THREAD *me = (LIBEVENT_THREAD *)arg;
    CQ_ITEM *item;
    char buf[1];

    if (read(fd, buf, 1) != 1)
        merror("can't read from libevent pipe!");

    if ('c' == buf[0]) {
        item = cq_pop(me->new_conn_queue);

        if (NULL != item) {
            if (0 == item->type) {
                listener_info *li = (listener_info *)item->data;
                conn *c = conn_new();
                if (NULL != c) {
                    struct bufferevent* bev = bufferevent_socket_new(me->base, item->fd,
                            BEV_OPT_THREADSAFE | BEV_OPT_CLOSE_ON_FREE | BEV_OPT_DEFER_CALLBACKS);
                    c->bev = bev;
                    if (NULL == bev) {
                        merror("create bufferevent failed!");
                        conn_decref(c);
                    } else {
                        strncpy(c->addrtext, li->addrtext, 32);
                        /* multi-thread write */
                        evbuffer *output = bufferevent_get_output(bev);
                        evbuffer_enable_locking(output, NULL);
                        bufferevent_setcb(bev, conn_read_cb, conn_write_cb, conn_event_cb, c);
                        c->data = li->l;
                        c->user = NULL;
                        c->state = STATE_CONNECTED;
                        c->thread = me;
                        if (li->l->cb.connect)
                            (*(li->l->cb.connect))(c, 1);
                        bufferevent_enable(bev, EV_READ);
                        mdebug("new connection %s established!", c->addrtext);
                    }
                }
                free(item->data);
            } else if (1 == item->type) {
                do {
                    connector *cr = (connector *)item->data;
                    conn *c = conn_new();
                    if (c) {
                        struct bufferevent *bev = bufferevent_socket_new(me->base, -1,
                                BEV_OPT_THREADSAFE | BEV_OPT_CLOSE_ON_FREE | BEV_OPT_DEFER_CALLBACKS);
                        c->bev = bev;
                        if (NULL != bev) {
                            /* multi-thread write */
                            evbuffer *output = bufferevent_get_output(bev);
                            evbuffer_enable_locking(output, NULL);
                            bufferevent_setcb(bev, NULL, NULL, connecting_event_cb, c);
                            c->data = cr;
                            c->user = NULL;
                            c->thread = me;
                            c->state = STATE_NOT_CONNECTED;

                            pthread_mutex_lock(&cr->lock);
                            cr->c = c;
                            pthread_mutex_unlock(&cr->lock);

                            bufferevent_socket_connect(c->bev, cr->sa, cr->socklen);
                        } else {
                        }
                    }

                } while (0);
            }
            cqi_free(item);
        }

    } else if ('k' == buf[0]) {
        event_base_loopbreak(me->base);
    }
}

static void setup_thread(LIBEVENT_THREAD *me) {
    me->base = event_base_new();
    if (NULL == me->base) {
        mfatal("allocate event base failed!");
        exit(1);
    }

    event_set(&me->notify_event, me->notify_receive_fd,
            EV_READ | EV_PERSIST, thread_libevent_process, me);
    event_base_set(me->base, &me->notify_event);

    if (event_add(&me->notify_event, 0) == -1) {
        mfatal("can't monitor libevent notify pipe!");
        exit(1);
    }

    me->new_conn_queue = (struct conn_queue *)malloc(sizeof(struct conn_queue));
    if (NULL == me->new_conn_queue) {
        mfatal("connection queue alloc failed!");
        exit(EXIT_FAILURE);
    }
    cq_init(me->new_conn_queue);
}

static void create_worker(void *(*func)(void *), void *arg, pthread_t *th) {
    pthread_attr_t attr;
    int ret;

    pthread_attr_init(&attr);

    if ((ret = pthread_create(th, &attr, func, arg)) != 0) {
        mfatal("pthread_create failed!");
        exit(1);
    }
}

int net_init()
{
    int ret = evthread_use_pthreads();
    if (0 > ret) {
        return ret;
    }
    return 0;
}

static int init_count = 0;
static pthread_mutex_t init_lock;
static pthread_cond_t init_cond;

static void wait_for_thread_registration(int nthreads) {
    while (init_count < nthreads) {
        pthread_cond_wait(&init_cond, &init_lock);
    }
}

static void register_thread_initialized() {
    pthread_mutex_lock(&init_lock);
    init_count++;
    pthread_cond_signal(&init_cond);
    pthread_mutex_unlock(&init_lock);
}

static void *worker_libevent(void *arg) {
    LIBEVENT_THREAD *me = (LIBEVENT_THREAD *)arg;

    register_thread_initialized();
    event_base_dispatch(me->base);
    return NULL;
}

void thread_init(struct event_base *main_base, int nthreads, pthread_t *th)
{
    int i;

    pthread_mutex_init(&init_lock, NULL);
    pthread_cond_init(&init_cond, NULL);

    pthread_mutex_init(&cqi_freelist_lock, NULL);
    cqi_freelist = NULL;

    dispatcher_thread.base = main_base;
    dispatcher_thread.thread_id = pthread_self();

    threads = (LIBEVENT_THREAD *)calloc(nthreads, sizeof(LIBEVENT_THREAD));
    if (NULL == threads) {
        mfatal("allocate threads failed!");
        exit(1);
    }

    for (i = 0; i < nthreads; i++) {
        int fds[2];
        if (pipe(fds)) {
            mfatal("can't create notify pipe!");
            exit(1);
        }

        threads[i].notify_receive_fd = fds[0];
        threads[i].notify_send_fd = fds[1];

        setup_thread(&threads[i]);
    }

    for (i = 0; i < nthreads; i++) {
        create_worker(worker_libevent, &threads[i], th + i);
    }

    pthread_mutex_lock(&init_lock);
    wait_for_thread_registration(nthreads);
    pthread_mutex_unlock(&init_lock);

    num_threads = nthreads;
}

static int last_thread = -1;

void dispatch_conn_new(int fd, char key, void *arg) {

    if ('k' == key) {

        for (int i = 0; i < num_threads; i++) {
            LIBEVENT_THREAD *thread = threads + i;
            char buf[1];
            buf[0] = key;
            if (write(thread->notify_send_fd, buf, 1) != 1) {
                merror("writing to thread notify pipe failed!");
            }
        }
    } else {
        CQ_ITEM *item = cqi_new();
        if (NULL == item) {
            merror("cqi_new failed!");
            return;
        }

        int tid = (last_thread + 1) % num_threads;

        LIBEVENT_THREAD *thread = threads + tid;

        last_thread = tid;

        item->fd = fd;
        item->type = 0;
        if ('t' == key)
            item->type = 1;
        item->data = arg;

        cq_push(thread->new_conn_queue, item);

        char buf[1] = {'c'};
        if (write(thread->notify_send_fd, buf, 1) != 1) {
            merror("writing to thread notify pipe failed!");
        }
    }
}
