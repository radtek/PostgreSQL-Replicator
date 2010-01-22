/*-----------------------
 * mcp_hosts.h
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group,
 * Copyright (c) 2006, Command Prompt, Inc.
 *
 * $Id: mcp_hosts.h 2186 2009-06-25 12:14:51Z alexk $
 * ------------------------
 */
#ifndef MCP_HOSTS_H
#define MCP_HOSTS_H

#include "mammoth_r/mcp_queue.h"
#include "mb/pg_wchar.h"

extern int mcp_max_slaves;

/* struct definition appears in mcp_hosts.c */
typedef struct MCPHosts MCPHosts;

/* Definitions for record numbers stored in the MCPHosts header */
/* Global record numbers */
typedef enum McphRecnoKind
{
	McphRecnoKindLastAcked,
	McphRecnoKindSafeToAck,
	McphRecnoKindMax	/* must be last */
} McphRecnoKind;

/* Per-host record numbers */
typedef enum McphHostRecnoKind
{
	McphHostRecnoKindSendNext,
	McphHostRecnoKindAcked,
	McphHostRecnoKindMax	/* must be last */
} McphHostRecnoKind;

extern MCPHosts *MCPHostsInit(void);
extern void		BootStrapMCPHosts(void);
extern void 	MCPHostsShmemInit(void);
extern void		MCPHostsClose(MCPHosts *h);
extern ullong MCPHostsNextTx(MCPHosts *h, int hostno, ullong last_recno);
extern MCPQSync	MCPHostsGetSync(MCPHosts *h, int hostno);
extern void		MCPHostsSetSync(MCPHosts *h, int hostno, MCPQSync sync);
extern ullong	MCPHostsGetRecno(MCPHosts *h, McphRecnoKind kind);
extern void	MCPHostsSetRecno(MCPHosts *h, McphRecnoKind kind,
				 ullong recno);
extern ullong	MCPHostsGetHostRecno(MCPHosts *h, McphHostRecnoKind kind,
					 int host);
extern void		MCPHostsSetHostRecno(MCPHosts *h, McphHostRecnoKind kind,
					 int host, ullong recno);
extern ullong	MCPHostsGetMinAckedRecno(MCPHosts *h, pid_t *node_pid);

extern void		MCPHostsCleanup(MCPHosts *h, MCPQueue *q, ullong recno);

extern void		MCPHostsLogTabStatus(int elevel, MCPHosts *h, int hostno,
									 char *prefix, pid_t *node_pid);
extern int		MCPHostsGetMaxHosts(MCPHosts *h);
extern time_t	MCPHostsGetTimestamp(MCPHosts *h, int hostno);
/* encoding-specific functions */
extern void		MCPHostsSetEncoding(MCPHosts *h, pg_enc new_encoding);
extern pg_enc 	MCPHostsGetEncoding(MCPHosts *h);

#endif /* MCP_HOSTS_H */
