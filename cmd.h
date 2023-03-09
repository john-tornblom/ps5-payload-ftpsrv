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

#pragma once

#include <limits.h>

/**
 *
 **/
typedef struct ftp_env {
  int  data_fd;
  int  active_fd;
  int  passive_fd;
  char cwd[PATH_MAX];
  char type;
  off_t data_offset;
  char rename_path[PATH_MAX];
} ftp_env_t;


/**
 * Callback function prototype for ftp commands.
 **/
typedef int (ftp_command_fn_t)(int argc, char **argv, ftp_env_t *env);

int ftp_cmd_CDUP(int argc, char **argv, ftp_env_t *env);
int ftp_cmd_CWD (int argc, char **argv, ftp_env_t *env);
int ftp_cmd_DELE(int argc, char **argv, ftp_env_t *env);
int ftp_cmd_LIST(int argc, char **argv, ftp_env_t *env);
int ftp_cmd_MKD (int argc, char **argv, ftp_env_t *env);
int ftp_cmd_NOOP(int argc, char **argv, ftp_env_t *env);
int ftp_cmd_PASV(int argc, char **argv, ftp_env_t *env);
int ftp_cmd_PWD (int argc, char **argv, ftp_env_t *env);
int ftp_cmd_QUIT(int argc, char **argv, ftp_env_t *env);
int ftp_cmd_REST(int argc, char **argv, ftp_env_t *env);
int ftp_cmd_RETR(int argc, char **argv, ftp_env_t *env);
int ftp_cmd_RMD (int argc, char **argv, ftp_env_t *env);
int ftp_cmd_RNFR(int argc, char **argv, ftp_env_t *env);
int ftp_cmd_RNTO(int argc, char **argv, ftp_env_t *env);
int ftp_cmd_SIZE(int argc, char **argv, ftp_env_t *env);
int ftp_cmd_STOR(int argc, char **argv, ftp_env_t *env);
int ftp_cmd_SYST(int argc, char **argv, ftp_env_t *env);
int ftp_cmd_TYPE(int argc, char **argv, ftp_env_t *env);
int ftp_cmd_USER(int argc, char **argv, ftp_env_t *env);

int ftp_cmd_MTRW(int argc, char **argv, ftp_env_t *env);

int ftp_cmd_NA(int argc, char **argv, ftp_env_t *env);
