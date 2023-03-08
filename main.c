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

#include "cmd.h"


/**
 * Constants used for parsing FTP commands.
 **/
#define FTP_LINE_BUFSIZE 1024
#define FTP_TOK_BUFSIZE  128
#define FTP_ARG_DELIM    " \t\r\n\a"
#define FTP_CMD_DELIM    ";"


/**
 * Map names of commands to function entry points.
 **/
typedef struct ftp_command {
  const char       *name;
  ftp_command_fn_t *func;
} ftp_command_t;


/**
 *
 **/
ftp_command_t commands[] = {
  {"CDUP", ftp_cmd_CDUP},
  {"CWD",  ftp_cmd_CWD},
  {"DELE", ftp_cmd_DELE},
  {"LIST", ftp_cmd_LIST},
  {"MKD",  ftp_cmd_MKD},
  {"NOOP", ftp_cmd_NOOP},
  {"PASV", ftp_cmd_PASV},
  {"PWD",  ftp_cmd_PWD},
  {"REST", ftp_cmd_REST},
  {"RETR", ftp_cmd_RETR},
  {"RMD",  ftp_cmd_RMD},
  {"RNFR", ftp_cmd_RNFR},
  {"RNTO", ftp_cmd_RNTO},
  {"SIZE", ftp_cmd_SIZE},
  {"STOR", ftp_cmd_STOR},
  {"SYST", ftp_cmd_SYST},
  {"USER", ftp_cmd_USER},
  {"TYPE", ftp_cmd_TYPE},
  {"QUIT", ftp_cmd_QUIT},

  {"XCUP", ftp_cmd_NA},
  {"XMKD", ftp_cmd_NA},
  {"XPWD", ftp_cmd_NA},
  {"XRCP", ftp_cmd_NA},
  {"XRMD", ftp_cmd_NA},
  {"XRSQ", ftp_cmd_NA},
  {"XSEM", ftp_cmd_NA},
  {"XSEN", ftp_cmd_NA},
};


/**
 *
 **/
#define NB_FTP_COMMANDS (sizeof(commands)/sizeof(ftp_command_t))


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

  for(int i=0; i<NB_FTP_COMMANDS; i++) {
    if(strcmp(argv[0], commands[i].name)) {
      continue;
    }

    return commands[i].func(argc, argv, env);
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
 * Entry point for new FTP connections
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

  while(running) {
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

  if((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
    perror("socket");
    return;
  }

  if(setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int)) < 0) {
    perror("setsockopt");
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

  while(1) {
    if((connfd=accept(sockfd, (struct sockaddr*)&client_addr, &addr_len)) < 0) {
      perror("accept");
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

  ftp_serve(port);

  return 0;
}

