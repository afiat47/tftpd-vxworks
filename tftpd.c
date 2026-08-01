#include <taskLib.h>
#include "tftpd.h"

static int g_active_reads = 0;
static int g_write_active = 0;
tftp_session_t *g_sessions[TFTPD_MAX_SESSIONS];
SEM_ID g_sessions_sem = NULL;

void tftpd_register_cmds(void);

/* ================================
 * Initialization
 * ================================ */
void tftpd_init(void)
{
    /* Version Structure */
	struct version_list tftpd_version = {
        .module_type = MODULE_TYPE_TFTPD,
        .module_name = "TFTPD",
        .version = TFTPD_VERSION,
        .module_description = "TFTP Server Module",
        .next = NULL
	};
	
    register_module_version(&tftpd_version);
    interface_set_showrunning_service(MODULE_TYPE_TFTPD, tftp_showrunning);
    tftpd_register_cmds();
    return;
}

/* ==================================================
 * Startup Configuration of Non Default Commands
 * List the Non default Configs in show running Command
 * =================================================== */
int tftp_showrunning(DEVICE_ID dev_id)
{
    if (dev_id == ZERO)
    {   if (g_enabled == YES)
            vty_printf("tftp server enable\n");

        if (g_port != TFTPD_DEFAULT_PORT)
            vty_printf("tftp server port %d\n", g_port);

        if (g_timeout_s != TFTPD_DEFAULT_TIMEOUT_S || g_retry != TFTPD_DEFAULT_RETRY)
            vty_printf("tftp server retransmit %d %d\n", g_timeout_s, g_retry);
    }

    return 0;
}

/* ============================================================
 * Handle Read/Write Request
 * Parse Filename, Opcodes, validate and spawn a session task
 * ============================================================ */
void tftpd_handle_listen_event(int listen_sock, MSG_Q_ID qid)
{
    unsigned char buf[PACKET_BUFFER_SIZE];
    struct soaddr client;
    int addrlen = sizeof(client);
    unsigned opcode;

    int rc = so_recvfrom(listen_sock, buf, sizeof(buf), MSG_DONTWAIT, &client, &addrlen);
    if (rc <= 0)
        return;

    char filename[TFTPD_FILENAME_MAXLEN] = {0};
    char mode[MODE_SIZE] = {0};
    int opt_off = NO;

    if (parse_rrq_wrq_header(buf, rc, filename, sizeof(filename), mode, sizeof(mode), &opt_off) < 0)
        return;

    opcode = ntohs(*(unsigned short*)buf);
    if (opcode != TFTP_OP_RRQ && opcode != TFTP_OP_WRQ)
        return;

    /****** concurrency handling *****/
    if (!g_sessions_sem)
        g_sessions_sem = sys_sm_create(SEM_Q_FIFO, SEM_FULL);

    sys_sm_p(g_sessions_sem, WAIT_FOREVER);
    int can_spawn = YES;
    if (opcode == TFTP_OP_RRQ)
    {
        if (g_write_active || g_active_reads >= TFTPD_MAX_CONCURRENT_READS)
            can_spawn = NO;
        else
            g_active_reads++;
    }
    else
    {
        if (g_write_active || g_active_reads > 0)
            can_spawn = NO;
        else
            g_write_active = YES;
    }
    sys_sm_v(g_sessions_sem);

    if (!can_spawn)
    {
        send_error_pkt(listen_sock, &client, addrlen, TFTP_ERR_ACCESS, "Server busy");
        return;
    }

    /***** Create and initialize session *****/
    tftp_session_t *sess = sys_mem_malloc(sizeof(tftp_session_t));
    if (!sess)
    {
        send_error_pkt(listen_sock, &client, addrlen, TFTP_ERR_UNDEF, "Server error");
        goto rollback;
    }
    memset(sess, ZERO, sizeof(tftp_session_t));

    sess->client_addr = client;
    sess->is_read = (opcode == TFTP_OP_RRQ);
    sess->timeout_s = g_timeout_s ? g_timeout_s : TFTPD_DEFAULT_TIMEOUT_S;
    sess->retry = g_retry ? g_retry : TFTPD_DEFAULT_RETRY;
    sess->blksize = TFTPD_DEFAULT_BLKSIZE;
    sess->filename = sys_mem_malloc(strlen(filename) + 1);
    if (!sess->filename) 
    {
        send_error_pkt(listen_sock, &client, addrlen, TFTP_ERR_UNDEF, "No memory");
        sys_mem_free(sess);
        goto rollback;
    }
    strcpy(sess->filename, filename);

    /***** Parse and validate options *****/
    if (opt_off < rc)
    {
        if (process_tftp_options(sess, buf + opt_off, rc - opt_off, opcode, filename) != 0)
        {
            send_error_pkt(listen_sock, &client, addrlen, TFTP_ERR_UNDEF, "Option parse failed");
            sys_mem_free(sess);
            goto rollback;
        }
    }

    /***** File handling *****/
    if (sess->is_read)
    {
        sess->file_ptr = file_open(filename, "r", NULL);
        if (!sess->file_ptr)
        {
            send_error_pkt(listen_sock, &client, addrlen, TFTP_ERR_NOTFOUND, "File not found");
            sys_mem_free(sess);
            goto rollback;
        }
    }
    else
    {
        FCB_POINT *chk = file_open(filename, "r", NULL);
        if (chk)
        {
            file_close(chk);
            send_error_pkt(listen_sock, &client, addrlen, TFTP_ERR_EXISTS, "File exists");
            sys_mem_free(sess);
            goto rollback;
        }

        sess->file_ptr = file_open(filename, "w", NULL);
        if (!sess->file_ptr)
        {
            send_error_pkt(listen_sock, &client, addrlen, TFTP_ERR_DISKFULL, "Cannot open file");
            sys_mem_free(sess);
            goto rollback;
        }
    }

    /***** Register session *****/
    if (session_register(sess) != SUCCESS)
    {
        if (sess->file_ptr)
            file_close(sess->file_ptr);

        sys_mem_free(sess);

        goto rollback;
    }

    /***** Spawn session *****/
    if (spawn_session_task_and_release_on_fail(sess->is_read ? TFTPD_SESS_READ : TFTPD_SESS_WRITE, sess) != 0)
    {
        send_error_pkt(listen_sock, &client, addrlen, TFTP_ERR_UNDEF, "Spawn failed");
        if (sess->file_ptr)
            file_close(sess->file_ptr);

        session_unregister(sess);
        sys_mem_free(sess);

        goto rollback;
    }

    return;

rollback:
    sys_sm_p(g_sessions_sem, WAIT_FOREVER);
    if (opcode == TFTP_OP_RRQ)
    {
        if (g_active_reads > ZERO)
            g_active_reads--;
    }
    else
    {
        g_write_active = NO;
    }
    sys_sm_v(g_sessions_sem);
}

/* ==================================================
 * Function to Spawn the New Read or Write Session Task
 * =================================================== */
int spawn_session_task_and_release_on_fail(tftpd_sess_type_t type, tftp_session_t *sess)
{
    char tname[TASK_NAME_SIZE];
    if (type == TFTPD_SESS_READ)
        snprintf(tname, sizeof(tname), "tftprd");
    else
        snprintf(tname, sizeof(tname), "tftpwr");

    int tid = taskSpawn(tname, TFTPD_TASK_PRIO, 0, TFTPD_SESSION_STACK,
                        (FUNCPTR)(type == TFTPD_SESS_READ ? tftpd_session_read_task : tftpd_session_write_task),
                        (int)sess, 0,0,0,0,0,0,0,0,0);
    if (tid == ERROR)
    {
        return -1;
    }
    return 0;
}

/* ================================================================
 * Process WRQ data packet: validate source, write to file, send ACK
 * Handle incoming DATA packets for a WRQ (write request session)
 * ================================================================ */
int tftpd_process_wrq_data(tftp_session_t *sess, unsigned char *rx_buf, size_t pkt_buf_sz, uint16_t *expected)
{
    struct soaddr from;
    struct soaddr_in *pin;
    struct soaddr_in *pc;
    int fromlen,clientlen, rc, wrc, dlen;
    unsigned short opc, block;
    unsigned char ackb[PACKET_HEADER_SIZE];
    tftp_data_t *pd;

    fromlen = sizeof(from);
    rc = so_recvfrom(sess->sockfd, rx_buf, pkt_buf_sz, MSG_DONTWAIT, &from, &fromlen);
    if (rc <= 0)
        return 0;
    
    pin = (struct soaddr_in*)&from;
    pc  = (struct soaddr_in*)&sess->client_addr;

    /* Check for unknown TID */
    if (pin->sin_addr.s_addr != pc->sin_addr.s_addr || pin->sin_port != pc->sin_port)
    {
        send_error_pkt(sess->sockfd, &from, fromlen, TFTP_ERR_UNKNOWN_TID, "Unknown TID");
        return 0;
    }

    if (rc < PACKET_HEADER_SIZE)
        return 0;

    opc = ntohs(*(uint16_t*)rx_buf);
    if (opc == TFTP_OP_DATA)
    {
        pd = (tftp_data_t*)rx_buf;
        block = ntohs(pd->block);
        dlen = rc - PACKET_HEADER_SIZE;
        
        if (block == *expected)
        {
            if (dlen > 0)
            {
                wrc = file_write(sess->file_ptr, (rx_buf + PACKET_HEADER_SIZE), dlen);
                if (wrc < 0)
                {
                    /* critical disk write error */
                    syslog(LOG_ERR, "tftpd: write failed (disk full?) for file=%s client=%s:%u\n", sess->filename, inet_ntoa(pin->sin_addr), ntohs(pin->sin_port));
                    send_error_pkt(sess->sockfd, &sess->client_addr, sizeof(sess->client_addr), TFTP_ERR_DISKFULL, "Disk full or write failed");
                    return -1;
                }
            }

            pack_ack(ackb, block);
            so_sendto(sess->sockfd, ackb, PACKET_HEADER_SIZE, 0, &sess->client_addr, sizeof(sess->client_addr));
			sys_start_timer(sess->timer_id, TIMER_RESOLUTION_S | sess->timeout_s);
            /* If data length smaller than negotiated blocksize, this is last packet */
            if (dlen < sess->blksize)
                return 1; /* last packet */

            /* increment expected block */
            (*expected)++;
            
            if(sess->options->options_present && block == 1)
        		return 2;
            
            if (*expected == ZERO_BLOCK)
    			*expected = FIRST_BLOCK;

        }
        else if (block < *expected)
        {
            /* duplicate DATA, resend ACK for that block */
            pack_ack(ackb, block);
            so_sendto(sess->sockfd, ackb, PACKET_HEADER_SIZE, 0, &sess->client_addr, sizeof(sess->client_addr));
            sys_start_timer(sess->timer_id, TIMER_RESOLUTION_S | sess->timeout_s);
        }
    }
    else if (opc == TFTP_OP_ERROR)
    {
        /* Client sent an error; propagate termination */
        syslog(LOG_ERR, "tftpd: received ERROR from client %s:%u for file=%s\n", inet_ntoa(pin->sin_addr), ntohs(pin->sin_port), sess->filename ? sess->filename : "(unknown)");
        return -1;
    }

    return 0;
}

/* ==================================================
 * Common session initialization for read/write tasks
 * Initialize session common resources (socket, msgq, timer) 
 * ================================================== */
int tftpd_session_common_init(tftp_session_t *sess)
{
    sess->msgq_id = sys_msgq_create(MAX_MSQ_MESSAGES, MSG_Q_FIFO);
    sess->created_msgq = (sess->msgq_id != NULL);
    if (!sess->msgq_id)
        return -1;

    sess->sockfd = create_ephemeral_socket();
    
    
	struct soaddr_in addr_in;
	int len = sizeof(addr_in);
	
    if (sess->sockfd < ZERO)
    {
        sys_msgq_delete(sess->msgq_id);
        sess->msgq_id = NULL;
        return -1;
    }

    if (socket_register(sess->sockfd, sess->msgq_id, TFTPD_MSG_TYPE_SOCKET_RCVD))
    {
        so_close(sess->sockfd);
        sess->sockfd = INVALID_SOCKET;
        sys_msgq_delete(sess->msgq_id);
        sess->msgq_id = NULL;
        return -1;
    }

    memset(&sess->timer_info, 0, sizeof(sess->timer_info));
    sess->timer_info.msg.qid = sess->msgq_id;
    sess->timer_info.msg.msg_buf[MSG_INDEX] = TFTPD_MSG_TYPE_TIMER_EXPIRY;
    sess->timer_id = ZERO;
    sess->timer_added = NO;

    if (sys_add_timer(TIMER_LOOP | TIMER_MSG_METHOD, &sess->timer_info, &sess->timer_id) == 0)
    {
        sess->timer_added = YES;
    }

    return SUCCESS;
}

/* ========================================================
 * Wait for ACK for an expected block on the session socket
 * ======================================================== */
int tftpd_wait_for_ack(tftp_session_t *sess, unsigned char *ack_buf, size_t pkt_buf_sz, uint16_t expected_block)
{
    struct soaddr from;
    struct soaddr_in *pin;
    struct soaddr_in *pc;
    int fromlen, rc;
    unsigned short opc;
    tftp_ack_t *ack;
    
    fromlen = sizeof(from);
    rc = so_recvfrom(sess->sockfd, ack_buf, pkt_buf_sz, MSG_DONTWAIT, &from, &fromlen);
    if (rc <= ZERO)
        return 0;
    
    pin = (struct soaddr_in*)&from;
    pc  = (struct soaddr_in*)&sess->client_addr;
    
    /* Check for unknown TID */
    if (pin->sin_addr.s_addr != pc->sin_addr.s_addr || pin->sin_port != pc->sin_port)
    {
        send_error_pkt(sess->sockfd, &from, fromlen, TFTP_ERR_UNKNOWN_TID, "Unknown TID");
        return 0;
    }

    opc = ntohs(*(unsigned short*)ack_buf);
    if (opc == TFTP_OP_ACK)
    {
        ack = (tftp_ack_t*)ack_buf;
        if (ntohs(ack->block) == expected_block)
            return YES;
            
        else if (ntohs(ack->block) < expected_block)
        {
        	return DUPLICATE_ACK;
		}
            
    }
    else if (opc == TFTP_OP_ERROR)
    {
        return -1;
    }
 
    return 0;
}

/* ================================================================
 * Extracts filename and mode from RRQ/WRQ buffer
 * ================================================================ */
int parse_rrq_wrq_header(unsigned char *buf, int len, char *filename, int fnlen, char *mode, int mlen, int *opt_offset)
{
    int idx = OPCODE_FIELD_SIZE;
    int fi = 0, mi = 0;
    unsigned opcode;

    if (!buf || len < PACKET_HEADER_SIZE || !filename || !mode)
        return -1;

    opcode = ntohs(*(unsigned short*)buf);
    if (opcode != TFTP_OP_RRQ && opcode != TFTP_OP_WRQ)
        return -1;

    /* filename */
    while (idx < len && buf[idx] != NULL_CHAR && fi < fnlen - 1)
        filename[fi++] = buf[idx++];

    filename[fi] = NULL_CHAR;
    if (idx < len && buf[idx] == NULL_CHAR)
        idx++;

    /* mode */
    while (idx < len && buf[idx] != NULL_CHAR && mi < mlen - 1)
        mode[mi++] = buf[idx++];

    mode[mi] = NULL_CHAR;
    if (idx < len && buf[idx] == NULL_CHAR)
        idx++;

    if (opt_offset)
        *opt_offset = idx;

    return 0;
}

/* ================================================================
 * Parses and validates options (blksize, timeout, tsize)
 * Applies defaults and stores them in sess->options
 * ================================================================ */
int process_tftp_options(tftp_session_t *sess, unsigned char *optbuf, int optlen, uint16_t opcode, char *filename)
{
    char blksz_req[TFTPD_BLKSIZE_BUF_LEN] = {0}, timeout_req[TFTPD_TIMEOUT_BUF_LEN] = {0}, tsize_req[64] = {0};
    char tsize_val[TFTPD_TSIZE_BUF_LEN] = "0";
    long tval, bval;
    char blk_val[BLOCK_VAL_SIZE];
    char tm_val[TIME_VAL_SIZE];
    
    if (optbuf && optlen > 0)
        parse_options_area((unsigned char*)optbuf, optlen, blksz_req, sizeof(blksz_req), timeout_req, sizeof(timeout_req), tsize_req, sizeof(tsize_req));

    /* Defaults */
    sess->blksize   = TFTPD_DEFAULT_BLKSIZE;
    sess->timeout_s = g_timeout_s ? g_timeout_s : TFTPD_DEFAULT_TIMEOUT_S;
    sess->retry     = g_retry ? g_retry : TFTPD_DEFAULT_RETRY;

    /* Validate blksize */
    if (blksz_req[0])
    {
        bval = strtol(blksz_req, NULL, TFTPD_STR_BASE);
        if (bval < TFTPD_MIN_BLKSIZE)
            bval = TFTPD_MIN_BLKSIZE;

        if (bval > TFTPD_MAX_BLKSIZE)
            bval = TFTPD_MAX_BLKSIZE;

        sess->blksize = bval;
        snprintf(blk_val, sizeof(blk_val), "%hu", bval);
    }

    /* Validate timeout: 1-255 */
    if (timeout_req[0])
    {
        tval = strtol(timeout_req, NULL, TFTPD_STR_BASE);
        if (tval < TFTPD_MIN_TIMEOUT || tval > TFTPD_MAX_TIMEOUT)
            tval = TFTPD_DEFAULT_TIMEOUT_S;
        sess->timeout_s = (unsigned int)tval;
        snprintf(tm_val, sizeof(tm_val), "%u", tval);
    }

    /* Enforce timeout * retry = 255 */
    if ((sess->timeout_s * sess->retry) > TFTPD_MAX_TIMEOUT)
    {
        sess->timeout_s = TFTPD_DEFAULT_TIMEOUT_S;
        sess->retry     = TFTPD_DEFAULT_RETRY;
    }

    /* Compute tsize */
    if (tsize_req[0])
    {
        if (opcode == TFTP_OP_RRQ)
        {
            FCB_POINT *fp = file_open((UINT8*)filename, (UINT8*)"r", NULL);
            if (fp)
            {
                int fs = getfilelen(fp);
                snprintf(tsize_val, sizeof(tsize_val), "%d", fs);
                file_close(fp);
            }
        }
        else
        {
            strncpy(tsize_val, tsize_req, sizeof(tsize_val)-1);
        }
    }

    /* Allocate tftp_options_t */
    tftp_options_t *opts = sys_mem_malloc(sizeof(tftp_options_t));
    if (!opts)
        return -1;

    memset(opts, 0, sizeof(*opts));

    opts->blksize = blksz_req[0] ? tftp_strdup(blk_val) : NULL;
    opts->timeout = timeout_req[0] ? tftp_strdup(tm_val) : NULL;
    opts->tsize   = tsize_req[0]   ? tftp_strdup(tsize_val)   : NULL;

    if ((blksz_req[0] && !opts->blksize) || (timeout_req[0] && !opts->timeout) || (tsize_req[0] && !opts->tsize))
    {
        sys_mem_free(opts);
        return -1;
    }

    opts->options_present = (opts->blksize || opts->timeout || opts->tsize);
    sess->options = opts;
    return 0;
}

/* ================================================================
 * Read session task: reads file and sends DATA packets to client
 * ================================================================ */
void tftpd_session_read_task(void *arg)
{
    msgq_message ev;
    int attempts, got_ack0;
    int ack, rv, last;
    unsigned short block;
    unsigned char *rx_buf;
    unsigned char *tx_buf;
    unsigned int pkt_buf_sz;
    
    tftp_session_t *sess = (tftp_session_t*)arg;
    if (!sess)
        return;

    if (tftpd_session_common_init(sess) == ERROR)
        goto cleanup_and_exit;

    pkt_buf_sz = sess->blksize + PACKET_HEADER_SIZE;
    tx_buf = sys_mem_malloc(pkt_buf_sz);
    rx_buf = sys_mem_malloc(pkt_buf_sz);

    if (!tx_buf || !rx_buf)
        goto sess_cleanup;

    /* OACK negotiation (if options) */
    if (sess->options && sess->options->options_present) 
    {
        attempts = 1;
        got_ack0 = NO;
        
        send_oack_pkt(sess->sockfd, &sess->client_addr, sizeof(sess->client_addr), sess->options);
		sys_start_timer(sess->timer_id, TIMER_RESOLUTION_S | sess->timeout_s);

        while (attempts < sess->retry && !got_ack0)
        {		
            rv = sys_msgq_receive(sess->msgq_id, (char*)&ev, WAIT_FOREVER);
            if (rv == ERROR || ev.msg_type == TFTPD_MSG_TYPE_TIMER_EXPIRY)
            {
            	send_oack_pkt(sess->sockfd, &sess->client_addr, sizeof(sess->client_addr), sess->options);
				sys_start_timer(sess->timer_id, TIMER_RESOLUTION_S | sess->timeout_s);
                attempts++;
                continue;
            }
            else if (ev.msg_type == SOCKET_DATARCVD)
            {
            	sys_stop_timer(sess->timer_id);
                ack = tftpd_wait_for_ack(sess, rx_buf, pkt_buf_sz, ZERO_BLOCK);
                if (ack == FIRST_BLOCK)
                {
                    got_ack0 = YES;
                    break;
                }

                if (ack < ZERO)
                    goto sess_cleanup;
            }
        }

        if (!got_ack0)
        {
        	syslog(LOG_WARNING, "tftpd: read aborted for file=%s client=%s:%u (OACK negotiation failed)",
                   sess->filename ? sess->filename : "(unknown)",
                   inet_ntoa(((struct soaddr_in*)&sess->client_addr)->sin_addr),
                   ntohs(((struct soaddr_in*)&sess->client_addr)->sin_port));
            goto sess_cleanup;
        }
    }

    /* File read/send loop */
    block = FIRST_BLOCK;
    last = NO;
    while (!last)
    {
        int readn, outlen;
        int got_ack;

        readn = file_read(sess->file_ptr, tx_buf + PACKET_HEADER_SIZE, sess->blksize);
        if (readn < 0)
            readn = 0;

        pack_data(tx_buf, block, tx_buf + PACKET_HEADER_SIZE, readn);
        outlen = PACKET_HEADER_SIZE + readn;

        so_sendto(sess->sockfd, tx_buf, outlen, 0, &sess->client_addr, sizeof(sess->client_addr));
	
		sys_start_timer(sess->timer_id, TIMER_RESOLUTION_S | sess->timeout_s);
		
        attempts = 1;
        got_ack = NO;
        while (attempts < sess->retry && !got_ack)
        {
            rv = sys_msgq_receive(sess->msgq_id, (char*)&ev, WAIT_FOREVER);
            if (rv == ERROR || ev.msg_type == TFTPD_MSG_TYPE_TIMER_EXPIRY)
            {
                attempts++;
                so_sendto(sess->sockfd, tx_buf, outlen, 0, &sess->client_addr, sizeof(sess->client_addr));
                sys_start_timer(sess->timer_id, TIMER_RESOLUTION_S | sess->timeout_s);
                continue;
            }
            else if (ev.msg_type == SOCKET_DATARCVD)
            {   
				sys_stop_timer(sess->timer_id);
                int ack = tftpd_wait_for_ack(sess, rx_buf, pkt_buf_sz, block);
                if (ack == GOT_ACK)
                {
                    got_ack = YES;
                    break;
                }
                else if(ack == DUPLICATE_ACK)
                {
                	so_sendto(sess->sockfd, tx_buf, outlen, 0, &sess->client_addr, sizeof(sess->client_addr));
                    sys_start_timer(sess->timer_id, TIMER_RESOLUTION_S | sess->timeout_s);
                    continue;
				}	 	
                else if (ack < ZERO)
                {
                	goto sess_cleanup;
				}         
            }
        }

        if (!got_ack)
        {
        	syslog(LOG_WARNING, "tftpd: read aborted for file=%s client=%s:%u",
                   sess->filename ? sess->filename : "(unknown)",
                   inet_ntoa(((struct soaddr_in*)&sess->client_addr)->sin_addr),
                   ntohs(((struct soaddr_in*)&sess->client_addr)->sin_port));
                   
            goto sess_cleanup;
        }

        if (readn < sess->blksize)
        {
            last = YES;
            break;
        }

        block++;

        if (block == ZERO_BLOCK)
            block = FIRST_BLOCK;
    }

    /* Log successful read completion */
    if (last == YES)
	{
        struct soaddr_in *sin = (struct soaddr_in*)&sess->client_addr;
        syslog(LOG_INFO, "tftpd: read finished for file=%s client=%s:%u",
               sess->filename ? sess->filename : "(unknown)",
               inet_ntoa(sin->sin_addr),
               ntohs(sin->sin_port));
    }

sess_cleanup:
    if (tx_buf)
        sys_mem_free(tx_buf);

    if (rx_buf)
        sys_mem_free(rx_buf);

    if (sess->sockfd >= ZERO)
    {
        so_close(sess->sockfd);
        sess->sockfd = INVALID_SOCKET;
    }

    if (sess->timer_added)
    {
        sys_stop_timer(sess->timer_id);
        sys_delete_timer(sess->timer_id);
    }

    if (sess->msgq_id)
    {
        sys_msgq_delete(sess->msgq_id);
        sess->msgq_id = NULL;
    }

    goto cleanup_and_exit;

cleanup_and_exit:
	if (sess->file_ptr)
    {
        file_close(sess->file_ptr);
        sess->file_ptr = NULL;
    }
    
    if (sess->options)
	{
    	if (sess->options->blksize) 
			sys_mem_free(sess->options->blksize);
        
		if (sess->options->timeout) 
			sys_mem_free(sess->options->timeout);
       
	    if (sess->options->tsize) 
			sys_mem_free(sess->options->tsize);
		
		sys_mem_free(sess->options);
	}
        sys_mem_free(sess->options);

    if (sess->filename)
        sys_mem_free(sess->filename);

    session_unregister(sess);
    sys_mem_free(sess);
    return;
}

/* ================================================================
 * Write session task: ACK initial, accept DATA packets until last
 * ================================================================ */
void tftpd_session_write_task(void *arg)
{
    int rv, initial_expected, res, done, attempts, got_data1;
    msgq_message ev;
    unsigned char *data_buf, ack_buf[PACKET_HEADER_SIZE];
    unsigned int pkt_buf_sz;
    unsigned short expected;

    tftp_session_t *sess = (tftp_session_t*)arg;
    if (!sess)
        return;

    if (tftpd_session_common_init(sess) < 0)
        goto wcleanup;

    /* use PACKET_HEADER_SIZE instead of hardcoded 4 */
    pkt_buf_sz = (size_t)sess->blksize + PACKET_HEADER_SIZE;
    data_buf = sys_mem_malloc(pkt_buf_sz);

    if (!data_buf)
        goto wclose;

    initial_expected = 1;
    if (sess->options && sess->options->options_present)
    {
        attempts = 1; 
        got_data1 = NO;

        while (attempts < (int)sess->retry && !got_data1)
        {
            send_oack_pkt(sess->sockfd, &sess->client_addr, sizeof(sess->client_addr), sess->options);
            sys_start_timer(sess->timer_id, TIMER_RESOLUTION_S | sess->timeout_s);
            
			rv = sys_msgq_receive(sess->msgq_id, (char*)&ev, WAIT_FOREVER);
            if (rv == ERROR || ev.msg_type == TFTPD_MSG_TYPE_TIMER_EXPIRY)
            {
                attempts++;
                continue;
            }

            if (ev.msg_type == SOCKET_DATARCVD)
            {   
            	sys_stop_timer(sess->timer_id);
                expected = 1;
                res = tftpd_process_wrq_data(sess, data_buf, pkt_buf_sz, &expected);
                if (res == 2)
                {
                    /* got first data block successfully */
                    got_data1 = YES;
                    break;
                }

                if (res < 0)
                    goto wclose;
            }
        }

        if (!got_data1)
        {
        	 syslog(LOG_WARNING, "tftpd: write aborted for file=%s client=%s:%u (OACK negotiation failed or no data received)",
                   sess->filename ? sess->filename : "(unknown)",
                   inet_ntoa(((struct soaddr_in*)&sess->client_addr)->sin_addr),
                   ntohs(((struct soaddr_in*)&sess->client_addr)->sin_port));
            
			goto wclose;
		}
           

        initial_expected = 2;
    }
    else
    {
        /* send ACK(0) to tell client to start sending DATA(1) */
        pack_ack(ack_buf, ZERO_BLOCK);
        so_sendto(sess->sockfd, ack_buf, PACKET_HEADER_SIZE, 0, &sess->client_addr, sizeof(sess->client_addr));
        sys_start_timer(sess->timer_id, TIMER_RESOLUTION_S | sess->timeout_s);
    }

    /* Main data loop */
    expected = (initial_expected == FIRST_BLOCK) ? 1 : initial_expected;
    done = NO;
    attempts = 1;

    while (!done)
    {
        rv = sys_msgq_receive(sess->msgq_id, (char*)&ev, WAIT_FOREVER);
        if (rv == ERROR || ev.msg_type == TFTPD_MSG_TYPE_TIMER_EXPIRY)
        {
            attempts++;
            if (attempts >= sess->retry)
                break;

            unsigned char lastack[PACKET_HEADER_SIZE]; 
            pack_ack(lastack, expected == FIRST_BLOCK ? 0 : expected - 1);
            so_sendto(sess->sockfd, lastack, PACKET_HEADER_SIZE, 0, &sess->client_addr, sizeof(sess->client_addr));
            sys_start_timer(sess->timer_id, TIMER_RESOLUTION_S | sess->timeout_s);
            continue;
        }

        if (ev.msg_type == SOCKET_DATARCVD)
        {
        	sys_stop_timer(sess->timer_id);
            res = tftpd_process_wrq_data(sess, data_buf, pkt_buf_sz, &expected);
            if (res == 1)
            {
                done = YES;
                break;
            }

            if (res < 0)
                break;
           
            attempts = 1;
        }
    }

    /* Log successful write completion */
    if(done == YES){
        struct soaddr_in *sin = (struct soaddr_in*)&sess->client_addr;
        syslog(LOG_INFO, "tftpd: write finished for file=%s client=%s:%u",
               sess->filename ? sess->filename : "(unknown)",
               inet_ntoa(sin->sin_addr),
               ntohs(sin->sin_port));
    }
    else
    {
    	 syslog(LOG_WARNING, "tftpd: write aborted for file=%s client=%s:%u (timeout waiting for data block %u)",
                       sess->filename ? sess->filename : "(unknown)",
                       inet_ntoa(((struct soaddr_in*)&sess->client_addr)->sin_addr),
                       ntohs(((struct soaddr_in*)&sess->client_addr)->sin_port),
                       expected);
	}

    if (data_buf)
        sys_mem_free(data_buf);

wclose:
    if (sess->sockfd >= 0)
    {
        so_close(sess->sockfd);
        sess->sockfd = INVALID_SOCKET;
    }

    if (sess->timer_added)
    {
        sys_stop_timer(sess->timer_id);
        sys_delete_timer(sess->timer_id);
    }

    if (sess->msgq_id)
    {
        sys_msgq_delete(sess->msgq_id);
        sess->msgq_id = NULL;
    }

    goto wcleanup;

wcleanup:
	if (sess->file_ptr)
    {
        file_close(sess->file_ptr); 
		sess->file_ptr = NULL;
    }
    
    if (sess->options)
	{
    	if (sess->options->blksize) 
			sys_mem_free(sess->options->blksize);
        
		if (sess->options->timeout) 
			sys_mem_free(sess->options->timeout);
       
	    if (sess->options->tsize) 
			sys_mem_free(sess->options->tsize);
		
		sys_mem_free(sess->options);
	}

    if (sess->filename)
        sys_mem_free(sess->filename);

    session_unregister(sess);
    sys_mem_free(sess);
    return;
}

/* ================================================================
 * Sessions list handling and registration helpers
 * ================================================================ */
void sessions_init(void)
{
    if (!g_sessions_sem)
        g_sessions_sem = sys_sm_create(SEM_Q_FIFO, SEM_FULL);
    memset(g_sessions, 0, sizeof(g_sessions));
}

/* Register a newly created session in the table */
int session_register(tftp_session_t *sess)
{
    int i;
    if (!g_sessions_sem)
        sessions_init();

    sys_sm_p(g_sessions_sem, WAIT_FOREVER);
    for (i = 0; i < TFTPD_MAX_SESSIONS; i++)
    {
        if (g_sessions[i] == NULL)
        {
            g_sessions[i] = sess;
            sys_sm_v(g_sessions_sem);
            return SUCCESS;
        }
    }

    sys_sm_v(g_sessions_sem);
    return -1;
}

/* ============================================
 * Unregister and cleanup session entry
 * ============================================ */
void session_unregister(tftp_session_t *sess)
{
    int i;
    if (!g_sessions_sem)
        return;

    sys_sm_p(g_sessions_sem, WAIT_FOREVER);
    for (i = 0; i < TFTPD_MAX_SESSIONS; i++)
    {
        if (g_sessions[i] == sess)
        {
            g_sessions[i] = NULL;
            break;
        }
    }

    if (sess->is_read)
    {
        if (g_active_reads > ZERO)
            g_active_reads--;
    }
    else
    {
        g_write_active = NO;
    }

    sys_sm_v(g_sessions_sem);
}

/* ============================================
 * Show TFTP sessions details
 * ============================================ */
void tftpd_show_sessions(void)
{
    if (!g_sessions_sem)
        sessions_init();

    sys_sm_p(g_sessions_sem, WAIT_FOREVER);
    Print("\n--- Active TFTP Sessions ---\n");

    int found = NO, i;
    for (i = 0; i < TFTPD_MAX_SESSIONS; i++)
    {
        if (g_sessions[i])
        {
            struct soaddr_in *sin = (struct soaddr_in*)&g_sessions[i]->client_addr;
            Print("  [%d] %-3s  %15s:%-5d  filename=%s  blksize=%u, timeout=%u, retry=%u\n",
                  i,
                  g_sessions[i]->is_read ? "RRQ" : "WRQ",
                  inet_ntoa(sin->sin_addr),
                  ntohs(sin->sin_port),
                  g_sessions[i]->filename,
                  g_sessions[i]->blksize,
                  g_sessions[i]->timeout_s,
                  g_sessions[i]->retry
                  );

            found = YES;
        }
    }
    if (!found)
        Print("  No active sessions.\n");

    Print("----------------------------\n");
    sys_sm_v(g_sessions_sem);
}

/* ====================================
 * Helper to dublicate string
 * ===================================== */
char *tftp_strdup(const char *s)
{
    if (!s)
        return NULL;

    size_t len = strlen(s) + 1;
    char *str = malloc(len);
    if (!str)
        return NULL;

    memcpy(str, s, len);

    return str;
}

/* ====================================
 * send_error_pkt (unchanged)
 * ===================================== */
void send_error_pkt(int sock, struct soaddr *peer_addr, int peer_addr_len, unsigned short error_code, char *error_message)
{
    unsigned int message_len, header_size, available_space, packet_len, max_message_len;
    unsigned char packet_buffer[TFTP_ERROR_BUF_SIZE];
    tftp_error_t *error_pkt = (tftp_error_t *)packet_buffer;

    error_pkt->opcode = htons(TFTP_OP_ERROR);
    error_pkt->errcode = htons(error_code);

    /* Determine error message length */
    message_len = (error_message ? strlen(error_message) : 0);

    /* Compute header size and available space */
    header_size = offsetof(tftp_error_t, errormsg);
    available_space = sizeof(packet_buffer);

    if (available_space <= header_size)
    {

        packet_len = header_size + 1;  /* header + null terminator */
        packet_buffer[header_size] = NULL_CHAR;
        so_sendto(sock, packet_buffer, packet_len, 0, peer_addr, peer_addr_len);
        return;
    }

    /* Compute maximum message size */
    max_message_len = available_space - header_size - 1; /*reserve space for null terminator */
    if (message_len > max_message_len)
    {
        message_len = max_message_len;
    }

    if (message_len > 0)
    {
        memcpy(error_pkt->errormsg, error_message, message_len);
    }
    error_pkt->errormsg[message_len] = NULL_CHAR;

    packet_len = header_size + message_len + 1; /*total packet size */
    so_sendto(sock, packet_buffer, packet_len, 0, peer_addr, peer_addr_len);
}

/* ====================================
 * Pack Data
 * ===================================== */
void pack_data(unsigned char *buf, uint16_t block, const unsigned char *data, int dlen)
{
    tftp_data_t *pd = (tftp_data_t *)buf;
    pd->opcode = htons(TFTP_OP_DATA);
    pd->block = htons(block);
    if (dlen > ZERO)
        memcpy(pd->data, data, dlen);

    return;
}

/* ====================================
 * Pack ACK
 * ===================================== */
void pack_ack(unsigned char *buf, uint16_t block)
{
    tftp_ack_t *pa = (tftp_ack_t *)buf;
    pa->opcode = htons(TFTP_OP_ACK);
    pa->block = htons(block);
    return;
}

/* ====================================
 * Create Ephemeral Socket
 * ===================================== */
int create_ephemeral_socket(void)
{
	int bufsize = TFTPD_MAX_BLKSIZE;
    int ret;
    int s = so_socket(AF_INET, SOCK_DGRAM, 0);
    struct soaddr sa;
    struct soaddr_in *sin = (struct soaddr_in *)&sa;

    if (s < ZERO)
         return ERROR;
         
    ret = so_setsockopt(s, SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(bufsize));
    if (ret < 0) 
		return 1;

    // Set receive buffer size
    ret = so_setsockopt(s, SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(bufsize));
    if (ret < 0)
		return 1;

    memset(&sa, 0, sizeof(sa));
    sin->sin_len = sizeof(struct soaddr_in);
    sin->sin_family = AF_INET;
    sin->sin_port = htons(ZERO);
    sin->sin_addr.s_addr = INADDR_ANY;
    if (so_bind(s, &sa, sizeof(sa)) < ZERO)
    {
        so_close(s);
        return ERROR;
    }
    
    return s;
}

/* ====================================
 * OACK Packet Sender
 * ===================================== */
int send_oack_pkt(int sock, struct soaddr *peer, int peerlen, tftp_options_t *opts) {

    unsigned char buf[PACKET_BUFFER_SIZE];
    unsigned int pos = 0;

    *(unsigned short*)(buf + pos) = htons(TFTP_OP_OACK);
    pos += OPCODE_FIELD_SIZE;

    /* helper to append key value */

    if (opts->blksize && opts->blksize[0])
        APPEND("blksize", opts->blksize);
    if (opts->timeout && opts->timeout[0])
        APPEND("timeout", opts->timeout);
    if (opts->tsize && opts->tsize[0])
        APPEND("tsize", opts->tsize);

    /* send OACK */
    so_sendto(sock, buf, pos, 0, peer, peerlen);
    return 0;
}

/* ====================================
 * Option Parser
 * ===================================== */
void parse_options_area(unsigned char *opt, int optlen, char *blksz, int blen, char *timeout, int tlen, char *tsize, int tslen)
{
    int i = 0;

    while (i < optlen)
    {
        char key_buf[MAX_OPTION_KEY_LEN] = {INITIALIZED_TO_ZERO};
        char val_buf[MAX_OPTION_VALUE_LEN] = {INITIALIZED_TO_ZERO};
        int key_idx = 0, val_idx = 0;

        /* Parse key */
        while (i < optlen && opt[i] != NULL_CHAR && key_idx < (sizeof(key_buf) - 1))
        {
            key_buf[key_idx++] = opt[i++];
        }

        if (i < optlen && opt[i] == NULL_CHAR)
            i++;

        /* Parse value */
        while (i < optlen && opt[i] != NULL_CHAR && val_idx < (sizeof(val_buf) - 1))
        {
            val_buf[val_idx++] = opt[i++];
        }

        if (i < optlen && opt[i] == NULL_CHAR)
            i++;

        /* Match keys and copy values */
        if (strcasecmp(key_buf, "blksize") == ZERO)
        {
            strncpy(blksz, val_buf, blen - 1);
            blksz[blen - 1] = NULL_CHAR;
        }
        else if (strcasecmp(key_buf, "timeout") == ZERO)
        {
            strncpy(timeout, val_buf, tlen - 1);
            timeout[tlen - 1] = NULL_CHAR;
        }
        else if (strcasecmp(key_buf, "tsize") == ZERO)
        {
            strncpy(tsize, val_buf, tslen - 1);
            tsize[tslen - 1] = NULL_CHAR;
        }
    }
}




