/* Copyright (C) 2023 John Törnblom

This program is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License as published by the
Free Software Foundation; either version 3, or (at your option) any
later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; see the file COPYING. If not, see
<http://www.gnu.org/licenses/>.  */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>


#include "cmd.h"


/**
 * Map names of commands to function entry points.
 **/
typedef struct ftp_command {
  const char       *name;
  ftp_command_fn_t *func;
} ftp_command_t;


/**
 * Data structure used to send UI notifications on the PS5.
 **/
typedef struct notify_request {
  char useless1[45];
  char message[3075];
} notify_request_t;


/**
 * Send a UI notification request (PS5 only).
 **/
int sceKernelSendNotificationRequest(int, notify_request_t*, size_t, int);


/**
 * Lookup table for FTP commands.
 **/
static ftp_command_t commands[] = {
  {"CDUP", ftp_cmd_CDUP},
  {"CWD",  ftp_cmd_CWD},
  {"DELE", ftp_cmd_DELE},
  {"LIST", ftp_cmd_LIST},
  {"MKD",  ftp_cmd_MKD},
  {"NOOP", ftp_cmd_NOOP},
  {"PASV", ftp_cmd_PASV},
  {"PORT", ftp_cmd_PORT},
  {"PWD",  ftp_cmd_PWD},
  {"QUIT", ftp_cmd_QUIT},
  {"REST", ftp_cmd_REST},
  {"RETR", ftp_cmd_RETR},
  {"RMD",  ftp_cmd_RMD},
  {"RNFR", ftp_cmd_RNFR},
  {"RNTO", ftp_cmd_RNTO},
  {"SIZE", ftp_cmd_SIZE},
  {"STOR", ftp_cmd_STOR},
  {"SYST", ftp_cmd_SYST},
  {"TYPE", ftp_cmd_TYPE},
  {"USER", ftp_cmd_USER},

  // custom commands
  {"KILL", ftp_cmd_KILL},
  {"MTRW", ftp_cmd_MTRW},

  // not yet implemnted
  {"XCUP", ftp_cmd_unavailable},
  {"XMKD", ftp_cmd_unavailable},
  {"XPWD", ftp_cmd_unavailable},
  {"XRCP", ftp_cmd_unavailable},
  {"XRMD", ftp_cmd_unavailable},
  {"XRSQ", ftp_cmd_unavailable},
  {"XSEM", ftp_cmd_unavailable},
  {"XSEN", ftp_cmd_unavailable},
};


/**
 * Number of FTP commands in the lookup table.
 **/
static int nb_ftp_commands = (sizeof(commands)/sizeof(ftp_command_t));


/**
 * Read a line from a file descriptor.
 **/
static char*
ftp_readline(int fd) {
  int bufsize = 1024;
  int position = 0;
  char *buffer_backup;
  char *buffer = calloc(bufsize, sizeof(char));
  char c;

  if(!buffer) {
    perror("[ftpsrv.elf] malloc");
    return NULL;
  }

  while(1) {
    int len = read(fd, &c, 1);
    if(len == -1 && errno == EINTR) {
      continue;
    }

    if(len <= 0) {
      free(buffer);
      return NULL;
    }

    if(c == '\r') {
      buffer[position] = '\0';
      position = 0;
      continue;
    }

    if(c == '\n') {
      return buffer;
    }

    buffer[position++] = c;

    if(position >= bufsize) {
      bufsize += 1024;
      buffer_backup = buffer;
      buffer = realloc(buffer, bufsize);
      if(!buffer) {
	perror("[ftpsrv.elf] realloc");
	free(buffer_backup);
	return NULL;
      }
    }
  }
}


/**
 * Execute an FTP command.
 **/
static int
ftp_execute(ftp_env_t *env, char *line) {
  char *sep = strchr(line, ' ');
  char *arg = strchr(line, 0);

  if(sep) {
    sep[0] = 0;
    arg = sep + 1;
  }

  for(int i=0; i<nb_ftp_commands; i++) {
    if(strcmp(line, commands[i].name)) {
      continue;
    }

    return commands[i].func(env, arg);
  }

  return ftp_cmd_unknown(env, arg);
}


/**
 * Greet a new FTP connection.
 **/
static int
ftp_greet(ftp_env_t *env) {
  const char *msg = "220 Service is ready\r\n";
  size_t len = strlen(msg);

  if(write(env->active_fd, msg, len) != len) {
    return -1;
  }

  return 0;
}


/**
 * Entry point for new FTP connections.
 **/
static void*
ftp_thread(void *args) {
  ftp_env_t env;
  bool running;
  char *line;

  env.data_fd     = -1;
  env.passive_fd  = -1;
  env.active_fd   = (int)(long)args;

  env.type        = 'A';
  env.data_offset = 0;

  strcpy(env.cwd, "/");
  memset(env.rename_path, 0, sizeof(env.rename_path));
  memset(&env.data_addr, 0, sizeof(env.data_addr));

  running = !ftp_greet(&env);

  while(running) {
    if(!(line=ftp_readline(env.active_fd))) {
      break;
    }

    if(ftp_execute(&env, line)) {
      running = false;
    }

    free(line);
  }

  if(env.active_fd > 0) {
    close(env.active_fd);
  }

  if(env.passive_fd > 0) {
    close(env.passive_fd);
  }

  if(env.data_fd > 0) {
    close(env.data_fd);
  }

  pthread_exit(NULL);

  return NULL;
}


/**
 * Serve FTP on a given port.
 **/
static int
ftp_serve(uint16_t port) {
  struct sockaddr_in server_addr;
  struct sockaddr_in client_addr;
  char ip[INET_ADDRSTRLEN];
  struct ifaddrs *ifaddr;
  notify_request_t req;
  int ifaddr_wait = 1;
  socklen_t addr_len;
  pthread_t trd;
  int connfd;
  int srvfd;

  if(getifaddrs(&ifaddr) == -1) {
    perror("[ftpsrv.elf] getifaddrs");
    _exit(EXIT_FAILURE);
  }

  signal(SIGPIPE, SIG_IGN);

  // Enumerate all AF_INET IPs
  for(struct ifaddrs *ifa=ifaddr; ifa!=NULL; ifa=ifa->ifa_next) {
    if(ifa->ifa_addr == NULL) {
      continue;
    }

    if(ifa->ifa_addr->sa_family != AF_INET) {
      continue;
    }

    // skip localhost
    if(!strncmp("lo", ifa->ifa_name, 2)) {
      continue;
    }
    // skip interfaces without an IP
    if(!strncmp("0.", ifa->ifa_name, 2)) {
      continue;
    }

    struct sockaddr_in *in = (struct sockaddr_in*)ifa->ifa_addr;
    inet_ntop(AF_INET, &(in->sin_addr), ip, sizeof(ip));

    // skip interfaces without an ip
    if(!strncmp("0.", ip, 2)) {
      continue;
    }

    bzero(&req, sizeof(req));
    sprintf(req.message, "Serving FTP on %s:%d (%s)", ip, port, ifa->ifa_name);
    printf("[ftpsrv.elf] %s\n", req.message);
    ifaddr_wait = 0;

#ifdef __PROSPERO__
    sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
#endif
  }

  freeifaddrs(ifaddr);

  if(ifaddr_wait) {
    return 0;
  }

  if((srvfd=socket(AF_INET, SOCK_STREAM, 0)) < 0) {
    perror("[ftpsrv.elf] socket");
    return -1;
  }

  if(setsockopt(srvfd, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int)) < 0) {
    perror("[ftpsrv.elf] setsockopt");
    return -1;
  }

  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  server_addr.sin_port = htons(port);

  if(bind(srvfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) != 0) {
    perror("[ftpsrv.elf] bind");
    return -1;
  }

  if(listen(srvfd, 5) != 0) {
    perror("[ftpsrv.elf] listen");
    return -1;
  }

  addr_len = sizeof(client_addr);

  while(1) {
    if((connfd=accept(srvfd, (struct sockaddr*)&client_addr, &addr_len)) < 0) {
      perror("[ftpsrv.elf] accept");
      break;
    }

    pthread_create(&trd, NULL, ftp_thread, (void*)(long)connfd);
  }

  return close(srvfd);
}


/**
 *
 **/
static void
init_stdio(void) {
  int fd = open("/dev/console", O_RDWR);

  close(STDERR_FILENO);
  close(STDOUT_FILENO);
  close(STDIN_FILENO);

  dup2(fd, STDIN_FILENO);
  dup2(fd, STDOUT_FILENO);
  dup2(fd, STDERR_FILENO);

  close(fd);
}


/**
 * Launch payload.
 **/
int
main() {
  uint16_t port = 2121;

#ifdef __PROSPERO__
  syscall(SYS_setsid);
  syscall(SYS_thr_set_name, -1, "ftpsrv.elf");
  init_stdio();
#endif

  printf("[ftpsrv.elf] FTP server was compiled at %s %s\n", __DATE__, __TIME__);
  while(1) {
    ftp_serve(port);
    sleep(3);
  }

  return 0;
}

