#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <readline/history.h>
#include <readline/readline.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

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
    MSG_OWN,
    MSG_NORMAL,
    MSG_JOIN,
    MSG_QUIT
};

enum urgent_type {
    URGENT_ALL = 0,
    URGENT_MSG_ONLY    = 1,
    URGENT_NONE = 2
};

struct client_state {
    char nick[NICK_SIZE]; /* own nick */
    char prompt[PROMPT_SIZE]; /* custom prompt string */
    int clear_mode; /* is clear mode enabled? */
    int transient_mode; /* is transient mode enabled? */
    int urgent_mode; /* urgent mode */
    int num_pending_msg;
    int should_exit;
};

/* this is what gets passed on the wire */
__attribute__((packed)) struct msg {
    enum msg_type type;
    time_t time;
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

void update_display();
void clear_display();

void ignore_user1(int signum)
{
}

void clear_history()
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
void clear_display()
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

void copy_msg(struct node *node, struct msg *msg)
{
    strncpy(node->msg.msg, msg->msg, MSG_SIZE);
    strncpy(node->msg.nick, msg->nick, NICK_SIZE);
    node->msg.time = msg->time;
    if (strncmp(msg->nick, g_client_state.nick, NICK_SIZE) == 0) {
        node->msg.type = MSG_OWN;
    } else {
        node->msg.type = msg->type;
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

void update_display()
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
            case MSG_OWN:
                printf("%s", COLOR_CYAN);
                break;
            case MSG_NORMAL:
                printf("%s", COLOR_YELLOW);
                break ;
            default:
                printf("%s", COLOR_NONE);
                break;
        }

        if (iter->msg.type == MSG_OWN || iter->msg.type == MSG_NORMAL) {
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
    struct nick_entry nicks[MAX_USERS+1];

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
            nicks[num_fds].fd = client_fd;
            memset(nicks[num_fds].nick, 0, NICK_SIZE);

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
                int total_read = 0;
                int bytes_read = 0;
                /* read ALL THE DATA */
                while (total_read < sizeof(struct msg)) {
                    bytes_read = read(fds[i].fd, ((char *)&msg) + total_read, sizeof(struct msg) - total_read);
                    if (bytes_read == 0) {
                        break;
                    }
                    total_read += bytes_read;
                }
                if (bytes_read == -1 && errno != EAGAIN) {
                    printf("uh oh... read() failed\n");
                    /* TODO do we trust the message we received? probably? do we close the socket?
                     * what condition would cause a read() failure and not a corresponding error
                     * in poll()?
                     */
                }
                if (total_read > 0) {
                    /* switch() for message types? */
                    switch(msg.type) {
                        case MSG_JOIN:
                            if (nicks[i].nick[0] == '\0') {
                                snprintf(nicks[i].nick, NICK_SIZE-1, "%s", msg.nick);
                                /* ensure null-terminated */
                                snprintf(msg.msg, sizeof(msg.msg), "%s joined the chat!", nicks[i].nick);
                            } else {
                                printf("ignoring re-join\n");
                                total_read = 0;
                            }
                            break;
                        case MSG_QUIT:
                            if (nicks[i].nick[0] != '\0') {
                                snprintf(msg.msg, sizeof(msg.msg), "%s left the chat!", nicks[i].nick);
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

                    strncpy(msg.nick, nicks[i].nick, NICK_SIZE-1);

                    /* propogate this message to all other sockets */
                    /* we want to write this message back to the socket it came from, too */
                    for (int j = 1; j < num_fds; j++) {
                        int total_written = 0;
                        int bytes_written = 0;
                        /* write ALL THE DATA */
                        while (total_written < total_read) {
                            bytes_written = write(fds[j].fd, ((char *)&msg) + total_written, total_read - total_written);
                            if (bytes_written <= 0) {
                                printf("write() <= 0 ??\n");
                                break;
                            }
                            total_written += bytes_written;
                        }
                    }
                }
            }

            if (fds[i].revents & POLLHUP) {
                printf("fd %d hungup; removing from list\n", i);
                remove = 1;
            }
            if (fds[i].revents & POLLERR || fds[i].revents & POLLNVAL) {
                printf("problem with fd %u, removing from list\n", i);
                remove = 1;
            }
            if (remove) {
                close(fds[i].fd);

                /* consolidate lists */
                fds[i].fd = fds[num_fds-1].fd;
                fds[i].events = POLLIN;

                nicks[i].fd = nicks[num_fds-1].fd;
                memcpy(nicks[i].nick, nicks[num_fds-1].nick, NICK_SIZE);

                memset(nicks[num_fds-1].nick, 0, NICK_SIZE);

                num_fds--;
            }
        }
    }

    pthread_exit(0);
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

void *user_input_thread(void *arg)
{
    int fd = *(int *)arg;
    struct msg msg = {0};
    char *rl_str = NULL;

    /* first, prompt for nick */
    do {
        rl_str = readline("enter nick: ");
    } while (rl_str == NULL);

    memcpy(g_client_state.nick, rl_str, NICK_SIZE-1);
    free(rl_str);

    strncpy(msg.nick, g_client_state.nick, NICK_SIZE-1);
    msg.type = MSG_JOIN;
    msg.time = time(NULL);
    write_msg(fd, &msg);

    using_history();
    clear_display();
    pthread_mutex_lock(&msg_mutex);
    update_display();
    pthread_mutex_unlock(&msg_mutex);

    /* main msg processing loop */
    while ((rl_str = readline(g_client_state.prompt)) != NULL) {
        if (rl_str[0] == '\0') {
            pthread_mutex_lock(&msg_mutex);
            update_display();
            pthread_mutex_unlock(&msg_mutex);
            continue;
        }
        memset(&msg, 0, sizeof(struct msg));
        strncpy(msg.msg, rl_str, MSG_SIZE-1);
        msg.time = time(NULL);
        msg.type = MSG_NORMAL;
        write_msg(fd, &msg);
        free(rl_str);
    }

    printf("user_input_thread() exiting\n");
    memset(&msg, 0, sizeof(struct msg));
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
            printf("server hung up!\n");
            break;
        }
        pthread_mutex_lock(&msg_mutex);
        add_new_message(&msg);
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

    new_action.sa_handler = ignore_user1;
    sigaction(SIGUSR1, &new_action, NULL);

    /* connect to socket to get fd */
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    printf("waiting to connect...\n");

    while (connect(fd, (struct sockaddr *)sock, sizeof(struct sockaddr_un)) < 0 && errno == ENOENT && counter < 10) {
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
    char sockpath[sizeof(COMMS_DIR_TEMPLATE) + sizeof(JCHAT_SOCK_FILENAME)];

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
        //printf("key: %s\n", &comms_dir_template[11]);
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

    clear_display();
    fflush(stdout);

    client(&sock);

    if (is_server) {
        printf("server is still running... [ctrl-c] to stop\n");
        pthread_join(pt_server, &res);
    }

    return 0;
}
