/**
   @file bwchat_server.c
   @brief bwchat server
   @author defanor <defanor@thunix.net>
   @date 2024
   @copyright MIT license
*/

#include <stdio.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <stdlib.h>
#include <syslog.h>
#include <argp.h>

#include "bwchat.h"

#define MESSAGE_COUNT 20
#define LISTENER_COUNT 128

struct stream_listener {
  int sock;
  struct bwchat_message *msg;
};

/* Global state */
int server_sock = -1, client_sock = -1;
int message_listeners[LISTENER_COUNT];
struct stream_listener stream_listeners[LISTENER_COUNT];
int log_stderr = 0;

/* Settings */
const char *sock_path = "bwchat-socket";

static struct argp_option options[] = {
  {"log-stderr", 'l', 0, 0,
   "Write logs into stderr, in addition to syslog", 0 },
  {"socket-path", 's', "PATH", 0,
   "The Unix domain socket path to listen on", 0 },
  { 0 }
};
static error_t parse_opt (int key, char *arg, struct argp_state *state) {
  (void)state;
  switch (key) {
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
  { options, parse_opt, 0, "A basic web chat, the chat server", 0, 0, 0 };

void terminate (int signum) {
  int i;
  syslog(LOG_DEBUG, "Received signal %d, terminating", signum);
  for (i = 0; i < LISTENER_COUNT; i++) {
    if (message_listeners[i] != -1) {
      close(message_listeners[i]);
      message_listeners[i] = -1;
    }
    if (stream_listeners[i].sock != -1) {
      close(stream_listeners[i].sock);
      stream_listeners[i].sock = -1;
    }
  }
  if (client_sock != -1) {
    close(client_sock);
    client_sock = -1;
  }
  close(server_sock);
  server_sock = -1;
  unlink(sock_path);
  exit(0);
}

/*
  https://www.xiph.org/ogg/doc/framing.html -- Ogg
  https://www.rfc-editor.org/rfc/rfc7845#section-5 -- Opus
*/

int main (int argc, char **argv) {
  unsigned int oldest_message = 0;
  struct bwchat_message messages[MESSAGE_COUNT];
  struct sockaddr_un server_addr, client_addr;
  socklen_t server_addr_size, client_addr_size;
  char buf[1 + sizeof(struct bwchat_message)];
  int i;
  ssize_t len;

  argp_parse(&argp, argc, argv, 0, 0, 0);
  signal(SIGPIPE, SIG_IGN);
  signal(SIGTERM, terminate);
  signal(SIGINT, terminate);
  signal(SIGQUIT, terminate);
  openlog("bwchat-server", LOG_PID | log_stderr, 0);

  for (i = 0; i < MESSAGE_COUNT; i++) {
    messages[i].type = BWC_MESSAGE_NONE;
  }
  for (i = 0; i < LISTENER_COUNT; i++) {
    message_listeners[i] = -1;
    stream_listeners[i].sock = -1;
    stream_listeners[i].msg = NULL;
  }

  /* Create the socket. */
  server_sock = socket(AF_UNIX, SOCK_SEQPACKET, 0);
  if (server_sock < 0) {
    syslog(LOG_ERR, "socket() failure: %s", strerror(errno));
    return -1;
  }
  /* Bind a name to the socket. */
  server_addr.sun_family = AF_UNIX;
  strncpy(server_addr.sun_path, sock_path, sizeof(server_addr.sun_path));
  server_addr.sun_path[sizeof(server_addr.sun_path) - 1] = '\0';
  server_addr_size = sizeof(struct sockaddr_un);
  if (bind (server_sock, (struct sockaddr *) &server_addr, server_addr_size) < 0) {
    syslog(LOG_ERR, "bind() failure: %s", strerror(errno));
    return -1;
  }
  if (listen(server_sock, 10) < 0) {
    syslog(LOG_ERR, "listen() failure: %s", strerror(errno));
    return -1;
  }
  while (1) {
    client_sock = accept(server_sock,
                         (struct sockaddr *)&client_addr,
                         &client_addr_size);
    if (client_sock < 0) {
      syslog(LOG_ERR, "accept() failure: %s", strerror(errno));
      close(server_sock);
      client_sock = -1;
      return -1;
    }

    len = read(client_sock, buf, sizeof(buf));
    if (len <= 0) {
      if (len == 0) {
        syslog(LOG_WARNING,
               "The client disconnected without issuing a command");
      } else {
        syslog(LOG_ERR, "read() failure: %s", strerror(errno));
      }
      close(client_sock);
      client_sock = -1;
      continue;
    }

    if (buf[0] == BWC_CMD_ADD_MESSAGE &&
        len == sizeof(struct bwchat_message) + 1) {
      struct bwchat_message *src_msg = (struct bwchat_message *)(buf + 1);
      struct bwchat_message *upd_msg = NULL;
      int new_message = src_msg->type == BWC_MESSAGE_TEXT ||
        src_msg->type == BWC_MESSAGE_UPLOAD;
      if (src_msg->type == BWC_MESSAGE_AUDIO) {
        if (src_msg->data[5] & 0x02) {
          /* The beginning of a stream: this is going to be a new
             message if there is no stream with the same nick;
             otherwise updating that one. */
          new_message = 1;
        }
        for (i = 0; i < MESSAGE_COUNT; i++) {
          if (messages[i].type == BWC_MESSAGE_AUDIO &&
              strcmp(messages[i].nick, src_msg->nick) == 0) {
            new_message = 0;
            upd_msg = &(messages[i]);
            break;
          }
        }
      }
      if (new_message) {
        /* A new message */
        struct bwchat_message *dst_msg = &(messages[oldest_message]);
        if (dst_msg->type == BWC_MESSAGE_AUDIO) {
          /* Close sockets for audio listeners. */
          for (i = 0; i < LISTENER_COUNT; i++) {
            if (stream_listeners[i].msg == dst_msg) {
              close(stream_listeners[i].sock);
              stream_listeners[i].sock = -1;
              stream_listeners[i].msg = NULL;
            }
          }
        }
        oldest_message = (oldest_message + 1) % MESSAGE_COUNT;
        memcpy(dst_msg, src_msg, sizeof(struct bwchat_message));
        /* Send the new message to message listeners */
        for (i = 0; i < LISTENER_COUNT; i++) {
          if (message_listeners[i] != -1) {
            len = write(message_listeners[i], dst_msg,
                        sizeof(struct bwchat_message));
            if (len < (ssize_t)sizeof(struct bwchat_message)) {
              close(message_listeners[i]);
              message_listeners[i] = -1;
            }
          }
        }
      } else if (upd_msg != NULL) {
        if (src_msg->data[5] & 0x02) {
          /* A header page, replace the data. */
          memcpy(upd_msg->data, src_msg->data, src_msg->data_len);
          upd_msg->data_len = src_msg->data_len;
        }
        /* Send the new data to stream listeners */
        for (i = 0; i < LISTENER_COUNT; i++) {
          if (stream_listeners[i].msg == upd_msg &&
              stream_listeners[i].sock != -1) {
            len = write(stream_listeners[i].sock,
                        src_msg->data,
                        src_msg->data_len);
            if (len < (ssize_t)(src_msg->data_len)) {
              close(stream_listeners[i].sock);
              stream_listeners[i].sock = -1;
              stream_listeners[i].msg = NULL;
            }
          }
        }
      }
      close(client_sock);
      client_sock = -1;
    } else if (buf[0] == BWC_CMD_ALL_MESSAGES) {
      for (i = 0; i < MESSAGE_COUNT; i++) {
        struct bwchat_message *msg =
          &(messages[(oldest_message + i) % MESSAGE_COUNT]);
        if (msg->type != BWC_MESSAGE_NONE) {
          len = write(client_sock, msg, sizeof(*msg));
          if (len < (ssize_t)sizeof(*msg)) {
            break;
          }
        }
      }
      close(client_sock);
      client_sock = -1;
    } else if (buf[0] == BWC_CMD_NEW_MESSAGES) {
      for (i = 0; i < LISTENER_COUNT; i++) {
        if (message_listeners[i] == -1) {
          message_listeners[i] = client_sock;
          break;
        }
      }
      if (i == LISTENER_COUNT) {
        close(client_sock);
        client_sock = -1;
      }
    } else if (buf[0] == BWC_CMD_AUDIO_STREAM) {
      struct bwchat_message *msg = NULL;
      buf[BWC_NICK_LENGTH + 1] = '\0';
      for (i = 0; i < MESSAGE_COUNT; i++) {
        if (messages[i].type == BWC_MESSAGE_AUDIO &&
            strcmp(messages[i].nick, buf + 1) == 0) {
          msg = &(messages[i]);
          break;
        }
      }
      if (msg == NULL) {
        close(client_sock);
        client_sock = -1;
      } else {
        for (i = 0; i < LISTENER_COUNT; i++) {
          if (stream_listeners[i].sock == -1) {
            stream_listeners[i].sock = client_sock;
            stream_listeners[i].msg = msg;

            /* Send the header at once. */
            len = write(stream_listeners[i].sock,
                        msg->data,
                        msg->data_len);
            if (len < (ssize_t)(msg->data_len)) {
              close(stream_listeners[i].sock);
              stream_listeners[i].sock = -1;
              stream_listeners[i].msg = NULL;
            }
            break;
          }
        }
        if (i == LISTENER_COUNT) {
          close(client_sock);
          client_sock = -1;
        }
      }
    }
  }
  return 0;
}
