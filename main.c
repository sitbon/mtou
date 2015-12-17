#include "local.h"
#include "mtou.yucc"

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <sys/mman.h>
#include <pthread.h>
#include <time.h>
#include <signal.h>
#include <sys/wait.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>

#define MAX_QUEUE       16

typedef struct {
    size_t len;
    buf_t buf[];
} msg_t;

typedef struct {
    msg_t *msg[MAX_QUEUE];
    size_t cnt;
    pthread_mutex_t lock;
    pthread_cond_t can_produce;
    pthread_cond_t can_consume;
    pthread_t thr_prod;
    pthread_t thr_cons;
} queue_t;

typedef struct {
    int fd_mc;
    int fd_uc;
    struct sockaddr_in addr_out;
    struct sockaddr_in addr_mc;
    bool verbose;
    bool reverse;
} param_t;

static int udp_addr(struct addrinfo **addr, const char *host, const char *service, bool passive);

static void start(param_t *param);
static void stop(void);
static void *rx_thread(void *param);
static void *tx_thread(void *param);


static queue_t queue = {
        .cnt = 0,
        .lock = PTHREAD_MUTEX_INITIALIZER,
        .can_produce = PTHREAD_COND_INITIALIZER,
        .can_consume = PTHREAD_COND_INITIALIZER
};


int main(int argc, char *argv[])
{
    yuck_t argp[1];
    struct addrinfo *addr_info = NULL;
    struct sockaddr_in addr_bind;
    struct sockaddr_in addr_mcast;
    struct sockaddr_in addr_iface;
    const struct sockaddr_in addr_local = { .sin_family = AF_INET };
    struct sockaddr_in addr_out;
    struct ip_mreq mreq;
    param_t param;
    int fd_mc, fd_uc, opt;

    yuck_parse(argp, argc, argv);

    if (argp->iface_arg && !strlen(argp->iface_arg)) {
        fputs("interface is required\n", stderr);
        exit(1);
    }

    if (!argp->in_arg || !strlen(argp->in_arg)) {
        fputs("input is required\n", stderr);
        exit(1);
    }

    if (!argp->port_arg || !strlen(argp->port_arg)) {
        fputs("port is required\n", stderr);
        exit(1);
    }

    if (!argp->out_arg || !strlen(argp->out_arg)) {
        fputs("output is required\n", stderr);
        exit(1);
    }

    if (!argp->out_port_arg || !strlen(argp->out_port_arg)) {
        argp->out_port_arg = argp->port_arg;
    }

    //printf("%s -> %s :: %s\n", argp->in_arg, argp->out_arg, argp->port_arg);

    if (udp_addr(&addr_info, argp->out_arg, argp->out_port_arg, false)) {
        exit(1);
    }

    addr_out = *(struct sockaddr_in *)addr_info->ai_addr;

    if (udp_addr(&addr_info, argp->in_arg, argp->port_arg, false)) {
        exit(1);
    }

    addr_bind = addr_mcast = *(struct sockaddr_in *)addr_info->ai_addr;

    if (udp_addr(&addr_info, "0.0.0.0", argp->port_arg, false)) {
        exit(1);
    }

    addr_iface = *(struct sockaddr_in *)addr_info->ai_addr;

    if (argp->iface_arg) {
        struct ifaddrs *addr;

        if (getifaddrs(&addr)) {
            perror("getifaddrs()");
            exit(1);
        }

        while (addr) {
            if (!addr->ifa_addr || addr->ifa_addr->sa_family != AF_INET) goto next;
            if (strcmp(argp->iface_arg, addr->ifa_name)) goto next;

            const in_addr_t in_addr = ntohl(((struct sockaddr_in *)addr->ifa_addr)->sin_addr.s_addr);
            if (in_addr == INADDR_LOOPBACK || in_addr == INADDR_NONE || in_addr == INADDR_ANY) goto next;

            addr_iface.sin_addr = ((struct sockaddr_in *)addr->ifa_addr)->sin_addr;
            break;

            next:
            addr = addr->ifa_next;
        }

        if (!addr) {
            fputs("no suitable interfaces found", stderr);
            exit(1);
        }
    }

    char addr_iface_str[64], addr_mcast_str[64], addr_out_str[64];
    strcpy(addr_iface_str, inet_ntoa(addr_iface.sin_addr));
    strcpy(addr_mcast_str, inet_ntoa(addr_mcast.sin_addr));
    strcpy(addr_out_str, inet_ntoa(addr_out.sin_addr));

    if (argp->reverse_flag) {
        printf("%s:%u -> <%s> %s:%u\n",
                addr_out_str, ntohs(addr_out.sin_port),
                addr_iface_str, addr_mcast_str, ntohs(addr_mcast.sin_port)
        );
    } else {
        printf("<%s> %s:%u -> %s:%u\n",
                addr_iface_str, addr_mcast_str, ntohs(addr_mcast.sin_port),
                addr_out_str, ntohs(addr_out.sin_port)
        );
    }

    if ((fd_mc = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("socket(fd_mc)");
        exit(1);
    }

    if ((fd_uc = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) < 0) {
        perror("socket(fd_uc)");
        exit(1);
    }

    opt = 1;
    setsockopt(fd_mc, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(fd_mc, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
    setsockopt(fd_uc, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(fd_uc, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

    if (argp->reverse_flag) {
        if (bind(fd_mc, (struct sockaddr *)&addr_iface, sizeof(addr_iface))) {
            perror("bind(fd_mc)");
            exit(1);
        }

        if (bind(fd_uc, (struct sockaddr *)&addr_out, sizeof(addr_out))) {
            perror("bind(fd_uc)");
            exit(1);
        }
    } else {
        if (bind(fd_mc, (struct sockaddr *)&addr_bind, sizeof(addr_bind))) {
            perror("bind(fd_mc)");
            exit(1);
        }

        /*if (bind(fd_uc, (struct sockaddr *)&addr_local, sizeof(addr_local))) {
            perror("bind(fd_uc)");
            exit(1);
        }*/
    }

    /*if (connect(fd_uc, (struct sockaddr *)&addr_out, sizeof(addr_out))) {
        perror("connect");
        exit(1);
    }*/

    mreq.imr_interface = addr_iface.sin_addr;
    mreq.imr_multiaddr = addr_mcast.sin_addr;

    if (setsockopt(fd_mc, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq))) {
        perror("setsockopt");
        exit(1);
    }

    if (argp->reverse_flag) {
        param.fd_mc = fd_uc;
        param.fd_uc = fd_mc;
    } else {
        param.fd_mc = fd_mc;
        param.fd_uc = fd_uc;
    }

    param.addr_out = addr_out;
    param.addr_mc = addr_mcast;
    param.verbose = argp->verbose_flag > 0;
    param.reverse = argp->reverse_flag > 0;

    yuck_free(argp);
    start(&param);
    while (getchar() != '\n');
    stop();
    return 0;
}

static void start(param_t *param)
{
    pthread_create(&queue.thr_prod, NULL, rx_thread, param);
    pthread_create(&queue.thr_cons, NULL, tx_thread, param);
}

static void stop(void)
{
    if (queue.thr_prod) {
        pthread_cancel(queue.thr_prod);
        pthread_join(queue.thr_prod, NULL);
    }

    if (queue.thr_cons) {
        pthread_cancel(queue.thr_cons);
        pthread_join(queue.thr_cons, NULL);
    }
}

static void *rx_thread(void *param)
{
    const param_t *const p = (param_t *)param;
    const int fd = p->fd_mc;
    const bool verbose = p->verbose;
    msg_t *qmsg;
    buf_t buf[MAX_DGRAM_LEN];
    struct sockaddr_in src;
    socklen_t src_len;
    ssize_t len;

    set_thread_prio(true);

    while (true) {
        src_len = sizeof(src);
        len = recvfrom(fd, buf, MAX_DGRAM_LEN, MSG_WAITALL, (struct sockaddr *)&src, &src_len);

        if (len <= 0) {
            if (errno == ENOTCONN) {
                sleep(1);
                continue;
            }

            if (errno) perror("recvfrom");
            else fprintf(stderr, "rx: len=%li\n", len);
            sleep(1);
            continue;
        }

        if (verbose) {
            if (src_len >= sizeof(src)) printf("[%s:%u] ", inet_ntoa(src.sin_addr), src.sin_port);
            print_hex(stdout, buf, (u32)len, false);
        }

        qmsg = malloc(sizeof(msg_t) + len);
        qmsg->len = (size_t)len;
        memcpy(qmsg->buf, buf, qmsg->len);
        pthread_mutex_lock(&queue.lock);

        while (queue.cnt >= MAX_QUEUE)
            pthread_cond_wait(&queue.can_produce, &queue.lock);

        queue.msg[queue.cnt] = qmsg;
        queue.cnt++;

        pthread_cond_signal(&queue.can_consume);
        pthread_mutex_unlock(&queue.lock);
    }

    return NULL;
}

static void *tx_thread(void *param)
{
    const param_t *const p = (param_t *)param;
    const int fd = p->fd_uc;
    const struct sockaddr_in *const addr = p->reverse ? &p->addr_mc : &p->addr_out;
    msg_t *qmsg;
    ssize_t len;

    set_thread_prio(false);

    while (true) {
        pthread_mutex_lock(&queue.lock);

        while (!queue.cnt)
            pthread_cond_wait(&queue.can_consume, &queue.lock);

        qmsg = queue.msg[--queue.cnt];
        queue.msg[queue.cnt + 1] = NULL;
        pthread_cond_signal(&queue.can_produce);
        pthread_mutex_unlock(&queue.lock);

        if (!qmsg) {
            fprintf(stderr, "tx: msg == NULL\n");
            continue;
        }

        send:
        len = sendto(fd, qmsg->buf, qmsg->len, MSG_WAITALL, (struct sockaddr *)addr, sizeof(*addr));

        if (len != qmsg->len) {
            if (len < 0 && errno == EINTR) goto send;
            if (errno) perror("sendto");
            else fprintf(stderr, "tx: len=%li exp=%li\n", len, qmsg->len);
        }

        free(qmsg);
    }

    return NULL;
}

static int udp_addr(struct addrinfo **addr, const char *host, const char *service, bool passive)
{
    struct addrinfo hints = {
            .ai_family = AF_INET,
            .ai_socktype = SOCK_DGRAM,
            .ai_protocol = IPPROTO_UDP,
            .ai_flags = AI_ADDRCONFIG
    };
    
    if (passive) hints.ai_flags |= AI_PASSIVE;

    const int err = getaddrinfo(host, service, &hints, addr);

    if (err) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(err));
    }

    return err;
}
