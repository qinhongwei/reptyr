/*
 * Copyright (C) 2011 by Nelson Elhage
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include <sys/types.h>
#include <stdint.h>
#include <dirent.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <signal.h>
#include <limits.h>
#include <time.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <linux/major.h>
#include <linux/net.h>

#include "ptrace.h"
#include "reptyr.h"
#include "reallocarray.h"

#define assert_nonzero(expr) ({                         \
            typeof(expr) __val = expr;                  \
            if (__val == 0)                             \
                die("Unexpected: %s == 0!\n", #expr);   \
            __val;                                      \
        })

struct fd_array {
    int *fds;
    int n;
    int allocated;
};

int fd_array_push(struct fd_array *fda, int fd) {
    int *tmp;

    if (fda->n == fda->allocated) {
        fda->allocated = fda->allocated ? 2 * fda->allocated : 2;
        tmp = xreallocarray(fda->fds, fda->allocated, sizeof *tmp);
        if (tmp == NULL) {
            free(fda->fds);
            fda->fds = NULL;
            fda->allocated = 0;
            return -1;
        }
        fda->fds = tmp;
    }
    fda->fds[fda->n++] = fd;
    return 0;
}

#define TASK_COMM_LENGTH 16
struct proc_stat {
    pid_t pid;
    char comm[TASK_COMM_LENGTH+1];
    char state;
    pid_t ppid, sid, pgid;
    dev_t ctty;
};

#define do_syscall(child, name, a0, a1, a2, a3, a4, a5) \
    ptrace_remote_syscall((child), ptrace_syscall_numbers((child))->nr_##name, \
                          a0, a1, a2, a3, a4, a5)

// Define lowercased versions of the socketcall numbers, so that we
// can assemble them with ## in the macro below
#define socketcall_socket SYS_SOCKET
#define socketcall_connect SYS_CONNECT
#define socketcall_sendmsg SYS_SENDMSG

#define do_socketcall(child, name, a0, a1, a2, a3, a4)                  \
    ({                                                                  \
        int __ret;                                                      \
        if (ptrace_syscall_numbers((child))->nr_##name) {               \
            __ret = do_syscall((child), name, a0, a1, a2, a3, a4, 0);   \
        } else {                                                        \
            __ret = do_syscall((child), socketcall, socketcall_##name,  \
                               a0, a1, a2, a3, a4);                     \
        }                                                               \
        __ret; })

int parse_proc_stat(int statfd, struct proc_stat *out) {
    char buf[1024];
    int n;
    unsigned dev;
    lseek(statfd, 0, SEEK_SET);
    if (read(statfd, buf, sizeof buf) < 0)
        return assert_nonzero(errno);
    n = sscanf(buf, "%d (%16[^)]) %c %d %d %d %u",
               &out->pid, out->comm,
               &out->state, &out->ppid, &out->pgid,
               &out->sid, &dev);
    if (n == EOF)
        return assert_nonzero(errno);
    if (n != 7) {
        return EINVAL;
    }
    out->ctty = dev;
    return 0;
}

int read_proc_stat(pid_t pid, struct proc_stat *out) {
    char stat_path[PATH_MAX];
    int statfd;
    int err;

    snprintf(stat_path, sizeof stat_path, "/proc/%d/stat", pid);
    statfd = open(stat_path, O_RDONLY);
    if (statfd < 0) {
        error("Unable to open %s: %s", stat_path, strerror(errno));
        return -statfd;
    }

    err = parse_proc_stat(statfd, out);
    close(statfd);
    return err;
}

static void do_unmap(struct ptrace_child *child, child_addr_t addr, unsigned long len) {
    if (addr == (unsigned long)-1)
        return;
    do_syscall(child, munmap, addr, len, 0, 0, 0, 0);
}

int *get_child_tty_fds(struct ptrace_child *child, int statfd, int *count) {
    struct proc_stat child_status;
    struct stat tty_st, console_st, st;
    char buf[PATH_MAX];
    struct fd_array fds = {};
    DIR *dir;
    struct dirent *d;

    debug("Looking up fds for tty in child.");
    if ((child->error = parse_proc_stat(statfd, &child_status)))
        return NULL;

    debug("Resolved child tty: %x", (unsigned)child_status.ctty);

    if (stat("/dev/tty", &tty_st) < 0) {
        child->error = assert_nonzero(errno);
        error("Unable to stat /dev/tty");
        return NULL;
    }

    if (stat("/dev/console", &console_st) < 0) {
        child->error = errno;
        error("Unable to stat /dev/console");
        return NULL;
    }

    snprintf(buf, sizeof buf, "/proc/%d/fd/", child->pid);
    if ((dir = opendir(buf)) == NULL)
        return NULL;
    while ((d = readdir(dir)) != NULL) {
        if (d->d_name[0] == '.') continue;
        snprintf(buf, sizeof buf, "/proc/%d/fd/%s", child->pid, d->d_name);
        if (stat(buf, &st) < 0)
            continue;

        if (st.st_rdev == child_status.ctty
            || st.st_rdev == tty_st.st_rdev
            || st.st_rdev == console_st.st_rdev) {
            debug("Found an alias for the tty: %s", d->d_name);
            if (fd_array_push(&fds, atoi(d->d_name)) != 0) {
                child->error = assert_nonzero(errno);
                error("Unable to allocate memory for fd array.");
                goto out;
            }
        }
    }
 out:
    *count = fds.n;
    closedir(dir);
    return fds.fds;
}

void move_process_group(struct ptrace_child *child, pid_t from, pid_t to) {
    DIR *dir;
    struct dirent *d;
    pid_t pid;
    char *p;
    int err;

    if ((dir = opendir("/proc/")) == NULL)
        return;

    while ((d = readdir(dir)) != NULL) {
        if (d->d_name[0] == '.') continue;
        pid = strtol(d->d_name, &p, 10);
        if (*p) continue;
        if (getpgid(pid) == from) {
            debug("Change pgid for pid %d", pid);
            err = do_syscall(child, setpgid, pid, to, 0, 0, 0, 0);
            if (err < 0)
                error(" failed: %s", strerror(-err));
        }
    }
    closedir(dir);
}

int do_setsid(struct ptrace_child *child) {
    int err = 0;
    struct ptrace_child dummy;

    err = do_syscall(child, fork, 0, 0, 0, 0, 0, 0);
    if (err < 0)
        return err;

    debug("Forked a child: %ld", child->forked_pid);

    err = ptrace_finish_attach(&dummy, child->forked_pid);
    if (err < 0)
        goto out_kill;

    dummy.state = ptrace_after_syscall;
    memcpy(&dummy.user, &child->user, sizeof child->user);
    if (ptrace_restore_regs(&dummy)) {
        err = dummy.error;
        goto out_kill;
    }

    err = do_syscall(&dummy, setpgid, 0, 0, 0, 0, 0, 0);
    if (err < 0) {
        error("Failed to setpgid: %s", strerror(-err));
        goto out_kill;
    }

    move_process_group(child, child->pid, dummy.pid);

    err = do_syscall(child, setsid, 0, 0, 0, 0, 0, 0);
    if (err < 0) {
        error("Failed to setsid: %s", strerror(-err));
        move_process_group(child, dummy.pid, child->pid);
        goto out_kill;
    }

    debug("Did setsid()");

 out_kill:
    kill(dummy.pid, SIGKILL);
    ptrace_detach_child(&dummy);
    ptrace_wait(&dummy);
    do_syscall(child, wait4, dummy.pid, 0, WNOHANG, 0, 0, 0);
    return err;
}

int ignore_hup(struct ptrace_child *child, unsigned long scratch_page) {
    int err;
    if (ptrace_syscall_numbers(child)->nr_signal != -1) {
        err = do_syscall(child, signal, SIGHUP, (unsigned long)SIG_IGN, 0, 0, 0, 0);
    } else {
        struct sigaction act = {
            .sa_handler = SIG_IGN,
        };
        err = ptrace_memcpy_to_child(child, scratch_page,
                                     &act, sizeof act);
        if (err < 0)
            return err;
        err = do_syscall(child, rt_sigaction,
                         SIGHUP, scratch_page,
                         0, 8, 0, 0);
    }
    return err;
}

/*
 * Wait for the specific pid to enter state 'T', or stopped. We have to pull the
 * /proc file rather than attaching with ptrace() and doing a wait() because
 * half the point of this exercise is for the process's real parent (the shell)
 * to see the TSTP.
 *
 * In case the process is masking or ignoring SIGTSTP, we time out after a
 * second and continue with the attach -- it'll still work mostly right, you
 * just won't get the old shell back.
 */
void wait_for_stop(pid_t pid, int fd) {
    struct timeval start, now;
    struct timespec sleep;
    struct proc_stat st;

    gettimeofday(&start, NULL);
    while (1) {
        gettimeofday(&now, NULL);
        if ((now.tv_sec > start.tv_sec && now.tv_usec > start.tv_usec)
            || (now.tv_sec - start.tv_sec > 1)) {
            error("Timed out waiting for child stop.");
            break;
        }
        /*
         * If anything goes wrong reading or parsing the stat node, just give
         * up.
         */
        if (parse_proc_stat(fd, &st))
            break;
        if (st.state == 'T')
            break;

        sleep.tv_sec  = 0;
        sleep.tv_nsec = 10000000;
        nanosleep(&sleep, NULL);
    }
}

int copy_tty_state(pid_t pid, const char *pty) {
    char buf[PATH_MAX];
    int fd, err = EINVAL;
    struct termios tio;
    int i;

    for (i = 0; i < 3 && err; i++) {
        err = 0;
        snprintf(buf, sizeof buf, "/proc/%d/fd/%d", pid, i);

        if ((fd = open(buf, O_RDONLY)) < 0) {
            err = -fd;
            continue;
        }

        if (!isatty(fd)) {
            err = ENOTTY;
            goto retry;
        }

        if (tcgetattr(fd, &tio) < 0) {
            err = -assert_nonzero(errno);
        }
    retry:
        close(fd);
    }

    if (err)
        return err;

    if ((fd = open(pty, O_RDONLY)) < 0)
        return -assert_nonzero(errno);

    if (tcsetattr(fd, TCSANOW, &tio) < 0)
        err = assert_nonzero(errno);
    close(fd);
    return -err;
}

int check_pgroup(pid_t target) {
    pid_t pg;
    DIR *dir;
    struct dirent *d;
    pid_t pid;
    char *p;
    int err = 0;
    struct proc_stat pid_stat;

    debug("Checking for problematic process group members...");

    pg = getpgid(target);
    if (pg < 0) {
        error("Unable to get pgid for pid %d", (int)target);
        return errno;
    }

    if ((dir = opendir("/proc/")) == NULL)
        return assert_nonzero(errno);

    while ((d = readdir(dir)) != NULL) {
        if (d->d_name[0] == '.') continue;
        pid = strtol(d->d_name, &p, 10);
        if (*p) continue;
        if (pid == target) continue;
        if (getpgid(pid) == pg) {
            /*
             * We are actually being somewhat overly-conservative here
             * -- if pid is a child of target, and has not yet called
             * execve(), reptyr's setpgid() strategy may suffice. That
             * is a fairly rare case, and annoying to check for, so
             * for now let's just bail out.
             */
            if ((err = read_proc_stat(pid, &pid_stat))) {
                memcpy(pid_stat.comm, "???", 4);
            }
            error("Process %d (%.*s) shares %d's process group. Unable to attach.\n"
                  "(This most commonly means that %d has suprocesses).",
                  (int)pid, TASK_COMM_LENGTH, pid_stat.comm, (int)target, (int)target);
            err = EINVAL;
            goto out;
        }
    }
 out:
    closedir(dir);
    return err;
}

int mmap_scratch(struct ptrace_child *child, unsigned long *addr) {
    long mmap_syscall;
    unsigned long scratch_page;

    mmap_syscall = ptrace_syscall_numbers(child)->nr_mmap2;
    if (mmap_syscall == -1)
        mmap_syscall = ptrace_syscall_numbers(child)->nr_mmap;
    scratch_page = ptrace_remote_syscall(child, mmap_syscall, 0,
                                         sysconf(_SC_PAGE_SIZE), PROT_READ|PROT_WRITE,
                                         MAP_ANONYMOUS|MAP_PRIVATE, 0, 0);

    if (scratch_page > (unsigned long)-1000) {
        return -(signed long)scratch_page;
    }

    *addr = scratch_page;
    debug("Allocated scratch page: %lx", scratch_page);

    return 0;
}

int grab_pid(pid_t pid, struct ptrace_child *child, unsigned long *scratch) {
    int err;

    if (ptrace_attach_child(child, pid)) {
        err = child->error;
        goto out;
    }
    if (ptrace_advance_to_state(child, ptrace_at_syscall)) {
        err = child->error;
        goto out;
    }
    if (ptrace_save_regs(child)) {
        err = child->error;
        goto out;
    }

    if ((err = mmap_scratch(child, scratch)))
        goto out_restore_regs;

    return 0;

 out_restore_regs:
    ptrace_restore_regs(child);

 out:
    ptrace_detach_child(child);

    return err;
}

int attach_child(pid_t pid, const char *pty, int force_stdio) {
    struct ptrace_child child;
    unsigned long scratch_page = -1;
    int *child_tty_fds = NULL, n_fds, child_fd, statfd;
    int i;
    int err = 0;
    long page_size = sysconf(_SC_PAGE_SIZE);
    char stat_path[PATH_MAX];

    if ((err = check_pgroup(pid))) {
        return err;
    }

    if ((err = copy_tty_state(pid, pty))) {
        if (err == ENOTTY && !force_stdio) {
            error("Target is not connected to a terminal.\n"
                  "    Use -s to force attaching anyways.");
            return err;
        }
    }

    snprintf(stat_path, sizeof stat_path, "/proc/%d/stat", pid);
    statfd = open(stat_path, O_RDONLY);
    if (statfd < 0) {
        error("Unable to open %s: %s", stat_path, strerror(errno));
        return -statfd;
    }

    kill(pid, SIGTSTP);
    wait_for_stop(pid, statfd);

    if ((err = grab_pid(pid, &child, &scratch_page))) {
        goto out_cont;
    }

    if (force_stdio) {
        child_tty_fds = malloc(3 * sizeof(int));
        if (!child_tty_fds) {
            err = ENOMEM;
            goto out_unmap;
        }
        n_fds = 3;
        child_tty_fds[0] = 0;
        child_tty_fds[1] = 1;
        child_tty_fds[2] = 2;
    } else {
        child_tty_fds = get_child_tty_fds(&child, statfd, &n_fds);
        if (!child_tty_fds) {
            err = child.error;
            goto out_unmap;
        }
    }

    if (ptrace_memcpy_to_child(&child, scratch_page, pty, strlen(pty)+1)) {
        err = child.error;
        error("Unable to memcpy the pty path to child.");
        goto out_free_fds;
    }

    child_fd = do_syscall(&child, open,
                          scratch_page, O_RDWR|O_NOCTTY,
                          0, 0, 0, 0);
    if (child_fd < 0) {
        err = child_fd;
        error("Unable to open the tty in the child.");
        goto out_free_fds;
    }

    debug("Opened the new tty in the child: %d", child_fd);

    err = ignore_hup(&child, scratch_page);
    if (err < 0)
        goto out_close;

    err = do_syscall(&child, getsid, 0, 0, 0, 0, 0, 0);
    if (err != child.pid) {
        debug("Target is not a session leader, attempting to setsid.");
        err = do_setsid(&child);
    } else {
        do_syscall(&child, ioctl, child_tty_fds[0], TIOCNOTTY, 0, 0, 0, 0);
    }
    if (err < 0)
        goto out_close;

    err = do_syscall(&child, ioctl, child_fd, TIOCSCTTY, 0, 0, 0, 0);
    if (err < 0) {
        error("Unable to set controlling terminal.");
        goto out_close;
    }

    debug("Set the controlling tty");

    for (i = 0; i < n_fds; i++)
        do_syscall(&child, dup2, child_fd, child_tty_fds[i], 0, 0, 0, 0);


    err = 0;

 out_close:
    do_syscall(&child, close, child_fd, 0, 0, 0, 0, 0);
 out_free_fds:
    free(child_tty_fds);

 out_unmap:
    do_unmap(&child, scratch_page, page_size);

    ptrace_restore_regs(&child);
    ptrace_detach_child(&child);

    if (err == 0) {
        kill(child.pid, SIGSTOP);
        wait_for_stop(child.pid, statfd);
    }
    kill(child.pid, SIGWINCH);
 out_cont:
    kill(child.pid, SIGCONT);
    close(statfd);

    return err < 0 ? -err : err;
}

struct steal_pty_state {
    struct proc_stat target_stat;

    pid_t emulator_pid;
    struct fd_array master_fds;

    char tmpdir[PATH_MAX];
    union {
        struct sockaddr addr;
        struct sockaddr_un addr_un;
    };
    int sockfd;

    struct ptrace_child child;
    unsigned long child_scratch;
    int child_fd;

    int ptyfd;
};

// Find the PID of the terminal emulator for `target's terminal.
//
// We assume that the terminal emulator is the parent of the session
// leader. This is true in most cases, although in principle you can
// construct situations where it is false. We should fail safe later
// on if this turns out to be wrong, however.
int find_terminal_emulator(struct steal_pty_state *steal) {
    debug("session leader of pid %d = %d",
          (int)steal->target_stat.pid,
          (int)steal->target_stat.sid);
    struct proc_stat leader_st;
    int err;
    if ((err = read_proc_stat(steal->target_stat.sid, &leader_st)))
        return err;
    debug("found terminal emulator process: %d", (int) leader_st.ppid);
    steal->emulator_pid = leader_st.ppid;
    return 0;
}

int get_terminal_state(struct steal_pty_state *steal, pid_t target) {
    int err;

    if ((err = read_proc_stat(target, &steal->target_stat)))
        return err;

    if (major(steal->target_stat.ctty) != UNIX98_PTY_SLAVE_MAJOR) {
        error("Child is not connected to a pseudo-TTY. Unable to steal TTY.");
        return EINVAL;
    }

    if ((err = find_terminal_emulator(steal)))
        return err;

    return 0;
}

int setup_steal_socket(struct steal_pty_state *steal) {
    strcpy(steal->tmpdir, "/tmp/reptyr.XXXXXX");
    if (mkdtemp(steal->tmpdir) == NULL)
        return errno;

    chmod(steal->tmpdir, 0755);

    steal->addr_un.sun_family = AF_UNIX;
    snprintf(steal->addr_un.sun_path, sizeof(steal->addr_un.sun_path),
             "%s/reptyr.sock", steal->tmpdir);

    if ((steal->sockfd = socket(AF_UNIX, SOCK_DGRAM, 0)) < 0)
        return errno;

    if (bind(steal->sockfd, &steal->addr, sizeof(steal->addr_un)) < 0)
        return errno;

    chmod(steal->addr_un.sun_path, 0666);

    return 0;
}

// ptmx(4) and Linux Documentation/devices.txt document
// /dev/ptmx has having major 5 and minor 2. I can't find any
// constants in headers after a brief glance that I should be
// using here.
#define PTMX_DEVICE (makedev(5, 2))

// Find the fd(s) in the terminal emulator process that corresponds to
// the master side of the target's pty. Store the result in
// steal->master_fds.
int find_master_fd(struct steal_pty_state *steal) {
    DIR *dir;
    struct dirent *d;
    struct stat st;
    int err;
    char buf[PATH_MAX];

    snprintf(buf, sizeof buf, "/proc/%d/fd/", steal->child.pid);
    if ((dir = opendir(buf)) == NULL)
        return errno;
    while ((d = readdir(dir)) != NULL) {
        if (d->d_name[0] == '.') continue;
        snprintf(buf, sizeof buf, "/proc/%d/fd/%s", steal->child.pid, d->d_name);
        if (stat(buf, &st) < 0)
            continue;

        debug("Checking fd: %s: st_dev=%x", d->d_name, (int)st.st_rdev);

        if (st.st_rdev != PTMX_DEVICE)
            continue;

        debug("found a ptmx fd: %s", d->d_name);
        err = do_syscall(&steal->child, ioctl,
                         atoi(d->d_name),
                         TIOCGPTN,
                         steal->child_scratch,
                         0, 0, 0);
        if (err < 0) {
            debug(" error doing TIOCGPTN: %s", strerror(-err));
            continue;
        }
        int ptn;
        err = ptrace_memcpy_from_child(&steal->child, &ptn,
                                       steal->child_scratch, sizeof(ptn));
        if (err < 0) {
            debug(" error getting ptn: %s", strerror(steal->child.error));
            continue;
        }
        if (ptn == (int)minor(steal->target_stat.ctty)) {
            debug("found a master fd: %d", atoi(d->d_name));
            if (fd_array_push(&steal->master_fds, atoi(d->d_name)) != 0) {
                error("unable to allocate memory for fd array!");
                return ENOMEM;
            }
        }
    }

    if (steal->master_fds.n == 0) {
        return ESRCH;
    }
    return 0;
}

int setup_steal_socket_child(struct steal_pty_state *steal) {
    int err;
    err = do_socketcall(&steal->child, socket, AF_UNIX, SOCK_DGRAM, 0, 0, 0);
    if (err < 0)
        return -err;
    steal->child_fd = err;
    debug("Opened fd %d in the child.", steal->child_fd);
    err = ptrace_memcpy_to_child(&steal->child, steal->child_scratch,
                                 &steal->addr_un, sizeof(steal->addr_un));
    if (err < 0)
        return steal->child.error;
    err = do_socketcall(&steal->child, connect, steal->child_fd, steal->child_scratch,
                     sizeof(steal->addr_un),0,0);
    if (err < 0)
        return -err;
    debug("Connected to the shared socket.");
    return 0;
}

int steal_child_pty(struct steal_pty_state *steal) {
    struct {
        struct msghdr msg;
        unsigned char buf[CMSG_SPACE(sizeof(int))];
    } buf = {};
    struct cmsghdr *cm;
    int err;

    buf.msg.msg_control = buf.buf;
    buf.msg.msg_controllen = CMSG_SPACE(sizeof(int));
    cm = CMSG_FIRSTHDR(&buf.msg);
    cm->cmsg_level = SOL_SOCKET;
    cm->cmsg_type  = SCM_RIGHTS;
    cm->cmsg_len   = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cm), &steal->master_fds.fds[0], sizeof(int));
    buf.msg.msg_controllen = cm->cmsg_len;

    // Relocate for the child
    buf.msg.msg_control = (void*)(steal->child_scratch +
                                  ((uint8_t*)buf.msg.msg_control - (uint8_t*)&buf));

    if (ptrace_memcpy_to_child(&steal->child,
                               steal->child_scratch,
                               &buf, sizeof(buf))) {
        return steal->child.error;
    }

    steal->child.error = 0;
    err = do_socketcall(&steal->child, sendmsg,
                     steal->child_fd,
                     steal->child_scratch,
                     MSG_DONTWAIT, 0,0);
    if (err < 0) {
        return steal->child.error ? steal->child.error : -err;
    }

    debug("Sent the pty fd, going to receive it.");

    buf.msg.msg_control = buf.buf;
    buf.msg.msg_controllen = CMSG_SPACE(sizeof(int));

    err = recvmsg(steal->sockfd, &buf.msg, MSG_DONTWAIT);
    if (err < 0) {
        error("Error receiving message.");
        return errno;
    }

    debug("Got a message: %d bytes, %ld control",
          err, (long)buf.msg.msg_controllen);

    if (buf.msg.msg_controllen < CMSG_LEN(sizeof(int))) {
        error("No fd received?");
        return EINVAL;
    }

    memcpy(&steal->ptyfd, CMSG_DATA(cm), sizeof(steal->ptyfd));

    debug("Got tty fd: %d", steal->ptyfd);

    return 0;
}

// Attach to the session leader of the stolen session, and block
// SIGHUP so that if and when the terminal emulator tries to HUP it,
// it doesn't die.
int steal_block_hup(struct steal_pty_state *steal) {
    struct ptrace_child leader;
    unsigned long scratch;
    int err = 0;

    if ((err = grab_pid(steal->target_stat.sid, &leader, &scratch)))
        return err;

    err = ignore_hup(&leader, scratch);

    ptrace_restore_regs(&leader);
    ptrace_detach_child(&leader);

    return err;
}

int steal_cleanup_child(struct steal_pty_state *steal) {
    if (ptrace_memcpy_to_child(&steal->child,
                               steal->child_scratch,
                               "/dev/null", sizeof("/dev/null"))) {
        return steal->child.error;
    }

    int nullfd = do_syscall(&steal->child, open, steal->child_scratch, O_RDONLY, 0, 0, 0, 0);
    if (nullfd < 0) {
        return steal->child.error;
    }

    int i;
    for (i = 0; i < steal->master_fds.n; ++i) {
        do_syscall(&steal->child, dup2, nullfd, steal->master_fds.fds[i], 0, 0, 0, 0);
    }

    do_syscall(&steal->child, close, nullfd, 0, 0, 0, 0, 0);
    do_syscall(&steal->child, close, steal->child_fd, 0, 0, 0, 0, 0);

    steal->child_fd = 0;

    ptrace_restore_regs(&steal->child);

    ptrace_detach_child(&steal->child);
    ptrace_wait(&steal->child);
    return 0;
}

int steal_pty(pid_t pid, int *pty) {
    int err = 0;
    struct steal_pty_state steal = {};
    long page_size = sysconf(_SC_PAGE_SIZE);

    if ((err = get_terminal_state(&steal, pid)))
        goto out;

    if ((err = setup_steal_socket(&steal)))
        goto out;

    debug("Listening on socket: %s", steal.addr_un.sun_path);

    if ((err = grab_pid(steal.emulator_pid, &steal.child, &steal.child_scratch)))
        goto out;

    debug("Attached to terminal emulator (pid %d)",
          (int)steal.emulator_pid);

    if ((err = find_master_fd(&steal))) {
        error("Unable to find the fd for the pty!");
        goto out;
    }

    if ((err = setup_steal_socket_child(&steal)))
        goto out;

    if ((err = steal_child_pty(&steal)))
        goto out;

    if ((err = steal_block_hup(&steal)))
        goto out;

    if ((err = steal_cleanup_child(&steal)))
        goto out;

    goto out_no_child;

out:
    if (steal.ptyfd) {
        close(steal.ptyfd);
        steal.ptyfd = 0;
    }

    if (steal.child_fd > 0)
        do_syscall(&steal.child, close, steal.child_fd, 0, 0, 0, 0, 0);

    if (steal.child_scratch > 0)
        do_unmap(&steal.child, steal.child_scratch, page_size);

    if (steal.child.state != ptrace_detached) {
        ptrace_restore_regs(&steal.child);
        ptrace_detach_child(&steal.child);
    }

out_no_child:

    if (steal.sockfd > 0) {
        close(steal.sockfd);
        unlink(steal.addr_un.sun_path);
    }

    if (steal.tmpdir[0]) {
        rmdir(steal.tmpdir);
    }

    if (steal.ptyfd)
        *pty = steal.ptyfd;

    free(steal.master_fds.fds);

    return err;
}
