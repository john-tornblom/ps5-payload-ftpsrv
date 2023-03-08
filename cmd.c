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
#include <string.h>
#include <stddef.h>
#include <stdarg.h>
#include <unistd.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <time.h>
#include <dirent.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <limits.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <inttypes.h>

#include "cmd.h"


/**
 * Create a string representation of a file mode.
 **/
static void
ftp_mode_string(mode_t mode, char *buf) {
  char c, d;
  int i, bit;

  buf[10]=0;
  for (i=0; i<9; i++) {
    bit = mode & (1<<i);
    c = i%3;
    if (!c && (mode & (1<<((d=i/3)+9)))) {
      c = "tss"[(int)d];
      if (!bit) c &= ~0x20;
    } else c = bit ? "xwr"[(int)c] : '-';
    buf[9-i] = c;
  }

  if (S_ISDIR(mode)) c = 'd';
  else if (S_ISBLK(mode)) c = 'b';
  else if (S_ISCHR(mode)) c = 'c';
  else if (S_ISLNK(mode)) c = 'l';
  else if (S_ISFIFO(mode)) c = 'p';
  else if (S_ISSOCK(mode)) c = 's';
  else c = '-';
  *buf = c;
}


/**
 * Open a new PASSV FTP data connection.
 **/
static int
ftp_data_open(ftp_env_t *env) {
  struct sockaddr_in data_addr;
  socklen_t addr_len;

  if((env->data_fd=accept(env->passive_fd, (struct sockaddr*)&data_addr,
			  &addr_len)) < 0) {
    return -1;
  }

  return 0;
}


/**
 * Transmit a formatted string via an existing data connection.
 **/
static int
ftp_data_printf(ftp_env_t *env, const char *fmt, ...) {
  char buf[0x1000];
  size_t len = 0;
  va_list args;

  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);

  len = strlen(buf);
  if(write(env->data_fd, buf, len) != len) {
    return -1;
  }

  return 0;
}


/**
 * Read data from an existing data connection.
 **/
static int
ftp_data_read(ftp_env_t *env, void *buf, size_t count) {
  return recv(env->data_fd, buf, count, 0);
}


/**
 * Transmit data on an existing data connection.
 **/
static int
ftp_data_send(ftp_env_t *env, void *buf, size_t count) {
  return send(env->data_fd, buf, count, 0);
}


/**
 * Close an existing data connection.
 **/
static int
ftp_data_close(ftp_env_t *env) {
  return close(env->data_fd);
}


/**
 * Transmit a formatted string via an active connection.
 **/
static int
ftp_active_printf(ftp_env_t *env, const char *fmt, ...) {
  char buf[0x1000];
  size_t len = 0;
  va_list args;

  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);

  len = strlen(buf);

  if(write(env->active_fd, buf, len) != len) {
    return -1;
  }

  return 0;
}


/**
 * Transmit an errno string via an active connection.
 **/
static int
ftp_perror(ftp_env_t *env) {
  char buf[255];

  if(strerror_r(errno, buf, sizeof(buf))) {
    strncpy(buf, "Unknown error", sizeof(buf));
  }

  return ftp_active_printf(env, "550 %s\r\n", buf);
}


/**
 *
 **/
static void
ftp_abspath(ftp_env_t *env, char *abspath, const char *path) {
  char buf[PATH_MAX+1];

  if(path[0] != '/') {
    snprintf(buf, sizeof(buf), "%s/%s", env->cwd, path);
    strncpy(abspath, buf, PATH_MAX);
  } else {
    strncpy(abspath, path, PATH_MAX);
  }
}


/**
 *
 **/
int
ftp_cmd_CWD(int argc, char **argv, ftp_env_t *env) {
  char pathbuf[PATH_MAX];
  struct stat st;

  if(argc < 2) {
    return ftp_active_printf(env, "501 Usage: %s <PATH>\r\n", argv[0]);
  }

  ftp_abspath(env, pathbuf, argv[1]);
  if(stat(pathbuf, &st)) {
    return ftp_perror(env);
  }

  if(!S_ISDIR(st.st_mode)) {
    return ftp_active_printf(env, "550 No such directory\r\n");
  }

  snprintf(env->cwd, sizeof(env->cwd), "%s", pathbuf);

  return ftp_active_printf(env, "250 OK\r\n");
}


/**
 *
 **/
int
ftp_cmd_LIST(int argc, char **argv, ftp_env_t *env) {
  char pathbuf[PATH_MAX+256+2];
  struct dirent **namelist;
  const char *p = env->cwd;
  struct stat statbuf;
  char timebuf[20];
  char modebuf[20];
  struct tm * tm;
  int n;

  for(int i=1; i<argc; i++) {
    if(argv[i][0] != '-') {
      p = argv[i];
    }
  }

  if((n=scandir(p, &namelist, NULL, alphasort)) < 0) {
    return ftp_perror(env);
  }

  if(ftp_data_open(env)) {
    return ftp_perror(env);
  }

  ftp_active_printf(env, "150 Opening data transfer\r\n");

  for(int i=0; i<n; i++) {
    if(p[0] == '/') {
      snprintf(pathbuf, sizeof(pathbuf), "%s/%s", p, namelist[i]->d_name);
    } else {
      snprintf(pathbuf, sizeof(pathbuf), "/%s/%s/%s", env->cwd, p, namelist[i]->d_name);
    }

    if(stat(pathbuf, &statbuf) != 0) {
      continue;
    }

    ftp_mode_string(statbuf.st_mode, modebuf);
    tm = localtime((const time_t *)&(statbuf.st_ctim)); // TODO: not threadsafe
    strftime(timebuf, sizeof(timebuf), "%b %d %H:%M", tm);

    ftp_data_printf(env, "%s %lu %lu %lu %llu %s %s\r\n", modebuf,
		    statbuf.st_nlink, statbuf.st_uid, statbuf.st_gid,
		    statbuf.st_size, timebuf, namelist[i]->d_name);
    free(namelist[i]);
  }

  free(namelist);

  if(ftp_data_close(env)) {
    return ftp_perror(env);
  }

  return ftp_active_printf(env, "226 Transfer complete\r\n");
}


/**
 * Enter passive mode.
 **/
int
ftp_cmd_PASV(int argc, char **argv, ftp_env_t *env) {
  socklen_t sockaddr_len = sizeof(struct sockaddr_in);
  struct sockaddr_in sockaddr;
  uint32_t addr = 0;
  uint16_t port = 0;

  if(getsockname(env->active_fd, (struct sockaddr*)&sockaddr, &sockaddr_len)) {
    return ftp_perror(env);
  }
  addr = sockaddr.sin_addr.s_addr;

  if((env->passive_fd=socket(AF_INET, SOCK_STREAM, 0)) < 0) {
    return ftp_perror(env);
  }

  memset(&sockaddr, 0, sockaddr_len);
  sockaddr.sin_family = AF_INET;
  sockaddr.sin_addr.s_addr = htonl(INADDR_ANY);
  sockaddr.sin_port = htons(0);

  if(bind(env->passive_fd, (struct sockaddr*)&sockaddr, sockaddr_len) != 0) {
    int ret = ftp_perror(env);
    close(env->passive_fd);
    return ret;
  }

  if(listen(env->passive_fd, 5) != 0) {
    int ret = ftp_perror(env);
    close(env->passive_fd);
    return ret;
  }

  if(getsockname(env->passive_fd, (struct sockaddr*)&sockaddr, &sockaddr_len)) {
    int ret = ftp_perror(env);
    close(env->passive_fd);
    return ret;
  }
  port = sockaddr.sin_port;

  return ftp_active_printf(env, "227 Entering Passive Mode (%hhu,%hhu,%hhu,%hhu,%hhu,%hhu).\r\n",
			   (addr >> 0) & 0xFF,
			   (addr >> 8) & 0xFF,
			   (addr >> 16) & 0xFF,
			   (addr >> 24) & 0xFF,
			   (port >> 0) & 0xFF,
			   (port >> 8) & 0xFF);
}

/**
 * Return the size of a given file.
 **/
int
ftp_cmd_RETR(int argc, char **argv, ftp_env_t *env) {
  char pathbuf[PATH_MAX];
  char buf[0x1000];
  struct stat st;
  size_t len;
  int fd;

  if(argc < 2) {
    return ftp_active_printf(env, "501 Usage: %s <PATH>\r\n", argv[0]);
  }

  ftp_abspath(env, pathbuf, argv[1]);
  if(stat(pathbuf, &st)) {
    return ftp_perror(env);
  }

  if(S_ISDIR(st.st_mode)) {
    return ftp_cmd_LIST(argc, argv, env);
  }

  if((fd=open(pathbuf, O_RDONLY, 0)) < 0) {
    return ftp_active_printf(env, "550 %s\r\n", strerror(errno));
  }

  if(ftp_active_printf(env, "150 Opening data transfer\r\n")) {
    return ftp_perror(env);
  }

  if(ftp_data_open(env)) {
    return ftp_perror(env);
  }

  if(lseek(fd, env->data_offset, SEEK_SET) == -1) {
    int ret = ftp_perror(env);
    close(fd);
    return ret;
  }

  while((len=read(fd, buf, sizeof(buf)))) {
    if(ftp_data_send(env, buf, len) != len) {
      int ret = ftp_perror(env);
      close(fd);
      return ret;
    }
  }

  close(fd);

  if(ftp_data_close(env)) {
    return ftp_perror(env);
  }

  return ftp_active_printf(env, "226 Transfer completed\r\n");
}



/**
 * Return the size of a given file.
 **/
int
ftp_cmd_SIZE(int argc, char **argv, ftp_env_t *env) {
  struct stat st;

  if(argc < 2) {
    return ftp_active_printf(env, "501 Usage: %s <FILENAME>\r\n", argv[0]);
  }

  if(stat(argv[1], &st)) {
    return ftp_perror(env);
  }

  return ftp_active_printf(env, "213 %"  PRIu64 "\r\n", st.st_size);
}


/**
 *
 **/
int
ftp_cmd_STOR(int argc, char **argv, ftp_env_t *env) {
  uint8_t readbuf[0x4000];
  char pathbuf[PATH_MAX];
  size_t len;
  int fd;

  if(argc < 2) {
    return ftp_active_printf(env, "501 Usage: %s <FILENAME>\r\n", argv[0]);
  }

  ftp_abspath(env, pathbuf, argv[1]);
  if((fd=open(pathbuf, O_CREAT | O_WRONLY | O_TRUNC, 0777)) < 0) {
    return ftp_perror(env);
  }

  if(ftp_active_printf(env, "150 Opening data transfer\r\n")) {
    close(fd);
    return -1;
  }

  if(ftp_data_open(env)) {
    int ret = ftp_perror(env);
    close(fd);
    return ret;
  }

  while((len=ftp_data_read(env, readbuf, sizeof(readbuf)))) {
    if(write(fd, readbuf, len) != len) {
      int ret = ftp_perror(env);
      ftp_data_close(env);
      close(fd);
      return ret;
    }
  }

  close(fd);

  if(ftp_data_close(env)) {
    return ftp_perror(env);
  }

  return ftp_active_printf(env, "226 Data transfer complete\r\n");
}


/**
 * Print working directory.
 **/
int
ftp_cmd_PWD(int argc, char **argv, ftp_env_t *env) {
  return ftp_active_printf(env, "257 \"%s\"\r\n", env->cwd);
}


/**
 * Disconnect user.
 **/
int
ftp_cmd_QUIT(int argc, char **argv, ftp_env_t *env) {
  ftp_active_printf(env, "221 Goodbye\r\n");
  return -1;
}


/**
 * Return system type.
 **/
int
ftp_cmd_SYST(int argc, char **argv, ftp_env_t *env) {
  return ftp_active_printf(env, "215 UNIX Type: L8\r\n");
}


/**
 * Sets the transfer mode (ASCII or Binary).
 **/
int
ftp_cmd_TYPE(int argc, char **argv, ftp_env_t *env) {
  if(argc < 2) {
    return ftp_active_printf(env, "501 Usage: %s <A|I>\r\n", argv[0]);
  }

  switch(argv[1][0]) {
  case 'A':
  case 'I':
    env->type = argv[1][0];
    return ftp_active_printf(env, "200 Type set to %c\r\n", env->type);
  }

  return ftp_active_printf(env, "501 Invalid argument to TYPE\r\n");
}


/**
 * Authenticate user.
 **/
int
ftp_cmd_USER(int argc, char **argv, ftp_env_t *env) {
  return ftp_active_printf(env, "230 User logged in\r\n");
}


/**
 * Unsupported command.
 **/
int
ftp_cmd_NA(int argc, char **argv, ftp_env_t *env) {
  return ftp_active_printf(env, "502 Command not implemented\r\n");
}


/**
 * No operation.
 **/
int
ftp_cmd_NOOP(int argc, char **argv, ftp_env_t *env) {
  return ftp_active_printf(env, "200 NOOP OK\r\n");
}


/**
 *
 **/
int
ftp_cmd_REST(int argc, char **argv, ftp_env_t *env) {
  if(argc < 2) {
    return ftp_active_printf(env, "501 Usage: %s <OFFSET>\r\n", argv[0]);
  }

  env->data_offset = atol(argv[1]);

  return 0;
}


/**
 *
 **/
int
ftp_cmd_DELE(int argc, char **argv, ftp_env_t *env) {
  char pathbuf[PATH_MAX];

  if(argc < 2) {
    return ftp_active_printf(env, "501 Usage: %s <FILENAME>\r\n", argv[0]);
  }

  ftp_abspath(env, pathbuf, argv[1]);
  if(remove(pathbuf)) {
    return ftp_perror(env);
  }

  return ftp_active_printf(env, "226 File deleted\r\n");
}


/**
 *
 **/
int
ftp_cmd_MKD(int argc, char **argv, ftp_env_t *env) {
  char pathbuf[PATH_MAX];

  if(argc < 2) {
    return ftp_active_printf(env, "501 Usage: %s <DIRNAME>\r\n", argv[0]);
  }

  ftp_abspath(env, pathbuf, argv[1]);
  if(mkdir(pathbuf, 0777)) {
    return ftp_perror(env);
  }

  return ftp_active_printf(env, "226 Directory created\r\n");
}


/**
 *
 **/
int
ftp_cmd_CDUP(int argc, char **argv, ftp_env_t *env) {
  int pos = -1;

  for(size_t i=0; i<sizeof(env->cwd); i++) {
    if(!env->cwd[i]) {
      break;
    } else if(env->cwd[i] == '/') {
      pos = i;
    }
  }

  if(pos > 0) {
    env->cwd[pos] = '\0';
  }

  return ftp_active_printf(env, "250 OK\r\n");
}


/**
 *
 **/
int
ftp_cmd_RMD(int argc, char **argv, ftp_env_t *env) {
  char pathbuf[PATH_MAX];

  if(argc < 2) {
    return ftp_active_printf(env, "501 Usage: %s <DIRNAME>\r\n", argv[0]);
  }

  ftp_abspath(env, pathbuf, argv[1]);
  if(rmdir(pathbuf)) {
    return ftp_perror(env);
  }

  return ftp_active_printf(env, "226 Directory deleted\r\n");
}


/**
 *
 **/
int
ftp_cmd_RNFR(int argc, char **argv, ftp_env_t *env) {
  struct stat st;

  if(argc < 2) {
    return ftp_active_printf(env, "501 Usage: %s <PATH>\r\n", argv[0]);
  }

  ftp_abspath(env, env->rename_path, argv[1]);
  if(stat(env->rename_path, &st)) {
    return ftp_perror(env);
  }

  return ftp_active_printf(env, "350 Awaiting new name\r\n");
}


/**
 *
 **/
int
ftp_cmd_RNTO(int argc, char **argv, ftp_env_t *env) {
  char pathbuf[PATH_MAX];
  struct stat st;

  if(argc < 2) {
    return ftp_active_printf(env, "501 Usage: %s <PATH>\r\n", argv[0]);
  }

  if(stat(env->rename_path, &st)) {
    return ftp_perror(env);
  }

  ftp_abspath(env, pathbuf, argv[1]);
  if(rename(env->rename_path, pathbuf)) {
    return ftp_perror(env);
  }

  return ftp_active_printf(env, "226 Path renamed\r\n");
}

