/**
   @file bwchat_cgi.c
   @brief bwchat CGI
   @author defanor <defanor@thunix.net>
   @date 2024
   @copyright MIT license
*/

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <libgen.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <syslog.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <sys/select.h>
#include <argp.h>

#include "bwchat.h"

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#ifdef HAVE_FCGI
#include "fcgi_stdio.h"
#endif

#define BOUNDARY_LENGTH 128
#define FIELD_NAME_LENGTH 128
#define FILENAME_LENGTH 128

enum form_data_parsing_state {
  FORM_PARSE_START,
  FORM_PARSE_DONE,
  FORM_PARSE_FAIL,
  FORM_PARSE_HEADER,
  FORM_PARSE_DATA
};

enum param_read_state {
  PARAM_READ_SEARCH,
  PARAM_READ_FOUND_COLON,
  PARAM_READ_FOUND_PARAM,
  PARAM_READ_QUOTED,
  PARAM_READ_UNQUOTED,
  PARAM_READ_SKIP_QUOTED
};

/* Global state */
int sock = -1;

/* Settings */
const char *upload_dir_url = "upload/";
const char *js_url = "bwchat.js";
const char *sock_path = "bwchat-socket";
int log_stderr = 0;

char *html_escape (char *dst, const char *src, size_t sz) {
  size_t i, j;
  for (i = 0, j = 0; (i < (sz - 1)) && (j < (sz - 1)); i++, j++) {
    if (src[i] == '<') {
      strncpy(dst + j, "&lt;", sz - j);
      j += 3;
    } else if (src[i] == '>') {
      strncpy(dst + j, "&gt;", sz - j);
      j += 3;
    } else if (src[i] == '"') {
      strncpy(dst + j, "&quot;", sz - j);
      j += 5;
    } else if (src[i] == '&') {
      strncpy(dst + j, "&amp;", sz - j);
      j += 4;
    } else {
      dst[j] = src[i];
    }
  }
  if (j > sz - 1) {
    j = sz - 1;
  }
  dst[j] = '\0';
  return dst;
}

/*
  https://www.rfc-editor.org/rfc/rfc822 -- Internet text messages
  https://www.rfc-editor.org/rfc/rfc2183 -- Content-Disposition
  https://www.rfc-editor.org/rfc/rfc7578 -- multipart/form-data
*/
char *read_param (const char *line, const char *name, char *dst, size_t sz) {
  size_t i, j;
  size_t line_len = strlen(line);
  size_t name_len = strlen(name);
  enum param_read_state s = PARAM_READ_SEARCH;
  dst[0] = '\0';
  for (i = 0, j = 0; i < line_len && j < sz; i++) {
    if (s == PARAM_READ_SEARCH) {
      if (line[i] == ';') {
        s = PARAM_READ_FOUND_COLON;
      } else if (line[i] == '"') {
        s = PARAM_READ_SKIP_QUOTED;
      }
    } else if (s == PARAM_READ_FOUND_COLON && line[i] != ' ') {
      if (i + name_len + 1 < line_len &&
          strncmp(line + i, name, name_len) == 0 &&
          line[i + name_len] == '=') {
        s = PARAM_READ_FOUND_PARAM;
        i += name_len;
      }
    } else if (s == PARAM_READ_FOUND_PARAM) {
      if (line[i] == '"') {
        s = PARAM_READ_QUOTED;
      } else {
        s = PARAM_READ_UNQUOTED;
        dst[j] = line[i];
        j++;
      }
    } else if (s == PARAM_READ_SKIP_QUOTED) {
      if (line[i] == '\\') {
        i++;
      } else if (line[i] == '"') {
        s = PARAM_READ_SEARCH;
      }
    } else if (s == PARAM_READ_UNQUOTED) {
      if (line[i] == '\r' || line[i] == '\n' ||
          line[i] == ';' || line[i] == ' ') {
        dst[j] = '\0';
        return dst;
      }
      dst[j] = line[i];
      j++;
    } else if (s == PARAM_READ_QUOTED) {
      if (line[i] == '\\') {
        i++;
      } else if (line[i] == '"') {
        dst[j] = '\0';
        return dst;
      }
      dst[j] = line[i];
      j++;
    }
  }
  if (j > 0) {
    dst[j] = '\0';
    return dst;
  }
  return NULL;
}


/* Reads data until a given substring is found. Aims successive calls,
   to read data in chunks. The zero byte is inserted after the data,
   making it suitable for textual data reading. */
int read_till (const char *end,
               char *out,
               size_t out_buf_len,
               size_t *out_data_len,
               size_t *matched)
{
  size_t end_len = strlen(end);

  /* Check for a previous iteration's leftover, move it. */
  if (*out_data_len > 0) {
    memmove(out, out + *out_data_len, *matched);
    *out_data_len = 0;
  }

  while (*out_data_len + *matched < out_buf_len) {
    int c = getchar();
    if (c == -1) {
      return -1;
    }
    out[*out_data_len + *matched] = c;
    if (out[*out_data_len + *matched] == end[*matched]) {
      /* Extending the match */
      *matched = *matched + 1;
    } else {
      /* Not matching: find the new matching prefix */
      *out_data_len = *out_data_len + 1;
      while (*matched > 0 &&
             strncmp(out + *out_data_len, end, *matched) != 0) {
        *matched = *matched - 1;
        *out_data_len = *out_data_len + 1;
      }
    }

    /* Found the match */
    if (end_len == *matched) {
      out[*out_data_len] = '\0';
      return 0;
    }
  }
  /* Not found yet: the caller should consume the read bytes and keep
     iterating. */
  return 1;
}


int sock_conn() {
  struct sockaddr_un addr;
  socklen_t addr_size;
  sock = socket(AF_UNIX, SOCK_SEQPACKET, 0);
  if (sock < 0) {
    return -1;
  }
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);
  addr_size = sizeof(struct sockaddr_un);
  if (connect(sock, (struct sockaddr *) &addr, addr_size) < 0) {
    return -1;
  }
  return sock;
}

int print_message (struct bwchat_message *msg) {
  char nick[BWC_NICK_LENGTH];
  char message[BWC_MESSAGE_LENGTH];
  struct tm *btime;

  if (msg->type == BWC_MESSAGE_NONE) {
    return 0;
  }
  btime = localtime(&(msg->timestamp));
  strftime(message, BWC_MESSAGE_LENGTH, "%H:%M", btime);
  html_escape(nick, msg->nick, BWC_NICK_LENGTH);
  if (printf("      <div>%s <b>%s</b>: ", message, nick) < 0) {
    return -1;
  }
  if (msg->type == BWC_MESSAGE_TEXT) {
    html_escape(message, msg->data, BWC_MESSAGE_LENGTH);
    if (printf("%s", message) < 0) {
      return -1;
    }
  } else if (msg->type == BWC_MESSAGE_UPLOAD) {
    html_escape(message, msg->data, BWC_MESSAGE_LENGTH);
    if (printf("<a href=\"%s%s\">%s</a>", upload_dir_url,
               message, message) < 0) {
      return -1;
    }
  } else if (msg->type == BWC_MESSAGE_AUDIO) {
    if (printf
        ("<audio controls=\"\" preload=\"none\" src=\"stream?%s\"></audio>",
         nick) < 0) {
      return -1;
    }
  }
  if (puts("</div>") < 0) {
    return -1;
  }
  return 0;
}

int print_messages () {
  struct bwchat_message msg;
  ssize_t len;
  char c = BWC_CMD_ALL_MESSAGES;
  if (printf("    <div id=\"messages\">\n") < 0) {
    return -1;
  }
  write(sock, &c, 1);
  while (1) {
    len = read(sock, &msg, sizeof(msg));
    if (len == 0) {
      break;
    }
    if (len < (ssize_t)sizeof(msg)) {
      return -1;
    }
    if (print_message(&msg) != 0) {
      return -1;
    }
  }
  if (printf("    </div>\n") < 0) {
    return -1;
  }
  return 0;
}


int serve_messages () {
  fd_set rset;
  struct timeval timeout;
  struct bwchat_message msg;
  char c = BWC_CMD_NEW_MESSAGES;
  int ret;
  if (printf("Content-type: text/html\r\n"
             "Cache-Control: no-cache\r\n"
             "X-Accel-Buffering: no\r\n"
             "\r\n") < 0) {
    return -1;
  }
  write(sock, &c, 1);

  while (1) {
    timeout.tv_sec = 10;
    timeout.tv_usec = 0;
    FD_ZERO(&rset);
    FD_SET(sock, &rset);
    ret = select(sock + 1, &rset, NULL, NULL, &timeout);
    if (ret == 0) {
      /* Timeout, send a ping. Primarily to see if the client is still
         there, though it would also let the client know that the
         connection is live, and possibly help to avoid gateway
         timeouts. */
      if (puts("") < 0) {
        break;
      }
    } else if (ret == 1) {
      /* Input available */
      if (read(sock, &msg, sizeof(msg)) != sizeof(msg)) {
        syslog(LOG_WARNING, "serve_messages: bwchat-server is gone");
        return 0;
      }
      if (print_message(&msg) != 0) {
        break;
      }
    } else if (ret == -1) {
      /* Error */
      syslog(LOG_ERR, "select() error in serve_messages");
      break;
    }
    if (fflush(stdout) < 0) {
      break;
    }
  }
  syslog(LOG_DEBUG, "a message listener is gone");
  return 0;
}

int serve_stream () {
  fd_set rset;
  struct timeval timeout;
  char *query_string = getenv("QUERY_STRING");
  char buf[BWC_MESSAGE_LENGTH];
  size_t len;
  int ret;
  buf[0] = BWC_CMD_AUDIO_STREAM;
  strncpy(buf + 1, query_string, BWC_NICK_LENGTH);
  write(sock, buf, BWC_NICK_LENGTH + 1);

  /* Send HTTP headers */
  printf("Content-type: audio/ogg\r\n"
         "Cache-Control: no-cache\r\n"
         "X-Accel-Buffering: no\r\n"
         "\r\n");

  /* Wait for new stream chunks, pass them to the client */
  while (1) {
    timeout.tv_sec = 10;
    timeout.tv_usec = 0;
    FD_ZERO(&rset);
    FD_SET(sock, &rset);
    ret = select(sock + 1, &rset, NULL, NULL, &timeout);
    if (ret != 1) {
      /* Timeout or error: break. */
      break;
    }
    len = read(sock, &buf, sizeof(buf));
    if (len <= 0) {
      syslog(LOG_WARNING, "serve_stream: bwchat-server is gone");
      return 0;
    }
    if (fwrite(buf, 1, len, stdout) < len) {
      break;
    }
    if (fflush(stdout) < 0) {
      break;
    }
  }
  syslog(LOG_DEBUG, "an audio stream listener is gone");
  return 0;
}

int handle_chat () {
  size_t message_len = 0, matched = 0, len = 0;
  enum form_data_parsing_state ps = FORM_PARSE_START;
  char
    *request_method = getenv("REQUEST_METHOD"),
    *content_type = getenv("CONTENT_TYPE"),
    message[BWC_MESSAGE_LENGTH + BOUNDARY_LENGTH + 4] = "\0",
    nick[BWC_NICK_LENGTH + BOUNDARY_LENGTH + 4] = "\0",
    filename[FILENAME_LENGTH] = "\0",
    boundary[BOUNDARY_LENGTH + 4],
    field_name[FIELD_NAME_LENGTH],
    buf[4096];
  int stream = 0;

  if (strcmp(request_method, "POST") == 0) {
    if (content_type != NULL &&
        strncmp(content_type, "multipart/form-data;", 20) == 0) {
      /* Parse form data: nick, message, file, stream */
      strcpy(boundary, "\r\n--");
      read_param(content_type, "boundary",
                 boundary + 4, BOUNDARY_LENGTH - 2);
      if (read_till(boundary + 2, buf, 4096, &len, &matched) != 0) {
        syslog(LOG_ERR, "No initial boundary found");
        ps = FORM_PARSE_FAIL;
      }
      while (ps != FORM_PARSE_FAIL && ps != FORM_PARSE_DONE) {
        len = 0;
        matched = 0;
        if (ps == FORM_PARSE_START) {
          if (read_till("\r\n", buf, 4096, &len, &matched) == 0) {
            if (len == 0) {
              ps = FORM_PARSE_HEADER;
            } else if (len == 2 && strcmp(buf, "--") == 0) {
              ps = FORM_PARSE_DONE;
            }
          } else {
            syslog(LOG_ERR, "Failed to start parsing");
            ps = FORM_PARSE_FAIL;
          }
        } else if (ps == FORM_PARSE_HEADER) {
          if (read_till("\r\n", buf, 4096, &len, &matched) == 0) {
            if (strncmp(buf, "Content-Disposition: form-data;", 31) == 0) {
              read_param(buf, "name", field_name, FIELD_NAME_LENGTH);
              read_param(buf, "filename", filename, FILENAME_LENGTH);
            } else if (len == 0) {
              ps = FORM_PARSE_DATA;
            }
          } else {
            syslog(LOG_ERR, "Failed to parse a header");
            ps = FORM_PARSE_FAIL;
          }
        } else if (ps == FORM_PARSE_DATA) {
          if (strcmp(field_name, "nick") == 0) {
            if (read_till(boundary, nick,
                          BWC_NICK_LENGTH + BOUNDARY_LENGTH + 4,
                          &len, &matched) == 0) {
              ps = FORM_PARSE_START;
            } else {
              syslog(LOG_ERR, "No boundary after nick");
              ps = FORM_PARSE_FAIL;
            }
          } else if (strcmp(field_name, "message") == 0) {
            if (read_till(boundary, message,
                          BWC_MESSAGE_LENGTH + BOUNDARY_LENGTH + 4,
                          &message_len, &matched) == 0) {
              ps = FORM_PARSE_START;
            } else {
              syslog(LOG_ERR, "No boundary after message");
              ps = FORM_PARSE_FAIL;
            }
          } else if (strcmp(field_name, "file") == 0 &&
                     filename[0] != '\0') {
            FILE *f = fopen(basename(filename), "w");
            if (f == NULL) {
              syslog(LOG_ERR, "Failed to open a file: %s", strerror(errno));
            } else {
              int r;
              do {
                r = read_till(boundary, buf, 4096, &len, &matched);
                if (r >= 0) {
                  if (fwrite(buf, 1, len, f) < len) {
                    syslog(LOG_ERR, "Failed to write into a file: %s",
                            strerror(errno));
                    break;
                  }
                } else {
                  syslog(LOG_ERR, "No boundary after file contents");
                }
              } while (r > 0);
              if (fclose(f) != 0) {
                syslog(LOG_ERR, "Failed to close a file: %s", strerror(errno));
              }
            }
            ps = FORM_PARSE_START;
          } else if (strcmp(field_name, "stream") == 0) {
            if (read_till(boundary, buf, 4096, &len, &matched) == 0) {
              stream = 1;
              ps = FORM_PARSE_START;
            } else {
              syslog(LOG_ERR, "No boundary after stream");
              ps = FORM_PARSE_FAIL;
            }
          } else {
            /* Skip unknown fields */
            while (read_till(boundary, buf, 4096, &len, &matched) > 0);
            ps = FORM_PARSE_START;
          }
        }
      }

      /* Process the parsed form data */
      if (nick[0] != '\0') {
        char buf[sizeof(struct bwchat_message) + 1];
        struct bwchat_message *msg = (struct bwchat_message *)(buf + 1);
        time(&(msg->timestamp));
        strncpy(msg->nick, nick, BWC_NICK_LENGTH);
        msg->nick[sizeof(msg->nick) - 1] = '\0';
        buf[0] = BWC_CMD_ADD_MESSAGE;
        if (stream && message_len > 0) {
          /* A chunk of stream */
          msg->type = BWC_MESSAGE_AUDIO;
          msg->data_len = message_len;
          memcpy(msg->data, message, BWC_MESSAGE_LENGTH);
          len = write(sock, buf, sizeof(buf));
        } else if (message[0] != '\0' || filename[0] != '\0') {
          /* A new message: either textual or file upload. */
          if (message[0] != '\0') {
            /* New text message */
            msg->type = BWC_MESSAGE_TEXT;
            strncpy(msg->data, message, BWC_MESSAGE_LENGTH);
            msg->data[sizeof(msg->data) - 1] = '\0';
          } else if (filename[0] != '\0') {
            /* New file upload message */
            msg->type = BWC_MESSAGE_UPLOAD;
            strncpy(msg->data, filename, FILENAME_LENGTH);
          }
          msg->data_len = strlen(msg->data);
          if (write(sock, buf, sizeof(buf)) != sizeof(buf)) {
            syslog(LOG_ERR, "Failed to submit a new message: %s",
                    strerror(errno));
          }
          /* Reopen the socket, since we will request messages after. */
          if (close(sock) < 0) {
            syslog(LOG_ERR, "Socket closing error: %s", strerror(errno));
          }
          if (sock_conn() < 0) {
            syslog(LOG_ERR, "Failed to reconnect to the chat server: %s",
                    strerror(errno));
            return -1;
          }
        }
      }
    }
  }

  /* Send a response to the client */
  if (stream) {
    printf("Content-type: text/html\r\n"
           "\r\n");
  } else {
    printf
      ("Content-type: text/html\r\n"
       "\r\n"
       "<!DOCTYPE html>\n"
       "<html>\n"
       "  <head>\n"
       "    <title>Chat</title>\n"
       "    <script src=\"%s\"></script>\n"
       "  </head>\n"
       "  <body>\n",
       js_url);
    print_messages();
    printf
      ("    <form id=\"chatInputForm\" method=\"post\""
       " enctype=\"multipart/form-data\" >\n"
       "      <input type=\"text\" name=\"nick\" value=\"%s\" />\n"
       "      <input type=\"text\" name=\"message\" autofocus=\"\""
       " size=\"60\" />\n"
       "      <input type=\"file\" name=\"file\" />\n"
       "      <input type=\"submit\" />\n"
       "    </form>\n"
       "  </body>\n"
       "</html>\n",
       (nick[0] != '\0') ? nick : "Anonymous");
  }
  return 0;
}

static struct argp_option options[] = {
  {"js-url", 'j', "URL", 0,
   "JavaScript (bwchat.js) URL to reference from HTML", 0 },
  {"log-stderr", 'l', 0, 0,
   "Write logs into stderr, in addition to syslog", 0 },
  {"socket-path", 's', "PATH", 0,
   "The bwchat-server's Unix domain socket path", 0 },
  {"upload-dir-url", 'u', "URL", 0,
   "The URL to use in hyperlinks", 0 },
  { 0 }
};
static error_t parse_opt (int key, char *arg, struct argp_state *state) {
  (void)state;
  switch (key) {
  case 'u':
    upload_dir_url = arg;
    break;
  case 'j':
    js_url = arg;
    break;
  case 's':
    sock_path = arg;
    break;
  case 'l':
    log_stderr = LOG_PERROR;
    break;
  default:
    return ARGP_ERR_UNKNOWN;
  }
  return 0;
}
static struct argp argp =
  { options, parse_opt, 0, "A basic web chat, the CGI program", 0, 0, 0 };

int main (int argc, char **argv) {
  argp_parse(&argp, argc, argv, 0, 0, 0);
  openlog("bwchat-cgi", LOG_PID | log_stderr, 0);

#ifdef HAVE_FCGI
  while (FCGI_Accept() >= 0) {
#endif
    char *script_name, *script_bname;
    if (sock_conn() < 0) {
      syslog(LOG_DEBUG,
             "Failed to connect to the chat server at %s: %s",
             sock_path, strerror(errno));
#ifdef HAVE_FCGI
      continue;
#else
      return -1;
#endif
    }
    script_name = getenv("SCRIPT_NAME");
    script_bname = basename(script_name);
    if (strcmp(script_bname, "stream") == 0) {
      serve_stream();
    } else if (strcmp(script_bname, "messages") == 0) {
      serve_messages();
    } else {
      handle_chat();
    }
    if (close(sock) < 0) {
      syslog(LOG_ERR, "Socket closing error: %s", strerror(errno));
    }
#ifdef HAVE_FCGI
  }
#endif
  return 0;
}
