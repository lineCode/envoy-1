/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 *
 * Copyright (C) Simon Gomizelj, 2012
 */

#include "common.h"

#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <getopt.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <err.h>
#include <fcntl.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <systemd/sd-daemon.h>

enum agent {
    AGENT_SSH_AGENT,
    AGENT_GPG_AGENT,
    INVALID_AGENT
};

struct agent_t {
    const char *bin;
    char *const *argv;
};

struct agent_info_t {
    uid_t uid;
    struct agent_data_t d;
    struct agent_info_t *next;
};

static const struct agent_t Agent[INVALID_AGENT] = {
    [AGENT_SSH_AGENT] = {
        .bin  = "/usr/bin/ssh-agent",
        .argv = (char *const []){ "ssh-agent", NULL }
    },
    [AGENT_GPG_AGENT] = {
        .bin  = "/usr/bin/gpg-agent",
        .argv = (char *const []){ "gpg-agent", "--daemon", "--enable-ssh-support", NULL }
    }
};

static struct agent_info_t *agents = NULL;
static bool sd_activated = false;
static int server_sock;
static const struct agent_t *agent = &Agent[AGENT_SSH_AGENT];

static void cleanup(void)
{
    close(server_sock);

    while (agents) {
        if (agents->d.pid <= 0)
            continue;
        kill(agents->d.pid, SIGTERM);
        agents = agents->next;
    }
}

static void sighandler(int signum)
{
    switch (signum) {
    case SIGINT:
    case SIGTERM:
        if (!sd_activated)
            cleanup();
        exit(EXIT_SUCCESS);
    }
}

static void parse_agentdata_line(char *val, struct agent_data_t *info)
{
    char *eol, *var;

    eol = strchr(val, ';');
    if (eol)
        *eol = '\0';

    if (strchr(val, '=') == NULL)
        return;

    var = strsep(&val, "=");

    if (strcmp(var, "SSH_AUTH_SOCK") == 0)
        strcpy(info->sock, val);
    else if (strcmp(var, "SSH_AGENT_PID") == 0)
        info->pid = atoi(val);
    else if (strcmp(var, "GPG_AGENT_INFO") == 0)
        strcpy(info->gpg, val);
}

static int parse_agentdata(int fd, struct agent_data_t *data)
{
    char b[BUFSIZ];
    char *l, *nl;
    ssize_t bytes_r;

    bytes_r = read(fd, b, sizeof(b));
    if (bytes_r <= 0)
        return bytes_r;

    b[bytes_r] = '\0';
    l = &b[0];

    while (l < &b[bytes_r]) {
        nl = strchr(l, '\n');
        if (!nl)
            break;

        *nl = '\0';
        parse_agentdata_line(l, data);

        l = nl + 1;
    }

    return 0;
}

static void start_agent(uid_t uid, gid_t gid, struct agent_data_t *data)
{
    int fd[2], stat = 0;
    struct passwd *pwd = getpwuid(uid);
    if (pwd == NULL || pwd->pw_dir == NULL)
        err(EXIT_FAILURE, "failed to lookup passwd entry");

    data->first_run = true;
    fprintf(stdout, "starting %s for uid=%zd gid=%zd\n",
            agent->argv[0], uid, gid);

    if (pipe(fd) < 0)
        err(EXIT_FAILURE, "failed to create pipe");

    switch (fork()) {
    case -1:
        err(EXIT_FAILURE, "failed to fork");
        break;
    case 0:
        dup2(fd[1], STDOUT_FILENO);
        close(fd[0]);

        if (setgid(gid) < 0 || setuid(uid) < 0)
            err(EXIT_FAILURE, "unable to drop to uid=%zd gid=%zd\n",
                uid, gid);

        /* gpg-agent expects HOME to be set */
        if (setenv("HOME", pwd->pw_dir, true))
            err(EXIT_FAILURE, "failed to set HOME=%s\n", pwd->pw_dir);

        /* gpg-agent expects GPG_TTY to be set or there will be blood */
        if (setenv("GPG_TTY", "/dev/null", true))
            err(EXIT_FAILURE, "failed to set GPG_TTY\n");

        if (execv(agent->bin, agent->argv) < 0)
            err(EXIT_FAILURE, "failed to start %s", agent->argv[0]);
        break;
    default:
        close(fd[1]);
        break;
    }

    if (parse_agentdata(fd[STDIN_FILENO], data) < 0)
        err(EXIT_FAILURE, "failed to parse %s output", agent->argv[0]);

    if (wait(&stat) < 1)
        err(EXIT_FAILURE, "failed to get process status");

    if (stat) {
        data->pid = 0;

        if (WIFEXITED(stat))
            fprintf(stderr, "%s exited with status %d\n",
                    agent->argv[0], WEXITSTATUS(stat));
        if (WIFSIGNALED(stat))
            fprintf(stderr, "%s terminated with signal %d\n",
                    agent->argv[0], WTERMSIG(stat));
    }
}

static int get_socket(void)
{
    int fd, n;

    n = sd_listen_fds(0);
    if (n > 1)
        err(EXIT_FAILURE, "too many file descriptors recieved");
    else if (n == 1) {
        fd = SD_LISTEN_FDS_START;
        sd_activated = true;
    } else {
        size_t len;
        union {
            struct sockaddr sa;
            struct sockaddr_un un;
        } sa;

        fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0)
            err(EXIT_FAILURE, "couldn't create socket");

        memset(&sa, 0, sizeof(sa));
        sa.un.sun_family = AF_UNIX;
        len = set_socket_path(&sa.un);

        if (bind(fd, &sa.sa, len) < 0)
            err(EXIT_FAILURE, "failed to bind");

        if (listen(fd, SOMAXCONN) < 0)
            err(EXIT_FAILURE, "failed to listen");
    }

    return fd;
}

static enum agent find_agent(const char *string)
{
    size_t i;

    for (i = 0; i < INVALID_AGENT; i++)
        if (strcmp(Agent[i].argv[0], string) == 0)
            break;

    return i;
}

static void __attribute__((__noreturn__)) usage(FILE *out)
{
    fprintf(out, "usage: %s [options]\n", program_invocation_short_name);
    fputs("Options:\n"
        " -h, --help           display this help and exit\n"
        " -v, --version        display version\n"
        " -a, --agent=AGENT    set the prefered agent\n", out);

    exit(out == stderr ? EXIT_FAILURE : EXIT_SUCCESS);
}

int main(int argc, char *argv[])
{
    enum agent id;
    uid_t uid = geteuid();

    static const struct option opts[] = {
        { "help",    no_argument,       0, 'h' },
        { "version", no_argument,       0, 'v' },
        { "agent",   required_argument, 0, 'a' },
        { 0, 0, 0, 0 }
    };

    while (true) {
        int opt = getopt_long(argc, argv, "hva:", opts, NULL);
        if (opt == -1)
            break;

        switch (opt) {
        case 'h':
            usage(stdout);
            break;
        case 'v':
            printf("%s %s\n", program_invocation_short_name, ENVOY_VERSION);
            return 0;
        case 'a':
            if ((id = find_agent(optarg)) == INVALID_AGENT)
                errx(EXIT_FAILURE, "unknown agent: %s", optarg);

            agent = &Agent[id];
            break;
        default:
            usage(stderr);
        }
    }

    server_sock = get_socket();

    signal(SIGTERM, sighandler);
    signal(SIGINT,  sighandler);

    while (true) {
        union {
            struct sockaddr sa;
            struct sockaddr_un un;
        } sa;
        socklen_t sa_len;

        int cfd = accept4(server_sock, &sa.sa, &sa_len, SOCK_CLOEXEC);
        if (cfd < 0)
            err(EXIT_FAILURE, "failed to accept connection");

        struct ucred cred;
        socklen_t cred_len = sizeof(struct ucred);

        if (getsockopt(cfd, SOL_SOCKET, SO_PEERCRED, &cred, &cred_len) < 0)
            err(EXIT_FAILURE, "couldn't obtain credentials from unix domain socket");

        if (uid != 0 && uid != cred.uid) {
            fprintf(stderr, "rejecting connection from uid=%zd\n", cred.uid);
            goto done;
        }

        struct agent_info_t *node = agents;
        while (node) {
            if (node->uid == cred.uid)
                break;
            node = node->next;
        }

        if (!node || node->d.pid == 0 || kill(node->d.pid, 0) < 0) {
            if (node && node->d.pid) {
                if (errno != ESRCH)
                    err(EXIT_FAILURE, "something strange happened with kill");
                fprintf(stdout, "%s for uid=%zd no longer running...\n",
                        agent->argv[0], cred.uid);
            } else if (!node) {
                node = calloc(1, sizeof(struct agent_info_t));
                node->uid = cred.uid;
                node->next = agents;
                agents = node;
            }

            start_agent(cred.uid, cred.gid, &node->d);
        }

        if (node->d.pid) {
            if (write(cfd, &node->d, sizeof(node->d)) < 0)
                err(EXIT_FAILURE, "failed to write agent data");
            node->d.first_run = false;
        }

done:
        fflush(stdout);
        fflush(stderr);
        close(cfd);
    }

    return 0;
}

// vim: et:sts=4:sw=4:cino=(0
