#ifndef NEWCHAT_H
#define NEWCHAT_H

#define COMMS_DIR_TEMPLATE "/tmp/comms.XXXXXX"
#define JCHAT_SOCK_FILENAME "/jchat.sock"
#define JCHAT_SOCK_FORMAT "%s" JCHAT_SOCK_FILENAME
#define MSG_MARK_STR "----- mark -----"

#define BUF_SIZE 1024
#define MSG_SIZE 4096
#define KEY_SIZE 7
#define MAX_CONNECT_RETRIES 10
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

/* ui commands */
#define UI_CHANGE_PROMPT_CMD 'p'
#define UI_CHANGE_TITLE_CMD 't'
#define UI_CLEAR_CMD 'c'
#define UI_CLEAR_HISTORY_CMD 'C'
#define UI_CYCLE_URGENT_MODE_CMD 'u'
#define UI_HELP_CMD 'h'
#define UI_MARK_CMD 'm'
#define UI_QUIT_CMD 'q'
#define UI_REDACT_CMD '-'
#define UI_RESET_CMD 'r'

/* TODO how will we use these (if it all)? */
#define TYPING_START_CMD '\\'
#define TYPING_END_CMD '/'
#define TYPING_STALLED_CMD '|'

enum msg_type {
    MSG_NORMAL,
    MSG_JOIN,
    MSG_JOIN_REJECTED,
    MSG_REDACT,
    MSG_CLEAR_HISTORY,
    MSG_MARK,
    MSG_QUIT
};

enum join_state {
    JOIN_PENDING = 0,
    JOIN_REJECTED,
    JOINED
};

enum urgent_type {
    URGENT_ALL = 0,
    URGENT_MSG_ONLY = 1,
    URGENT_NONE = 2
};

struct client_state {
    enum join_state join_state;
    int user_id;
    char prompt[PROMPT_SIZE]; /* custom prompt string */
    char key[KEY_SIZE];
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

/* FUNCTION DECLARATIONS */

void update_display(void);
void clear_display(void);
void add_new_message(struct msg *);
void ignore_signal(int signum);
void update_prompt(void);
void write_msg(int fd, struct msg *msg);
int read_msg(int fd, struct msg *msg);
void clear_history(void);
void delete_node(struct node *node);
void copy_msg(struct node *dst, struct msg *src);
int redact_message(int user_id);
void window_resized(int signum);
void process_message(struct msg *msg);
void client(const struct sockaddr_un *sock);

/* Responsible for the message multiplexing to clients. Only runs for the server (first user to connect) */
void *server_thread(void *arg);

/* Responsible for handling user input. Runs for all users */
void *user_input_thread(void *arg);

/* Responsible for handling messages received from the server thread for each client. Runs for all users */
void *server_processing_thread(void *arg);

#endif /* NEWCHAT_H */
