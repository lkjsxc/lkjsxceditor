/*
 * lkjsxceditor - A simple Vim-like text editor in one C file.
 *
 * Features:
 * - Normal, Insert, Command modes
 * - Basic movement (h,j,k,l, PageUp/Down, Home/End)
 * - Basic insertion and deletion (backspace, enter)
 * - Static memory allocation (fixed buffer pool)
 * - Minimal external dependencies (standard C library + POSIX termios/ioctl/fcntl)
 * - Basic commands: :w, :q, :wq, :w <filename>
 * - Minimal error handling (uses status bar)
 */

#include <ctype.h>  // iscntrl, isprint
#include <errno.h>  // errno, EAGAIN, ENOENT
#include <fcntl.h>  // open flags
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>  // ioctl, TIOCGWINSZ, winsize
#include <sys/stat.h>   // open, O_RDONLY, O_WRONLY, O_CREAT, O_TRUNC
#include <termios.h>    // termios, tcgetattr, tcsetattr, TCSAFLUSH, ECHO, ICANON, etc.
#include <time.h>       // time (currently unused for status timeout)
#include <unistd.h>     // write, read, close, STDIN_FILENO, STDOUT_FILENO

// --- Configuration ---
#define BUFCHUNK_SIZE 512         // Size of each text chunk
#define BUFCHUNK_COUNT 1024       // Number of chunks in the pool (512 KiB total)
#define SCREEN_BUFFER_SIZE 65536  // Static buffer for screen drawing (64 KiB)
#define COMMAND_BUFFER_MAX 100    // Max length for command mode input

// --- Enums ---
enum RESULT {
    RESULT_OK,
    RESULT_ERR
};

enum editorMode {
    MODE_NORMAL,
    MODE_INSERT,
    MODE_COMMAND
};

// Key codes for special keys (values >= 1000 to avoid conflict with ASCII)
enum editorKey {
    ARROW_LEFT = 1009,
    ARROW_RIGHT = 1008,
    ARROW_UP = 1006,
    ARROW_DOWN = 1007,
    DEL_KEY = 1003,
    HOME_KEY = 1000,
    END_KEY = 1001,
    PAGE_UP = 1004,
    PAGE_DOWN = 1005,
    BACKSPACE = 127  // Map ASCII DEL (127) to Backspace behavior
    // Note: Some terminals might send ^H (ASCII 8) for backspace. Handle later if needed.
};

// --- Data Structures ---

// Represents a chunk of text in the buffer's linked list
struct bufchunk {
    char data[BUFCHUNK_SIZE];
    struct bufchunk* prev;
    struct bufchunk* next;
    int size;  // Number of bytes currently used in data[]
};

// Represents a buffer (text, command, message) using linked chunks
struct bufclient {
    struct bufchunk* begin;         // First chunk in the buffer
    struct bufchunk* rbegin;        // Last chunk in the buffer
    struct bufchunk* cursor_chunk;  // Chunk containing the logical cursor/insertion point
    int cursor_index;               // Index within the cursor_chunk
    int size;                       // Total number of bytes stored across all chunks
};

// Global editor state structure
struct editorConfig {
    int cx, cy;                   // Screen cursor X, Y (0-based, relative to top-left of text area)
    int rx;                       // Rendered cursor X (0-based, handling tabs, relative to line start)
    int rowoff;                   // Row offset (which file row number (0-based) is at top of screen)
    int coloff;                   // Column offset (which file column number (0-based) is at left of screen)
    int screenrows;               // Number of rows available for text editing (Terminal rows - 2)
    int screencols;               // Number of columns available (Terminal cols)
    char filename[256];           // Current filename associated with the buffer
    char statusmsg[80];           // Status message buffer displayed on the message bar
    time_t statusmsg_time;        // Timestamp for status message (for timeout - currently unused)
    struct termios orig_termios;  // Stores original terminal settings for restoration
    enum editorMode mode;         // Current editor mode (Normal, Insert, Command)
    struct bufclient textbuf;     // Main text buffer where the file content resides
    struct bufclient cmdbuf;      // Buffer for typing commands in Command mode
    int quit_flag;                // Flag set to 1 to signal the main loop to exit
    // int modified_flag; // TODO: Add flag to track unsaved changes
};

// --- Globals ---
static struct bufchunk bufchunk_pool_data[BUFCHUNK_COUNT];  // Static pool of chunks
static struct bufchunk* bufchunk_pool_free = NULL;          // Head of the free chunk list
static struct editorConfig E;                               // Global editor state instance

// Static buffer for accumulating screen output to minimize write() calls
static char screen_buffer[SCREEN_BUFFER_SIZE];
static int screen_buffer_len = 0;  // Current length of content in screen_buffer

// --- Buffer Chunk Pool Management ---

// Initializes the free list for the buffer chunk pool.
void bufchunk_pool_init() {
    bufchunk_pool_free = NULL;
    // Link all chunks in the pool into a singly-linked free list
    for (int i = BUFCHUNK_COUNT - 1; i >= 0; i--) {
        bufchunk_pool_data[i].prev = NULL;  // Not needed for free list links
        bufchunk_pool_data[i].size = 0;
        bufchunk_pool_data[i].next = bufchunk_pool_free;  // Point to the previous head
        bufchunk_pool_free = &bufchunk_pool_data[i];      // Current chunk becomes the new head
    }
}

// Allocates a chunk from the free list. Returns NULL if pool is exhausted.
struct bufchunk* bufchunk_alloc() {
    if (!bufchunk_pool_free) {
        return NULL;  // Pool exhausted
    }
    // Get chunk from the head of the free list
    struct bufchunk* chunk = bufchunk_pool_free;
    bufchunk_pool_free = chunk->next;  // Advance free list head

    // Initialize the allocated chunk for use
    chunk->prev = NULL;
    chunk->next = NULL;
    chunk->size = 0;
    // Note: Existing data in chunk->data is not cleared for performance;
    // chunk->size indicates the valid portion.
    return chunk;
}

// Returns a chunk to the free list.
void bufchunk_free(struct bufchunk* chunk) {
    if (!chunk)
        return;
    // Add the chunk back to the head of the free list
    chunk->prev = NULL;  // Ensure links expected by buffer logic are cleared
    chunk->next = bufchunk_pool_free;
    chunk->size = 0;  // Reset size
    bufchunk_pool_free = chunk;
}

// --- Buffer Client (bufclient) Management ---

// Initializes a bufclient structure with a single empty chunk.
enum RESULT bufclient_init(struct bufclient* buf) {
    buf->begin = bufchunk_alloc();
    if (!buf->begin)
        return RESULT_ERR;           // Failed to get initial chunk from pool
    buf->rbegin = buf->begin;        // Initially, first chunk is also the last
    buf->cursor_chunk = buf->begin;  // Cursor starts in the first chunk
    buf->cursor_index = 0;           // Cursor starts at index 0
    buf->size = 0;                   // Buffer is initially empty
    return RESULT_OK;
}

// Frees all chunks associated with a bufclient, returning them to the pool.
void bufclient_free(struct bufclient* buf) {
    struct bufchunk* current = buf->begin;
    struct bufchunk* next;
    while (current) {
        next = current->next;
        bufchunk_free(current);  // Return chunk to the pool
        current = next;
    }
    // Reset bufclient structure fields to indicate it's empty/invalid
    buf->begin = NULL;
    buf->rbegin = NULL;
    buf->cursor_chunk = NULL;
    buf->cursor_index = 0;
    buf->size = 0;
}

// Helper: Calculate the absolute byte position (0-based) of the cursor
// within the buffer by summing sizes of preceding chunks and adding the index.
int bufclient_get_abs_pos(struct bufclient* buf) {
    int pos = 0;
    struct bufchunk* current = buf->begin;

    // Iterate through chunks before the cursor's chunk
    while (current != buf->cursor_chunk && current != NULL) {
        pos += current->size;
        current = current->next;
    }

    // If cursor chunk was found, add the index within that chunk
    if (current == buf->cursor_chunk) {
        pos += buf->cursor_index;
    }
    // Handle cases where cursor might be invalid or buffer is empty
    else if (buf->cursor_chunk == NULL && buf->begin != NULL && buf->size == 0) {
        // Empty buffer with an allocated initial chunk
        pos = 0;
    } else if (buf->cursor_chunk == NULL && buf->begin == NULL) {
        // Buffer truly empty (no chunks allocated)
        pos = 0;
    } else {
        // Fallback: If cursor chunk is inconsistent, return end position
        pos = buf->size;
    }
    return pos;
}

// Helper: Set the cursor (chunk/index) based on an absolute byte position.
enum RESULT bufclient_set_abs_pos(struct bufclient* buf, int target_pos) {
    // Validate target position
    if (target_pos < 0 || target_pos > buf->size)
        return RESULT_ERR;

    // Handle edge case: empty buffer
    if (buf->size == 0 && target_pos == 0) {
        if (!buf->begin) {  // If even the initial chunk isn't allocated (shouldn't happen after init)
            if (bufclient_init(buf) != RESULT_OK)
                return RESULT_ERR;  // Try re-init
        }
        buf->cursor_chunk = buf->begin;
        buf->cursor_index = 0;
        return RESULT_OK;
    }

    int current_pos = 0;
    struct bufchunk* current = buf->begin;
    while (current) {
        // Check if target position falls within the current chunk's range
        // Note: Can be *at* the end (index == size), meaning cursor is after last char
        if (target_pos >= current_pos && target_pos <= current_pos + current->size) {
            buf->cursor_chunk = current;
            buf->cursor_index = target_pos - current_pos;
            return RESULT_OK;
        }
        current_pos += current->size;

        // Check if target is exactly at the end of the entire buffer and we are in the last chunk
        if (current == buf->rbegin && target_pos == current_pos) {
            buf->cursor_chunk = current;
            buf->cursor_index = current->size;
            return RESULT_OK;
        }
        current = current->next;
    }
    // Position not found (should not happen if target_pos <= buf->size and buffer is consistent)
    return RESULT_ERR;
}

// Inserts a single character 'c' at the current cursor position in the buffer.
// Handles chunk splitting if the current chunk is full.
enum RESULT bufclient_insert_char(struct bufclient* buf, char c) {
    struct bufchunk* chunk = buf->cursor_chunk;
    int index = buf->cursor_index;

    // Ensure cursor chunk is valid, especially for an empty buffer after init
    if (!chunk) {
        if (buf->size == 0 && buf->begin) {
            chunk = buf->begin;  // Use the initial empty chunk
            buf->cursor_chunk = chunk;
            index = 0;
            buf->cursor_index = 0;
        } else {
            return RESULT_ERR;  // Invalid buffer state
        }
    }

    // Case 1: Current chunk has available space
    if (chunk->size < BUFCHUNK_SIZE) {
        // If inserting in the middle, shift existing data to the right
        if (index < chunk->size) {
            // memmove handles overlapping source and destination safely
            memmove(&chunk->data[index + 1], &chunk->data[index], chunk->size - index);
        }
        // Place the new character and update sizes/indices
        chunk->data[index] = c;
        chunk->size++;
        buf->cursor_index++;  // Move cursor forward
        buf->size++;          // Increment total buffer size
        return RESULT_OK;
    }

    // Case 2: Current chunk is full. Need to allocate a new chunk.
    struct bufchunk* new_chunk = bufchunk_alloc();
    if (!new_chunk)
        return RESULT_ERR;  // Out of memory (chunk pool exhausted)

    // Option A: Inserting exactly at the end of the full chunk (cursor_index == BUFCHUNK_SIZE)
    // This means we append the new chunk right after the current one.
    if (index == BUFCHUNK_SIZE) {
        // Link the new chunk into the list after the current chunk
        new_chunk->next = chunk->next;
        if (chunk->next)
            chunk->next->prev = new_chunk;  // Link back from following chunk
        else
            buf->rbegin = new_chunk;  // Update buffer's end pointer if inserting after last chunk
        new_chunk->prev = chunk;
        chunk->next = new_chunk;

        // Place the character in the new chunk
        new_chunk->data[0] = c;
        new_chunk->size = 1;

        // Move the cursor to the new chunk, positioned after the inserted character
        buf->cursor_chunk = new_chunk;
        buf->cursor_index = 1;
        buf->size++;
        return RESULT_OK;
    }

    // Option B: Inserting in the middle of the full chunk (index < BUFCHUNK_SIZE)
    // This requires splitting the current chunk.
    // Move the second half of data (from index onwards) from the full chunk to the new chunk.
    int move_count = BUFCHUNK_SIZE - index;
    memcpy(new_chunk->data, &chunk->data[index], move_count);
    new_chunk->size = move_count;
    chunk->size = index;  // Truncate the original chunk's size

    // Link the new chunk into the list immediately after the original (now truncated) chunk
    new_chunk->next = chunk->next;
    if (chunk->next)
        chunk->next->prev = new_chunk;
    else
        buf->rbegin = new_chunk;  // Update end pointer if splitting the last chunk
    new_chunk->prev = chunk;
    chunk->next = new_chunk;

    // Now the original chunk has space at the end (where data was moved from).
    // Insert the new character into the original chunk.
    chunk->data[index] = c;
    chunk->size++;
    buf->cursor_index++;  // Cursor remains in the original chunk, moved one position right
    buf->size++;
    return RESULT_OK;
}

// Deletes the character immediately *before* the cursor (Backspace behavior).
// Handles merging or removing chunks if they become empty.
enum RESULT bufclient_delete_char(struct bufclient* buf) {
    struct bufchunk* chunk = buf->cursor_chunk;
    int index = buf->cursor_index;

    // Cannot delete if the cursor is at the absolute beginning of the buffer
    if (chunk == buf->begin && index == 0) {
        return RESULT_ERR;  // Nothing to delete before the cursor
    }

    // Case 1: Deleting from within a chunk (cursor is not at the start of the chunk)
    if (index > 0) {
        // Shift data left to overwrite the character at index - 1
        // memmove handles overlapping source/destination
        memmove(&chunk->data[index - 1], &chunk->data[index], chunk->size - index);
        chunk->size--;
        buf->cursor_index--;  // Move cursor back one position
        buf->size--;

        // Optional Optimization: If this chunk becomes empty *and* it's not the
        // only chunk remaining in the buffer, remove it from the list.
        if (chunk->size == 0 && !(chunk == buf->begin && chunk == buf->rbegin)) {
            struct bufchunk* prev_chunk = chunk->prev;  // Should exist if not begin chunk
            struct bufchunk* next_chunk = chunk->next;

            // Update links of neighboring chunks to bypass the empty chunk
            if (prev_chunk)
                prev_chunk->next = next_chunk;
            else
                buf->begin = next_chunk;  // Update buffer start pointer if first chunk removed

            if (next_chunk)
                next_chunk->prev = prev_chunk;
            else
                buf->rbegin = prev_chunk;  // Update buffer end pointer if last chunk removed

            // Move cursor to the end of the previous chunk (logical position before deletion)
            // Note: prev_chunk is guaranteed to exist here because we checked chunk != buf->begin
            buf->cursor_chunk = prev_chunk;
            buf->cursor_index = prev_chunk->size;

            bufchunk_free(chunk);  // Return the now-empty chunk to the pool
        }
        // Further optimization (more complex): Check if adjacent chunks can now be merged
        // if their combined size is <= BUFCHUNK_SIZE. Not implemented for simplicity.
        return RESULT_OK;
    }

    // Case 2: Deleting at the start of a chunk (index == 0), but not the first chunk.
    // This means deleting the boundary between the previous chunk and this one.
    // The character logically deleted is the last character of the *previous* chunk.
    if (index == 0 && chunk != buf->begin) {
        struct bufchunk* prev_chunk = chunk->prev;

        // Sanity check: previous chunk must exist if current is not the beginning
        if (!prev_chunk)
            return RESULT_ERR;  // Should not happen in a consistent list

        // If previous chunk is already empty, something is wrong or delete has no effect
        if (prev_chunk->size == 0)
            return RESULT_ERR;  // Cannot delete last char of empty chunk

        // Move cursor logically to the end of the previous chunk *before* deleting its last char
        buf->cursor_chunk = prev_chunk;
        buf->cursor_index = prev_chunk->size;  // Cursor is now after the char to be deleted

        // Perform the deletion by simply reducing the size of the previous chunk
        prev_chunk->size--;
        buf->size--;

        // Optional Optimization: If the *current* chunk ('chunk') is now empty
        // (it might have been empty already), remove it.
        if (chunk->size == 0) {
            // Link previous chunk directly to the chunk after the current one
            prev_chunk->next = chunk->next;
            if (chunk->next)
                chunk->next->prev = prev_chunk;
            else
                buf->rbegin = prev_chunk;  // Update buffer end if 'chunk' was the last one

            bufchunk_free(chunk);  // Return the empty chunk to the pool
        }
        // Further optimization: Consider merging prev_chunk and chunk->next if space allows.
        return RESULT_OK;
    }

    // Should not be reachable if logic is correct
    return RESULT_ERR;
}

// --- Terminal Handling ---

// Prints error message and exits cleanly.
void die(const char* s) {
    // Attempt to clear screen and reset cursor for visibility of error
    write(STDOUT_FILENO, "\x1b[2J", 4);  // Clear entire screen
    write(STDOUT_FILENO, "\x1b[H", 3);   // Move cursor to top-left

    // Print the error message passed, plus system error if relevant
    perror(s);  // Prints 's' followed by system error message based on errno

    // Ensure terminal is restored before exiting
    // Check if termios struct looks initialized before trying to restore
    if (E.orig_termios.c_lflag != 0) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios);
    }

    exit(1);
}

// Restores original terminal settings.
void disableRawMode() {
    // Only restore if original settings were successfully saved
    if (E.orig_termios.c_lflag != 0) {  // Use c_lflag as indicator it was read
        if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1) {
            // Can't do much if restoration fails, maybe print warning?
            perror("lkjsxceditor: failed to restore terminal settings");
        }
    }
}

// Enables raw mode for terminal input (no canonical processing, no echo).
void enableRawMode() {
    // Get current terminal attributes
    if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1)
        die("tcgetattr failed");

    // Create a copy to modify
    struct termios raw = E.orig_termios;

    // Disable flags for raw input:
    // IXON: Disable Ctrl-S/Ctrl-Q software flow control
    // ICRNL: Disable translating Carriage Return (Enter key) to Newline
    // BRKINT, INPCK, ISTRIP: Miscellaneous flags often disabled in raw mode
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);

    // Disable output processing flags:
    // OPOST: Disable all output processing (e.g., translating '\n' to '\r\n')
    raw.c_oflag &= ~(OPOST);

    // Set character size to 8 bits per byte
    raw.c_cflag |= (CS8);

    // Disable local flags:
    // ECHO: Disable echoing typed characters back to the terminal
    // ICANON: Disable canonical (line-buffered) mode; read byte-by-byte
    // ISIG: Disable signal characters (Ctrl-C, Ctrl-Z)
    // IEXTEN: Disable implementation-defined input processing (e.g., Ctrl-V)
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);

    // Set control character timeouts for non-blocking reads:
    // VMIN = 0: read() returns immediately, even if no bytes available
    // VTIME = 1: Timeout after 1/10th of a second (100ms) if no bytes available
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;

    // Apply the modified attributes
    // TCSAFLUSH: Change occurs after all output written to fd has been transmitted,
    //            and all input received but not read is discarded before the change.
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
        die("tcsetattr failed on entry");

    // Note: No atexit() handler used; cleanup is called explicitly before main returns.
}

// Reads a single keypress from standard input, handling escape sequences.
// Returns the character code or a special key code (enum editorKey).
int editorReadKey() {
    int nread;
    char c;
    // Loop until a key is read or an error occurs (excluding EAGAIN)
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        // EINTR might happen if signal received, EAGAIN if timeout expires (VTIME)
        if (nread == -1 && errno != EAGAIN && errno != EINTR)
            die("read error");
        // Add check for terminal resize signal here if needed
    }

    // Check if the read character is the ESCAPE character (potential sequence start)
    if (c == '\x1b') {
        char seq[3];  // Buffer to read potential escape sequence parts

        // Try reading the next character immediately. If timeout/fail, it was just ESC key.
        if (read(STDIN_FILENO, &seq[0], 1) != 1)
            return '\x1b';
        // Try reading the character after that. If timeout/fail, it was ESC + one char.
        if (read(STDIN_FILENO, &seq[1], 1) != 1)
            return '\x1b';

        // Check for common CSI (Control Sequence Introducer) sequences: ESC [ ...
        if (seq[0] == '[') {
            if (seq[1] >= '0' && seq[1] <= '9') {
                // Sequences like ESC [ 3 ~ (Delete key)
                if (read(STDIN_FILENO, &seq[2], 1) != 1)
                    return '\x1b';  // Need final char
                if (seq[2] == '~') {
                    switch (seq[1]) {
                        // Map standard xterm escape sequences
                        case '1':
                            return HOME_KEY;  // Often Home
                        case '3':
                            return DEL_KEY;  // Often Delete
                        case '4':
                            return END_KEY;  // Often End
                        case '5':
                            return PAGE_UP;
                        case '6':
                            return PAGE_DOWN;
                        case '7':
                            return HOME_KEY;  // Sometimes Home
                        case '8':
                            return END_KEY;  // Sometimes End
                        default:
                            return '\x1b';  // Unrecognized sequence number
                    }
                } else {
                    return '\x1b';  // Unrecognized sequence format
                }
            } else {
                // Sequences like ESC [ A (Arrow Up)
                switch (seq[1]) {
                    case 'A':
                        return ARROW_UP;
                    case 'B':
                        return ARROW_DOWN;
                    case 'C':
                        return ARROW_RIGHT;
                    case 'D':
                        return ARROW_LEFT;
                    case 'H':
                        return HOME_KEY;  // Sometimes Home (vt100?)
                    case 'F':
                        return END_KEY;  // Sometimes End (vt100?)
                    default:
                        return '\x1b';  // Unrecognized char after '['
                }
            }
        }
        // Check for VT100/Linux console sequences: ESC O ...
        else if (seq[0] == 'O') {
            switch (seq[1]) {
                case 'H':
                    return HOME_KEY;  // Usually Home
                case 'F':
                    return END_KEY;  // Usually End
                default:
                    return '\x1b';  // Unrecognized char after 'O'
            }
        }
        // If none of the recognized patterns matched, return plain ESC
        return '\x1b';
    } else if (c == 127) {
        // Map the ASCII DEL character (often sent by Backspace key) to our BACKSPACE code
        return BACKSPACE;
    } else {
        // Regular character read
        return c;
    }
}

// Gets the current cursor position using ANSI escape codes (DSR).
// Used as a fallback for getting window size. Returns -1 on failure.
int getCursorPosition(int* rows, int* cols) {
    char buf[32];
    unsigned int i = 0;

    // Send DSR (Device Status Report) - cursor position query (ESC [ 6 n)
    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4)
        return -1;

    // Read the response from stdin: Should be ESC [ <rows> ; <cols> R
    // Read until 'R' or buffer is full or timeout
    while (i < sizeof(buf) - 1) {
        if (read(STDIN_FILENO, &buf[i], 1) != 1)
            break;  // Error or timeout
        if (buf[i] == 'R')
            break;  // End of response marker
        i++;
    }
    buf[i] = '\0';  // Null-terminate the buffer

    // Parse the response (e.g., "\x1b[24;80R")
    // Basic validation: must start with ESC [
    if (buf[0] != '\x1b' || buf[1] != '[')
        return -1;

    // Use sscanf for simplicity (careful with buffer overflows if not null-terminated)
    // Or manual parsing for robustness:
    char* ptr = &buf[2];
    int r = 0, c = 0;
    while (*ptr >= '0' && *ptr <= '9')
        r = r * 10 + (*ptr++ - '0');
    if (*ptr != ';')
        return -1;  // Expect semicolon separator
    ptr++;
    while (*ptr >= '0' && *ptr <= '9')
        c = c * 10 + (*ptr++ - '0');
    // Optional: Check if *ptr is now 'R'

    *rows = r;
    *cols = c;
    return 0;
}

// Gets the terminal window size (rows and columns).
// Uses ioctl(TIOCGWINSZ) first, falls back to cursor positioning trick if ioctl fails.
int getWindowSize(int* rows, int* cols) {
    struct winsize ws;

    // Try ioctl first - most reliable method
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        // Fallback: Move cursor far right (999C), then far down (999B),
        // then query its position using DSR (Device Status Report).
        // This works on many terminals that don't support TIOCGWINSZ.
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12)
            return -1;                         // Move cursor
        return getCursorPosition(rows, cols);  // Query position
    } else {
        // Success using ioctl
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}

// --- Buffer Navigation (Cursor Movement Logic) ---

// Helper: Find the absolute byte position of the start of the line
// containing the given absolute position 'abs_pos'.
// Scans backwards from 'abs_pos' looking for '\n' or buffer start.
// Returns 0 if 'abs_pos' is on the first line.
int find_line_start(int abs_pos) {
    if (abs_pos <= 0)
        return 0;  // Already at or before start

    int line_start_pos = 0;  // Default to start of buffer
    struct bufchunk* current_chunk = E.textbuf.begin;
    int current_chunk_start_abs = 0;

    // Iterate through chunks to find the one containing abs_pos or preceding it
    while (current_chunk && current_chunk_start_abs + current_chunk->size <= abs_pos) {
        // Scan within this chunk for the last newline before abs_pos
        for (int i = 0; i < current_chunk->size; i++) {
            if (current_chunk_start_abs + i >= abs_pos)
                break;  // Don't scan past target
            if (current_chunk->data[i] == '\n') {
                line_start_pos = current_chunk_start_abs + i + 1;  // Start is after the newline
            }
        }
        current_chunk_start_abs += current_chunk->size;
        current_chunk = current_chunk->next;
    }

    // Scan the chunk that actually contains abs_pos
    if (current_chunk) {
        int end_scan_index = abs_pos - current_chunk_start_abs;
        for (int i = 0; i < end_scan_index; i++) {
            if (current_chunk->data[i] == '\n') {
                line_start_pos = current_chunk_start_abs + i + 1;
            }
        }
    }

    // Ensure line_start doesn't exceed the original position (can happen if abs_pos is 0)
    if (line_start_pos > abs_pos)
        line_start_pos = abs_pos;
    return line_start_pos;
}

// Helper: Find the absolute byte position of the end of the line
// containing 'abs_pos'. The end is the position of the '\n' character itself,
// or the end of the buffer if no '\n' follows.
int find_line_end(int abs_pos) {
    struct bufchunk* current_chunk = E.textbuf.begin;
    int current_chunk_start_abs = 0;

    // First, find the chunk containing abs_pos
    while (current_chunk && current_chunk_start_abs + current_chunk->size <= abs_pos) {
        current_chunk_start_abs += current_chunk->size;
        current_chunk = current_chunk->next;
    }

    // If abs_pos is beyond buffer content (shouldn't happen if abs_pos <= E.textbuf.size)
    if (!current_chunk)
        return E.textbuf.size;

    // Now scan from abs_pos onwards within this chunk and subsequent ones
    int scan_start_index = abs_pos - current_chunk_start_abs;
    while (current_chunk) {
        for (int i = scan_start_index; i < current_chunk->size; i++) {
            if (current_chunk->data[i] == '\n') {
                return current_chunk_start_abs + i;  // Return position *of* the newline
            }
        }
        // Continue scan in the next chunk
        current_chunk_start_abs += current_chunk->size;
        scan_start_index = 0;  // Start scan from index 0 of the next chunk
        current_chunk = current_chunk->next;
    }

    // If no newline was found until the end of the buffer
    return E.textbuf.size;
}

// Moves the logical cursor (E.textbuf.cursor_chunk/index) based on the key pressed.
void editorMoveCursor(int key) {
    int current_abs_pos = bufclient_get_abs_pos(&E.textbuf);
    int target_abs_pos = current_abs_pos;  // Default to current position

    // Find current line start and column for reference (used by up/down)
    int current_line_start = find_line_start(current_abs_pos);
    int current_col = current_abs_pos - current_line_start;

    switch (key) {
        case ARROW_LEFT:
        case 'h':
            if (target_abs_pos > 0) {
                target_abs_pos--;
            }
            break;
        case ARROW_RIGHT:
        case 'l':
            if (target_abs_pos < E.textbuf.size) {
                target_abs_pos++;
            }
            break;
        case ARROW_UP:
        case 'k':
            if (current_line_start > 0) {  // Can only move up if not on the first line
                // Find start of the line *before* the current line_start
                int prev_line_start = find_line_start(current_line_start - 1);
                // Find the end of that previous line
                int prev_line_end = find_line_end(prev_line_start);
                // Try to move to the same column (current_col) on the previous line
                target_abs_pos = prev_line_start + current_col;
                // If the previous line was shorter, clamp cursor to the end of that line
                if (target_abs_pos > prev_line_end) {
                    target_abs_pos = prev_line_end;
                }
            }
            // else: Already on the first line, do nothing
            break;

        case ARROW_DOWN:
        case 'j': {
            int current_line_end = find_line_end(current_abs_pos);
            if (current_line_end < E.textbuf.size) {         // Can only move down if not on the last line
                int next_line_start = current_line_end + 1;  // Start is after the '\n'
                int next_line_end = find_line_end(next_line_start);
                // Try to move to the same column (current_col) on the next line
                target_abs_pos = next_line_start + current_col;
                // If the next line is shorter, clamp cursor to the end of that line
                if (target_abs_pos > next_line_end) {
                    target_abs_pos = next_line_end;
                }
            }
            // else: Already on the last line, do nothing
        } break;

        case PAGE_UP: {
            // Calculate actual cursor row number (cy_actual)
            int cy_actual = 0;
            struct bufchunk* c = E.textbuf.begin;
            int p = 0;
            while (c && p < current_abs_pos) {
                int end = (c == E.textbuf.cursor_chunk) ? E.textbuf.cursor_index : c->size;
                for (int i = 0; i < end && p < current_abs_pos; ++i) {
                    if (c->data[i] == '\n')
                        cy_actual++;
                    p++;
                }
                if (p == current_abs_pos)
                    break;
                if (c == E.textbuf.cursor_chunk && p + c->size > current_abs_pos)
                    break;
                c = c->next;
            }

            // Move target position up by screenrows lines, preserving column
            E.cy = cy_actual - E.rowoff;       // Update screen cy for reference
            target_abs_pos = current_abs_pos;  // Start from current position
            int lines_to_move = E.screenrows;
            while (lines_to_move-- > 0) {
                int line_start = find_line_start(target_abs_pos);
                if (line_start == 0) {   // Reached the first line
                    target_abs_pos = 0;  // Go to very beginning
                    break;
                }
                // Go to start of previous line for next iteration's calculation
                target_abs_pos = find_line_start(line_start - 1);
            }
            // Now target_abs_pos is at the start of the target line (or buffer start)
            // Adjust column similar to ARROW_UP logic
            int final_line_start = target_abs_pos;
            int final_line_end = find_line_end(final_line_start);
            // Use the original column offset (current_col)
            if (final_line_end >= final_line_start + current_col) {
                target_abs_pos = final_line_start + current_col;
            } else {
                target_abs_pos = final_line_end;  // Clamp to end of shorter line
            }

            // Adjust rowoff so the new cursor position is near the top of the screen
            int target_row = 0;
            c = E.textbuf.begin;
            p = 0;
            while (c && p < target_abs_pos) {
                int end = c->size;
                for (int i = 0; i < end; ++i) {
                    if (p == target_abs_pos)
                        goto end_row_count_pgup;
                    if (c->data[i] == '\n')
                        target_row++;
                    p++;
                }
                c = c->next;
            }
        end_row_count_pgup:;
            E.rowoff = target_row;  // Place target line at the top
            if (E.rowoff < 0)
                E.rowoff = 0;
        } break;
        case PAGE_DOWN: {
            // Calculate actual cursor row number (cy_actual)
            int cy_actual = 0;
            struct bufchunk* c = E.textbuf.begin;
            int p = 0;
            while (c && p < current_abs_pos) {
                int end = (c == E.textbuf.cursor_chunk) ? E.textbuf.cursor_index : c->size;
                for (int i = 0; i < end && p < current_abs_pos; ++i) {
                    if (c->data[i] == '\n')
                        cy_actual++;
                    p++;
                }
                if (p == current_abs_pos)
                    break;
                if (c == E.textbuf.cursor_chunk && p + c->size > current_abs_pos)
                    break;
                c = c->next;
            }

            E.cy = cy_actual - E.rowoff;  // Update screen cy for reference

            // Move target position down by screenrows lines, preserving column
            target_abs_pos = current_abs_pos;  // Start from current position
            int lines_to_move = E.screenrows;
            while (lines_to_move-- > 0) {
                int line_end = find_line_end(target_abs_pos);
                if (line_end >= E.textbuf.size) {     // Reached the last line
                    target_abs_pos = E.textbuf.size;  // Go to very end
                    break;
                }
                // Go to start of next line for next iteration's calculation
                target_abs_pos = line_end + 1;
            }
            // Now target_abs_pos is at the start of the target line (or buffer end)
            // Adjust column similar to ARROW_DOWN logic
            int final_line_start = (target_abs_pos == E.textbuf.size) ? find_line_start(target_abs_pos - (target_abs_pos > 0)) : find_line_start(target_abs_pos);
            int final_line_end = find_line_end(final_line_start);
            // Use the original column offset (current_col)
            if (final_line_end >= final_line_start + current_col) {
                target_abs_pos = final_line_start + current_col;
            } else {
                target_abs_pos = final_line_end;  // Clamp to end of shorter line
            }

            // Adjust rowoff so the new cursor position is near the bottom of the screen
            int target_row = 0;
            c = E.textbuf.begin;
            p = 0;
            while (c && p < target_abs_pos) {
                int end = c->size;
                for (int i = 0; i < end; ++i) {
                    if (p == target_abs_pos)
                        goto end_row_count_pgdn;
                    if (c->data[i] == '\n')
                        target_row++;
                    p++;
                }
                c = c->next;
            }
        end_row_count_pgdn:;
            E.rowoff = target_row - E.screenrows + 1;  // Place target line near bottom
            if (E.rowoff < 0)
                E.rowoff = 0;
        } break;
        case HOME_KEY:  // Move cursor to the beginning of the current line
            target_abs_pos = current_line_start;
            break;
        case END_KEY:  // Move cursor to the end of the current line (before newline)
            target_abs_pos = find_line_end(current_abs_pos);
            break;
    }

    // Apply the calculated target position if it's valid and different
    if (target_abs_pos >= 0 && target_abs_pos <= E.textbuf.size) {
        bufclient_set_abs_pos(&E.textbuf, target_abs_pos);
    }
    // Note: The actual screen update (scrolling) happens in editorScroll()
    // which is called by editorRefreshScreen().
}

// --- Screen Drawing ---

// Appends a string segment to the static screen buffer.
void screenAppend(const char* s, int len) {
    // Check if there's enough space in the static buffer
    if (screen_buffer_len + len >= SCREEN_BUFFER_SIZE) {
        // Buffer full, cannot append. Skip this segment. (Minimal error handling)
        // Could potentially flush early, but might cause flicker.
        return;
    }
    // Copy the string segment into the buffer
    memcpy(&screen_buffer[screen_buffer_len], s, len);
    screen_buffer_len += len;  // Update the length tracker
}

// Writes the accumulated content of the static screen buffer to standard output
// and resets the buffer length for the next screen refresh.
void screenFlush() {
    if (screen_buffer_len > 0) {
        // Write the entire buffer in one go (reduces flicker)
        if (write(STDOUT_FILENO, screen_buffer, screen_buffer_len) == -1) {
            // Ignore write error (minimal error handling)
            // A more robust editor might try to handle this.
        }
    }
    screen_buffer_len = 0;  // Reset buffer for the next refresh cycle
}

// Calculates the cursor's screen coordinates (E.cx, E.cy) based on its
// absolute position in the buffer and the current scroll offsets.
// Also adjusts scroll offsets (E.rowoff, E.coloff) if the cursor has moved
// outside the currently visible screen area.
// Calculates the rendered column E.rx (handling tabs if implemented).
void editorScroll() {
    int current_abs_pos = bufclient_get_abs_pos(&E.textbuf);

    // Calculate cursor's actual row (cy_actual, 0-based from buffer start)
    // and rendered column (E.rx, 0-based from current line start)
    int cy_actual = 0;
    int current_line_start = 0;
    E.rx = 0;  // Reset rendered X, recalculate below

    struct bufchunk* chunk = E.textbuf.begin;
    int abs_pos = 0;
    while (chunk && abs_pos < current_abs_pos) {
        int end_idx = chunk->size;
        for (int i = 0; i < end_idx; ++i) {
            if (abs_pos == current_abs_pos)
                goto end_scroll_calc;  // Reached cursor position

            if (chunk->data[i] == '\n') {
                cy_actual++;
                current_line_start = abs_pos + 1;  // Mark start of the next line
            }
            abs_pos++;
        }
        chunk = chunk->next;
    }
end_scroll_calc:;

    // Calculate rx (rendered x). Simple version: no tab expansion yet.
    E.rx = current_abs_pos - current_line_start;
    // TODO: Add tab expansion logic to calculate E.rx accurately if needed.
    // Need to iterate from current_line_start to current_abs_pos, counting columns
    // and expanding tabs to the next tab stop (e.g., every 4 or 8 columns).

    // --- Adjust Scroll Offsets ---

    // Vertical scroll: If cursor moved above visible area, scroll up.
    if (cy_actual < E.rowoff) {
        E.rowoff = cy_actual;
    }
    // Vertical scroll: If cursor moved below visible area, scroll down.
    if (cy_actual >= E.rowoff + E.screenrows) {
        E.rowoff = cy_actual - E.screenrows + 1;
    }

    // Horizontal scroll: If cursor moved left of visible area, scroll left.
    if (E.rx < E.coloff) {
        E.coloff = E.rx;
    }
    // Horizontal scroll: If cursor moved right of visible area, scroll right.
    if (E.rx >= E.coloff + E.screencols) {
        E.coloff = E.rx - E.screencols + 1;
    }

    // --- Calculate Final Screen Coordinates ---
    // E.cy is the cursor row relative to the top of the editing window (0-based)
    E.cy = cy_actual - E.rowoff;
    // E.cx is the cursor column relative to the left of the editing window (0-based)
    E.cx = E.rx - E.coloff;
}

// Draws the visible text rows onto the screen buffer.
// Handles vertical and horizontal scrolling based on E.rowoff, E.coloff.
// Draws '~' for lines below the end of the buffer.
void editorDrawRows() {
    struct bufchunk* current_chunk = E.textbuf.begin;
    int current_abs_pos = 0;
    int current_line_num = 0;  // 0-based line number from start of buffer

    // --- Find Starting Position for Drawing ---
    // Find the absolute byte position corresponding to the start of the first visible row (E.rowoff)
    if (E.rowoff > 0) {
        while (current_chunk && current_line_num < E.rowoff) {
            for (int i = 0; i < current_chunk->size; ++i) {
                if (current_chunk->data[i] == '\n') {
                    current_line_num++;
                    if (current_line_num == E.rowoff) {
                        current_abs_pos++;  // Start drawing *after* this newline
                        goto found_start_row_chunk_search;
                    }
                }
                current_abs_pos++;
            }
            current_chunk = current_chunk->next;
        }
        // If rowoff is beyond the actual number of lines, current_chunk will be NULL
    }

found_start_row_chunk_search:;
    // Now, find the specific chunk and index within it for current_abs_pos
    int start_chunk_idx = 0;
    if (E.rowoff > 0 && current_chunk) {  // Only search if we found the target line
        struct bufchunk* search_chunk = E.textbuf.begin;
        int search_abs_pos = 0;
        while (search_chunk) {
            if (current_abs_pos >= search_abs_pos && current_abs_pos < search_abs_pos + search_chunk->size) {
                current_chunk = search_chunk;                        // Found the starting chunk
                start_chunk_idx = current_abs_pos - search_abs_pos;  // Found the starting index
                break;
            }
            search_abs_pos += search_chunk->size;
            search_chunk = search_chunk->next;
            // Handle case where start position is exactly at end of buffer / chunk boundary
            if (!search_chunk && current_abs_pos == search_abs_pos) {
                current_chunk = E.textbuf.rbegin;  // Should be last chunk
                start_chunk_idx = current_chunk ? current_chunk->size : 0;
                break;
            }
        }
        // If search failed (e.g., current_abs_pos inconsistent), current_chunk might be wrong.
        // The outer loop below will handle current_chunk being NULL.
    } else if (E.rowoff == 0) {
        // Start from the beginning of the first chunk
        current_chunk = E.textbuf.begin;
        start_chunk_idx = 0;
        current_abs_pos = 0;  // Redundant but clear
    }
    // If E.rowoff > 0 but current_chunk is NULL, it means rowoff was too large.

    // --- Draw Visible Rows ---
    for (int y = 0; y < E.screenrows; y++) {
        int screen_col = 0;  // Current column being drawn on the screen (0 to screencols-1)
        int file_col = 0;    // Current column relative to start of file line (for coloff)

        if (!current_chunk) {
            // We are past the end of the buffer content. Draw tildes.
            // Only draw tilde if buffer wasn't empty OR if it's not the very first line drawn.
            if (E.textbuf.size > 0 || (y + E.rowoff) > 0) {
                screenAppend("~", 1);
            }
        } else {
            // Draw content from the current line
            int current_chunk_idx = start_chunk_idx;
            struct bufchunk* line_chunk = current_chunk;  // Iterate using line_chunk

            while (line_chunk) {
                int end_scan = line_chunk->size;
                for (int i = current_chunk_idx; i < end_scan; i++) {
                    char current_char = line_chunk->data[i];

                    // Stop drawing this line if a newline is encountered
                    if (current_char == '\n') {
                        current_chunk = line_chunk;                    // Update main iterator chunk
                        start_chunk_idx = i + 1;                       // Next line starts after newline
                        if (start_chunk_idx >= current_chunk->size) {  // If newline was last char
                            current_chunk = current_chunk->next;       // Move to next chunk
                            start_chunk_idx = 0;                       // Start at index 0
                        }
                        goto next_line;  // Finished drawing this screen line
                    }

                    // Check horizontal scroll offset (coloff)
                    if (file_col >= E.coloff) {
                        screen_col = file_col - E.coloff;  // Calculate visible screen column

                        // Stop drawing if we exceed screen width
                        if (screen_col >= E.screencols) {
                            // Line continues off screen, update position for next line start
                            current_chunk = line_chunk;
                            start_chunk_idx = i;  // Next line effectively starts here for drawing
                            // We didn't hit newline, so next draw iteration needs careful state.
                            // Let's simplify: Assume the outer loop handles finding the *real*
                            // start of the next line based on file content.
                            // The `goto next_line` simplifies state management here.
                            goto next_line;
                        }

                        // Handle control characters and tabs (simple rendering)
                        if (iscntrl(current_char) && current_char != '\t') {
                            screenAppend("?", 1);  // Represent control chars as '?'
                        } else if (current_char == '\t') {
                            // Simple tab expansion: draw spaces to next tab stop (multiple of 4)
                            int spaces_to_add = 4 - (screen_col % 4);
                            for (int s = 0; s < spaces_to_add; s++) {
                                if (screen_col + s < E.screencols) {
                                    screenAppend(" ", 1);
                                } else
                                    break;  // Stop if space goes past screen edge
                            }
                            file_col += spaces_to_add - 1;  // Adjust file_col (loop adds 1)
                        } else {
                            // Regular printable character
                            screenAppend(&current_char, 1);  // Append character to screen buffer
                        }
                    }  // end if horizontally visible
                    file_col++;  // Increment file column count
                }  // end for loop within chunk

                // Move to the next chunk in the buffer for this line
                line_chunk = line_chunk->next;
                current_chunk_idx = 0;  // Start scan from beginning of next chunk
            }  // end while(line_chunk) for the current line

            // If we exit the while loop, it means we reached the end of the buffer
            // without finding a newline for the current screen line.
            current_chunk = NULL;  // Signal end of buffer for subsequent rows
            start_chunk_idx = 0;

        }  // end else (if current_chunk was not NULL)

    next_line:
        screenAppend("\x1b[K", 3);  // ANSI: Clear from cursor to end of line
        screenAppend("\r\n", 2);    // Move to the beginning of the next screen line

    }  // end for y (screen rows)
}

// Draws the status bar at the bottom of the screen.
// Shows mode, filename, buffer size, and cursor position.
void editorDrawStatusBar() {
    screenAppend("\x1b[7m", 4);  // ANSI: Turn on inverse video (white background, black text typical)
    char status[80], rstatus[80];
    int len, rlen;

    // --- Left side of status bar ---
    const char* mode_str = "NORMAL";  // Determine mode string
    if (E.mode == MODE_INSERT)
        mode_str = "INSERT";
    else if (E.mode == MODE_COMMAND)
        mode_str = "COMMAND";

    // Format: [MODE] | [Filename|"No Name"] | [Size] [Modified indicator - TODO]
    len = snprintf(status, sizeof(status), " %.7s | %.20s | %d bytes ",
                   mode_str,
                   E.filename[0] ? E.filename : "[No Name]",  // Show filename or placeholder
                   E.textbuf.size                             // Show total buffer size in bytes
                   // Add E.modified_flag indicator here later
    );
    if (len < 0)
        len = 0;  // Safety check for snprintf error
    if (len > E.screencols)
        len = E.screencols;  // Truncate if too long

    // --- Right side of status bar ---
    // Calculate current line number (1-based) for display
    int current_abs_pos = bufclient_get_abs_pos(&E.textbuf);
    int current_line = 1;  // Start at line 1
    struct bufchunk* chunk = E.textbuf.begin;
    int abs_pos = 0;
    while (chunk && abs_pos < current_abs_pos) {
        int end_idx = chunk->size;
        for (int i = 0; i < end_idx; ++i) {
            if (abs_pos == current_abs_pos)
                break;  // Stop counting when cursor position reached
            if (chunk->data[i] == '\n') {
                current_line++;  // Increment line count on newline
            }
            abs_pos++;
        }
        if (abs_pos == current_abs_pos)
            break;
        chunk = chunk->next;
    }

    // Format: [Line Number]:[Rendered Column + 1]
    rlen = snprintf(rstatus, sizeof(rstatus), " %d:%d ",
                    current_line,
                    E.rx + 1);  // E.rx is 0-based, display 1-based column
    if (rlen < 0)
        rlen = 0;  // Safety check

    // --- Combine and print status bar ---
    screenAppend(status, len);  // Print left part

    // Fill remaining space with spaces, then print right part aligned to right edge
    while (len < E.screencols) {
        if (E.screencols - len == rlen) {  // If remaining space exactly fits right status
            screenAppend(rstatus, rlen);
            len += rlen;
            break;  // Done filling
        } else {
            screenAppend(" ", 1);  // Add padding space
            len++;
        }
    }

    screenAppend("\x1b[m", 3);  // ANSI: Turn off all attributes (including inverse video)
    screenAppend("\r\n", 2);    // Move to the start of the next line (message bar line)
}

// Draws the message bar (second line from bottom).
// Shows command prompt in Command mode, or status messages otherwise.
void editorDrawMessageBar() {
    screenAppend("\x1b[K", 3);  // ANSI: Clear the current line first
    int msglen = 0;

    if (E.mode == MODE_COMMAND) {
        // --- Show Command Prompt ---
        // Construct the command prompt string: ":" followed by current cmdbuf content
        char prompt[COMMAND_BUFFER_MAX + 2];  // Max command len + ':' + null
        prompt[0] = ':';
        int cmdlen = 1;  // Start after the ':'
        struct bufchunk* chunk = E.cmdbuf.begin;
        while (chunk && cmdlen < sizeof(prompt) - 1) {
            int count = chunk->size;
            if (cmdlen + count >= sizeof(prompt) - 1) {  // Prevent overflow
                count = sizeof(prompt) - 1 - cmdlen;
            }
            memcpy(&prompt[cmdlen], chunk->data, count);
            cmdlen += count;
            chunk = chunk->next;
        }
        prompt[cmdlen] = '\0';  // Null-terminate the assembled command string
        msglen = cmdlen;
        if (msglen > E.screencols)
            msglen = E.screencols;  // Truncate if too long for screen
        if (msglen > 0) {
            screenAppend(prompt, msglen);  // Display the command prompt
        }
    } else {
        // --- Show Status Message ---
        // Display the message in E.statusmsg (potentially with timeout later)
        msglen = strlen(E.statusmsg);
        if (msglen > E.screencols)
            msglen = E.screencols;  // Truncate if too long
        // Add timeout check here if needed:
        // if (msglen > 0 && (E.statusmsg_time == 0 || time(NULL) - E.statusmsg_time < 5)) {
        if (msglen > 0) {
            screenAppend(E.statusmsg, msglen);
        }
        // } else { E.statusmsg[0] = '\0'; } // Auto-clear message after timeout
    }
}

// Sets the status message displayed on the message bar. Simplified (no varargs).
void editorSetStatusMessage(const char* msg) {
    // Copy the message, ensuring null termination and respecting buffer size
    strncpy(E.statusmsg, msg, sizeof(E.statusmsg) - 1);
    E.statusmsg[sizeof(E.statusmsg) - 1] = '\0';  // Ensure null termination
    // Optionally set timestamp for message timeout feature:
    // E.statusmsg_time = time(NULL);
}

// Refreshes the entire screen: hides cursor, draws rows, status, message bars,
// then positions cursor correctly and shows it again.
void editorRefreshScreen() {
    editorScroll();  // Update scroll offsets and calculate E.cx, E.cy, E.rx

    screenAppend("\x1b[?25l", 6);  // ANSI: Hide cursor (l = low = hide)
    screenAppend("\x1b[H", 3);     // ANSI: Move cursor to top-left (1,1)

    editorDrawRows();        // Draw the text buffer content into screen buffer
    editorDrawStatusBar();   // Draw the status bar into screen buffer
    editorDrawMessageBar();  // Draw the message/command bar into screen buffer

    // --- Position the actual terminal cursor ---
    char buf[32];  // Buffer for formatting cursor position command
    int term_row, term_col;

    if (E.mode == MODE_COMMAND) {
        // In command mode, cursor goes on the message bar after the typed command
        term_row = E.screenrows + 2;  // Message bar is 2 lines below text area (1-based)
        // Calculate current length of command in cmdbuf to find column
        int cmd_len = 1;  // Start with 1 for the ':' prompt
        struct bufchunk* c = E.cmdbuf.begin;
        while (c) {
            cmd_len += c->size;
            c = c->next;
        }
        if (cmd_len > E.screencols)
            cmd_len = E.screencols;  // Clamp to screen width
        term_col = cmd_len + 1;      // Position cursor after the last character (1-based)
    } else {
        // In Normal or Insert mode, cursor goes in the text area
        // E.cy, E.cx are 0-based relative to text area, terminal is 1-based
        term_row = E.cy + 1;
        term_col = E.cx + 1;
    }

    // Format the ANSI command to move cursor: ESC [ <row> ; <col> H
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", term_row, term_col);
    screenAppend(buf, strlen(buf));  // Add cursor positioning command to buffer

    screenAppend("\x1b[?25h", 6);  // ANSI: Show cursor (h = high = show)

    screenFlush();  // Write the entire accumulated screen buffer to the terminal
}

// --- File I/O ---

// Opens the specified file and loads its content into the text buffer.
// If file doesn't exist, starts with an empty buffer.
enum RESULT editorOpen(const char* filename) {
    // Store filename, ensuring null termination
    strncpy(E.filename, filename, sizeof(E.filename) - 1);
    E.filename[sizeof(E.filename) - 1] = '\0';

    int fd = open(filename, O_RDONLY);  // Open for reading only
    if (fd == -1) {
        if (errno == ENOENT) {
            // File doesn't exist - this is fine, treat as a new file.
            editorSetStatusMessage("New file created");
            return RESULT_OK;  // Start with empty buffer (already initialized)
        } else {
            // Other error opening file (permissions, etc.)
            char msg[sizeof(E.filename) + 30];  // Buffer for error message
            snprintf(msg, sizeof(msg), "ERR: Cannot open '%s': %s", filename, strerror(errno));
            editorSetStatusMessage(msg);
            return RESULT_ERR;
        }
    }

    // File opened successfully, clear existing buffer content first.
    bufclient_free(&E.textbuf);
    if (bufclient_init(&E.textbuf) != RESULT_OK) {
        close(fd);  // Close file descriptor even on error
        editorSetStatusMessage("ERR: Out of memory re-initializing buffer");
        return RESULT_ERR;
    }

    // Read file content into buffer chunk by chunk
    char read_buf[BUFCHUNK_SIZE];  // Use a reasonable read buffer size
    ssize_t nread;
    enum RESULT res = RESULT_OK;  // Assume success initially
    while ((nread = read(fd, read_buf, sizeof(read_buf))) > 0) {
        for (ssize_t i = 0; i < nread; i++) {
            // Handle line endings: Simple approach - skip '\r' (common in CRLF files)
            if (read_buf[i] == '\r')
                continue;

            // Insert character into the text buffer
            if (bufclient_insert_char(&E.textbuf, read_buf[i]) != RESULT_OK) {
                editorSetStatusMessage("ERR: Out of buffer memory loading file");
                res = RESULT_ERR;
                goto open_cleanup;  // Stop reading on memory error
            }
        }
    }

    // Check for read errors after the loop
    if (nread == -1) {
        char msg[sizeof(E.filename) + 30];
        snprintf(msg, sizeof(msg), "ERR: Read error on '%s': %s", filename, strerror(errno));
        editorSetStatusMessage(msg);
        res = RESULT_ERR;
    }

open_cleanup:
    close(fd);  // Close the file descriptor

    // Reset cursor and scroll position after loading
    bufclient_set_abs_pos(&E.textbuf, 0);  // Move cursor to start
    E.rowoff = 0;
    E.coloff = 0;

    // Set success message only if no errors occurred
    if (res == RESULT_OK && nread != -1) {
        char msg[sizeof(E.filename) + 30];
        snprintf(msg, sizeof(msg), "\"%s\" loaded (%d bytes)", E.filename, E.textbuf.size);
        editorSetStatusMessage(msg);
    }
    // Reset modified flag here if implemented
    // E.modified_flag = 0;
    return res;
}

// Saves the current text buffer content to the associated filename (E.filename).
// Creates the file if it doesn't exist, truncates if it does.
enum RESULT editorSave() {
    // Check if a filename is set
    if (E.filename[0] == '\0') {
        // Could potentially prompt for filename using command mode here.
        // For now, require :w <filename> first if name isn't set.
        editorSetStatusMessage("ERR: No filename. Use :w <filename>");
        return RESULT_ERR;
    }

    // Open file for writing. O_CREAT: create if doesn't exist. O_TRUNC: clear if exists.
    // Permissions 0644: rw-r--r-- (owner read/write, group read, others read)
    int fd = open(E.filename, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd == -1) {
        char msg[sizeof(E.filename) + 30];
        snprintf(msg, sizeof(msg), "ERR: Cannot save '%s': %s", E.filename, strerror(errno));
        editorSetStatusMessage(msg);
        return RESULT_ERR;
    }

    // --- Write buffer content chunk by chunk ---
    struct bufchunk* current = E.textbuf.begin;
    int total_written = 0;
    enum RESULT res = RESULT_OK;  // Assume success initially

    while (current) {
        if (current->size > 0) {  // Only write chunks that contain data
            ssize_t written = write(fd, current->data, current->size);
            if (written == -1) {
                // Write error occurred
                char msg[sizeof(E.filename) + 30];
                snprintf(msg, sizeof(msg), "ERR: Write error on '%s': %s", E.filename, strerror(errno));
                editorSetStatusMessage(msg);
                res = RESULT_ERR;
                goto save_cleanup;  // Stop writing on error
            }
            // Check for short write (disk full, etc.)
            if (written != current->size) {
                editorSetStatusMessage("ERR: Short write error, file may be incomplete!");
                res = RESULT_ERR;
                goto save_cleanup;
            }
            total_written += written;
        }
        current = current->next;  // Move to the next chunk
    }

    // Sanity check: total bytes written should match buffer size
    if (res == RESULT_OK && total_written != E.textbuf.size) {
        editorSetStatusMessage("ERR: Internal error - size mismatch after save!");
        res = RESULT_ERR;  // Treat inconsistency as an error
    }

save_cleanup:
    // Close the file descriptor, check for close errors too
    if (close(fd) == -1) {
        // Report close error only if write seemed okay, as write error is more critical
        if (res == RESULT_OK) {
            char msg[sizeof(E.filename) + 30];
            snprintf(msg, sizeof(msg), "ERR: Error closing '%s': %s", E.filename, strerror(errno));
            editorSetStatusMessage(msg);
            res = RESULT_ERR;
        }
    }

    // If save was successful overall, report bytes written
    if (res == RESULT_OK) {
        char msg[sizeof(E.filename) + 30];
        snprintf(msg, sizeof(msg), "Wrote %d bytes to \"%s\"", total_written, E.filename);
        editorSetStatusMessage(msg);
        // Reset modified flag here if implemented
        // E.modified_flag = 0;
    }
    return res;
}

// --- Command Mode Processing ---

// Parses and executes the command entered in the command buffer (E.cmdbuf).
void editorProcessCommand() {
    // --- Extract command string from cmdbuf ---
    char cmdline[COMMAND_BUFFER_MAX + 1];  // Local buffer for command string
    int cmdlen = 0;
    struct bufchunk* chunk = E.cmdbuf.begin;
    // Iterate through chunks in cmdbuf and copy data to cmdline
    while (chunk && cmdlen < sizeof(cmdline) - 1) {
        int count = chunk->size;
        // Prevent overflow of cmdline buffer
        if (cmdlen + count >= sizeof(cmdline) - 1) {
            count = sizeof(cmdline) - 1 - cmdlen;
        }
        memcpy(&cmdline[cmdlen], chunk->data, count);
        cmdlen += count;
        chunk = chunk->next;
    }
    cmdline[cmdlen] = '\0';  // Ensure null termination

    // --- Clear command buffer and switch mode back ---
    bufclient_free(&E.cmdbuf);                     // Free the used command buffer chunks
    if (bufclient_init(&E.cmdbuf) != RESULT_OK) {  // Re-initialize for next command
        // This is unlikely to fail unless pool is exhausted in a weird state
        editorSetStatusMessage("ERR: Critical - Failed to re-init cmdbuf!");
        // Might need to handle this more gracefully
    }
    E.mode = MODE_NORMAL;        // Usually switch back to normal mode after command
    editorSetStatusMessage("");  // Clear the command prompt / message bar

    // --- Parse and Execute Command ---
    if (cmdlen == 0)
        return;  // Empty command does nothing

    // Quit command
    if (strcmp(cmdline, "q") == 0) {
        // TODO: Add check for unsaved changes (E.modified_flag)
        // if (E.modified_flag) {
        //    editorSetStatusMessage("ERR: Unsaved changes! Use :q! to force quit.");
        //    E.mode = MODE_NORMAL; // Stay in editor
        //    return;
        // }
        E.quit_flag = 1;  // Signal main loop to exit
    }
    // Force Quit command
    else if (strcmp(cmdline, "q!") == 0) {
        E.quit_flag = 1;  // Signal exit regardless of changes
    }
    // Write command (save to current filename)
    else if (strcmp(cmdline, "w") == 0) {
        editorSave();
    }
    // Write As command: :w <filename>
    else if (strncmp(cmdline, "w ", 2) == 0) {
        char* new_filename = cmdline + 2;  // Point to filename after "w "
        // Basic validation: check if filename is non-empty
        // TODO: Trim whitespace from filename?
        if (strlen(new_filename) > 0) {
            // Update the editor's current filename
            strncpy(E.filename, new_filename, sizeof(E.filename) - 1);
            E.filename[sizeof(E.filename) - 1] = '\0';
            editorSave();  // Save to the newly specified filename
        } else {
            editorSetStatusMessage("ERR: Usage: :w <filename>");
        }
    }
    // Write and Quit command
    else if (strcmp(cmdline, "wq") == 0) {
        // Try saving first
        if (editorSave() == RESULT_OK) {
            E.quit_flag = 1;  // Quit only if save succeeded
        }
        // If save failed, error message is set by editorSave(), remain in editor
    }
    // Unknown command
    else {
        char msg[sizeof(cmdline) + 20];
        snprintf(msg, sizeof(msg), "ERR: Unknown command: %s", cmdline);
        editorSetStatusMessage(msg);
    }
}

// --- Input Processing (Main Key Switch) ---

// Processes the key read by editorReadKey() based on the current editor mode.
void editorProcessKeypress() {
    int c = editorReadKey();  // Read the next keypress (handles escapes)

    // Special case: handle terminal resize events if detected (e.g., SIGWINCH)
    // TODO: Add signal handler for SIGWINCH that sets a flag, check flag here.
    // If resized: getWindowSize(), adjust E.screenrows/cols, redraw screen.

    switch (E.mode) {
        // --- Normal Mode ---
        case MODE_NORMAL:
            switch (c) {
                case ':':  // Enter Command Mode
                    E.mode = MODE_COMMAND;
                    // Clear/init command buffer, status message set by redraw
                    bufclient_free(&E.cmdbuf);
                    bufclient_init(&E.cmdbuf);
                    editorSetStatusMessage(":");  // Initial prompt (redraw shows full)
                    break;

                case 'i':  // Enter Insert Mode
                    E.mode = MODE_INSERT;
                    editorSetStatusMessage("-- INSERT --");
                    break;

                // --- Movement Keys ---
                case 'h':
                case ARROW_LEFT:
                case 'j':
                case ARROW_DOWN:
                case 'k':
                case ARROW_UP:
                case 'l':
                case ARROW_RIGHT:
                case PAGE_UP:
                case PAGE_DOWN:
                case HOME_KEY:
                case END_KEY:
                    editorMoveCursor(c);
                    break;

                    // --- Deletion Keys (Basic) ---
                    // case 'x': // Delete character under cursor
                    //     editorMoveCursor(ARROW_RIGHT); // Move past char
                    //     bufclient_delete_char(&E.textbuf); // Delete char before cursor
                    //     break;
                    // case 'd': // Start delete command (e.g., 'dd' for line delete)
                    //     // TODO: Handle multi-key commands
                    //     break;

                case '\x1b':  // Escape key pressed while already in normal mode
                    // Could cancel pending command (like 'd') if implemented
                    break;

                    // Add other normal mode commands here (y, p, u, etc.)

                default:
                    // Ignore other keys in normal mode
                    break;
            }
            break;  // End MODE_NORMAL

        // --- Insert Mode ---
        case MODE_INSERT:
            switch (c) {
                case '\r':
                case '\n':  // Enter key: Insert newline character
                    bufclient_insert_char(&E.textbuf, '\n');
                    break;

                case '\x1b':  // Escape key: Return to Normal Mode
                    E.mode = MODE_NORMAL;
                    editorSetStatusMessage("");  // Clear "-- INSERT --" message
                    // Optional Vim behavior: move cursor left after exiting insert mode via ESC
                    editorMoveCursor(ARROW_LEFT);
                    break;

                case BACKSPACE:                         // Backspace key (mapped from 127 or potentially 8)
                    bufclient_delete_char(&E.textbuf);  // Delete char before cursor
                    break;

                case DEL_KEY:  // Delete key (delete char *under* cursor)
                    // Achieve this by moving right, then deleting backward
                    editorMoveCursor(ARROW_RIGHT);
                    if (bufclient_get_abs_pos(&E.textbuf) > 0) {  // Check if move right succeeded
                        bufclient_delete_char(&E.textbuf);
                    }
                    break;

                // Allow navigation keys within insert mode
                case ARROW_UP:
                case ARROW_DOWN:
                case ARROW_LEFT:
                case ARROW_RIGHT:
                case PAGE_UP:
                case PAGE_DOWN:
                case HOME_KEY:
                case END_KEY:
                    editorMoveCursor(c);
                    break;

                default:
                    // Insert regular character if it's printable (or potentially UTF-8 byte)
                    // Basic check: ASCII printable range or >= 128
                    if ((c >= 32 && c <= 126) || c >= 128) {
                        bufclient_insert_char(&E.textbuf, (char)c);
                        // Set modified flag here: E.modified_flag = 1;
                    }
                    // Ignore other control characters in insert mode
                    break;
            }
            break;  // End MODE_INSERT

        // --- Command Mode ---
        case MODE_COMMAND:
            switch (c) {
                case '\r':
                case '\n':  // Enter key: Execute the typed command
                    editorProcessCommand();
                    // Mode is usually reset to NORMAL within editorProcessCommand
                    break;

                case '\x1b':  // Escape key: Abort command, return to Normal Mode
                    E.mode = MODE_NORMAL;
                    editorSetStatusMessage("");  // Clear command prompt
                    break;

                case BACKSPACE:  // Backspace key: Delete last char in command buffer
                    bufclient_delete_char(&E.cmdbuf);
                    // Redraw will update the message bar display
                    break;

                case DEL_KEY:  // Delete key: Not standard in command line, ignore?
                    // Or implement forward delete if desired. Ignore for simplicity.
                    break;

                // Arrow keys in command line for history/editing? Ignore for simplicity.
                case ARROW_UP:
                case ARROW_DOWN:
                case ARROW_LEFT:
                case ARROW_RIGHT:
                    // Could implement command history or inline editing here
                    break;

                default:
                    // Insert printable character into command buffer
                    if (c >= 32 && c <= 126) {  // Allow basic ASCII printable
                        // Check if command buffer has space (prevent overflow)
                        if (E.cmdbuf.size < COMMAND_BUFFER_MAX) {
                            bufclient_insert_char(&E.cmdbuf, (char)c);
                            // Redraw will update the message bar display
                        }
                    }
                    // Ignore control characters, high-ASCII in command line?
                    break;
            }
            break;  // End MODE_COMMAND
    }
}

// --- Initialization and Main Loop ---

// Initializes the editor state, buffers, terminal settings.
void lkjsxceditor_init() {
    // Initialize editor state structure fields
    E.cx = 0;
    E.cy = 0;
    E.rx = 0;
    E.rowoff = 0;
    E.coloff = 0;
    E.filename[0] = '\0';   // No filename initially
    E.statusmsg[0] = '\0';  // No status message initially
    E.statusmsg_time = 0;
    E.mode = MODE_NORMAL;  // Start in Normal mode
    E.quit_flag = 0;       // Don't quit yet
    // E.modified_flag = 0; // Initialize modified flag
    // Zero out termios struct to indicate it hasn't been read yet
    memset(&E.orig_termios, 0, sizeof(E.orig_termios));

    bufchunk_pool_init();  // Initialize memory pool *before* initializing buffers

    // Initialize text and command buffers using the pool
    if (bufclient_init(&E.textbuf) != RESULT_OK) {
        die("lkjsxceditor: Failed to initialize text buffer (out of memory?)");
    }
    if (bufclient_init(&E.cmdbuf) != RESULT_OK) {
        die("lkjsxceditor: Failed to initialize command buffer (out of memory?)");
    }

    // Get initial terminal window size
    if (getWindowSize(&E.screenrows, &E.screencols) == -1)
        die("getWindowSize failed");
    // Reserve bottom two rows for status bar and message bar
    if (E.screenrows > 2) {
        E.screenrows -= 2;
    } else {
        // Terminal too small to operate reasonably
        die("lkjsxceditor: Terminal too small (needs at least 3 rows)");
    }

    enableRawMode();  // Switch terminal to raw mode
}

// Cleans up resources before exiting: restores terminal settings.
void lkjsxceditor_cleanup() {
    // Free buffer chunks? Not strictly required with static pool, but good practice.
    // bufclient_free(&E.textbuf);
    // bufclient_free(&E.cmdbuf);
    // Chunks are returned to the static pool automatically on exit anyway.

    // Clear the screen and position cursor at top-left for clean exit
    write(STDOUT_FILENO, "\x1b[2J", 4);  // Clear screen
    write(STDOUT_FILENO, "\x1b[H", 3);   // Move cursor home

    disableRawMode();  // Restore original terminal settings
}

// --- Main Function ---
int main(int argc, char* argv[]) {
    // Initialize editor state, terminal, buffers
    lkjsxceditor_init();

    // Open file from command line argument if provided
    if (argc >= 2) {
        if (editorOpen(argv[1]) != RESULT_OK) {
            // Error opening file, but editor state is initialized, so continue
            // with an empty buffer. Error message is already set.
        }
    } else {
        // No file provided, show welcome/help message
        editorSetStatusMessage("lkjsxceditor | Press :q to quit");
    }

    // Main editor loop: continues until quit_flag is set
    while (E.quit_flag == 0) {
        editorRefreshScreen();    // Update the display based on current state
        editorProcessKeypress();  // Wait for and process user input
    }

    // Cleanup resources (restore terminal) before exiting
    lkjsxceditor_cleanup();
    return 0;  // Exit successfully
}