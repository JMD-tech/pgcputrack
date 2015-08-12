// Heavily based on Bewareofgeek's excellent pmon.c
// Original file at: http://bewareofgeek.livejournal.com/2945.html
// This file is licensed under the GPL v2 (http://www.gnu.org/licenses/gpl2.txt)

#include <sys/socket.h>
#include <linux/netlink.h>
#include <linux/connector.h>
#include <linux/cn_proc.h>
#include <proc/readproc.h>
#include <signal.h>
#include <errno.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/*
 * connect to netlink
 * returns netlink socket, or -1 on error
 */
static int nl_connect()
{
    int rc;
    int nl_sock;
    struct sockaddr_nl sa_nl;

    nl_sock = socket(PF_NETLINK, SOCK_DGRAM, NETLINK_CONNECTOR);
    if (nl_sock == -1) {
        perror("socket");
        return -1;
    }

    sa_nl.nl_family = AF_NETLINK;
    sa_nl.nl_groups = CN_IDX_PROC;
    sa_nl.nl_pid = getpid();

    rc = bind(nl_sock, (struct sockaddr *)&sa_nl, sizeof(sa_nl));
    if (rc == -1) {
        perror("bind");
        close(nl_sock);
        return -1;
    }

    return nl_sock;
}

/*
 * subscribe on proc events (process notifications)
 */
static int set_proc_ev_listen(int nl_sock, bool enable)
{
    int rc;
    struct __attribute__ ((aligned(NLMSG_ALIGNTO))) {
        struct nlmsghdr nl_hdr;
        struct __attribute__ ((__packed__)) {
            struct cn_msg cn_msg;
            enum proc_cn_mcast_op cn_mcast;
        };
    } nlcn_msg;

    memset(&nlcn_msg, 0, sizeof(nlcn_msg));
    nlcn_msg.nl_hdr.nlmsg_len = sizeof(nlcn_msg);
    nlcn_msg.nl_hdr.nlmsg_pid = getpid();
    nlcn_msg.nl_hdr.nlmsg_type = NLMSG_DONE;

    nlcn_msg.cn_msg.id.idx = CN_IDX_PROC;
    nlcn_msg.cn_msg.id.val = CN_VAL_PROC;
    nlcn_msg.cn_msg.len = sizeof(enum proc_cn_mcast_op);

    nlcn_msg.cn_mcast = enable ? PROC_CN_MCAST_LISTEN : PROC_CN_MCAST_IGNORE;

    rc = send(nl_sock, &nlcn_msg, sizeof(nlcn_msg), 0);
    if (rc == -1) {
        perror("netlink send");
        return -1;
    }

    return 0;
}

proc_t* read_procinfo(pid_t pid)
{
	static pid_t spid[2];
	spid[0]=pid; spid[1]=0;
	PROCTAB* proc = openproc(PROC_FILLCOM | PROC_FILLSTAT | PROC_PID, spid);
	// On wheezy the freeproc() function is present in libprocps header but not lib
	//  so we use a static structure to workaround memory leak...
	static proc_t proc_info;
	proc_t* res=readproc(proc, &proc_info);
	closeproc(proc);
	if (likely(res))
		return &proc_info;
	else
		return 0;
}

// On fork event, we add the process to the list of processes to monitor,
//  and store the starting time and initial command
void handle_fork_ev(struct proc_event &proc_ev)
{
	// Check that parent process name is postgres
	//possible perf upgrade here: cache postgres master process's PID to save one lookup per fork...
	//  also we would need to track exit of this process, and also exec of first postgres process to handle backend restart
	proc_t* proc_info=read_procinfo(proc_ev.event_data.fork.parent_pid);
	if (unlikely(!proc_info))
		{ printf("fork.parent_pid %u: Couldn't read procinfo\n",proc_ev.event_data.fork.parent_pid); return; }
	if (strcmp(proc_info->cmd,"postgres")) return;

	// Then check that child process name is postgres too
	if (unlikely(!read_procinfo(proc_ev.event_data.fork.child_pid)))
		{ printf("fork.child_pid %u: Couldn't read procinfo\n",proc_ev.event_data.fork.child_pid); return; }
	if (strcmp(proc_info->cmd,"postgres")) return;
	
	printf("fork: PID %u, PPID %u, cmd=%s time=%llu\n",proc_ev.event_data.fork.child_pid,proc_info->ppid,proc_info->cmd,proc_info->utime+proc_info->stime);
	

}

// Handle exit event
void handle_exit_ev(struct proc_event &proc_ev)
{
	proc_t* proc_info=read_procinfo(proc_ev.event_data.exit.process_pid);
	if (proc_info)
	{
		//char *cmdline=(proc_info.cmdline?*proc_info.cmdline:proc_info.cmd);
		//if (proc_info.cmdline) if (*proc_info.cmdline) cmdline=*proc_info.cmdline;
		//if (proc_info.cmdline)
		//	for (int argn=0;proc_info.cmdline[argn];++argn)
		//		printf("%u %s\n",argn,proc_info.cmdline[argn]);
		//freeproc(proc_info);
		//printf("exit: PID %u, PPID %u, cmd=%s time=%llu\n",proc_ev.event_data.exit.process_pid,proc_info->ppid,proc_info->cmd,proc_info->utime+proc_info->stime);
	}

}

/*
 * handle a single process event
 */
static volatile bool need_exit = false;
static int handle_proc_ev(int nl_sock)
{
    int rc;
    struct __attribute__ ((aligned(NLMSG_ALIGNTO))) {
        struct nlmsghdr nl_hdr;
        struct __attribute__ ((__packed__)) {
            struct cn_msg cn_msg;
            struct proc_event proc_ev;
        };
    } nlcn_msg;

    while (!need_exit) {
        rc = recv(nl_sock, &nlcn_msg, sizeof(nlcn_msg), 0);
        if (rc == 0) {
            /* shutdown? */
            return 0;
        } else if (rc == -1) {
            if (errno == EINTR) continue;
            perror("netlink recv");
            return -1;
        }
        switch (nlcn_msg.proc_ev.what) {
            case proc_event::PROC_EVENT_FORK:
				handle_fork_ev(nlcn_msg.proc_ev);
                break;
            case proc_event::PROC_EVENT_EXIT:
				handle_exit_ev(nlcn_msg.proc_ev);
                break;
			default:
				break;
        }
    }

    return 0;
}

static void on_sigint(int unused)
{
    need_exit = true;
}

int main(int argc, const char *argv[])
{
    int nl_sock;
    int rc = EXIT_SUCCESS;

    signal(SIGINT, &on_sigint);
    siginterrupt(SIGINT, true);

    nl_sock = nl_connect();
    if (nl_sock == -1)
        exit(EXIT_FAILURE);

    rc = set_proc_ev_listen(nl_sock, true);
    if (rc == -1) {
        rc = EXIT_FAILURE;
        goto out;
    }

    rc = handle_proc_ev(nl_sock);
    if (rc == -1) {
        rc = EXIT_FAILURE;
        goto out;
    }

	set_proc_ev_listen(nl_sock, false);

out:
    close(nl_sock);
    exit(rc);
}
