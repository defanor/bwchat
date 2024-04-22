# Installing bwchat.h for use by other programs, extending the
# chat. Otherwise it would go into _SOURCES.
include_HEADERS = bwchat.h
man1_MANS = bwchat-cgi.1 bwchat-server.1
dist_man_MANS = bwchat-cgi.1 bwchat-server.1
dist_data_DATA = bwchat.js
AM_CFLAGS = -std=c89 -Wall -Wextra -pedantic
bin_PROGRAMS = bwchat-server bwchat-cgi
bwchat_server_SOURCES = bwchat_server.c
bwchat_cgi_SOURCES = bwchat_cgi.c
