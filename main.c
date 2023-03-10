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
 * Split a string into an array of substrings seperated by
 * a delimiter.
 **/
static char**
ftp_splitstring(char *line, char *delim) {
  int bufsize = 1024;
  int position = 0;
  char **tokens = malloc(bufsize * sizeof(char*));
  char *token, **tokens_backup;

  if(!tokens) {
    perror("malloc");
    return NULL;
  }

  token = strtok(line, delim);
  while(token != NULL) {
    tokens[position] = token;
    position++;

    if(position >= bufsize) {
      bufsize += 1024;
      tokens_backup = tokens;
      tokens = realloc(tokens, bufsize * sizeof(char*));
      if(!tokens) {
	perror("realloc");
	free(tokens_backup);
	return NULL;
      }
    }

    token = strtok(NULL, delim);
  }
  tokens[position] = NULL;
  return tokens;
}


/**
 * Execute an FTP command.
 **/
static int
ftp_execute(ftp_env_t *env, char **argv) {
  int argc = 0;
  while(argv[argc]) {
    argc++;
  }

  if(!argc) {
    return -1;
  }

  if(!strcmp(argv[0], "CDUP")) {
    return ftp_cmd_CDUP(argc, argv, env);
  }
  if(!strcmp(argv[0], "CWD")) {
    return ftp_cmd_CWD(argc, argv, env);
  }
  if(!strcmp(argv[0], "DELE")) {
    return ftp_cmd_DELE(argc, argv, env);
  }
  if(!strcmp(argv[0], "LIST")) {
    return ftp_cmd_LIST(argc, argv, env);
  }
  if(!strcmp(argv[0], "MKD")) {
    return ftp_cmd_MKD(argc, argv, env);
  }
  if(!strcmp(argv[0], "NOOP")) {
    return ftp_cmd_NOOP(argc, argv, env);
  }
  if(!strcmp(argv[0], "PASV")) {
    return ftp_cmd_PASV(argc, argv, env);
  }
  if(!strcmp(argv[0], "PWD")) {
    return ftp_cmd_PWD(argc, argv, env);
  }
  if(!strcmp(argv[0], "REST")) {
    return ftp_cmd_REST(argc, argv, env);
  }
  if(!strcmp(argv[0], "RETR")) {
    return ftp_cmd_RETR(argc, argv, env);
  }
  if(!strcmp(argv[0], "RMD")) {
    return ftp_cmd_RMD(argc, argv, env);
  }
  if(!strcmp(argv[0], "RNFR")) {
    return ftp_cmd_RNFR(argc, argv, env);
  }
  if(!strcmp(argv[0], "RNTO")) {
    return ftp_cmd_RNTO(argc, argv, env);
  }
  if(!strcmp(argv[0], "SIZE")) {
    return ftp_cmd_SIZE(argc, argv, env);
  }
  if(!strcmp(argv[0], "STOR")) {
    return ftp_cmd_STOR(argc, argv, env);
  }
  if(!strcmp(argv[0], "SYST")) {
    return ftp_cmd_SYST(argc, argv, env);
  }
  if(!strcmp(argv[0], "USER")) {
    return ftp_cmd_USER(argc, argv, env);
  }
  if(!strcmp(argv[0], "TYPE")) {
    return ftp_cmd_TYPE(argc, argv, env);
  }
  if(!strcmp(argv[0], "QUIT")) {
    return ftp_cmd_QUIT(argc, argv, env);
  }

  // custom commands
  if(!strcmp(argv[0], "MTRW")) {
    return ftp_cmd_MTRW(argc, argv, env);
  }
  if(!strcmp(argv[0], "KILL")) {
    g_running = false; // TODO: atomic_store
    return 0;
  }
  
  const char *cmd = "500 Command not recognized\r\n";
  size_t len = strlen(cmd);

  if(write(env->active_fd, cmd, len) != len) {
    return -1;
  }

  return 0;
}


/**
 * Greet a new FTP connection.
 **/
static int
ftp_greet(ftp_env_t *env) {
  const char *cmd = "220 Service is ready\r\n";
  size_t len = strlen(cmd);

  if(write(env->active_fd, cmd, len) != len) {
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
  char **cmds;
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

    if(!(cmds=ftp_splitstring(line, FTP_CMD_DELIM))) {
      free(line);
      break;
    }

    for(int i=0; cmds[i]; i++) {
      if(!(args=ftp_splitstring(cmds[i], FTP_ARG_DELIM))) {
	continue;
      }

      if(ftp_execute(&env, args)) {
	running = false;
      }
      free(args);
    }

    free(line);
    free(cmds);
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
static void
ftp_serve(uint16_t port) {
  struct sockaddr_in server_addr;
  struct sockaddr_in client_addr;
  socklen_t addr_len;
  pthread_t trd;
  int sockfd;
  int connfd;
  int flags;
  
  if((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
    perror("socket");
    return;
  }

  if(setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int)) < 0) {
    perror("setsockopt");
    return;
  }

  if((flags=fcntl(sockfd, F_GETFL)) < 0) {
    perror("fcntl");
    return;
  }

  if(fcntl(sockfd, F_SETFL, flags | O_NONBLOCK) < 0) {
    perror("fcntl");
    return;
  }

  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  server_addr.sin_port = htons(port);

  if(bind(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) != 0) {
    perror("bind");
    return;
  }

  if(listen(sockfd, 5) != 0) {
    perror("listen");
    return;
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
      return;
    }
  
    if(fcntl(connfd, F_SETFL, flags & ~O_NONBLOCK) < 0) {
      perror("fcntl");
      continue;
    }

    pthread_create(&trd, NULL, ftp_thread, (void*)(long)connfd);
  }

  close(sockfd);
}


/**
 * Launch payload.
 **/
int
main() {
  uint16_t port = 2121;

  printf("Launching FTP server on port %d\n", port);
  signal(SIGPIPE, SIG_IGN);
  ftp_serve(port);
  printf("Server killed\n");

  return 0;
}

