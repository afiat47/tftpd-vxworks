#include "tftpd.h"

static MSG_Q_ID g_listen_msgq = NULL;
int g_listen_sock = INVALID_SOCKET;
int g_event_task_id = ERROR;
int g_running = NO;

unsigned short g_port = TFTPD_DEFAULT_PORT;
unsigned int g_timeout_s = TFTPD_DEFAULT_TIMEOUT_S;
unsigned int g_retry = TFTPD_DEFAULT_RETRY;
int g_enabled = NO;
SEM_ID g_close_sem = NULL;

/* ===========================================
 * Non default Retransmission Parameter Setter
 * =========================================== */
void tftpd_set_retransmit(unsigned int timeout_s, unsigned int retry) 
{
    if (timeout_s * retry > TFTPD_MAX_TIMEOUT_PRODUCT) 
	{
        syslog(LOG_ERR, "Failed to set timeout and retry");
        return;
    }
    
    g_timeout_s = timeout_s;
    g_retry = retry;
    
    syslog(LOG_INFO, "TFTP server retransmission timeout=%u and retry=%u\n", g_timeout_s, g_retry);
}

/* =======================================
 * Default Retransmission Parameter Setter
 * ======================================= */
void retry_timeout_default(void)
{
    g_timeout_s = TFTPD_DEFAULT_TIMEOUT_S;
    g_retry = TFTPD_DEFAULT_RETRY;
    
     syslog(LOG_INFO, "TFTP server retransmission changed to default\n", g_timeout_s, g_retry);
}

/* ================================
 * Non default server port Setter
 * ================================ */
void tftpd_set_port(unsigned short port) 
{
    if (port == ZERO) 
        port = TFTPD_DEFAULT_PORT;
    
	g_port = port;
    
	syslog(LOG_INFO, "TFTP server port set to %u\n", g_port);
}

/* ================================
 * Default server port Setter
 * ================================ */
void default_port(void)
{
    g_port = TFTPD_DEFAULT_PORT;
    syslog(LOG_INFO, "TFTP server changed to default port: %u\n", g_port);
}

/* ================================
 * Enable Server
 * ================================ */
void tftpd_start(void) 
{
    if (g_running) 
        return;
    
    g_enabled = YES;

    /* initialize sessions table */
    sessions_init();

    /* create message queue used for socket notifications and control messages */
    g_listen_msgq = sys_msgq_create(MAX_MSQ_MESSAGES, MSG_Q_FIFO);
    if (g_listen_msgq == NULL) 
        return;
        
    if (create_listen_socket(g_port, &g_listen_sock) != 0) 
    {
        sys_msgq_delete(g_listen_msgq);
        g_listen_msgq = NULL;
        return;
    }

    /* register socket to message queue; subtype indicates listen socket */
    if (socket_register(g_listen_sock, g_listen_msgq, TFTPD_MSG_SOCKET_LISTEN)) 
    {
        so_close(g_listen_sock);
        g_listen_sock = INVALID_SOCKET;
        sys_msgq_delete(g_listen_msgq);
        g_listen_msgq = NULL;
        return;
    }

    /* spawn event task */
    g_event_task_id = taskSpawn("tftpd_ev", TFTPD_TASK_PRIO, 0, TFTPD_SESSION_STACK,
                                (FUNCPTR)tftpd_event_task, 0,0,0,0,0,0,0,0,0,0);
    if (g_event_task_id == ERROR) 
    {
        so_close(g_listen_sock);
        g_listen_sock = INVALID_SOCKET;
        sys_msgq_delete(g_listen_msgq);
        g_listen_msgq = NULL;
        return;
    }
    
    syslog(LOG_INFO, "TFTP server started on port %d\n", g_port);
    g_running = YES;
    return;
}


/* ================================
 * Disable Server
 * ================================ */
void tftpd_stop(void) 
{
    if (!g_running) 
        return;
        
    g_enabled = NO;
    msgq_message stopmsg;
    stopmsg.msg_type = TFTPD_MSG_STOP;
    sys_msgq_send(g_listen_msgq, (char*)&stopmsg, MSG_PRI_NORMAL, NO_WAIT);

    if (!g_close_sem)
        g_close_sem = sys_sm_create(SEM_Q_FIFO, SEM_EMPTY);
    
    sys_sm_p(g_close_sem, WAIT_FOREVER);
    
    if (g_listen_sock != INVALID_SOCKET) 
    {
        so_close(g_listen_sock);
        g_listen_sock = INVALID_SOCKET;
    }
    
    if (g_event_task_id != ERROR) {
        
        taskDelete(g_event_task_id);
    }
    
    if (g_listen_msgq) 
    {
        sys_msgq_delete(g_listen_msgq);
        g_listen_msgq = NULL;
    }
    
    syslog(LOG_INFO, "TFTP server stopped on port %d\n", g_port);
    g_running = NO;
    return;
}


/* =====================================
 * create & bind UDP socket 
 * ===================================== */
int create_listen_socket(unsigned short port, int *sockfd) 
{
    int s = so_socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) 
    {
        return -1;
    }
    struct soaddr serv;
    struct soaddr_in *sin = (struct soaddr_in*)&serv;
    memset(&serv, 0, sizeof(serv));
    sin->sin_len = sizeof(struct soaddr_in);
    sin->sin_family = AF_INET;
    sin->sin_port = htons(port);
    sin->sin_addr.s_addr = INADDR_ANY;
    
    if (so_bind(s, &serv, sizeof(serv)) < 0) 
    {
        so_close(s);
        return -1;
    }
    
    *sockfd = s;
    return 0;
}

/* =====================================
 * Event task loop 
 * waits for socket and control messages 
 * ===================================== */
void tftpd_event_task(void) 
{
    msgq_message ev;
    while (1) 
    {
        int rv = sys_msgq_receive(g_listen_msgq, (char*)&ev, WAIT_FOREVER);
        if (rv == ERROR) 
            continue;

        if(ev.msg_type == TFTPD_MSG_STOP)
            break;
        
        if (ev.msg_type == TFTPD_MSG_SOCKET_LISTEN) 
            tftpd_handle_listen_event(g_listen_sock, g_listen_msgq);
    }
     
    sys_sm_v(g_close_sem);
}

/* =====================================
 * TFTP server status shows 
 * ===================================== */
void show_tftp_server() 
{
    Print("\n=== TFTP SERVER STATUS ===\n");
    
    Print("TFTP Enable     : %s\n", g_enabled ? "Yes" : "No");
    Print("TFTP Port       : %d\n", g_port);
    Print("Timeout         : %d sec\n", g_timeout_s);
    Print("Retry           : %d\n", g_retry);

    tftpd_show_sessions();

}
