#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stlx/net.h>

#define MAX_RECORDS 2048

struct options {
    int all;        /* -a: listeners too */
    int internal;   /* -i: the protocol state behind each entry */
    int counters;   /* -s: the stack-wide counters */
    int interval_s; /* -w: repeat every interval, 0 for once */
};

static struct stlx_tcp_record g_records[MAX_RECORDS];

static const char* state_name(uint8_t state) {
    switch (state) {
    case STLX_TCP_CLOSED:      return "CLOSED";
    case STLX_TCP_LISTEN:      return "LISTEN";
    case STLX_TCP_SYN_SENT:    return "SYN_SENT";
    case STLX_TCP_SYN_RCVD:    return "SYN_RECV";
    case STLX_TCP_ESTABLISHED: return "ESTABLISHED";
    case STLX_TCP_FIN_WAIT_1:  return "FIN_WAIT1";
    case STLX_TCP_FIN_WAIT_2:  return "FIN_WAIT2";
    case STLX_TCP_CLOSE_WAIT:  return "CLOSE_WAIT";
    case STLX_TCP_CLOSING:     return "CLOSING";
    case STLX_TCP_LAST_ACK:    return "LAST_ACK";
    case STLX_TCP_TIME_WAIT:   return "TIME_WAIT";
    default:                   return "UNKNOWN";
    }
}

static const char* timer_name(uint8_t kind) {
    switch (kind) {
    case STLX_TCP_TIMER_RTO:      return "rto";
    case STLX_TCP_TIMER_ORPHAN:   return "orphan";
    case STLX_TCP_TIMER_TIMEWAIT: return "timewait";
    case STLX_TCP_TIMER_PROBE:    return "probe";
    default:                      return "none";
    }
}

static void format_endpoint(uint32_t addr, uint16_t port, char* buf, size_t size) {
    if (addr == 0 && port == 0) {
        snprintf(buf, size, "*:*");
    } else if (addr == 0) {
        snprintf(buf, size, "*:%u", port);
    } else {
        snprintf(buf, size, "%u.%u.%u.%u:%u",
                 (addr >> 24) & 0xFF, (addr >> 16) & 0xFF, (addr >> 8) & 0xFF, addr & 0xFF, port);
    }
}

static void print_record(const struct stlx_tcp_record* r, const struct options* opts) {
    char local[24];
    char remote[24];
    format_endpoint(r->local_addr, r->local_port, local, sizeof(local));
    format_endpoint(r->remote_addr, r->remote_port, remote, sizeof(remote));

    int listener = r->kind == STLX_TCP_KIND_LISTENER;
    uint32_t recv_q = listener ? r->accepted : r->rcv_queued;
    uint32_t send_q = listener ? r->backlog : r->snd_queued;
    printf("tcp   %6u %6u %-22s %-22s %s\r\n", recv_q, send_q, local, remote, state_name(r->state));

    if (!opts->internal) {
        return;
    }

    if (listener) {
        printf("      iface %s backlog %u requests %u accepted %u\r\n",
               r->iface[0] ? r->iface : "any", r->backlog, r->requests, r->accepted);
        return;
    }

    printf("      iface %s snd_una %u snd_nxt %u rcv_nxt %u snd_wnd %u rcv_wnd %u\r\n",
           r->iface, r->snd_una, r->snd_nxt, r->rcv_nxt, r->snd_wnd, r->rcv_wnd);
    printf("      mss %u wscale %u/%u%s%s%s%s%s retrans %u/%u timer %s %ums",
           r->snd_mss, r->snd_wscale, r->rcv_wscale,
           (r->flags & STLX_TCP_TIMESTAMPS) ? " ts" : "",
           (r->flags & STLX_TCP_SACK) ? " sack" : "",
           (r->flags & STLX_TCP_ORPHANED) ? " orphaned" : "",
           (r->flags & STLX_TCP_FIN_SENT) ? " fin_sent" : "",
           (r->flags & STLX_TCP_FIN_RCVD) ? " fin_rcvd" : "",
           r->retransmits, r->total_retransmits, timer_name(r->timer_kind), r->timer_ms);

    if (r->error) {
        printf(" error %d", r->error);
    }

    printf("\r\n");
    if (r->kind != STLX_TCP_KIND_CONNECTION) {
        return;
    }

    printf("      cwnd %u unacked %u rtt %u.%03u/%u.%03u ms rto %u ms backoff %u rcvbuf %u sndbuf %u ooo %u/%u\r\n",
           r->cwnd, r->unacked, r->srtt_us / 1000, r->srtt_us % 1000, r->rttvar_us / 1000, r->rttvar_us % 1000,
           r->rto_ms, r->backoff, r->rcv_buf, r->snd_buf, r->ooo_packets, r->ooo_bytes);
}

static void print_counters(const struct stlx_tcp_counters* c) {
    printf("Tcp:\r\n");
    printf("    %u listeners, %u requests, %u connections, %u time-wait\r\n",
           c->listeners, c->requests, c->connections, c->timewaits);
    printf("    %llu segments received\r\n", (unsigned long long)c->segments_in);
    printf("    %llu segments sent\r\n", (unsigned long long)c->segments_out);
    printf("    %llu segments retransmitted\r\n", (unsigned long long)c->retransmits);
    printf("    %llu resets sent\r\n", (unsigned long long)c->rsts_sent);
    printf("    %llu resets received\r\n", (unsigned long long)c->rsts_received);
    printf("    %llu bad checksums\r\n", (unsigned long long)c->checksum_failures);
    printf("    %llu listen drops\r\n", (unsigned long long)c->listen_drops);
    printf("    %llu PAWS drops\r\n", (unsigned long long)c->paws_drops);
    printf("    %llu challenge acks\r\n", (unsigned long long)c->challenge_acks);
}

static int show(const struct options* opts) {
    struct stlx_tcp_info info;
    memset(&info, 0, sizeof(info));
    if (stlx_tcp_get_info(g_records, MAX_RECORDS, &info) != 0) {
        printf("netstat: failed to query the TCP tables\r\n");
        return 1;
    }

    if (opts->counters) {
        print_counters(&info.counters);
        return 0;
    }

    printf("Proto Recv-Q Send-Q %-22s %-22s State\r\n", "Local Address", "Foreign Address");
    for (uint32_t i = 0; i < info.count; i++) {
        if (g_records[i].kind == STLX_TCP_KIND_LISTENER && !opts->all) {
            continue;
        }

        print_record(&g_records[i], opts);
    }

    if (info.total > info.count) {
        printf("(%u of %u entries shown)\r\n", info.count, info.total);
    }

    return 0;
}

static void usage(void) {
    printf("Usage: netstat [-a] [-i] [-s] [-w [seconds]]\r\n");
    printf("  -a  show listeners as well as connections\r\n");
    printf("  -i  show the protocol state behind each entry\r\n");
    printf("  -s  show the stack-wide counters\r\n");
    printf("  -w  repeat every second, or every given number of seconds\r\n");
}

static int parse_options(int argc, char** argv, struct options* opts) {
    memset(opts, 0, sizeof(*opts));
    for (int i = 1; i < argc; i++) {
        const char* arg = argv[i];
        if (arg[0] != '-' || arg[1] == '\0') {
            return -1;
        }

        for (const char* c = arg + 1; *c; c++) {
            switch (*c) {
            case 't':
            case 'n':
                break;
            case 'a':
                opts->all = 1;
                break;
            case 'i':
                opts->internal = 1;
                break;
            case 's':
                opts->counters = 1;
                break;
            case 'w':
                opts->interval_s = 1;
                if (i + 1 < argc && argv[i + 1][0] != '-') {
                    opts->interval_s = atoi(argv[++i]);
                    if (opts->interval_s <= 0) {
                        return -1;
                    }
                }
                break;
            default:
                return -1;
            }
        }
    }

    return 0;
}

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IONBF, 0);

    struct options opts;
    if (parse_options(argc, argv, &opts) != 0) {
        usage();
        return 1;
    }

    if (opts.interval_s == 0) {
        return show(&opts);
    }

    for (;;) {
        printf("\033[2J\033[H");
        if (show(&opts) != 0) {
            return 1;
        }

        struct timespec wait = {opts.interval_s, 0};
        nanosleep(&wait, NULL);
    }
}
