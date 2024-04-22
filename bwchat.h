#include <stddef.h>
#include <time.h>

#define BWC_MESSAGE_LENGTH (32 * 1024)
#define BWC_NICK_LENGTH 32

enum bwchat_command {
  BWC_CMD_ADD_MESSAGE,
  BWC_CMD_ALL_MESSAGES,
  BWC_CMD_NEW_MESSAGES,
  BWC_CMD_AUDIO_STREAM
};

enum bwchat_message_type {
  BWC_MESSAGE_NONE,
  BWC_MESSAGE_TEXT,
  BWC_MESSAGE_UPLOAD,
  BWC_MESSAGE_AUDIO
};

struct bwchat_message {
  time_t timestamp;
  char nick[BWC_NICK_LENGTH];
  enum bwchat_message_type type;
  char data[BWC_MESSAGE_LENGTH];
  size_t data_len;
};
