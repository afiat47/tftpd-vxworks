#ifndef TFTPD_H
#define TFTPD_H

#include <ip/socket.h>
#include <ip/inet.h>
#include <ip/msg.h>
#include <ip/in.h>
#include <taskLib.h>
#include <semLib.h>
#include <sysLib.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <libfile/file_sys.h>
#include <sys/stat.h>
#include <libsys/timer.h>
#include <arpa/inet.h>
#include <stddef.h> 
#include <limits.h> 
#include <libsys/vos/vos_msgq.h>
#include <libsys/vos/vos_semaphore.h>
#include <libsys/memory.h>
#include <libsys/verctl.h>
#include<libdev/modules.h>
#include <errno.h>

#define TFTPD_DEFAULT_PORT           69
#define TFTPD_DEFAULT_BLKSIZE        512
#define TFTPD_MAX_BLKSIZE            65464
#define TFTPD_DEFAULT_TIMEOUT_S      5
#define TFTPD_DEFAULT_RETRY          10
#define TFTPD_MAX_SESSIONS           4
#define MAX_MSQ_MESSAGES             64
#define TFTPD_MSG_STOP               5

#define TFTPD_MAX_CONCURRENT_READS   3
#define TFTPD_FILENAME_MAXLEN        128
#define TFTPD_SESSION_STACK          (1024*8)
#define TFTPD_TASK_PRIO              128
#define TASK_NAME_SIZE               16

#define TFTPD_MSG_SOCKET_LISTEN      0x10
#define TFTPD_MSG_TYPE_TIMER_EXPIRY  0xA0
#define TFTPD_MSG_TYPE_SOCKET_RCVD   SOCKET_DATARCVD
#define INVALID_SOCKET               -1
#define ZERO                          0

#define YES                           1
#define NO                            0  
#define ACK_BUFFER_SIZE               4
#define PACKET_BUFFER_SIZE            1500
#define PACKET_HEADER_SIZE            4  
#define TFTP_ERROR_BUF_SIZE           600
#define MODE_SIZE                     16 
#define MAX_OPTION_KEY_LEN            64
#define MAX_OPTION_VALUE_LEN          128 
#define OPCODE_FIELD_SIZE             2
#define MSG_INDEX                     3
#define SUCCESS                       0
#define FIRST_BLOCK                   1
#define ZERO_BLOCK                    0
#define GOT_ACK                       1
#define INITIALIZED_TO_ZERO           0
#define DUPLICATE_ACK                 2

#define TFTPD_BLKSIZE_BUF_LEN         32
#define TFTPD_TIMEOUT_BUF_LEN         32
#define TFTPD_TSIZE_BUF_LEN           64

#define TFTPD_STR_BASE                10
#define TFTPD_MIN_BLKSIZE             8
#define TFTPD_MIN_TIMEOUT             1
#define TFTPD_MAX_TIMEOUT             255
#define TFTPD_MIN_RETRY               1
#define TFTPD_MAX_RETRY               6
#define TFTPD_MIN_PORT                1
#define TFTPD_MAX_PORT                65535
#define TFTPD_MAX_TIMEOUT_PRODUCT     255
#define NULL_CHAR                    '\0'
#define TFTPD_VERSION                 0x0001
#define TIME_VAL_SIZE                 12
#define BLOCK_VAL_SIZE                6

#define APPEND(k, v) do { \
    size_t kl = strlen(k); size_t vl = strlen(v); \
    if (kl == 0 || vl == 0) break; \
    if (pos + kl + 1 + vl + 1 > sizeof(buf)) return -1; \
    memcpy(buf + pos, (k), kl); pos += kl; buf[pos++] = 0; \
    memcpy(buf + pos, (v), vl); pos += vl; buf[pos++] = 0; \
} while (0)

/* TFTP opcodes */
enum {
    TFTP_OP_RRQ = 1,
    TFTP_OP_WRQ = 2,
    TFTP_OP_DATA = 3,
    TFTP_OP_ACK = 4,
    TFTP_OP_ERROR = 5,
    TFTP_OP_OACK = 6
};

/* TFTP errors */
enum {
    TFTP_ERR_UNDEF = 0,
    TFTP_ERR_NOTFOUND = 1,
    TFTP_ERR_ACCESS = 2,
    TFTP_ERR_DISKFULL = 3,
    TFTP_ERR_ILLEGAL = 4,
    TFTP_ERR_UNKNOWN_TID = 5,
    TFTP_ERR_EXISTS = 6,
    TFTP_ERR_NOUSER = 7
};

typedef int8_t  i8;
typedef int16_t i16;


#pragma pack(1)
struct remote_communication_msg
{
	i8 msg_type;
	i16 msg_len;
	i8 msg[0];
};
#pragma pack()

/* Packet overlay structs (packed) */
typedef struct tftp_hdr {
    uint16_t opcode; 
} __attribute__((packed)) tftp_hdr_t;


typedef struct tftp_ack {
    uint16_t opcode;
    uint16_t block;
} __attribute__((packed)) tftp_ack_t;

typedef struct tftp_data {
    uint16_t opcode;
    uint16_t block;
    uint8_t  data[0]; 
} __attribute__((packed)) tftp_data_t;

typedef struct tftp_error {
    uint16_t opcode;
    uint16_t errcode;
    uint8_t  errormsg[0];
} __attribute__((packed)) tftp_error_t;

typedef struct tftp_oack {
    uint16_t opcode;
    uint8_t  opts[0];
} __attribute__((packed)) tftp_oack_t;

typedef struct tftp_rrq_wrq {
    uint16_t opcode;
    uint8_t  rest[0]; 
} __attribute__((packed)) tftp_rrq_wrq_t;

typedef enum {
    TFTPD_SESS_READ = 1,
    TFTPD_SESS_WRITE = 2
} tftpd_sess_type_t;

typedef struct {
    char *blksize;  
    char *timeout;  
    char *tsize;    
    int  options_present;
} tftp_options_t;

typedef struct {
    int sockfd;
    MSG_Q_ID msgq_id;
    TIMER_USER_DATA timer_info;
    unsigned long timer_id;
    FCB_POINT *file_ptr;
    struct soaddr client_addr;
    unsigned short blksize;
    unsigned short last_block;
    unsigned int timeout_s;
    unsigned int retry;
    int is_read;
    int done;
    int attempts;
    char *filename;
    tftp_options_t *options;
    int created_msgq;
    int timer_added;
} tftp_session_t;

typedef struct{
    uint32_t reserved1;
    uint32_t reserved2;
    uint32_t data;
    uint32_t msg_type;
    uint32_t reserved3;
} msgq_message;

void tftpd_start(void);     
void tftpd_stop(void);          
void tftpd_set_retransmit(unsigned int timeout_s, unsigned int retry);
void tftpd_set_port(unsigned short port);
void default_port(void);
void retry_timeout_default(void);
void show_tftp_server(void);
void tftpd_handle_listen_event(int listen_sock, MSG_Q_ID qid);
int tftpd_wait_for_ack(tftp_session_t *sess, unsigned char *rx_buf, size_t pkt_buf_sz, uint16_t expected_block);
int parse_rrq_wrq_header(unsigned char *buf, int len, char *filename, int fnlen, char *mode, int mlen, int *opt_offset);
int process_tftp_options(tftp_session_t *sess, unsigned char *optbuf, int optlen, uint16_t opcode, char *filename);
int spawn_session_task_and_release_on_fail(tftpd_sess_type_t type,tftp_session_t *sess);
int tftpd_process_wrq_data(tftp_session_t *sess, unsigned char *rx_buf, size_t pkt_buf_sz, uint16_t *expected);
int tftpd_session_common_init(tftp_session_t *sess);
void tftpd_session_read_task(void *arg);
void tftpd_session_write_task(void *arg);
char *tftp_strdup(const char *s);
int valid_filename(const char *fn);
void send_error_pkt(int sock, struct soaddr *peer_addr, int peer_addr_len, unsigned short error_code, char *error_message);
void pack_data(unsigned char *buf, uint16_t block, const unsigned char *data, int dlen);
void pack_ack(unsigned char *buf, uint16_t block);
int create_ephemeral_socket(void);
int send_oack_pkt(int sock, struct soaddr *peer, int peerlen, tftp_options_t *opts);
void parse_options_area(unsigned char *opt, int optlen, char *blksz, int blen, char *timeout, int tlen, char *tsize, int tslen);
void sessions_init(void);
int session_register(tftp_session_t *sess);
void session_unregister(tftp_session_t *sess);
void tftpd_show_sessions(void);
int create_listen_socket(unsigned short port, int *sockfd);
void tftpd_event_task(void);
int tftp_showrunning(DEVICE_ID dev_id);
void tftpd_handle_listen_event(int listen_sock, MSG_Q_ID qid);
int register_module_version(struct version_list *v);

extern int interface_set_showrunning_service(MODULE_TYPE module_type, INT32 (*func)(DEVICE_ID));
extern const char *tftpd_version_str;
extern unsigned int g_timeout_s;
extern unsigned int g_retry;
extern int g_enabled;
extern unsigned short g_port;

#endif /* TFTPD_H */


