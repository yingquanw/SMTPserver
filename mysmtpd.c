#include "netbuffer.h"
#include "mailuser.h"
#include "server.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/utsname.h>
#include <ctype.h>

#define MAX_LINE_LENGTH 1024

typedef enum state {
    Undefined,
    // TODO: Add additional states as necessary
    Greeted,
    Mailed,
    Recipient,
    Sending
} State;

typedef struct smtp_state {
    int fd;
    net_buffer_t nb;
    char recvbuf[MAX_LINE_LENGTH + 1];
    char *words[MAX_LINE_LENGTH];
    int nwords;
    State state;
    struct utsname my_uname;
    user_list_t user_list;
} smtp_state;

static void handle_client(int fd);

int main(int argc, char *argv[]) {

    if (argc != 2) {
        fprintf(stderr, "Invalid arguments. Expected: %s <port>\n", argv[0]);
        return 1;
    }

    run_server(argv[1], handle_client);

    return 0;
}

// syntax_error returns
//   -1 if the server should exit
//    1  otherwise
int syntax_error(smtp_state *ms) {
    if (send_formatted(ms->fd, "501 %s\r\n", "Syntax error in parameters or arguments") <= 0) return -1;
    return 1;
}

// checkstate returns
//   -1 if the server should exit
//    0 if the server is in the appropriate state
//    1 if the server is not in the appropriate state
int checkstate(smtp_state *ms, State s) {
    if (ms->state != s) {
        if (send_formatted(ms->fd, "503 %s\r\n", "Bad sequence of commands") <= 0) return -1;
        return 1;
    }
    return 0;
}

// All the functions that implement a single command return
//   -1 if the server should exit
//    0 if the command was successful
//    1 if the command was unsuccessful

int do_quit(smtp_state *ms) {
    dlog("Executing quit\n");
    // TODO: Implement this function
    if (send_formatted(ms->fd, "221 Service closing transmission channel\r\n") < 0) {
        return 1;
    }
    return -1;
}

int do_helo(smtp_state *ms) {
    dlog("Executing helo\n");
    // TODO: Implement this function
    if (send_formatted(ms->fd, "250 %s\r\n", ms->my_uname.nodename) < 0) {
        return 1;
    }
    ms->state = Greeted;
    return 0;
}

int do_rset(smtp_state *ms) {
    dlog("Executing rset\n");
    // TODO: Implement this function
    if (send_formatted(ms->fd, "250 %s\r\n", "State reset") <= 0) {
        return 1;
    }
    ms->state = Greeted;
    user_list_destroy(ms->user_list);
    ms->user_list = user_list_create();
    return 0;
}

int do_mail(smtp_state *ms) {
    dlog("Executing mail\n");
    // TODO: Implement this function
    if (checkstate(ms, Greeted)) {
        return 1;
    }
    if (ms->nwords != 2) return syntax_error(ms);
    if (ms->words[1] == NULL) return syntax_error(ms);
    char *result = strstr(ms->words[1], "FROM:");
    if (result == NULL) return syntax_error(ms);
    char* email_brackets_first = strchr(result, '<');
    if (email_brackets_first == NULL) return syntax_error(ms);
    char* email_brackets_second = strchr(email_brackets_first, '>');
    if (email_brackets_second == NULL) return syntax_error(ms);
    char* email = trim_angle_brackets(email_brackets_first);
    if (email == NULL) return syntax_error(ms);
    if (send_formatted(ms->fd, "250 %s\r\n", "Requested mail action ok, completed") < 0) {
        return 1;
    }
    ms->state = Mailed;
    return 0;
}

int do_rcpt(smtp_state *ms) {
    dlog("Executing rcpt\n");
    // TODO: Implement this function
    if (ms->state != Mailed && ms->state != Recipient) {
        checkstate(ms, Mailed);
        return 1;
    }
    if (ms->nwords != 2) return syntax_error(ms);
    if (ms->words[1] == NULL) return syntax_error(ms);
    char *result = strstr(ms->words[1], "TO:");
    if (result == NULL) return syntax_error(ms);
    char* email_brackets_first = strchr(result, '<');
    if (email_brackets_first == NULL) return syntax_error(ms);
    char* email_brackets_second = strchr(email_brackets_first, '>');
    if (email_brackets_second == NULL) return syntax_error(ms);
    char* email = trim_angle_brackets(email_brackets_first);
    if (email == NULL) return syntax_error(ms);
    if (is_valid_user(email, NULL)) {
        if (send_formatted(ms->fd, "250 %s\r\n", "Requested mail action ok, completed") <= 0) {
            return 1;
        }
        ms->state = Recipient;
        if (ms->user_list == NULL) {
            ms->user_list = user_list_create();
        }
        user_list_add(&ms->user_list, email);
    } else {
        if (send_formatted(ms->fd, "550 No such user - %s\r\n", email) <= 0) {
            return 1;
        }
    }
    return 0;
}

int do_data(smtp_state *ms) {
    dlog("Executing data\n");
    // TODO: Implement this function
    if (checkstate(ms, Recipient)) {
        return 1;
    }
    if (send_formatted(ms->fd, "354 %s\r\n", "Waiting for data, finish with <CR><LF>.<CR><LF>") <= 0) {
        return 1;
    }
    ms->state = Sending;

    const char *file_name = "mail";
    FILE *file = fopen(file_name, "w");
    if (file == NULL) {
        perror("Error opening file");
        return 1;
    }

    size_t len;
    while ((len = nb_read_line(ms->nb, ms->recvbuf)) >= 0) {
        while (isspace(ms->recvbuf[len - 1])) ms->recvbuf[--len] = 0;
        if (len == 1 && ms->recvbuf[len - 1] == '.') {
            fclose(file);
            break;
        }
        if (ms->recvbuf[0] == '.' && ms->recvbuf[1] == '.') {
            fprintf(file, "%s\r\n", ms->recvbuf + 1);
        } else {
            if (fprintf(file, "%s\r\n", ms->recvbuf) < 0) {
                perror("Error writing to file");
                fclose(file);
                return 1;
            }
        }
    }
   save_user_mail("mail", ms->user_list);
    if (send_formatted(ms->fd, "250 %s\r\n", "Requested mail action ok, completed") <= 0) {
        return 1;
    }
    ms->state = Recipient;
    return 0;
}

int do_noop(smtp_state *ms) {
    dlog("Executing noop\n");
    if (send_formatted(ms->fd, "250 %s\r\n", "OK (noop)") < 0) {
        return 1;
    }
    // TODO: Implement this function
    return 0;
}

int do_vrfy(smtp_state *ms) {
    dlog("Executing vrfy\n");
    // TODO: Implement this function
    if (ms->nwords < 2) return syntax_error(ms);
    char* email = trim_angle_brackets(ms->words[1]);
    if (is_valid_user(email, NULL)) {
        if (send_formatted(ms->fd, "250 user - %s\r\n", email) <= 0) {
            return 1;
        }
    } else {
        if (send_formatted(ms->fd, "550 No such user - %s\r\n", email) <= 0) {
            return 1;
        }
    }
    return 0;
}

void handle_client(int fd) {

    size_t len;
    smtp_state mstate, *ms = &mstate;

    ms->fd = fd;
    ms->nb = nb_create(fd, MAX_LINE_LENGTH);
    ms->state = Undefined;
    ms->user_list = user_list_create();
    uname(&ms->my_uname);

    if (send_formatted(fd, "220 %s Service ready\r\n", ms->my_uname.nodename) <= 0) return;


    while ((len = nb_read_line(ms->nb, ms->recvbuf)) >= 0) {
        if (ms->recvbuf[len - 1] != '\n') {
            // command line is too long, stop immediately
            send_formatted(fd, "500 Syntax error, command unrecognized\r\n");
            break;
        }
        if (strlen(ms->recvbuf) < len) {
            // received null byte somewhere in the string, stop immediately.
            send_formatted(fd, "500 Syntax error, command unrecognized\r\n");
            break;
        }

        // Remove CR, LF and other space characters from end of buffer
        while (isspace(ms->recvbuf[len - 1])) ms->recvbuf[--len] = 0;

        dlog("Command is %s\n", ms->recvbuf);

        // Split the command into its component "words"
        ms->nwords = split(ms->recvbuf, ms->words);
        char *command = ms->words[0];

        if (!strcasecmp(command, "QUIT")) {
            if (do_quit(ms) == -1) break;
        } else if (!strcasecmp(command, "HELO") || !strcasecmp(command, "EHLO")) {
            if (do_helo(ms) == -1) break;
        } else if (!strcasecmp(command, "MAIL")) {
            if (do_mail(ms) == -1) break;
        } else if (!strcasecmp(command, "RCPT")) {
            if (do_rcpt(ms) == -1) break;
        } else if (!strcasecmp(command, "DATA")) {
            if (do_data(ms) == -1) break;
        } else if (!strcasecmp(command, "RSET")) {
            if (do_rset(ms) == -1) break;
        } else if (!strcasecmp(command, "NOOP")) {
            if (do_noop(ms) == -1) break;
        } else if (!strcasecmp(command, "VRFY")) {
            if (do_vrfy(ms) == -1) break;
        } else if (!strcasecmp(command, "EXPN") ||
                   !strcasecmp(command, "HELP")) {
            dlog("Command not implemented \"%s\"\n", command);
            if (send_formatted(fd, "502 Command not implemented\r\n") <= 0) break;
        } else {
            // invalid command
            dlog("Illegal command \"%s\"\n", command);
            if (send_formatted(fd, "500 Syntax error, command unrecognized\r\n") <= 0) break;
        }
    }

    nb_destroy(ms->nb);
}
