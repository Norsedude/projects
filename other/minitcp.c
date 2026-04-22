/* mini-tcp.c
   UDP reliable transport
   one program: -S sender, -R receiver
   Sequence Numbers and ACKs
    Retransmission timeouts
    Go-Back-N window
    Basic Checksum
    Simulated loss and reordering
    Mateo Ortega
*/

#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>



// Maximum bytes of file data per packet (not including header)
#ifndef MTCP_PAYLOAD_SIZE
#define MTCP_PAYLOAD_SIZE 1000
#endif

// Sliding window size: max unacknowledged packets sender can have in flight
#ifndef MTCP_WINDOW_SIZE
#define MTCP_WINDOW_SIZE 10
#endif

// Time in milliseconds before sender retransmits unacknowledged packets
#ifndef MTCP_TIMEOUT_MS
#define MTCP_TIMEOUT_MS 300
#endif

// Packet types
enum {
    MTCP_TYPE_DATA = 1,
    MTCP_TYPE_ACK = 2,
    MTCP_TYPE_FIN = 3,
    MTCP_TYPE_FINACK = 4,
};

#define MTCP_HEADER_SIZE 10
#define MTCP_MAX_PACKET_SIZE (MTCP_HEADER_SIZE + MTCP_PAYLOAD_SIZE)

typedef struct {
    uint8_t type;              // Packet type
    uint16_t length;           // Payload length in bytes
    uint32_t seq;              // Sequence number (or ACK value for ACK/FINACK)
    const uint8_t *payload;    // Pointer to payload data in original buffer
} mtcp_parsed_t;

static void die(const char *msg) {
    perror(msg);
    exit(EXIT_FAILURE);
}

static void diex(const char *msg) {
    fprintf(stderr, "%s\n", msg);
    exit(EXIT_FAILURE);
}

static uint64_t now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        die("clock_gettime");
    }
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000ULL);
}

// Standard 16-bit Internet checksum (one's complement sum)
static uint16_t checksum16(const uint8_t *data, size_t len) {
    uint32_t sum = 0;

    for (size_t i = 0; i + 1 < len; i += 2) {
        uint16_t word = (uint16_t)((data[i] << 8) | data[i + 1]);
        sum += word;
    }

    if (len & 1) {
        uint16_t last = (uint16_t)(data[len - 1] << 8);
        sum += last;
    }

    // Fold carries
    while (sum >> 16) {
        sum = (sum & 0xFFFFu) + (sum >> 16);
    }

    return (uint16_t)(~sum & 0xFFFFu);
}

static size_t build_packet(uint8_t type, uint32_t seq, const uint8_t *payload, uint16_t length,
                           uint8_t *out, size_t out_cap) {
    if (length > MTCP_PAYLOAD_SIZE) {
        diex("payload too large");
    }
    if (MTCP_HEADER_SIZE + (size_t)length > out_cap) {
        diex("output buffer too small");
    }

    out[0] = type;
    out[1] = 0;

    uint16_t len_n;
    uint32_t seq_n;

    len_n = htons(length);
    seq_n = htonl(seq);

    memcpy(out + 2, &len_n, sizeof(len_n));
    memcpy(out + 4, &seq_n, sizeof(seq_n));

    uint16_t csum0 = 0;
    memcpy(out + 8, &csum0, sizeof(csum0));

    if (length > 0 && payload != NULL) {
        memcpy(out + MTCP_HEADER_SIZE, payload, length);
    }

    size_t pkt_len = MTCP_HEADER_SIZE + (size_t)length;
    uint16_t csum = checksum16(out, pkt_len);
    uint16_t csum_n = htons(csum);
    memcpy(out + 8, &csum_n, sizeof(csum_n));

    return pkt_len;
}

static bool parse_packet(const uint8_t *buf, size_t len, mtcp_parsed_t *out) {
    if (len < MTCP_HEADER_SIZE) {
        return false;
    }

    // Verify checksum: if correct, checksum16 over whole packet == 0
    if (checksum16(buf, len) != 0) {
        return false;
    }

    // Extract length and sequence number from wire format 
    uint16_t plen_n;
    uint32_t seq_n;
    memcpy(&plen_n, buf + 2, sizeof(plen_n));
    memcpy(&seq_n, buf + 4, sizeof(seq_n));

    uint16_t plen = ntohs(plen_n);
    uint32_t seq = ntohl(seq_n);

    // Validate payload length
    if (plen > MTCP_PAYLOAD_SIZE) {
        return false;
    }

    // Enforce exact packet length (no extra padding)
    if (MTCP_HEADER_SIZE + (size_t)plen != len) {
        return false;
    }

    // Fill output structure
    out->type = buf[0];
    out->length = plen;
    out->seq = seq;
    out->payload = (plen > 0) ? (buf + MTCP_HEADER_SIZE) : NULL;

    return true;
}

typedef struct {
    bool sender;           // Run in sender mode (-S)
    bool receiver;         // Run in receiver mode (-R)
    const char *file_path; // File to send or receive
    const char *ip;        // Receiver IP address (sender mode only)
    int port;              // UDP port
    int loss_percent;      // Simulated packet loss % (0-100, sender only)
    int reorder_percent;   // Simulated reordering % (0-100, sender only)
} config_t;

static void usage(const char *argv0) {
    fprintf(stderr,
            "Usage:\n"
            "  Sender:   %s -S -f file-to-transfer -l loss%% -r reorder%% -p receiver_port -i receiver_ip\n"
            "  Receiver: %s -R -p udp-port -f file_to_receive\n"
            "\n"
            "Notes:\n"
            "  -l and -r are integers 0..100 (sender only).\n"
            "  -p is required in both modes.\n"
            "  -f is required in both modes.\n",
            argv0, argv0);
}

static int clamp_percent(int x) {
    // Constrain percentage to 0-100 range
    if (x < 0) return 0;
    if (x > 100) return 100;
    return x;
}

static config_t parse_args(int argc, char **argv) {
    config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.port = -1;
    cfg.loss_percent = 0;
    cfg.reorder_percent = 0;

    int opt;
    while ((opt = getopt(argc, argv, "SRf:l:r:p:i:")) != -1) {
        switch (opt) {
            case 'S':
                cfg.sender = true;
                break;
            case 'R':
                cfg.receiver = true;
                break;
            case 'f':
                cfg.file_path = optarg;
                break;
            case 'l':
                cfg.loss_percent = clamp_percent(atoi(optarg));
                break;
            case 'r':
                cfg.reorder_percent = clamp_percent(atoi(optarg));
                break;
            case 'p':
                cfg.port = atoi(optarg);
                break;
            case 'i':
                cfg.ip = optarg;
                break;
            default:
                usage(argv[0]);
                exit(EXIT_FAILURE);
        }
    }

    // Require exactly one mode 
    if (cfg.sender == cfg.receiver) {
        diex("You must specify exactly one of -S (sender) or -R (receiver)");
    }

    // Always require file and port
    if (!cfg.file_path) {
        diex("Missing -f <file>");
    }
    if (cfg.port <= 0 || cfg.port > 65535) {
        diex("Missing/invalid -p <port>");
    }

    // Mode-specific validation
    if (cfg.sender) {
        if (!cfg.ip) {
            diex("Sender mode requires -i <receiver-ip-address>");
        }
    } else {
        // receiver
        if (cfg.ip) {
            fprintf(stderr, "Warning: -i ignored in receiver mode\n");
        }
        if (cfg.loss_percent != 0 || cfg.reorder_percent != 0) {
            fprintf(stderr, "Warning: -l/-r ignored in receiver mode\n");
        }
    }

    return cfg;
}

typedef struct {
    int loss_percent;                    // Probability to drop a packet (0-100)
    int reorder_percent;                 // Probability to hold and reorder (0-100)
    bool held_valid;                     // Is there a held packet?
    uint8_t held_pkt[MTCP_MAX_PACKET_SIZE]; // Buffer for held packet
    size_t held_len;                     // Length of held packet
} impairment_t;

static bool should_drop(int loss_percent) {
    if (loss_percent <= 0) return false;
    int r = rand() % 100; // 0..99
    return r < loss_percent;
}

static int send_maybe_drop(int sockfd, const uint8_t *buf, size_t len, int loss_percent) {
    if (should_drop(loss_percent)) {
        // dropped
        return 0;
    }

    ssize_t n = send(sockfd, buf, len, 0);
    if (n < 0) {
        return -1;
    }
    if ((size_t)n != len) {
        errno = EMSGSIZE;
        return -1;
    }
    return 0;
}

static int impair_send(int sockfd, impairment_t *imp, const uint8_t *buf, size_t len, bool allow_reorder) {
    // With probability reorder%, hold this packet and send it after the next normal send
    if (allow_reorder && imp->reorder_percent > 0 && !imp->held_valid) {
        int r = rand() % 100;
        if (r < imp->reorder_percent) {
            memcpy(imp->held_pkt, buf, len);
            imp->held_len = len;
            imp->held_valid = true;
            return 0;
        }
    }

    if (send_maybe_drop(sockfd, buf, len, imp->loss_percent) < 0) {
        return -1;
    }

    // Flush any held packet (no additional reordering, but loss may still apply)
    if (imp->held_valid) {
        uint8_t tmp[MTCP_MAX_PACKET_SIZE];
        size_t tmp_len = imp->held_len;
        memcpy(tmp, imp->held_pkt, tmp_len);
        imp->held_valid = false;
        imp->held_len = 0;

        if (send_maybe_drop(sockfd, tmp, tmp_len, imp->loss_percent) < 0) {
            return -1;
        }
    }

    return 0;
}

// Sender 
typedef struct {
    bool valid;                          // Is this slot occupied?
    uint32_t seq;                        // Sequence number of packet in this slot
    size_t pkt_len;                      // Length of packet
    uint8_t pkt[MTCP_MAX_PACKET_SIZE];   // Serialized packet data
} pkt_slot_t;

static int run_sender(const config_t *cfg) {
    // Seed RNG for impairments
    srand((unsigned)time(NULL) ^ (unsigned)getpid());

    FILE *fp = fopen(cfg->file_path, "rb");
    if (!fp) {
        die("fopen (sender input file)");
    }

    // Create UDP socket
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        die("socket");
    }

    // Set up receiver address
    struct sockaddr_in peer;
    memset(&peer, 0, sizeof(peer));
    peer.sin_family = AF_INET;
    peer.sin_port = htons((uint16_t)cfg->port);
    if (inet_pton(AF_INET, cfg->ip, &peer.sin_addr) != 1) {
        diex("Invalid receiver IP address (use dotted IPv4 like 127.0.0.1)");
    }

    // Connect socket to receiver (allows using send() instead of sendto())
    if (connect(sockfd, (struct sockaddr *)&peer, sizeof(peer)) != 0) {
        die("connect (UDP)");
    }

    // Initialize network impairment simulator
    impairment_t imp;
    memset(&imp, 0, sizeof(imp));
    imp.loss_percent = cfg->loss_percent;
    imp.reorder_percent = cfg->reorder_percent;

    // Window management variables
    const uint32_t window = MTCP_WINDOW_SIZE;
    // Buffer capacity must be > window so sequence numbers map uniquely to slots
    const uint32_t cap = MTCP_WINDOW_SIZE + 1;

    pkt_slot_t *slots = calloc(cap, sizeof(pkt_slot_t));
    if (!slots) {
        die("calloc");
    }

    uint32_t base = 0;      // Oldest unacknowledged sequence number
    uint32_t nextseq = 0;   // Next sequence number to send

    bool eof_reached = false;
    bool fin_prepared = false;
    uint32_t fin_seq = 0;

    uint64_t timer_start = 0;
    bool timer_running = false;

    uint8_t payload[MTCP_PAYLOAD_SIZE];

    fprintf(stderr,
            "[sender] sending '%s' -> %s:%d (loss=%d%% reorder=%d%% window=%u timeout=%dms)\n",
            cfg->file_path, cfg->ip, cfg->port, cfg->loss_percent, cfg->reorder_percent, window, MTCP_TIMEOUT_MS);

    while (1) {
        // Fill the window with new packets as allowed
        while (nextseq < base + window) {
            if (!eof_reached) {
                // Try to read next chunk of file data
                size_t nread = fread(payload, 1, sizeof(payload), fp);
                if (nread > 0) {
                    // Create new DATA packet
                    uint32_t seq = nextseq;
                    uint32_t idx = seq % cap;

                    slots[idx].valid = true;
                    slots[idx].seq = seq;
                    slots[idx].pkt_len = build_packet(MTCP_TYPE_DATA, seq, payload, (uint16_t)nread,
                                                      slots[idx].pkt, sizeof(slots[idx].pkt));

                    // Send with simulated impairments
                    if (impair_send(sockfd, &imp, slots[idx].pkt, slots[idx].pkt_len, true) < 0) {
                        die("send (DATA)");
                    }

                    // Start retransmission timer if not already running
                    if (!timer_running) {
                        timer_running = true;
                        timer_start = now_ms();
                    }

                    nextseq++;
                    continue;
                }

                // EOF or read error
                if (ferror(fp)) {
                    die("fread");
                }
                eof_reached = true;
                fclose(fp);
                fp = NULL;
            }

            // After EOF, send FIN packet once
            if (eof_reached && !fin_prepared) {
                fin_seq = nextseq;
                uint32_t idx = fin_seq % cap;
                slots[idx].valid = true;
                slots[idx].seq = fin_seq;
                slots[idx].pkt_len = build_packet(MTCP_TYPE_FIN, fin_seq, NULL, 0,
                                                  slots[idx].pkt, sizeof(slots[idx].pkt));

                if (impair_send(sockfd, &imp, slots[idx].pkt, slots[idx].pkt_len, true) < 0) {
                    die("send (FIN)");
                }

                if (!timer_running) {
                    timer_running = true;
                    timer_start = now_ms();
                }

                fin_prepared = true;
                nextseq++;
                continue;
            }

            // nothing new to send
            break;
        }

        // Done condition, FIN was prepared AND base has advanced past FIN seq (i.e., FIN acknowledged)
        if (fin_prepared && base > fin_seq) {
            fprintf(stderr, "[sender] transfer complete (FIN acked)\n");
            break;
        }

        // If no outstanding packets, just wait briefly for potential stray ACKs
        int timeout_ms;
        if (!timer_running) {
            // No outstanding packets: just wait a bit for stray ACKs
            timeout_ms = 100;
        } else {
            uint64_t elapsed = now_ms() - timer_start;
            if (elapsed >= (uint64_t)MTCP_TIMEOUT_MS) {
                // Timeout occurred, will retransmit
                timeout_ms = 0;
            } else {
                // Time remaining until timeout
                timeout_ms = (int)(MTCP_TIMEOUT_MS - elapsed);
            }
        }

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(sockfd, &rfds);

        struct timeval tv;
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;

        int rv = select(sockfd + 1, &rfds, NULL, NULL, &tv);
        if (rv < 0) {
            if (errno == EINTR) {
                continue;
            }
            die("select");
        }

        if (rv == 0) {
            // Timeout
            if (timer_running && base < nextseq) {
                fprintf(stderr, "[sender] timeout: resending seq %u..%u\n", base, nextseq - 1);

                timer_start = now_ms();

                for (uint32_t s = base; s < nextseq; s++) {
                    uint32_t idx = s % cap;
                    if (!slots[idx].valid || slots[idx].seq != s) {
                        diex("internal error: missing packet in retransmit buffer");
                    }

                    if (impair_send(sockfd, &imp, slots[idx].pkt, slots[idx].pkt_len, false) < 0) {
                        die("send (retransmit)");
                    }
                }
            }
            continue;
        }

        // Read ACK
        uint8_t buf[MTCP_MAX_PACKET_SIZE];
        ssize_t n = recv(sockfd, buf, sizeof(buf), 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            die("recv");
        }

        // Parse ACK packet
        mtcp_parsed_t pkt;
        if (!parse_packet(buf, (size_t)n, &pkt)) {
            // ignore corrupt packet
            continue;
        }

        if (pkt.type != MTCP_TYPE_ACK && pkt.type != MTCP_TYPE_FINACK) {
            continue;
        }

        uint32_t ack = pkt.seq;
        if (ack > nextseq) {
            // Ignore impossible ACKs
            continue;
        }

        if (ack > base) {
            uint32_t old_base = base;
            base = ack;

            // Mark acked slots invalid
            for (uint32_t s = old_base; s < base; s++) {
                uint32_t idx = s % cap;
                slots[idx].valid = false;
            }

            if (base == nextseq) {
                timer_running = false;
            } else {
                timer_running = true;
                timer_start = now_ms();
            }
        }
    }

    free(slots);
    close(sockfd);
    return 0;
}

// Receiver
static int run_receiver(const config_t *cfg) {
    // Open output file for writing
    FILE *fp = fopen(cfg->file_path, "wb");
    if (!fp) {
        die("fopen (receiver output file)");
    }

    // Create UDP socket
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        die("socket");
    }

    // Bind to receiving port
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY); // Accept on all interfaces
    addr.sin_port = htons((uint16_t)cfg->port);

    if (bind(sockfd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        die("bind");
    }

    bool peer_set = false;
    struct sockaddr_in peer;
    socklen_t peer_len = sizeof(peer);

    uint32_t expected = 0;

    fprintf(stderr,"receiver on %d writing %s\n", cfg->port, cfg->file_path);

    while (1) {
        uint8_t buf[MTCP_MAX_PACKET_SIZE];
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);

        ssize_t n = recvfrom(sockfd, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fromlen);
        if (n < 0) {
            if (errno == EINTR) continue;
            die("recvfrom");
        }

        if (!peer_set) {
            peer = from;
            peer_len = fromlen;
            peer_set = true;

            char ipstr[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &peer.sin_addr, ipstr, sizeof(ipstr));
            fprintf(stderr, "[receiver] peer set to %s:%u\n", ipstr, ntohs(peer.sin_port));

            // Optional: connect UDP socket to this peer so we can use send()/recv()
            (void)connect(sockfd, (struct sockaddr *)&peer, peer_len);
        }

        // Parse received packet
        mtcp_parsed_t pkt;
        if (!parse_packet(buf, (size_t)n, &pkt)) {
            // Corrupt packet: re-ACK expected
            uint8_t ackbuf[MTCP_MAX_PACKET_SIZE];
            size_t alen = build_packet(MTCP_TYPE_ACK, expected, NULL, 0, ackbuf, sizeof(ackbuf));
            (void)sendto(sockfd, ackbuf, alen, 0, (struct sockaddr *)&peer, peer_len);
            continue;
        }

        // Handle DATA packets
        if (pkt.type == MTCP_TYPE_DATA) {
            if (pkt.seq == expected) {
                if (pkt.length > 0) {
                    size_t nw = fwrite(pkt.payload, 1, pkt.length, fp);
                    if (nw != pkt.length) {
                        die("fwrite");
                    }
                }
                expected++;
            }
            // If out-of-order, we silently discard (sender will retransmit)

            // Always send cumulative ACK with current expected value
            uint8_t ackbuf[MTCP_MAX_PACKET_SIZE];
            size_t alen = build_packet(MTCP_TYPE_ACK, expected, NULL, 0, ackbuf, sizeof(ackbuf));
            ssize_t sn = sendto(sockfd, ackbuf, alen, 0, (struct sockaddr *)&peer, peer_len);
            if (sn < 0) {
                die("sendto (ACK)");
            }

            continue;
        }

        // Handle FIN (end of transfer)
        if (pkt.type == MTCP_TYPE_FIN) {
            if (pkt.seq == expected) {
                expected++;

                // Send FINACK to confirm we received everything
                uint8_t finack[MTCP_MAX_PACKET_SIZE];
                size_t flen = build_packet(MTCP_TYPE_FINACK, expected, NULL, 0, finack, sizeof(finack));
                (void)sendto(sockfd, finack, flen, 0, (struct sockaddr *)&peer, peer_len);

                // Close file and exit
                fflush(fp);
                fclose(fp);
                fp = NULL;

                fprintf(stderr, "[receiver] FIN received; file complete\n");
                break;
            } else {
                // Not the expected FIN yet; re-ACK expected
                uint8_t ackbuf[MTCP_MAX_PACKET_SIZE];
                size_t alen = build_packet(MTCP_TYPE_ACK, expected, NULL, 0, ackbuf, sizeof(ackbuf));
                (void)sendto(sockfd, ackbuf, alen, 0, (struct sockaddr *)&peer, peer_len);
            }
            continue;
        }

        // Unexpected type: ignore but re-ACK expected to help sender
        uint8_t ackbuf[MTCP_MAX_PACKET_SIZE];
        size_t alen = build_packet(MTCP_TYPE_ACK, expected, NULL, 0, ackbuf, sizeof(ackbuf));
        (void)sendto(sockfd, ackbuf, alen, 0, (struct sockaddr *)&peer, peer_len);
    }

    if (fp) fclose(fp);
    close(sockfd);
    return 0;
}

int main(int argc, char **argv) {
    config_t cfg = parse_args(argc, argv);

    if (cfg.sender) {
        return run_sender(&cfg);
    }

    return run_receiver(&cfg);
}