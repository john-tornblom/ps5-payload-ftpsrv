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

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <limits.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>

#include "cmd.h"


/**
 * Constants used for parsing FTP commands.
 **/
#define FTP_LINE_BUFSIZE 1024
#define FTP_TOK_BUFSIZE  128
#define FTP_ARG_DELIM    " \t\r\n\a"
#define FTP_CMD_DELIM    ";"


/**
 * Global state.
 * 
 * TODO: update ps5-payload-sdk with freebsd-11 headers and use atomic_bool
 **/
static bool g_running;


/**
 * Read a line from a file descriptor.
 **/
static char*
ftp_readline(int fd) {
  int bufsize = 1024;
  int position = 0;
  char *buffer_backup;
  char *buffer = malloc(sizeof(char) * bufsize);
  char c;

  if(!buffer) {
    perror("malloc");
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
	perror("realloc");
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

  if(!strcmp(line, "CDUP")) {
    return ftp_cmd_CDUP(env, arg);
  }
  if(!strcmp(line, "CWD")) {
    return ftp_cmd_CWD(env, arg);
  }
  if(!strcmp(line, "DELE")) {
    return ftp_cmd_DELE(env, arg);
  }
  if(!strcmp(line, "LIST")) {
    return ftp_cmd_LIST(env, arg);
  }
  if(!strcmp(line, "MKD")) {
    return ftp_cmd_MKD(env, arg);
  }
  if(!strcmp(line, "NOOP")) {
    return ftp_cmd_NOOP(env, arg);
  }
  if(!strcmp(line, "PASV")) {
    return ftp_cmd_PASV(env, arg);
  }
  if(!strcmp(line, "PWD")) {
    return ftp_cmd_PWD(env, arg);
  }
  if(!strcmp(line, "REST")) {
    return ftp_cmd_REST(env, arg);
  }
  if(!strcmp(line, "RETR")) {
    return ftp_cmd_RETR(env, arg);
  }
  if(!strcmp(line, "RMD")) {
    return ftp_cmd_RMD(env, arg);
  }
  if(!strcmp(line, "RNFR")) {
    return ftp_cmd_RNFR(env, arg);
  }
  if(!strcmp(line, "RNTO")) {
    return ftp_cmd_RNTO(env, arg);
  }
  if(!strcmp(line, "SIZE")) {
    return ftp_cmd_SIZE(env, arg);
  }
  if(!strcmp(line, "STOR")) {
    return ftp_cmd_STOR(env, arg);
  }
  if(!strcmp(line, "SYST")) {
    return ftp_cmd_SYST(env, arg);
  }
  if(!strcmp(line, "USER")) {
    return ftp_cmd_USER(env, arg);
  }
  if(!strcmp(line, "TYPE")) {
    return ftp_cmd_TYPE(env, arg);
  }
  if(!strcmp(line, "QUIT")) {
    return ftp_cmd_QUIT(env, arg);
  }

  // custom commands
  if(!strcmp(line, "MTRW")) {
    return ftp_cmd_MTRW(env, arg);
  }
  if(!strcmp(line, "KILL")) {
    g_running = false; // TODO: atomic_store
    return 0;
  }
  
  const char *msg = "500 Command not recognized\r\n";
  size_t len = strlen(msg);

  if(write(env->active_fd, msg, len) != len) {
    return -1;
  }

  return 0;
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

  // init env
  env.type = 'A';
  env.cwd[0] = '/';
  env.cwd[1] = 0;
  env.data_fd = -1;
  env.passive_fd = -1;
  env.active_fd = (int)(long)args;
  env.data_offset = 0;
  env.rename_path[0] = '\0';

  running = !ftp_greet(&env);

  while(running && g_running) { //TODO: atomic_load
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

  if(env.passive_fd) {
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
  socklen_t addr_len;
  pthread_t trd;
  int sockfd;
  int connfd;
  int flags;

  printf("Launching FTP server on port %d\n", port);
  signal(SIGPIPE, SIG_IGN);

  if((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
    perror("socket");
    return EXIT_FAILURE;
  }

  if(setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int)) < 0) {
    perror("setsockopt");
    return EXIT_FAILURE;
  }

  if((flags=fcntl(sockfd, F_GETFL)) < 0) {
    perror("fcntl");
    return EXIT_FAILURE;
  }

  if(fcntl(sockfd, F_SETFL, flags | O_NONBLOCK) < 0) {
    perror("fcntl");
    return EXIT_FAILURE;
  }

  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  server_addr.sin_port = htons(port);

  if(bind(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) != 0) {
    perror("bind");
    return EXIT_FAILURE;
  }

  if(listen(sockfd, 5) != 0) {
    perror("listen");
    return EXIT_FAILURE;
  }

  addr_len = sizeof(client_addr);
  g_running = true; //TODO: atomic_init

  while(g_running) { // TODO: atomic_load
    if((connfd=accept(sockfd, (struct sockaddr*)&client_addr, &addr_len)) < 0) {
      if(errno != EWOULDBLOCK) {
	perror("accept");
      } else {
	usleep(50 * 1000);
      }
      continue;
    }

    if((flags=fcntl(connfd, F_GETFL)) < 0) {
      perror("fcntl");
      continue;
    }
  
    if(fcntl(connfd, F_SETFL, flags & ~O_NONBLOCK) < 0) {
      perror("fcntl");
      continue;
    }

    pthread_create(&trd, NULL, ftp_thread, (void*)(long)connfd);
  }

  close(sockfd);
  printf("Server killed\n");

  return EXIT_SUCCESS;
}


/**
 * Launch payload.
 **/
int
main() {
  uint16_t port = 2121;

#if defined(__FreeBSD__) && defined(FORK_SERVER)
  if (rfork_thread(RFPROC | RFFDG,
		   malloc(0x4000),
		   (void*)ftp_serve,
		   (void*)(long)port) < 0) {
    return EXIT_FAILURE;
  } else {
    return EXIT_SUCCESS;
  }
#elif defined(FORK_SERVER)
  switch(fork()) {
  case 0:
    return ftp_serve(port);
  case -1:
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
#else
    return ftp_serve(port);
#endif
}

