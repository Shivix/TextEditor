#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

struct termios orig_termios;

enum Mode {
    Normal,
    Insert,
};

// constexpr size_t InsertBufMax = 30; Use when my clangd stops yelling at me
#define InsertBufMax 30

struct State {
    struct Cursor {
        size_t x;
        size_t y;
    } cursor;
    size_t offset;
    size_t rows;
    size_t cols;
    char* file_name;
    char insert_buf[InsertBufMax];
    size_t insert_n;
    enum Mode mode;
};

struct SubBuffer {
    char* data;
    size_t size;
};
struct Buffer {
    struct SubBuffer* data;
    size_t rows;
};

void free_buffer(struct Buffer* buf) {
    for (size_t i = 0; i < buf->rows; i++) {
        free(buf->data[i].data);
    }
    free(buf->data);
}

void cleanup_exit(const char* s) {
    // TODO:
    perror(s);
    exit(1);
}

struct State new_state(char** argv) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        cleanup_exit("failed to get window size");
    }
    struct State state = {
        .cols = ws.ws_col,
        .rows = ws.ws_row,
        .offset = 0,
        .cursor =
            {
                .x = 0,
                .y = 0,
            },
        .file_name = argv[1],
        .insert_buf = {'\0'},
        .insert_n = 0,
        .mode = Normal,
    };
    return state;
}

struct Buffer buffer_from_file(const char* file_name) {
    struct Buffer buffer = {
        nullptr,
        0,
    };

    FILE* file = fopen(file_name, "r");
    if (!file) {
        cleanup_exit("failed to open file");
    }

    char line[512];
    while (fgets(line, sizeof(line), file)) {
        size_t line_len = strlen(line);
        struct SubBuffer buf_line = {
            (char*)malloc(line_len + 1),
            line_len,
        };

        if (!buf_line.data) {
            free_buffer(&buffer);
            fclose(file);
            cleanup_exit("failed to malloc buffer");
        }

        strcpy(buf_line.data, line);
        buf_line.data[buf_line.size] = '\0';
        buffer.data =
            (struct SubBuffer*)realloc(buffer.data, (buffer.rows + 1) * sizeof(struct SubBuffer));
        buffer.data[buffer.rows] = buf_line;
        ++buffer.rows;
    }
    fclose(file);
    return buffer;
}

// TODO: insert_line when trying to add new line also.
void insert_line(struct Buffer* buf, struct State* state) {
    struct SubBuffer* line = &buf->data[state->cursor.y];
    line->data = realloc(line->data, line->size + state->insert_n);
    line->size += state->insert_n;
    memmove(line->data + state->cursor.x + state->insert_n, line->data + state->cursor.x,
            line->size - state->insert_n + 1);
    strncpy(line->data + state->cursor.x, state->insert_buf, state->insert_n);
    memset(state->insert_buf, '\0', sizeof(state->insert_buf));
    state->insert_n = 0;
}

void delete_char(struct Buffer* buf, struct State* state) {
    struct SubBuffer* line = &buf->data[state->cursor.y];
    for (size_t i = state->cursor.x; i < line->size - 1; ++i) {
        line->data[i] = line->data[i + 1];
    }
    line->data[line->size - 1] = '\0';
    --line->size;
}

void disableRawMode() {
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios) == -1) {
        perror("failed to reset terminal settings");
    }
}

void enableRawMode() {
    if (tcgetattr(STDIN_FILENO, &orig_termios) == -1) {
        cleanup_exit("failed to get terminal settings");
    }
    atexit(disableRawMode);
    struct termios raw = orig_termios;
    // Turn off cannonical and echo mode in the terminal
    raw.c_lflag &= ~(ECHO | ICANON);
    // Disable ctrl C
    raw.c_cc[VINTR] = _POSIX_VDISABLE;
    // set timeout for read
    //raw.c_cc[VMIN] = 0;
    //raw.c_cc[VTIME] = 5;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) {
        cleanup_exit("failed to set terminal settings");
    }
}

void clear_screen() {
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
}

char read_keypress() {
    int nread;
    char c;
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        if (nread == -1 && errno != EAGAIN) {
            cleanup_exit("failed to read from stdin");
        }
    }
    return c;
}

int handle_keypress(struct Buffer* buf, struct State* state) {
    char c = read_keypress();
    struct Cursor* cursor = &state->cursor;
    size_t half_height = state->rows / 2;
    if (state->mode == Insert) {
        state->insert_buf[state->insert_n] = c;
        ++state->insert_n;
        if (state->insert_n == InsertBufMax - 1) {
            insert_line(buf, state);
        }
        if (c == CTRL('o')) {
            insert_line(buf, state);
            state->mode = Normal;
        }
        return 0;
    }
    switch (c) {
    case 'h':
        if (cursor->x != 0) {
            --cursor->x;
        }
        break;
    case 'j':
        if (cursor->y != state->rows) {
            ++cursor->y;
        } else if (state->offset != state->cols) {
            ++state->offset;
        }
        break;
    case 'k':
        if (cursor->y != 0) {
            --cursor->y;
        } else if (state->offset != 0) {
            --state->offset;
        }
        break;
    case 'l':
        if (cursor->y != state->rows) {
            ++cursor->x;
        }
        break;
    case 'i':
        state->mode = Insert;
        break;
    case 'x':
        delete_char(buf, state);
        break;
    case CTRL('d'):
        if (cursor->y + half_height > state->rows) {
            cursor->y = state->rows;
            break;
        }
        cursor->y += half_height;
        break;
    case CTRL('u'):
        if (cursor->y < half_height) {
            cursor->y = 0;
            break;
        }
        cursor->y -= half_height;
        break;
    case CTRL('q'):
        return 1;
    }
    return 0;
}

void move_cursor(int x, int y) {
    char buf[27] = {'\0'};
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", y + 1, x + 1);
    write(STDOUT_FILENO, buf, sizeof(buf));
}

void draw_buffer_contents(struct Buffer* buf, struct State* state) {
    move_cursor(0, 0);
    size_t max_lines = buf->rows < state->rows ? buf->rows : state->rows;
    for (size_t i = state->offset; i < max_lines - 1; ++i) {
        write(STDOUT_FILENO, buf->data[i].data, buf->data[i].size);
    }
}

void draw_empty_space(size_t from, struct State* state) {
    if (from > state->rows) {
        return;
    }
    move_cursor(0, from);
    write(STDOUT_FILENO, "~", 1);
    for (size_t y = from; y < state->rows; y++) {
        write(STDOUT_FILENO, "\n~", 2);
    }
}

void draw_status_line(struct State* state) {
    move_cursor(0, state->rows);
    char buf[128] = {'\0'};
    // TODO: Get term code for right align
    // TODO: Could put a log message in status line
    snprintf(buf, sizeof(buf), " Normal | %s | %zu:%zu ", state->file_name, state->cursor.y,
             state->cursor.x);
    write(STDOUT_FILENO, buf, sizeof(buf));
}

void update_screen(struct Buffer* buf, struct State* state) {
    clear_screen();
    draw_empty_space(buf->rows, state);
    draw_buffer_contents(buf, state);
    draw_status_line(state);

    size_t cursor_x = state->cursor.x < buf->data[state->cursor.y].size
        ? state->cursor.x
        : buf->data[state->cursor.y].size;
    move_cursor(cursor_x, state->cursor.y);
}

int main(int argc, char** argv) {
    if (argc != 2) {
        cleanup_exit("Please provide one file name to open");
    }
    struct State state = new_state(argv);
    struct Buffer buffer = buffer_from_file(state.file_name);
    enableRawMode();
    update_screen(&buffer, &state);
    while (!handle_keypress(&buffer, &state)) {
        update_screen(&buffer, &state);
    }
    clear_screen();
    free_buffer(&buffer);
}
