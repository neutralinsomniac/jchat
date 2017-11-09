#include <stdio.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <readline/readline.h>
#include <readline/history.h>
#include <signal.h>

#define BUFSIZE 1024
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
#define MSG_HISTORY_CLEARED "other side cleared history"

#define CMD_SIZE 3
#define CLEAR_CMD "c"
#define QUIT_CMD "q"
#define RESET_CMD "r"
#define CLEAR_HISTORY_CMD "C"
#define CHANGE_TITLE_CMD "t"
#define REDACT_CMD "-"

#define CHANGE_PROMPT_CMD "p"
#define CHANGE_OTHER_NAME_CMD "n"
#define MARK_CMD "m"
#define HELP_CMD "h"
#define TYPING_START_CMD "\x01\\"
#define TYPING_END_CMD "\x01/"
#define TYPING_STALLED_CMD "\x01|"
#define CYCLE_URGENT_MODE_CMD "u"

static int is_a = 0;
static int transient_mode = 0;

char custom_prompt[BUFSIZE];
char log_filename [BUFSIZE];
char a_filename [BUFSIZE];
char b_filename [BUFSIZE];
char lock_filename[BUFSIZE];
char dirname[BUFSIZE];
char other_name[BUFSIZE];
char window_title[BUFSIZE];

enum msg_type {
    ME,
    THEM,
    STATUS
};

enum urgent_type {
    URGENT_ALL = 0,
    URGENT_MSG_ONLY	= 1,
    URGENT_NONE = 2
};

#define NUM_URGENT_MODES 3

struct node {
    char *msg;
    enum msg_type type;
    time_t time;
    struct node *next;
    struct node *prev;
};

pthread_mutex_t msg_mutex;
pthread_t pt_reader, pt_writer;

struct node *root = NULL;
struct node *tail = NULL;
struct winsize w;

int clear = 0;
int them_clear = 0;
int pending_msg = 0;
int us_typing = 0;
int them_typing = 0;
int current_urgent_mode = URGENT_MSG_ONLY;
int typing_timer = 0;
int prev_rl_end = 0;
int stall_sent = 0;

void update_display();
void clear_display();
void update_prompt();

FILE* writer_fp;

void cycle_urgent_mode() {
    current_urgent_mode = (current_urgent_mode + 1) % NUM_URGENT_MODES;
}

int check_typing()
{
    if (rl_end == prev_rl_end) {
        typing_timer++ ;
    } else {
        typing_timer = 0;
        prev_rl_end = rl_end;
        stall_sent = 0;
    }

    if (rl_end > 0 && (us_typing == 0 || typing_timer <= 30)) {
        us_typing = 1;
        stall_sent = 0;
        fprintf(writer_fp, "%s\n", TYPING_START_CMD);
        fflush(writer_fp);
    } else if (rl_end == 0 && us_typing == 1) {

        us_typing = 0;
        stall_sent = 0;
        fprintf(writer_fp, "%s\n", TYPING_END_CMD);
        fflush(writer_fp);
    } else if (typing_timer > 30 && us_typing == 1 && stall_sent != 1) {
        fprintf(writer_fp, "%s\n", TYPING_STALLED_CMD);
        fflush(writer_fp);
        stall_sent = 1;
    }

    return 0;
}

void cleanup ()
{
    // reset terminal
    printf("%s", RESET_TERM);
    printf("%s", CLEAR_SCROLLBACK);
    printf("%s", CLEAR_SCREEN);

    unlink(a_filename);
    unlink(b_filename);
    unlink(lock_filename);
    rmdir(dirname);
}

void get_prompt(char *prompt)
{
    char them_typing_indicator;

    switch (them_typing) {
        case 0:
            them_typing_indicator = '>';
            break;
        case 1:
            them_typing_indicator = '.';
            break;
        case 2:
            them_typing_indicator = '-';
            break;
    }

    if (pending_msg > 0) {
        snprintf(prompt, BUFSIZE, "*(%u)%s%s%s%s%c ", 
                pending_msg, (transient_mode) ? "~" : "",
                (them_clear) ? "^" : "", (clear) ? "!" : "",
                custom_prompt, them_typing_indicator);
    } else {

        snprintf(prompt, BUFSIZE, "%s%s%s%s%c ", 
                (transient_mode) ? "~" : "",
                (them_clear) ? "^" : "", (clear) ? "!" : "",
                custom_prompt, them_typing_indicator);
    }
}

void update_prompt()
{
    char prompt[BUFSIZE];

    printf ("%s", CLEAR_LINE);
    get_prompt(prompt);
    rl_set_prompt(prompt);
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
    memset(node->msg, 0, strlen(node->msg));
    free(node->msg);
    memset(node, 0, sizeof(struct node));
    free(node);
    node = NULL;
}

void update_display()
{
    int count = 0, i;
    struct node *iter;
    struct tm timeinfo;
    struct tm now;
    time_t now_time;
    char time_str[BUFSIZE];
    char *newline;

    // save cursor
    printf("%s", SAVE_CURSOR);
    fflush(stdout);

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


    if (clear) {
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
        localtime_r(&iter->time, &timeinfo);
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
        switch (iter->type) {
            case ME:
                printf("%s", COLOR_CYAN);
                printf("Me: %s", iter->msg);
                break;
            case THEM:
                printf("%s", COLOR_YELLOW);
                printf("%s: %s", other_name, iter->msg);  
                break ;
            case STATUS:
                printf("%s", COLOR_NONE); 
                printf("%s", iter->msg); 
                break;
        }
        printf("%s", COLOR_NONE) ;
        printf("\n");
        iter = iter->next;
        fflush(stdout);
    }

    //restore cursor
    printf( "%s", RESTORE_CURSOR);
    fflush(stdout);
}

int redact_last_msg(enum msg_type type) {
    struct node *iter;
    int found =  0;

    if (!tail) {
        return found;
    }

    iter = tail; 
    while (iter) {
        if (iter->type == type) {
            delete_node(iter);
            iter = NULL;
            found = 1; 
        } else {
            iter = iter->prev;
        }
    }

    if (found) {
        update_display();
    }

    return found;
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
        memset(iter->msg, 0, strlen(iter->msg));
        free(iter->msg);
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

void remove_mark_message()
{
    struct node *iter;

    iter = root;

    while (iter) {
        if (iter->type == STATUS && (strcmp(iter->msg, MSG_MARK) == 0)) {
            delete_node(iter);
            iter = NULL;
        } else {
            iter = iter->next;
        }
    }
}

void remove_history_cleared_messages()
{
    struct node *iter;

    iter = root;

    while (iter) {
        if (iter->type == STATUS &&
                (strcmp(iter->msg, MSG_HISTORY_CLEARED) == 0)) {
            struct node *next = iter->next;
            delete_node(iter);
            iter = next;
        } else {
            iter = iter->next;
        }
    }
}

void add_new_message(char *msg, enum msg_type type)
{
    struct node *new = NULL;
    struct node *iter;
    struct node *prev;
    int i;

    if (root == NULL) {
        root = malloc(sizeof(struct node));
        root->msg = malloc(strlen(msg) + 1);
        strncpy(root->msg, msg, strlen(msg) +  1);
        root->time = time(NULL);
        root->type = type;
        root->next = NULL;
        root->prev = NULL;
        tail = root;
    } else {
        new = malloc(sizeof(struct node));
        new->msg = malloc(strlen(msg) + 1);
        strncpy(new->msg, msg, strlen(msg) + 1);
        new->time = time(NULL);
        new->type = type;
        new->next = NULL;
        // update tail
        tail->next = new;
        new->prev = tail;
        tail = new;
    }

    // trim history for transient mode
    if (transient_mode) {
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

void *reader(void *arg)
{
    FILE* fp;
    int msg_len, total_len = 0;
    char msg[BUFSIZE];
    char *msg_assembled = NULL;
    int line_complete;

    if (is_a) {
        fp = fopen(b_filename, "r");
    } else {
        fp = fopen(a_filename, "r");
        fscanf(fp, "%u\n", &transient_mode);
    }

    while (1) {
        if (NULL == fgets(msg, sizeof(msg), fp)) {
            fclose(fp);
            pthread_exit(0);
        }

        msg_len =  strlen (msg);

        if (msg[msg_len - 1] != '\n') {
            line_complete = 0;
        } else {
            line_complete = 1;
        }

        total_len += msg_len;
        msg_assembled = realloc(msg_assembled, total_len + 1);
        if (total_len == msg_len) {
            memset(msg_assembled, 0, total_len + 1);
        }
        strncat(msg_assembled, msg, total_len +  1);

        // we can receive remote commands
        if (line_complete) {
            // strip newline
            msg_assembled[total_len - 1] = '\0';
            if (strncmp(CLEAR_CMD, msg_assembled, CMD_SIZE) == 0) {
                pthread_mutex_lock(&msg_mutex);
                them_clear = ~them_clear;
                update_prompt();
                pthread_mutex_unlock(&msg_mutex);
            } else if (strncmp(CLEAR_HISTORY_CMD, msg_assembled, CMD_SIZE) ==  0) {
                pthread_mutex_lock(&msg_mutex);
                add_new_message(MSG_HISTORY_CLEARED, STATUS);
                update_display();
                if (current_urgent_mode != URGENT_NONE) {
                    printf("%s", VISIBLE_BEEP);
                    fflush(stdout);
                }
                pthread_mutex_unlock(&msg_mutex);
            } else if (strncmp(REDACT_CMD, msg_assembled, CMD_SIZE) == 0) {
                pthread_mutex_lock(&msg_mutex);
                if (clear && pending_msg >  0) {
                    pending_msg--;
                    update_prompt();
                }
                redact_last_msg(THEM);
                pthread_mutex_unlock(&msg_mutex);
            } else if (strncmp (TYPING_START_CMD, msg_assembled, CMD_SIZE) == 0) {
                    pthread_mutex_lock(&msg_mutex);
                    them_typing = 1;
                    update_prompt();
                    printf(CHANGE_TITLE_IS_TYPING_FORMAT, window_title);
                    if (current_urgent_mode == URGENT_ALL) {
                        printf("%s", VISIBLE_BEEP);
                    }
                    fflush(stdout);
                    pthread_mutex_unlock(&msg_mutex);
            } else if (strncmp(TYPING_END_CMD, msg_assembled, CMD_SIZE) == 0) {
                pthread_mutex_lock(&msg_mutex);
                them_typing = 0;
                update_prompt();
                printf(CHANGE_TITLE_FORMAT, window_title);
                fflush(stdout);
                pthread_mutex_unlock(&msg_mutex);
            } else if (strncmp(TYPING_STALLED_CMD, msg_assembled, CMD_SIZE) == 0) {
                pthread_mutex_lock(&msg_mutex);
                them_typing = 2;
                update_prompt();
                printf(CHANGE_TITLE_FORMAT, window_title);
                fflush(stdout);
                pthread_mutex_unlock(&msg_mutex);
            } else if (strncmp (QUIT_CMD, msg_assembled, CMD_SIZE) == 0) {
                fclose(fp);
                pthread_mutex_lock(&msg_mutex);
                clear_history();
                pthread_mutex_unlock(&msg_mutex);
                clear_display();
                printf("%s", RESET_TERM);
                printf("other side closed connection\n");
                // signal writer to quit
                pthread_kill(pt_writer, SIGUSR1);
                pthread_exit(0);
            } else {
                pthread_mutex_lock(&msg_mutex);
                add_new_message(msg_assembled, THEM);
                if (clear) {
                    pending_msg++;
                    update_prompt();
                }
                update_display();
                if (current_urgent_mode != URGENT_NONE) {
                    printf("%s", VISIBLE_BEEP);
                    fflush(stdout);
                }
                pthread_mutex_unlock(&msg_mutex) ;
            }

            free(msg_assembled);
            msg_assembled = NULL;
            total_len = 0;
        }
    }
}

void *writer(void *arg)
{
    char *msg =  NULL;
    char prompt[BUFSIZE];

    if (is_a) {
        writer_fp = fopen(a_filename , "w");
        fprintf(writer_fp, "%u\n", transient_mode);
        fflush(writer_fp);
    } else {
        writer_fp = fopen(b_filename, "w");
    }

    using_history();
    rl_event_hook = check_typing;

    while (1) {
        if (msg) {
            free(msg);
            msg = NULL;
        }

        printf("%s", CLEAR_LINE);
        fflush(stdout);
        get_prompt(prompt);
        if (NULL == (msg = readline(prompt))) {
            // tell the other side to quit (if we can)
            pthread_mutex_lock(&msg_mutex);
            clear_history();
            fprintf(writer_fp , QUIT_CMD);
            fprintf(writer_fp, "\n");
            fclose(writer_fp);
            pthread_mutex_unlock(&msg_mutex);
            pthread_exit(0);
        }

        if (*msg == '\0') {
            // do nothing
            pthread_mutex_lock(&msg_mutex);
            update_display();
            pthread_mutex_unlock(&msg_mutex);
        } else if (strncmp(CHANGE_PROMPT_CMD, msg, CMD_SIZE) == 0) {
            // prompt for new name
            if (msg)
                free(msg);
            msg = readline("new prompt: ");
            snprintf(custom_prompt, sizeof(custom_prompt), "%s", msg);
            pthread_mutex_lock(&msg_mutex);
            clear_display();
            update_display();
            pthread_mutex_unlock(&msg_mutex);
        } else if (strncmp(CHANGE_TITLE_CMD, msg, CMD_SIZE) == 0) {
            // prompt for new title
            if (msg)
                free(msg);
            msg = readline("new window title: ");
            snprintf(window_title, sizeof(window_title), "%s", msg);
            printf(CHANGE_TITLE_FORMAT, window_title) ;
            fflush(stdout);
        } else if (strncmp(CYCLE_URGENT_MODE_CMD, msg, CMD_SIZE) == 0) {
            cycle_urgent_mode();
            printf("%s", SAVE_CURSOR);
            printf("%s", LINE_UP);
            printf("current urgent mode: ");
            switch (current_urgent_mode) {
                case URGENT_ALL:
                    printf("all\n");
                    break;
                case URGENT_MSG_ONLY:
                    printf("new msgs only\n");
                    break;
                case URGENT_NONE:
                    printf("none\n");
                    break;
            }
            printf("%s", RESTORE_CURSOR);
            fflush(stdout);
        } else if (strncmp(CHANGE_OTHER_NAME_CMD, msg, CMD_SIZE) == 0) {
            // prompt for new name
            if (msg)
                free(msg);
            msg = readline("new other name: ");
            snprintf(other_name, sizeof(other_name), "%s", msg);
            pthread_mutex_lock(&msg_mutex);
            clear_display();
            update_display();
            pthread_mutex_unlock(&msg_mutex);
        } else if (strncmp(HELP_CMD, msg, CMD_SIZE) == 0) {
            printf("%s", SAVE_CURSOR);
            printf("%s", LINE_UP);
            printf("q: quit\nc: toggle clear mode\nu: cycle urgent mode\nm: "
                    "mark current line\nt: change window title\nn: change "
                    "other name\np: change prompt string\nr: redraw "
                    "terminal\nC: clear history\n-: redact last message\n");
            printf("%s", RESTORE_CURSOR);
            fflush(stdout);
        } else if (strncmp(REDACT_CMD, msg, CMD_SIZE) == 0) {
            pthread_mutex_lock(&msg_mutex);
            // only send redact command if we have a message to redact
            if (redact_last_msg(ME)) {
                fprintf(writer_fp, "%s\n", msg);
                fflush(writer_fp);
            }
            pthread_mutex_unlock(&msg_mutex);
        } else if (strncmp(CLEAR_HISTORY_CMD, msg, CMD_SIZE) == 0) {
            pthread_mutex_lock(&msg_mutex);
            clear_history();
            clear_display();
            update_display();
            fprintf(writer_fp, "%s\n", msg);
            fflush(writer_fp);
            pthread_mutex_unlock(&msg_mutex);
        } else if (strncmp(RESET_CMD, msg, CMD_SIZE) == 0) {
            pthread_mutex_lock(&msg_mutex);
            clear_display();
            update_display();
            pthread_mutex_unlock(&msg_mutex);
        } else if (strncmp(QUIT_CMD, msg, CMD_SIZE) == 0) {
            pthread_mutex_lock(&msg_mutex);
            clear_history();
            pthread_mutex_unlock(&msg_mutex);
            fprintf(writer_fp, "%s\n", msg);
            fclose(writer_fp);
            pthread_exit(0);
        } else if (strncmp(CLEAR_CMD, msg, CMD_SIZE) == 0) {
            pthread_mutex_lock(&msg_mutex);
            clear = ~clear;
            if (!clear) {
                if (pending_msg == 0) {
                    remove_mark_message();
                }
                pending_msg = 0;
            } else {
                remove_mark_message();
                add_new_message(MSG_MARK, STATUS);
            }
            clear_display();
            update_display();
            // send this command to let the other side know we're in clear mode
            fprintf(writer_fp, "%s\n", msg);
            fflush(writer_fp);
            pthread_mutex_unlock(&msg_mutex);
        } else if (strncmp(MARK_CMD, msg, CMD_SIZE) == 0) {
            if (!clear) {
                pthread_mutex_lock(&msg_mutex);
                remove_mark_message();
                add_new_message(MSG_MARK, STATUS);
                update_display();
                pthread_mutex_unlock(&msg_mutex);
            }
        } else {
            pthread_mutex_lock(&msg_mutex);
            if (!clear) {
                // remove the clear message if we 're not in clear mode
                remove_mark_message();
                remove_history_cleared_messages();
            }
            add_new_message(msg, ME);
            add_history(msg);
            fprintf(writer_fp, "%s\n", msg);
            fflush(writer_fp);
            update_display();
            pthread_mutex_unlock(&msg_mutex);
        }
    }
}

int main(int argc, char *argv[])
{
    struct stat fstat;
    void *res;
    struct sigaction new_action;
    char *response;

    FILE *startupfp;
    FILE *lockfp;

    if	(argc != 1)  {
        printf("usage : %s\n", argv[0]);
        return 1;
    }

    response = readline("<enter> for new session, key for existing: ");
    if (response == NULL) {
        printf("goodbye!\n");
        return 1;
    } else if (response [0] == '\0') {
        snprintf(dirname, BUFSIZE, "/tmp/comms.XXXXXX");
        if (NULL == mkdtemp(dirname)) {
            printf("error in mkdtemp()\n");
            return 1;
        }
    } else {
        if (strlen(response) != 6) {
            printf("invalid key, goodbye!\n");
            return 1;
        } else {
          snprintf(dirname, BUFSIZE, "/tmp/comms.%s", response);
        }
    }

    snprintf(custom_prompt, sizeof(custom_prompt), "%s", &dirname[11]);
    snprintf(log_filename, sizeof(log_filename), "/tmp/%s.txt", custom_prompt);
    snprintf(a_filename, sizeof(a_filename), "%s/a", dirname);
    snprintf(b_filename, sizeof(b_filename), "%s/b", dirname);
    snprintf(lock_filename, sizeof(lock_filename), "%s/lock", dirname );

    // check lock file to make sure there isn't an existing session
    if (0 == stat(lock_filename, &fstat)) {
        printf("invalid key, goodbye!\n");
        return 1;
    }

    // meetup
    if (-1 == stat(a_filename, &fstat)) {
        if (errno == ENOENT) {
            // we're first, create the fifos
            umask(0);
            if (-1 == mkfifo(a_filename , 0600) ||
                    -1 == mkfifo(b_filename, 0600)) {
                printf("invalid key, goodbye!\n");
                return 1;
            }

            is_a = 1;

            response = readline("transient mode? (y/N): ");

            if (response != NULL && strcmp(response, "y") == 0) {
                transient_mode = 1;
            }

            printf("key: %s\nwaiting for connection...\n", custom_prompt);
            startupfp = fopen(a_filename, "r");

            if (startupfp) {
                fflush(NULL);
                fscanf(startupfp, "%*d\n");
                fclose(startupfp) ;
            } else {
                printf("error opening startup file for reading\n");
                return 1;
            }
        } else {
            printf("error stat'ing startup file\n");
            return 1;
        }
    } else {
        // we're second; write to the fifo to signal our arrival
        startupfp = fopen(a_filename, "w");
        if (startupfp) {
            fprintf(startupfp, "1\n");
            fclose(startupfp) ;
        } else {
            printf("error opening startup file for writing\n");
            return 1;
        }
    }

    // made it, lock!
    lockfp = fopen(lock_filename, "w");
    if (lockfp == NULL) {
        printf("error acquiring lock!\n");
        cleanup();
        exit(1);
    }

    fclose(lockfp);

    // clear our session name
    snprintf(window_title, sizeof(window_title), "%s", custom_prompt);
    printf(CHANGE_TITLE_FORMAT, window_title);
    memset(custom_prompt, 0, sizeof(custom_prompt));
    clear_display();
    printf("%s", VISIBLE_BEEP);
    fflush(stdout);

    snprintf(other_name, sizeof(other_name), "Them");

    sigemptyset(&new_action.sa_mask);
    new_action.sa_flags = 0;
    new_action.sa_handler = window_resized;
    sigaction(SIGWINCH, &new_action, NULL);

    // startup threads
    pthread_create(&pt_reader, NULL, &reader, NULL);
    pthread_create(&pt_writer, NULL, &writer, NULL) ;

    pthread_join(pt_writer, &res);
    free(res);

    cleanup();
    printf("bye!\n");
    return 0;
}
