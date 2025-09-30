#ifndef PROTO_TEXT_H
#define PROTO_TEXT_H

typedef struct token_s {
    char *value;
    size_t length;
} token_t;

/* text protocol handlers */
void complete_nread_ascii(conn *c);
int try_read_command_asciiauth(conn *c);
int try_read_command_ascii(conn *c);
void process_command_ascii(conn *c, char *command);

#endif
