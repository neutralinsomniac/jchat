#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <readline/readline.h>
#include <readline/history.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#include <stdint.h>

#define COMMS_DIR_TEMPLATE "/tmp/comms.XXXXXX"
#define JCHAT_SOCK_FILENAME "/jchat.sock"
#define JCHAT_SOCK_FORMAT "%s" JCHAT_SOCK_FILENAME

#define BUF_SIZE 1024
#define MSG_SIZE 4096
#define PROMPT_SIZE 32
#define NICK_SIZE 16
#define MAX_USERS 32
#define MAX_DISPLAY_MESSAGES 200
#define LINE_UP "\033[1F"
#define CLEAR_LINE "\033[K"
#define SAVE_CURSOR "\0337"
#define RESTORE_CURSOR "\0338"
#define CLEAR_SCREEN "\033[2J"
// for PuTTY. stupid...
#define CLEAR_SCROLLBACK "\033[3J"
#define VISIBLE_BEEP "\x07"
#define RESET_TERM "\033c"
#define CHANGE_TITLE_FORMAT "\033]2;%s\007"
#define CHANGE_TITLE_IS_TYPING_FORMAT "\033]2;%s...\007"

#define COLOR_NONE "\033[0m"
#define COLOR_RED "\033[31m"
#define COLOR_GREEN "\033[32m"
#define COLOR_YELLOW "\033[33m"
#define COLOR_CYAN "\033[36m"

#define MSG_MARK "----- mark -----"
#define MSG_HISTORY_CLEARED_FMT "%s cleared their history"

/* ui commands */
#define UI_CMD_SIZE 3
#define UI_CHANGE_PROMPT_CMD "p"
#define UI_CHANGE_TITLE_CMD "t"
#define UI_CLEAR_CMD "c"
#define UI_CLEAR_HISTORY_CMD "C"
#define UI_CYCLE_URGENT_MODE_CMD "u"
#define UI_HELP_CMD "h"
#define UI_MARK_CMD "m"
#define UI_QUIT_CMD "q"
#define UI_REDACT_CMD "-"
#define UI_RESET_CMD "r"

/* TODO how will we use these (if it all)? */
#define TYPING_START_CMD '\\'
#define TYPING_END_CMD '/'
#define TYPING_STALLED_CMD '|'

enum msg_type {
    MSG_NORMAL,
    MSG_JOIN,
    MSG_JOIN_REJECTED,
    MSG_QUIT
};

enum join_state {
    JOIN_PENDING = 0,
    JOIN_REJECTED,
    JOINED
};

enum urgent_type {
    URGENT_ALL = 0,
    URGENT_MSG_ONLY    = 1,
    URGENT_NONE = 2
};

struct client_state {
    enum join_state join_state;
    int user_id;
    char prompt[PROMPT_SIZE]; /* custom prompt string */
    uint8_t clear_mode; /* is clear mode enabled? */
    uint8_t transient_mode; /* is transient mode enabled? */
    uint8_t urgent_mode; /* urgent mode */
    uint32_t num_pending_msg;
    uint8_t should_exit;
};

/* this is what gets passed on the wire */
__attribute__((packed)) struct msg {
    enum msg_type type;
    time_t time;
    int user_id;
    char nick[NICK_SIZE];
    char msg[MSG_SIZE];
};

/* this is what gets stored in the client(s) */
struct node {
    struct msg msg; /* note: this is *NOT* packed */
    struct node *next;
    struct node *prev;
};


pthread_mutex_t msg_mutex;
pthread_t pt_user_input, pt_server_processing, pt_server;

struct node *root = NULL;
struct node *tail = NULL;
struct winsize w;

static struct client_state g_client_state = {0};

void update_display(void);
void clear_display(void);
void add_new_message(struct msg *);

void ignore_signal(int signum)
{
}

void write_msg(int fd, struct msg *msg)
{
    /* put this in a while() loop to ensure all data is written */
    int total_written = 0, bytes_written;
    while (total_written < sizeof(struct msg)) {
        bytes_written = write(fd, ((char *)msg) + total_written, sizeof(struct msg) - total_written);
        if (bytes_written > 0) {
            total_written += bytes_written;
        } else {
            break;
        }
    }
}

int read_msg(int fd, struct msg *msg)
{
    int total_read = 0, bytes_read;
    while (total_read < sizeof(struct msg)) {
        bytes_read = read(fd, ((char *)msg) + total_read, sizeof(struct msg) - total_read);
        if (bytes_read > 0) {
            total_read += bytes_read;
        } else {
            break;
        }
    }

    return total_read;
}


void clear_history(void)
{
    struct node *iter;
    struct node *next;

    if (!root) {
        return;
    }

    iter = root;

    while (iter) {
        next = iter->next;
        memset(iter, 0, sizeof(struct node));
        free(iter);
        iter = next;
    }
    root = NULL;
    tail = NULL;

    rl_clear_history();
}

void clear_display(void)
{
    printf("%s", RESET_TERM);
    printf("%s", CLEAR_SCROLLBACK);
    printf("%s", CLEAR_SCREEN);
    // get window size
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);

    // set scrollable region
    printf("\033[(1;%ur", w.ws_row - 1);

    // position cursor
    printf("\033[%u;1f", w.ws_row);
}

void delete_node(struct node *node)
{
    if (node == NULL)
        return;

    if (node->next) {
        (node->next)->prev = node->prev;
    }
    if (node->prev) {
        (node->prev)->next = node->next;
    }
    if (node == tail) {
        tail = node->prev;
    }
    if (node == root) {
        root = node->next;
    }
    memset(node, 0, sizeof(struct node));
    free(node);
    node = NULL;
}

void copy_msg(struct node *dst, struct msg *src)
{
    strncpy(dst->msg.msg, src->msg, MSG_SIZE);
    strncpy(dst->msg.nick, src->nick, NICK_SIZE);
    dst->msg.user_id = src->user_id;
    dst->msg.time = src->time;
    dst->msg.type = src->type;
}

void process_message(struct msg *msg)
{
	switch(msg->type) {
	case MSG_NORMAL:
	case MSG_JOIN:
	case MSG_QUIT:
		add_new_message(msg);
		break;
	default:
		/* ??? */
		break;
	}
}

void add_new_message(struct msg *msg)
{
    struct node *new = NULL;
    struct node *iter;
    struct node *prev;
    int i;

    if (root == NULL) {
        root = malloc(sizeof(struct node));
        copy_msg(root, msg);
        root->next = NULL;
        root->prev = NULL;
        tail = root;
    } else {
        new = malloc(sizeof(struct node));
        copy_msg(new, msg);
        new->next = NULL;
        // update tail
        tail->next = new;
        new->prev = tail;
        tail = new;
    }

    // trim history for transient mode
    if (g_client_state.transient_mode) {
        i = 0;

        iter = tail;
        while (iter && i < 10) {
            iter = iter->prev;
            i++;
        }

        while (iter) {
            prev = iter->prev;
            delete_node(iter);
            iter = prev;
        }
    }
}

void update_display(void)
{
    int count = 0, i;
    struct node *iter;
    struct tm timeinfo;
    struct tm now;
    time_t now_time;
    char time_str[BUF_SIZE];
    char *newline;

    // save cursor
    printf("%s", SAVE_CURSOR);

    // first, count # of messages to print
    iter = root;
    while (iter != NULL && count < MAX_DISPLAY_MESSAGES) {
        iter = iter->next;
        count++;
    }

    // clear screen (skipping input bar)
    for (i = w.ws_row; i != 0; i--) {
        printf("%s", LINE_UP);
        printf("%s", CLEAR_LINE);
    }

    printf("%s", RESTORE_CURSOR);
    fflush(stdout);

    if (g_client_state.clear_mode) {
        return;
    }

    // navigate backwards to the beginning of what we want to print
    iter = tail;
    for (; count != 0; count--) {
        printf("%s", LINE_UP);
        if (iter->prev != NULL) {
            iter = iter->prev;
        }
    }

    // print messages!
    while (iter != NULL) {
        localtime_r(&iter->msg.time, &timeinfo);
        now_time = time(NULL);
        localtime_r(&now_time, &now);
        if (now.tm_year == timeinfo.tm_year &&
                now.tm_mon == timeinfo.tm_mon &&
                now.tm_mday == timeinfo.tm_mday) {
            strftime(time_str, sizeof(time_str), "%T ", &timeinfo);
        } else {
            strftime(time_str, sizeof(time_str), "%a %T ", &timeinfo);
        }

        printf("%s", time_str);

        switch (iter->msg.type) {
            case MSG_NORMAL:
                if (iter->msg.user_id == g_client_state.user_id) {
                    printf("%s", COLOR_CYAN);
                } else {
                    printf("%s", COLOR_YELLOW);
                }
                break ;
            default:
                printf("%s", COLOR_NONE);
                break;
        }

        if (iter->msg.type == MSG_NORMAL) {
            printf("%s: %s", iter->msg.nick, iter->msg.msg);
        } else {
            printf("%s", iter->msg.msg);
        }

        printf("%s\n", COLOR_NONE) ;

        iter = iter->next;
    }

    //restore cursor
    printf("%s", RESTORE_CURSOR);
    fflush(stdout);
}

void update_prompt()
{
    printf ("%s", CLEAR_LINE);
    snprintf(g_client_state.prompt, PROMPT_SIZE-1, "> ");
    rl_set_prompt(g_client_state.prompt);
    rl_redisplay() ;
}

void window_resized(int signum)
{
    pthread_mutex_lock(&msg_mutex);
    clear_display();
    update_display();
    rl_redisplay();
    pthread_mutex_unlock(&msg_mutex);
}

void *server_thread(void *arg)
{
    int fd, client_fd;
    struct sockaddr_un client_sock;
    struct sockaddr_un *sock = (struct sockaddr_un *)arg;
    socklen_t client_sock_len;
    struct pollfd fds[MAX_USERS+1];
    int num_fds;
    int flags;
    struct nick_entry {
        int fd;
        char nick[NICK_SIZE];
    };
    char nicks[MAX_USERS+1][NICK_SIZE];

    memset(fds, 0, sizeof(fds));
    memset(nicks, 0, sizeof(nicks));

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    if (bind(fd, (struct sockaddr *)sock, sizeof(struct sockaddr_un)) < 0) {
        perror("bind");
        exit(EXIT_FAILURE);
    }

    if (listen(fd, 10) < 0) {
        perror("bind");
        exit(EXIT_FAILURE);
    }

    num_fds = 1;
    fds[0].fd = fd;
    fds[0].events = POLLIN;

    while (1) {
        //printf("poll returned %d\n", poll(fds, num_fds, -1));
        poll(fds, num_fds, -1);
        /* check new connection fd */
        if (fds[0].revents & POLLIN) {
            client_fd = accept(fd, (struct sockaddr *)&client_sock, &client_sock_len);

            /* non blocking socket */
            flags = fcntl(client_fd, F_GETFL, 0);
            fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);

            /* put client_fd into empty fds slot */
            fds[num_fds].fd = client_fd;
            fds[num_fds].events = POLLIN;
            fds[num_fds].revents = 0;

            /* put client_fd into empty nick slot + init nick */
            memset(nicks[num_fds], 0, NICK_SIZE);

            num_fds++;
        }
        if ((fds[0].revents & ~POLLIN) > 0) {
            printf("server fd has an event: %x\n", fds[0].revents);
        }
        int remove = 0;
        /* figure out which fds have events */
        for (int i = 1; i < num_fds; i++) {
            remove = 0;
            /* there is data available on this fd */
            if (fds[i].revents & POLLIN) {
                /* read this msg */
                struct msg msg = {0};
                /* read a message */
                int total_read = read_msg(fds[i].fd, &msg);

                if (total_read == sizeof(struct msg)) {
                    switch(msg.type) {
                    case MSG_JOIN:
                        if (nicks[i][0] == '\0') { /* if we don't have a nick for this user yet */
                            /* make sure the nick isn't taken already */
                            for (int j = 1; j < num_fds; j++) {
                                if (strcmp(nicks[j], msg.nick) == 0) {
                                    /* nick taken; reject this join */
                                    msg.type = MSG_JOIN_REJECTED;
                                    write_msg(fds[i].fd, &msg);
                                    total_read = 0;
                                    break;
                                }
                            }
                            /* did we survive the duplicate nick check? */
                            if (msg.type == MSG_JOIN) {
                                snprintf(nicks[i], NICK_SIZE-1, "%s", msg.nick);
                                /* ensure null-terminated */
                                snprintf(msg.msg, sizeof(msg.msg), "%s joined the chat!", nicks[i]);
                            }
                        } else {
                            /* ignore rejoin */
                            total_read = 0;
                        }
                        break;
                    case MSG_QUIT:
                        if (nicks[i][0] != '\0') {
                            snprintf(msg.msg, sizeof(msg.msg), "%s left the chat!", nicks[i]);
                        } else {
                            /* received quit from someone who hasn't given a nick yet */
                            total_read = 0;
                        }
                        remove = 1;
                        break;
                    case MSG_NORMAL:
                        break;
                    default:
                        printf("received unknown command: %d, ignoring\n", msg.type);
                        /* make sure we don't write anything to other clients */
                        total_read = 0;
                        break;

                    }

                    strncpy(msg.nick, nicks[i], NICK_SIZE-1);
                    msg.user_id = fds[i].fd;

                    /* propogate this message to all other sockets */
                    /* we want to write this message back to the socket it came from, too */
                    if (total_read == sizeof(struct msg)) {
                        for (int j = 1; j < num_fds; j++) {
                            /* write ALL THE DATA */
                            if (nicks[j][0] != '\0') {
                                write_msg(fds[j].fd, &msg);
                            }
                        }
                    }
                }
            }

            if (fds[i].revents & POLLHUP) {
                remove = 1;
            }
            if (fds[i].revents & POLLERR || fds[i].revents & POLLNVAL) {
                remove = 1;
            }
            if (remove) {
                close(fds[i].fd);

                /* consolidate lists */
                fds[i].fd = fds[num_fds-1].fd;
                fds[i].events = POLLIN;

                memcpy(nicks[i], nicks[num_fds-1], NICK_SIZE);

                memset(nicks[num_fds-1], 0, NICK_SIZE);

                num_fds--;
            }
        }
    }

    pthread_exit(0);
}

void *user_input_thread(void *arg)
{
    int fd = *(int *)arg;
    struct msg msg = {0};
    char *rl_str = NULL;

    while (1) {
        g_client_state.join_state = JOIN_PENDING;

        /* first, prompt for nick */
        rl_str = readline("enter nick: ");

        if (rl_str == NULL) {
            g_client_state.should_exit = 1;
            pthread_exit(0);
        }

        if (rl_str[0] == '\0') {
            continue;
        }

        strncpy(msg.nick, rl_str, NICK_SIZE-1);
        free(rl_str);
        msg.type = MSG_JOIN;
        msg.time = time(NULL);
        write_msg(fd, &msg);

        while (g_client_state.join_state == JOIN_PENDING) {
            sleep(0.5f);
        }

        if (g_client_state.join_state == JOINED) {
            break;
        }

        printf("%s", SAVE_CURSOR);
        printf("%s", LINE_UP);
        printf("%s", CLEAR_LINE);
        printf("nick taken! try again\n");
        printf("%s", RESTORE_CURSOR);
        printf("%s", CLEAR_LINE);
    }

    using_history();
    clear_display();
    pthread_mutex_lock(&msg_mutex);
    update_display();
    pthread_mutex_unlock(&msg_mutex);

    /* main msg processing loop */
    while (!g_client_state.should_exit && (rl_str = readline(g_client_state.prompt)) != NULL) {
        switch (strlen(rl_str)) {
        /* user pressed enter with no text entered; do a full screen refresh */
        case 0:
            pthread_mutex_lock(&msg_mutex);
            clear_display();
            update_display();
            pthread_mutex_unlock(&msg_mutex);
            continue;
            break;
        /* single letter command */
        case 1:
            switch (rl_str[0]) {
                case 'q':
                    g_client_state.should_exit = 1;
                    continue;
                    break;
                default:
                    break;
            }
        default:
            break;

        }

        memset(&msg, 0, sizeof(struct msg));
        strncpy(msg.msg, rl_str, MSG_SIZE-1);
        msg.time = time(NULL);
        msg.type = MSG_NORMAL;
        write_msg(fd, &msg);
        add_history(rl_str);
        free(rl_str);
        printf("%s", CLEAR_LINE);
    }

    memset(&msg, 0, sizeof(struct msg));
    msg.time = time(NULL);
    msg.type = MSG_QUIT;
    write_msg(fd, &msg);
    g_client_state.should_exit = 1;
    pthread_exit(0);
}

void *server_processing_thread(void *arg)
{
    int fd = *(int *)arg;

    struct msg msg = {0};

    while (!g_client_state.should_exit) {
        if (read_msg(fd, &msg) != sizeof(struct msg)) {
            break;
        }

        if (g_client_state.join_state == JOIN_PENDING) {
            switch (msg.type) {
                case MSG_JOIN:
                    g_client_state.join_state = JOINED;
                    /* since this is the first message we will receive, this message *must* contain our user_id */
                    g_client_state.user_id = msg.user_id;
                    break;
                case MSG_JOIN_REJECTED:
                    g_client_state.join_state = JOIN_REJECTED;
                    continue;
                    break;
                default:
                    break;
            }
        }

        pthread_mutex_lock(&msg_mutex);
        process_message(&msg);
        if (g_client_state.clear_mode) {
            g_client_state.num_pending_msg++;
            //update_prompt();
        }
        update_display();
        if (g_client_state.urgent_mode != URGENT_NONE) {
            printf("%s", VISIBLE_BEEP);
            fflush(stdout);
        }
        pthread_mutex_unlock(&msg_mutex);
    }

    g_client_state.should_exit = 1;
    pthread_exit(0);
}

void client(const struct sockaddr_un *sock)
{
    struct sigaction new_action;
    void *res;
    int fd, counter = 0;

    /* TODO need to ensure this runs *after* server starts */
    sigemptyset(&new_action.sa_mask);
    new_action.sa_flags = 0;
    new_action.sa_handler = window_resized;
    sigaction(SIGWINCH, &new_action, NULL);

    new_action.sa_handler = ignore_signal;
    sigaction(SIGUSR1, &new_action, NULL);
    sigaction(SIGINT, &new_action, NULL);

    /* connect to socket to get fd */
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    printf("waiting to connect...\n");

    while (connect(fd, (struct sockaddr *)sock, sizeof(struct sockaddr_un)) < 0 && counter < 10) {
        sleep(0.1f);
        counter++;
    }

    clear_display();

    if (fd < 0) {
        perror("failed to connect");
        exit(EXIT_FAILURE);
    }

    /* init state */
    g_client_state.urgent_mode = URGENT_ALL;
    g_client_state.num_pending_msg = 0;

    /* need to create threads for user input + server processing */
    pthread_create(&pt_user_input, NULL, &user_input_thread, (void*)&fd);
    pthread_create(&pt_server_processing, NULL, &server_processing_thread, (void*)&fd);

    while (!g_client_state.should_exit) {
        sleep(1);
    }

    // one of our threads signaled exit; signal our other thread(s) to exit
    pthread_kill(pt_server_processing, SIGUSR1);
    pthread_kill(pt_user_input, SIGUSR1);

    return;
}

int main(int argc, char **argv)
{
    int is_server = 0;
    void *res;
    char comms_dir_template[] = COMMS_DIR_TEMPLATE;
    char sockpath[sizeof(COMMS_DIR_TEMPLATE) + sizeof(JCHAT_SOCK_FILENAME)] = {0};

    char *response = NULL;

    struct sockaddr_un sock = {
        .sun_family = AF_UNIX
    };

    if (argc != 1) {
        printf("usage: %\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    response = readline("<enter> for new session, key for existing: ");
    if (response == NULL) {
        printf("goodbye!\n");
        return 0;
    } else if (response[0] == '\0') {
        is_server = 1;
        if (NULL == mkdtemp(comms_dir_template)) {
            perror("mkdtemp");
            exit(EXIT_FAILURE);
        }
        snprintf(sockpath, sizeof(sockpath), comms_dir_template);
        snprintf(g_client_state.prompt, PROMPT_SIZE, "%s> ", &comms_dir_template[11]);
    } else {
        if (strlen(response) != 6) {
            printf("invalid key, goodbye!\n");
            exit(EXIT_FAILURE);
        } else {
            snprintf(sockpath, sizeof(sockpath), "/tmp/comms.%s", response);
            snprintf(g_client_state.prompt, PROMPT_SIZE, "> ");
        }
    }

    if (response) {
        free(response);
    }

    strcat(sockpath, JCHAT_SOCK_FILENAME);

    snprintf(sock.sun_path, sizeof(sock.sun_path), sockpath);

    if (is_server) {
        pthread_create(&pt_server, NULL, &server_thread, &sock);
    }

    client(&sock);

    // reset terminal
    printf("%s", RESET_TERM);
    printf("%s", CLEAR_SCROLLBACK);
    printf("%s", CLEAR_SCREEN);

    fflush(stdout);

    if (is_server) {
        printf("server is still running... [enter] to stop\n");
        readline(NULL);
        unlink(sockpath);
        rmdir(comms_dir_template);
    }

    return 0;
}
