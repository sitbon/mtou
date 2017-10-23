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
#include <semaphore.h>

#define MAX_QUEUE       2048

typedef struct {
    size_t len;
    buf_t buf[];
} msg_t;

typedef struct {
    msg_t *msg[MAX_QUEUE];
    size_t cnt;
    pthread_mutex_t lock;
    sem_t sem_produce;
    sem_t sem_consume;
    pthread_t thr_prod;
    pthread_t thr_cons;
} queue_t;

typedef struct {
    int fd_rx;
    int fd_tx;
    struct sockaddr_in addr_tx;
    bool verbose;
} param_t;

static int udp_addr(struct addrinfo **addr, const char *host, const char *service, bool passive);

static void start(param_t *param);
static void stop(void);
static void *rx_thread(void *param);
static void *tx_thread(void *param);


static queue_t queue = {
        .cnt = 0,
        .lock = PTHREAD_MUTEX_INITIALIZER,
};


int main(int argc, char *argv[])
{
    yuck_t argp[1];
    struct addrinfo *addr_info = NULL;
    struct sockaddr_in addr_rx, addr_rx_if;
    struct sockaddr_in addr_tx, addr_tx_if;
    struct ip_mreq mreq;
    param_t param;
    int fd_rx, fd_tx;

    yuck_parse(argp, argc, argv);

    if (argp->iface_arg && !strlen(argp->iface_arg)) {
        fputs("input interface cannot be empty\n", stderr);
        exit(1);
    }

    if ((!argp->in_arg || !strlen(argp->in_arg)) && !argp->iface_arg) {
        fputs("input is required when input interface not given\n", stderr);
        exit(1);
    }

    if (!argp->port_arg || !strlen(argp->port_arg)) {
        fputs("port is required\n", stderr);
        exit(1);
    }

    if (argp->oface_arg && !strlen(argp->oface_arg)) {
        fputs("output interface cannot be empty\n", stderr);
    }

    if (!argp->out_arg || !strlen(argp->out_arg)) {
        fputs("output is required\n", stderr);
        exit(1);
    }

    if (!argp->out_port_arg || !strlen(argp->out_port_arg)) {
        argp->out_port_arg = argp->port_arg;
    }


    if (udp_addr(&addr_info, "0.0.0.0", argp->port_arg, false)) {
        exit(1);
    }

    addr_tx_if = addr_rx_if = *(struct sockaddr_in *)addr_info->ai_addr;
    addr_tx_if.sin_port = 0;

    if (argp->iface_arg || argp->oface_arg) {
        struct ifaddrs *addr;

        if (getifaddrs(&addr)) {
            perror("getifaddrs()");
            exit(1);
        }

        while (addr) {
            if (!addr->ifa_addr || addr->ifa_addr->sa_family != AF_INET) goto next;

            const in_addr_t in_addr = ntohl(((struct sockaddr_in *)addr->ifa_addr)->sin_addr.s_addr);

            if (in_addr == INADDR_NONE || in_addr == INADDR_ANY) goto next;

            if (argp->iface_arg && !strcmp(argp->iface_arg, addr->ifa_name)) {
                addr_rx_if.sin_addr = ((struct sockaddr_in *) addr->ifa_addr)->sin_addr;
            }

            if (argp->oface_arg && !strcmp(argp->oface_arg, addr->ifa_name)) {
                addr_tx_if.sin_addr = ((struct sockaddr_in *) addr->ifa_addr)->sin_addr;
            }

            next:
            addr = addr->ifa_next;
        }

        if (argp->iface_arg && !addr_rx_if.sin_addr.s_addr) {
            fputs("no matching suitable input interface found\n", stderr);
            exit(1);
        }

        if (argp->oface_arg && !addr_tx_if.sin_addr.s_addr) {
            fputs("no matching suitable output interface found\n", stderr);
            exit(1);
        }
    }

    if (argp->in_arg) {
        if (udp_addr(&addr_info, argp->in_arg, argp->port_arg, false)) {
            exit(1);
        }

        addr_rx = *(struct sockaddr_in *) addr_info->ai_addr;
    } else {
        addr_rx = addr_rx_if;
    }

    if (udp_addr(&addr_info, argp->out_arg, argp->out_port_arg, false)) {
        exit(1);
    }

    addr_tx = *(struct sockaddr_in *)addr_info->ai_addr;

    if ((fd_rx = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) < 0) {
        perror("socket(rx)");
        exit(1);
    }

    if ((fd_tx = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) < 0) {
        perror("socket(tx)");
        exit(1);
    }

    int opt = 1;
    setsockopt(fd_rx, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(fd_rx, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
    setsockopt(fd_tx, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // [Multicast] sender binds to outgoing interface

    if (bind(fd_tx, (struct sockaddr *)&addr_tx_if, sizeof(addr_tx_if))) {
        perror("bind(tx)");
        exit(1);
    }

    socklen_t socklen = sizeof(addr_tx_if);
    getsockname(fd_tx, (struct sockaddr *) &addr_tx_if, &socklen);

    // [Multicast] receiver binds to source address

    if (bind(fd_rx, (struct sockaddr *)&addr_rx, sizeof(addr_rx))) {
        perror("bind(rx)");
        exit(1);
    }

    if (IN_MULTICAST(ntohl(addr_rx.sin_addr.s_addr))) {
        mreq.imr_interface = addr_rx_if.sin_addr;
        mreq.imr_multiaddr = addr_rx.sin_addr;

        if (setsockopt(fd_rx, SOL_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq))) {
            perror("setsockopt(rx/IP_ADD_MEMBERSHIP)");
            exit(1);
        }

    } else if (argp->iface_arg && argp->in_arg) {
        fputs("ignoring input interface\n", stderr);
    }

    if (IN_MULTICAST(ntohl(addr_tx.sin_addr.s_addr))) {
        mreq.imr_interface = addr_tx_if.sin_addr;
        mreq.imr_multiaddr = addr_tx.sin_addr;

        if (setsockopt(fd_tx, SOL_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq))) {
            perror("setsockopt(tx/IP_ADD_MEMBERSHIP)");
            exit(1);
        }
    }

    param.fd_rx = fd_rx;
    param.fd_tx = fd_tx;
    param.addr_tx = addr_tx;
    param.verbose = argp->verbose_flag > 0;

    char addr_rx_if_str[64], addr_tx_if_str[64], addr_rx_str[64], addr_tx_str[64];
    strcpy(addr_rx_if_str, inet_ntoa(addr_rx_if.sin_addr));
    strcpy(addr_tx_if_str, inet_ntoa(addr_tx_if.sin_addr));
    strcpy(addr_rx_str, inet_ntoa(addr_rx.sin_addr));
    strcpy(addr_tx_str, inet_ntoa(addr_tx.sin_addr));

    printf("<%s> %s:%u -> <%s:%u> %s:%u\n",
           addr_rx_if_str, addr_rx_str, ntohs(addr_rx.sin_port),
           addr_tx_if_str, ntohs(addr_tx_if.sin_port), addr_tx_str, ntohs(addr_tx.sin_port)
    );

    yuck_free(argp);
    start(&param);
    while (getchar() != '\n');
    stop();
    return 0;
}

static void start(param_t *param)
{
    sem_init(&queue.sem_produce, 0, MAX_QUEUE);
    sem_init(&queue.sem_consume, 0, 0);

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
    const int fd = p->fd_rx;
    const bool verbose = p->verbose;
    msg_t *qmsg;
    buf_t buf[MAX_DGRAM_LEN];
    struct sockaddr_in src;
    socklen_t src_len;
    ssize_t len;

    set_thread_prio(true);

    while (true) {
        src_len = sizeof(src);
        len = recvfrom(fd, buf, MAX_DGRAM_LEN, 0, (struct sockaddr *)&src, &src_len);

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
            if (src_len >= sizeof(src)) printf("[%s:%u] ", inet_ntoa(src.sin_addr), ntohs(src.sin_port));
            print_hex(stdout, buf, (u32)len, false);
            fflush(stdout);
        }

        qmsg = malloc(sizeof(msg_t) + len);
        qmsg->len = (size_t)len;
        memcpy(qmsg->buf, buf, qmsg->len);

        sem_wait(&queue.sem_produce);
        pthread_mutex_lock(&queue.lock);

        queue.msg[queue.cnt++] = qmsg;

        pthread_mutex_unlock(&queue.lock);
        sem_post(&queue.sem_consume);
    }

    return NULL;
}

static void *tx_thread(void *param)
{
    const param_t *const p = (param_t *)param;
    const int fd = p->fd_tx;
    const struct sockaddr_in *const addr = &p->addr_tx;
    msg_t *qmsg;
    ssize_t len;

    set_thread_prio(false);

    while (true) {
        sem_wait(&queue.sem_consume);
        pthread_mutex_lock(&queue.lock);

        qmsg = queue.msg[--queue.cnt];
        queue.msg[queue.cnt + 1] = NULL;

        pthread_mutex_unlock(&queue.lock);
        sem_post(&queue.sem_produce);

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
