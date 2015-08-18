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
#include <time.h>
#include <map>
#include <vector>
#include <string>

using std::string;

uint8_t outlev=2;		// Output verbosity level:
#define	VL_ERROR   (0)	// Errors only
#define VL_WARN    (1)	// + Warnings
#define VL_RESULTS (2)	// + Results
#define VL_ADDINFO (3)	// + Additionnal info
#define VL_DEBUG   (4)	// + Debugging info

// We store both startup timestamp and corresponding millisecond monotonic clock reference
time_t sup_time;
int64_t sup_millis;

int64_t getmillis()
{
	struct timespec tp;
	clock_gettime(CLOCK_MONOTONIC,&tp);
	return (int64_t)tp.tv_sec*1000+tp.tv_nsec/1000000;
}

std::vector<string> explode( const string& s, const string& separ ) {
	std::vector<string> resu; string token("");
	for ( auto const &c: s ) {
		if (separ.find_first_of(c)==string::npos) token+=c;
		else { resu.push_back(token); token=""; }
	}
	if (token.size()) resu.push_back(token);
	return resu;
}

class pgprocinfo {
	public:
		pgprocinfo();
		pgprocinfo(proc_t* proc_info);
		bool cx_ident=false;
		unsigned long long cputime=0,cputime_before=0;
		int64_t start_time=0,stop_time=0;
		string db,user,from;
		bool update_from(proc_t* proc_info);
};

// For processes we get a start notification, get millisecond resolution time
pgprocinfo::pgprocinfo() { start_time=getmillis()-sup_millis; }

// For processes started before, we extract the start time (seconds) from proc_info struct
//  nope => we store how much CPU was used before monitoring and use start_time=0;
// and we do the update_from procinfo now
pgprocinfo::pgprocinfo(proc_t* proc_info)
{
	cputime_before=proc_info->stime+proc_info->utime;
	update_from(proc_info);
}

bool pgprocinfo::update_from(proc_t* proc_info)
{
	if (unlikely(!proc_info)) return false;
	
	if (!cx_ident)	// process with not yet identified user/db/ip origin => we parse cmdline
	{
		if (likely(proc_info->cmdline)) {
			std::vector<string> args=explode(*proc_info->cmdline," ");
			if (likely(args.size()>=4))
			{
				if (outlev>=VL_DEBUG) printf("# PID=%u, cmd=%s, cmdline=%s, args%lu\n",proc_info->tid,proc_info->cmd,*proc_info->cmdline,args.size());
				//TODO: erase if writer process/wal writer process/autovacuum launcher process/stats collector process ?
				// args[0] should be "postgres:"
				user=args[1]; db=args[2]; from=args[3];
				cx_ident=true;
			}
			else
			{
				// pg backend on this VM takes about 4ms to change client process title
				if (outlev>=VL_ADDINFO) printf("# PID %u: not yet enough args to identify, cmdline=\"%s\"\n",proc_info->tid,*proc_info->cmdline);
			}
		}
	}
	
	// Update CPU time
	cputime=proc_info->stime+proc_info->utime;
	
	return true;
}

std::map<pid_t,pgprocinfo> pgprocs;		// map of tracked processes

/*
 * connect to netlink
 * returns netlink socket, or -1 on error
 */
static int nl_connect()
{
    int rc;
    int nl_sock;
    struct sockaddr_nl sa_nl;
	memset(&sa_nl, 0, sizeof(sa_nl));

    nl_sock = socket(PF_NETLINK, SOCK_DGRAM, NETLINK_CONNECTOR);
    if (nl_sock == -1) {
        perror("# socket");
        return -1;
    }

    sa_nl.nl_family = AF_NETLINK;
    sa_nl.nl_groups = CN_IDX_PROC;
    sa_nl.nl_pid = getpid();

    rc = bind(nl_sock, (struct sockaddr *)&sa_nl, sizeof(sa_nl));
    if (rc == -1) {
        perror("# bind");
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
        perror("# netlink send");
        return -1;
    }

    return 0;
}

void save_data_atexit(pid_t pid)
{
	if (pgprocs.at(pid).cx_ident)
		if (outlev>=VL_RESULTS) printf("PID %u consumed %llu ms CPU on %s, user %s from %s\n",pid,(pgprocs.at(pid).cputime*10),pgprocs.at(pid).db.c_str(),pgprocs.at(pid).user.c_str(),pgprocs.at(pid).from.c_str());
}

//TODO: possible perf upgrade: use PID vector instead of multiple calls
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
	//   could allow to detect on cases of parent:"couldn't read procinfo" if it was a PostgreSQL process...
	proc_t* proc_info=read_procinfo(proc_ev.event_data.fork.parent_pid);
	if (unlikely(!proc_info)) return;  // short lived unknown parent process, unrelated to postgres unless main backend stopped
	if (strcmp(proc_info->cmd,"postgres")) return;

	// Then check that child process name is postgres too
	if (unlikely(!read_procinfo(proc_ev.event_data.fork.child_pid)))
	{
		// short lived process forked by a postgres process
		if (outlev>=VL_ADDINFO) printf("# fork.child_pid %u: Couldn't read procinfo\n",proc_ev.event_data.fork.child_pid);
		return;
	}
	if (unlikely(strcmp(proc_info->cmd,"postgres")))
	{
		// Can only hapen if postgres-forked process had time to change its cmd before we got there. (does it changes cmd also? => NO, cmdline only)
		if (outlev>=VL_WARN) printf("# (CAN'T HAPPEN!) postgres %u forked (changed cmd): %u %s\n",proc_ev.event_data.fork.parent_pid,proc_ev.event_data.fork.child_pid,proc_info->cmd);
		return;
	}
	
	// Create the process information class
	pgprocs[proc_ev.event_data.fork.child_pid];
}

// Handle exit event
void handle_exit_ev(const struct proc_event& proc_ev)
{
	pid_t pid=proc_ev.event_data.exit.process_pid;
	// We ignore notifications for processes not identified at fork time
	if (!pgprocs.count(pid)) return;
	proc_t* proc_info=read_procinfo(pid);
	if (proc_info)
	{
		if (outlev>=VL_ADDINFO) printf("# got all proc_info at exit, PID=%u\n",pid);
		// update CPU and ident cmdline if not yet done...
		pgprocs.at(pid).update_from(proc_info);
		save_data_atexit(pid);
	}
	else
	{
		if (outlev>=VL_ADDINFO) printf("# missing proc_info at exit, PID=%u\n",pid);
		save_data_atexit(pid);
	}
	pgprocs.erase(pid);
}

// handle a single process event
static int handle_proc_ev(int nl_sock)
{
    struct __attribute__ ((aligned(NLMSG_ALIGNTO))) {
        struct nlmsghdr nl_hdr;
        struct __attribute__ ((__packed__)) {
            struct cn_msg cn_msg;
            struct proc_event proc_ev;
        };
    } nlcn_msg;
	
	int rc = recv(nl_sock, &nlcn_msg, sizeof(nlcn_msg), 0);
	if (rc == 0) {
		/* shutdown? */
		return 0;
	} else if (rc == -1) {
		if (errno == EINTR) return 0;
		perror("# netlink recv");
		return -1;
	}
	switch (nlcn_msg.proc_ev.what) {
		case proc_event::PROC_EVENT_FORK:
			handle_fork_ev((proc_event&)nlcn_msg.proc_ev);
			break;
		case proc_event::PROC_EVENT_EXIT:
			handle_exit_ev((proc_event&)nlcn_msg.proc_ev);
			break;
		default:
			break;
	}
	return 1;
}

static volatile bool need_exit = false;
static int main_loop(int nl_sock)
{
    int rs; fd_set Rsocks; struct timeval timeout;
		
    while (likely(!need_exit)) {
		
		FD_ZERO(&Rsocks); FD_SET(nl_sock,&Rsocks);
		timeout.tv_sec=0; timeout.tv_usec=10000;	// default 10ms resolution
		// consumes <1% CPU on our production setup during peaks at 10ms interval, raising it at 20ms incurs no significant difference,
		//  and lowering it is pointless as 10ms is the resolution of kernel's processes utime+stime counter
		rs = select(nl_sock+1, &Rsocks, (fd_set *) 0, (fd_set *) 0, &timeout);
		if (unlikely((rs == -1))) {
			if (errno == EINTR) continue;
			perror("# select");
			return -1;
		}
		
		if (rs)
			handle_proc_ev(nl_sock);
		else
		{
			// Update pg processes info struct
			std::vector<pid_t> deadprocs;
			for (auto &it: pgprocs)
				if (!it.second.update_from(read_procinfo(it.first)))
				{
					if (outlev>=VL_ADDINFO) printf("# no more proc info for: %u\n",it.first);
					save_data_atexit(it.first);
					deadprocs.push_back(it.first);
				}
		
			// Remove those that weren't running anymore
			for (auto const &it: deadprocs) pgprocs.erase(it);
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
	
	sup_millis=getmillis();
	time(&sup_time);
	if (outlev>=VL_RESULTS)
	{
		struct tm *tp=localtime(&sup_time);
		printf("START %04d-%02d-%02d %02d:%02d:%02d\n",tp->tm_year+1900,tp->tm_mon+1,tp->tm_mday,tp->tm_hour,tp->tm_min,tp->tm_sec);
	}

    nl_sock = nl_connect();
    if (nl_sock == -1)
        exit(EXIT_FAILURE);

    rc = set_proc_ev_listen(nl_sock, true);
    if (rc == -1) {
        rc = EXIT_FAILURE;
        goto out;
    }

    rc = main_loop(nl_sock);
    if (rc == -1) {
        rc = EXIT_FAILURE;
        goto out;
    }

	set_proc_ev_listen(nl_sock, false);

out:
    close(nl_sock);
    exit(rc);
}
