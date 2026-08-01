#include <libcmd/cmdparse.h>
#include <libcmd/argparse.h>
#include <libcmd/cmderror.h>
#include "tftpd.h"

void tftpd_register_cmds(void);
int show_tftp(int argc, char **argv, struct user *u);
static int second_tftp_config(int argc, char *argv[], struct user *u);
static int top_tftp_config(int argc, char **argv, struct user *u);
static int third_tftp_server_disabled_config(int argc, char *argv[], struct user *u);
static int third_tftp_server_enabled_config(int argc, char *argv[], struct user *u);
static int third_tftp_port_config(int argc, char *argv[], struct user *u);
static int third_tftp_default_port_config(int argc, char *argv[], struct user *u);
static int third_tftp_retry_timeout_config(int argc, char *argv[], struct user *u);
static int third_tftp_retry_timeout_default_config(int argc, char *argv[], struct user *u);
static int show_third_tftp_show_tftp_server_config(int argc, char *argv[], struct user *u);

struct topcmds top_tftp_cmd[] = 
{
	{ "tftp", cmdPref(PF_CMDNO, PF_CMDDEF, 0), IF_NULL, ~FG_GLOBAL, IF_NULL, FG_CONFIG, 
		top_tftp_config, NULL, NULL, 0, 0,
		"tftp           -- tftp configuration",
		"tftp           -- tftp configuration",
		NULLCHAR, NULLCHAR
	},
	{ NULLCHAR }
};

struct cmds show_tftp_cmds[] = 
{
	{ "tftp", MATCH_AMB, 0, 0, show_tftp, NULL, NULL, 2, 0,
		"tftp               -- show tftp",
		"tftp               -- show tftp",
		NULLCHAR, NULLCHAR
	},
	{ NULLCHAR }
};

struct cmds show_tftp_second_cmds[] = 
{
	{ "server", MATCH_AMB, 0, 0, show_third_tftp_show_tftp_server_config, NULL, NULL, 0, 1,
		"server		   --  show tftp server status",
		"server		   --  show tftp server status",
		NULLCHAR, NULLCHAR
	},
	{ NULL }
};

static struct cmds third_tftp_cmd[] =
{
	{ "port", MATCH_AMB, cmdPref(PF_CMDNO, PF_CMDDEF, 0), 0,
		third_tftp_port_config, third_tftp_default_port_config, NULL, 2, 2,
		"port            -- configure server port number",
		"port            -- configure server port number",
		NULLCHAR, NULLCHAR
	},
	{ "enable", MATCH_AMB, cmdPref(PF_CMDNO, PF_CMDDEF, 0), 0,
		third_tftp_server_enabled_config, third_tftp_server_disabled_config, NULL, 0, 1,
		"enable        -- enable server",
		"enable        -- enable server",
		NULLCHAR, NULLCHAR
	},
	{ "retransmit", MATCH_AMB, cmdPref(PF_CMDNO, PF_CMDDEF, 0), 0,
		third_tftp_retry_timeout_config, third_tftp_retry_timeout_default_config, NULL, 3, 3,
		"retransmit        -- configure timeout and retry parameter",
		"retransmit        -- configure timeout and retry parameter",
		NULLCHAR, NULLCHAR
	},
	{ NULLCHAR }
};

struct cmds second_tftp_cmd[] = {
	{ "server", MATCH_AMB, cmdPref(PF_CMDNO, PF_CMDDEF, 0), 0,
		second_tftp_config, NULL, NULL, 2, 0,
		"server       -- configure server",
		"server       -- configure server",
		NULLCHAR, NULLCHAR
	},
	{ NULLCHAR }
};

static int second_tftp_config(int argc, char *argv[], struct user *u)
{
   return subcmd(third_tftp_cmd, NULL, argc, argv, u);
}

/* ====================================
 * Disable TFTP Server Configurer
 * ===================================== */
static int third_tftp_server_disabled_config(int argc, char *argv[], struct user *u)
{
	tftpd_stop();
	return 0;
}

/* ====================================
 * Enable TFTP Server Configurer
 * ===================================== */
static int third_tftp_server_enabled_config(int argc, char *argv[], struct user *u)
{
	tftpd_start();
	return 0;
}

/* ====================================
 * TFTP Server Port Configurer
 * ===================================== */
static int third_tftp_port_config(int argc, char *argv[], struct user *u)
{
	struct parameter param;
	unsigned int port;
	int error;
	
	memset(&param, 0, sizeof(param));
	param.type = ARG_INT;
	param.min = 1;
	param.max = 65535;
	param.ylabel = "  <1-65535>	  	-- port\n";
	param.hlabel = "  <1-65535>	  	-- port\n";
	param.flag = ARG_MIN | ARG_MAX | ARG_LABEL;
	if ((error = getparameter(argc--, argv++, u, &param)))
		return error;
		
	port = param.value.v_int;
	tftpd_set_port(port);
    
	return 0;
}

/* ====================================
 * TFTP Server Default Port Configurer
 * ===================================== */
static int third_tftp_default_port_config(int argc, char *argv[], struct user *u)
{
	default_port();
	return 0;
}

/* =======================================
 * TFTP Server Timeout and Retry Configurer
 * ======================================= */
static int third_tftp_retry_timeout_config(int argc, char *argv[], struct user *u)
{
	struct parameter param;
	unsigned int timeout, retry;
	int error;
	
	memset(&param, 0, sizeof(param));
	param.type = ARG_INT;
	param.min = 1;
	param.max = 255;
	param.ylabel = "  <1-255>	  	-- timeout\n";
	param.hlabel = "  <1-255>	  	-- timeout\n";
	param.flag = ARG_MIN | ARG_MAX | ARG_LABEL;
	if ((error = getparameter(argc--, argv++, u, &param)))
		return error;

	timeout = param.value.v_int;
	
	memset(&param, 0, sizeof(param));
	param.type = ARG_INT;
	param.min = 1;
	param.max = 6;
	param.ylabel = "  <1-6>	  	-- retry\n";
	param.hlabel = "  <1-6>	  	-- retry\n";
	param.flag = ARG_MIN | ARG_MAX | ARG_LABEL;
	if ((error = getparameter(argc--, argv++, u, &param)))
		return error;
	
	retry = param.value.v_int;	
	tftpd_set_retransmit(timeout, retry);

	return 0;
}

/* =======================================
 * Default Timeout and Retry Configurer
 * ======================================= */
static int third_tftp_retry_timeout_default_config(int argc, char *argv[], struct user *u)
{
	retry_timeout_default();
	return 0;
}

/* =======================================
 * TFTP server Status Show Configurer
 * ======================================= */
static int show_third_tftp_show_tftp_server_config(int argc, char *argv[], struct user *u)
{
	show_tftp_server();
	return 0;
}

int show_tftp(int argc, char **argv, struct user *u)
{
	int error = 0;
	
	error = subcmd(show_tftp_second_cmds, &u->cmd_mskbits, argc, argv, u);
	if (error)
		return error;
	
	return 0;
}

static int top_tftp_config(int argc, char **argv, struct user *u)
{
	return subcmd(second_tftp_cmd, NULL, argc, argv, u);
}

void tftpd_register_cmds(void)
{
	registercmd(top_tftp_cmd);
	
	register_subcmd_tab("show", 0, IF_NULL, FG_ENABLE, show_tftp_cmds,
								sizeof(show_tftp_cmds) / sizeof(struct cmds) - 1);
}
