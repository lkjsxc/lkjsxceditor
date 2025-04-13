#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>  // for NULL, size_t
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

// *** Defines ***
#define LKJSXCEDITOR_VERSION "0.0.1"
#define BUFCHUNK_SIZE 512      // Size of each text chunk
#define BUFCHUNK_COUNT 32768   // Number of chunks (32768 * 512 = 16MB for text)
#define SCREEN_BUF_SIZE 65536  // Buffer for screen rendering (64KB)
#define FILE_BUF_SIZE 4096     // Buffer for file I/O (4KB)
#define STATUS_BUF_SIZE 128    // Buffer for status messages
#define CMD_BUF_SIZE 128       // Max command length
#define TAB_STOP 8
#define QUIT_TIMES 1  // Require 1 confirmation to quit if modified

// *** Enums ***
enum RESULT {
    RESULT_OK,
    RESULT_ERR
};

enum editorMode {
    MODE_NORMAL,
    MODE_INSERT,
    MODE_COMMAND
};

// Custom key codes for non-ASCII keys
enum editorKey {
    KEY_NULL = 0,       // Null key
    BACKSPACE = 127,    // ASCII backspace
    ARROW_LEFT = 1000,  // Assign arbitrary codes > 255
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    DEL_KEY,
    HOME_KEY,
    END_KEY,
    PAGE_UP,
    PAGE_DOWN
};

// *** Structs ***
struct bufchunk {
    char data[BUFCHUNK_SIZE];
    struct bufchunk* prev;
    struct bufchunk* next;
    int size;  // Number of bytes used in data
};

struct bufclient {
    struct bufchunk* begin;         // First chunk
    struct bufchunk* rbegin;        // Last chunk (reverse begin)
    struct bufchunk* cursor_chunk;  // Chunk where the cursor is located
    int cursor_rel_i;               // Relative index within cursor_chunk
    int cursor_abs_i;               // Absolute index from the start of the buffer
    int cursor_abs_y;               // Absolute line number (0-based)
    int cursor_abs_x;               // Calculated visual column number on the line (0-based, handles tabs)
    int cursor_goal_x;              // Desired visual X position when moving vertically
    int size;                       // Total size of the buffer in bytes
    char filename[256];             // Associated filename
    int dirty;                      // 1 if modified since last save, 0 otherwise
    // Display offsets
    int rowoff;  // First visible row (line number)
    int coloff;  // First visible visual column
    // Cache for finding start of rowoff quickly (can be NULL if invalid)
    struct bufchunk* rowoff_chunk;  // Chunk containing the start of the first visible row
    int rowoff_rel_i;               // Relative index within rowoff_chunk
    int rowoff_abs_i;               // Absolute index for start of rowoff
};

// *** Global Variables ***
static int screenrows;                     // Terminal height (text area)
static int screencols;                     // Terminal width
static struct termios orig_termios;        // Original terminal settings
static volatile int terminate_editor = 0;  // Flag to signal exit from main loop
static enum editorMode mode = MODE_NORMAL;
static struct bufclient textbuf;   // Main text buffer
static char cmdbuf[CMD_BUF_SIZE];  // Command line buffer
static int cmdbuf_len = 0;
static char statusbuf[STATUS_BUF_SIZE];  // Status message buffer
static time_t statusbuf_time = 0;        // Timestamp for status message display

// Buffer Chunk Pool
static struct bufchunk bufchunk_pool_data[BUFCHUNK_COUNT];
static struct bufchunk* bufchunk_pool_free = NULL;
static int bufchunk_pool_used = 0;

// Screen buffer (using a static buffer instead of dynamic abuf)
static char screenbuf[SCREEN_BUF_SIZE];
static int screenbuf_len = 0;

// *** Function Prototypes ***

// Core Utils
void die(const char* s);
void editorSetStatusMessage(const char* msg);

// Buffer Chunk Pool
void bufchunk_pool_init();
struct bufchunk* bufchunk_alloc();
void bufchunk_free(struct bufchunk* chunk);

// Buffer Client Helpers
enum RESULT bufclient_find_pos(struct bufclient* buf, int target_abs_i, struct bufchunk** chunk_out, int* rel_i_out);
enum RESULT bufclient_find_line_start(struct bufclient* buf, int target_abs_y, struct bufchunk** chunk_out, int* rel_i_out, int* start_abs_i_out);
enum RESULT bufclient_update_cursor_coords(struct bufclient* buf);  // Update abs_x, abs_y from abs_i

// Buffer Client API
enum RESULT bufclient_init(struct bufclient* buf);
void bufclient_free(struct bufclient* buf);
enum RESULT bufclient_insert_char(struct bufclient* buf, char c);
enum RESULT bufclient_delete_char(struct bufclient* buf);  // Deletes char *before* cursor
void bufclient_move_cursor_to(struct bufclient* buf, int target_abs_i);
void bufclient_move_cursor_relative(struct bufclient* buf, int key);  // Uses enum editorKey
void bufclient_clear(struct bufclient* buf);

// Terminal Handling
void disableRawMode();
enum RESULT enableRawMode();
enum RESULT getWindowSize(int* rows, int* cols);
enum editorKey editorReadKey();

// Output / Rendering
void screenbuf_append(const char* s, int len);
void screenbuf_clear();
void editorScroll();
void editorDrawRows();
void editorDrawStatusBar();
void editorDrawCommandLine();
void editorRefreshScreen();
int calculate_visual_x(struct bufclient* buf, int target_abs_y, int target_abs_i);

// File I/O
enum RESULT editorOpen(const char* filename);
enum RESULT editorSave();

// Editor Operations
void initEditor();
void editorProcessCommand();
void editorProcessKeypress();

// *** Buffer Chunk Pool Implementation ***
void bufchunk_pool_init() {
    int i;
    // Link all chunks into the free list
    for (i = 0; i < BUFCHUNK_COUNT - 1; i++) {
        bufchunk_pool_data[i].next = &bufchunk_pool_data[i + 1];
    }
    bufchunk_pool_data[BUFCHUNK_COUNT - 1].next = NULL;
    bufchunk_pool_free = &bufchunk_pool_data[0];
    bufchunk_pool_used = 0;
}

struct bufchunk* bufchunk_alloc() {
    if (bufchunk_pool_free == NULL) {
        return NULL;  // Pool exhausted
    }
    struct bufchunk* chunk = bufchunk_pool_free;
    bufchunk_pool_free = chunk->next;

    // Initialize the allocated chunk
    chunk->prev = NULL;
    chunk->next = NULL;
    chunk->size = 0;
    // chunk->data is uninitialized
    bufchunk_pool_used++;
    return chunk;
}

void bufchunk_free(struct bufchunk* chunk) {
    if (chunk == NULL)
        return;
    // Add chunk back to the head of the free list
    chunk->next = bufchunk_pool_free;
    bufchunk_pool_free = chunk;
    bufchunk_pool_used--;
}

// *** Buffer Client Helper Implementation ***

// Find chunk and relative index for a given absolute index. (Inefficient linear scan)
enum RESULT bufclient_find_pos(struct bufclient* buf, int target_abs_i, struct bufchunk** chunk_out, int* rel_i_out) {
    if (target_abs_i < 0 || target_abs_i > buf->size) {
        return RESULT_ERR;
    }
    // Optimization: Check if target is near current cursor? Maybe later.

    // Handle edge case: position at the very end of the buffer
    if (target_abs_i == buf->size) {
        *chunk_out = buf->rbegin;
        *rel_i_out = buf->rbegin ? buf->rbegin->size : 0;
        return RESULT_OK;
    }

    struct bufchunk* current_chunk = buf->begin;
    int current_abs_base = 0;  // Absolute index at the start of current_chunk
    while (current_chunk != NULL) {
        // Check if target_abs_i falls within this chunk
        if (target_abs_i >= current_abs_base && target_abs_i < current_abs_base + current_chunk->size) {
            *chunk_out = current_chunk;
            *rel_i_out = target_abs_i - current_abs_base;
            return RESULT_OK;
        }
        current_abs_base += current_chunk->size;
        current_chunk = current_chunk->next;
    }
    // Should be unreachable if target_abs_i <= buf->size and buffer not empty
    return RESULT_ERR;
}

// Find the chunk/offset and absolute index for the start of a given line. (Inefficient linear scan)
enum RESULT bufclient_find_line_start(struct bufclient* buf, int target_abs_y, struct bufchunk** chunk_out, int* rel_i_out, int* start_abs_i_out) {
    struct bufchunk* current_chunk = buf->begin;
    int current_rel_i = 0;
    int current_abs_i = 0;
    int current_abs_y = 0;

    // Default to start of buffer for line 0
    *chunk_out = buf->begin;
    *rel_i_out = 0;
    *start_abs_i_out = 0;

    if (target_abs_y == 0) {
        return RESULT_OK;  // Line 0 starts at the beginning
    }

    while (current_chunk != NULL) {
        while (current_rel_i < current_chunk->size) {
            if (current_chunk->data[current_rel_i] == '\n') {
                current_abs_y++;
                if (current_abs_y == target_abs_y) {
                    // Found start of target line (position after '\n')
                    *start_abs_i_out = current_abs_i + current_rel_i + 1;

                    // Determine the chunk and relative index for this start position
                    if (current_rel_i + 1 < current_chunk->size) {
                        *chunk_out = current_chunk;
                        *rel_i_out = current_rel_i + 1;
                    } else if (current_chunk->next != NULL) {
                        // Start of line is at the beginning of the next chunk
                        *chunk_out = current_chunk->next;
                        *rel_i_out = 0;
                    } else {
                        // This newline was the very last character of the buffer
                        *chunk_out = current_chunk;        // Still technically in this chunk
                        *rel_i_out = current_chunk->size;  // Positioned at the end
                    }
                    return RESULT_OK;
                }
            }
            current_rel_i++;
        }
        // Finished scanning this chunk, move to the next
        current_abs_i += current_chunk->size;
        current_chunk = current_chunk->next;
        current_rel_i = 0;  // Reset relative index for the new chunk
    }

    // If we finished scanning and haven't found target_abs_y, it's beyond the buffer content
    if (target_abs_y > current_abs_y) {
        *chunk_out = buf->rbegin;  // End of buffer
        *rel_i_out = buf->rbegin ? buf->rbegin->size : 0;
        *start_abs_i_out = buf->size;
        return RESULT_ERR;  // Line not found
    }

    // Should be unreachable if target_abs_y <= current_abs_y
    return RESULT_ERR;
}

// Update cursor_abs_y and cursor_abs_x based on cursor_abs_i. (Inefficient linear scan)
enum RESULT bufclient_update_cursor_coords(struct bufclient* buf) {
    struct bufchunk* current_chunk = buf->begin;
    int current_rel_i = 0;
    int current_abs_i = 0;
    int y = 0;
    int x = 0;                 // Visual column
    int line_start_abs_i = 0;  // Track start of current line for tab calculation

    // Find the line containing the cursor and calculate visual X
    while (current_chunk != NULL && current_abs_i <= buf->cursor_abs_i) {
        while (current_rel_i < current_chunk->size && current_abs_i <= buf->cursor_abs_i) {
            // If we reached the cursor position, we are done calculating coords
            if (current_abs_i == buf->cursor_abs_i) {
                buf->cursor_abs_y = y;
                buf->cursor_abs_x = calculate_visual_x(buf, y, buf->cursor_abs_i);
                return RESULT_OK;
            }

            if (current_chunk->data[current_rel_i] == '\n') {
                y++;
                line_start_abs_i = current_abs_i + 1;  // Next char starts a new line
            }

            current_rel_i++;
            current_abs_i++;
        }
        // Move to next chunk if needed
        current_chunk = current_chunk->next;
        current_rel_i = 0;
    }

    // If cursor_abs_i == buf->size (at the very end)
    if (buf->cursor_abs_i == buf->size) {
        buf->cursor_abs_y = y;
        buf->cursor_abs_x = calculate_visual_x(buf, y, buf->cursor_abs_i);
        return RESULT_OK;
    }

    // Cursor position not found? Should not happen if cursor_abs_i is valid.
    return RESULT_ERR;
}

// Helper to calculate visual X column for a given absolute index on a given line
// (Inefficient: requires scan from line start)
int calculate_visual_x(struct bufclient* buf, int target_abs_y, int target_abs_i) {
    struct bufchunk* line_chunk;
    int line_rel_i, line_start_abs_i;
    int visual_x = 0;

    if (bufclient_find_line_start(buf, target_abs_y, &line_chunk, &line_rel_i, &line_start_abs_i) != RESULT_OK) {
        return 0;  // Error finding line start
    }

    struct bufchunk* current_chunk = line_chunk;
    int current_rel_i = line_rel_i;
    int current_abs_i = line_start_abs_i;

    while (current_chunk != NULL && current_abs_i < target_abs_i) {
        while (current_rel_i < current_chunk->size && current_abs_i < target_abs_i) {
            char c = current_chunk->data[current_rel_i];
            if (c == '\n') {
                // Should not happen if target_abs_i is on target_abs_y
                return visual_x;  // Reached end of line before target_abs_i
            } else if (c == '\t') {
                visual_x += (TAB_STOP - (visual_x % TAB_STOP));
            } else if (iscntrl((unsigned char)c)) {
                visual_x += 2;  // Assume ^X representation takes 2 columns
            } else {
                visual_x += 1;  // Regular printable character
            }
            current_rel_i++;
            current_abs_i++;
        }
        current_chunk = current_chunk->next;
        current_rel_i = 0;
    }
    return visual_x;
}

// *** Buffer Client API Implementation ***
enum RESULT bufclient_init(struct bufclient* buf) {
    memset(buf, 0, sizeof(struct bufclient));  // Zero out the structure first
    buf->begin = bufchunk_alloc();
    if (buf->begin == NULL) {
        return RESULT_ERR;  // Out of memory
    }
    buf->rbegin = buf->begin;
    buf->cursor_chunk = buf->begin;
    buf->rowoff_chunk = buf->begin;  // Cache starts valid at beginning
    // Other fields are initialized to 0 by memset
    return RESULT_OK;
}

void bufclient_free(struct bufclient* buf) {
    struct bufchunk* current = buf->begin;
    struct bufchunk* next;
    while (current != NULL) {
        next = current->next;
        bufchunk_free(current);
        current = next;
    }
    // Reset bufclient structure fields
    memset(buf, 0, sizeof(struct bufclient));
}

void bufclient_clear(struct bufclient* buf) {
    char old_filename[sizeof(buf->filename)];
    strncpy(old_filename, buf->filename, sizeof(old_filename));
    bufclient_free(buf);
    bufclient_init(buf);                                          // Re-initialize to a single empty chunk
    strncpy(buf->filename, old_filename, sizeof(buf->filename));  // Restore filename
    buf->dirty = 1;                                               // Clearing makes it dirty unless it was already empty
}

enum RESULT bufclient_insert_char(struct bufclient* buf, char c) {
    // Find the effective insertion chunk/offset (handles cursor at end of chunk)
    struct bufchunk* insert_chunk = buf->cursor_chunk;
    int insert_rel_i = buf->cursor_rel_i;

    // If cursor is exactly at the end of a non-last chunk, insertion logically happens
    // at the start of the next chunk. This simplifies splitting logic.
    if (insert_rel_i == insert_chunk->size && insert_chunk->next != NULL) {
        insert_chunk = insert_chunk->next;
        insert_rel_i = 0;
    }

    // Invalidate rowoff cache if inserting before its position
    if (buf->rowoff_chunk && buf->cursor_abs_i < buf->rowoff_abs_i) {
        buf->rowoff_chunk = NULL;
    }

    if (insert_chunk->size < BUFCHUNK_SIZE) {
        // Case 1: Current chunk has space
        if (insert_rel_i < insert_chunk->size) {  // Shift data only if inserting mid-chunk
            memmove(insert_chunk->data + insert_rel_i + 1, insert_chunk->data + insert_rel_i, insert_chunk->size - insert_rel_i);
        }
        insert_chunk->data[insert_rel_i] = c;
        insert_chunk->size++;

        // Update cursor position: stays logically after the inserted char
        buf->cursor_chunk = insert_chunk;
        buf->cursor_rel_i = insert_rel_i + 1;

    } else {
        // Case 2: Target chunk is full, need to split/allocate new chunk *after* it
        struct bufchunk* new_chunk = bufchunk_alloc();
        if (new_chunk == NULL) {
            editorSetStatusMessage("Out of memory!");
            return RESULT_ERR;
        }

        // Link new_chunk *after* the full insert_chunk
        new_chunk->next = insert_chunk->next;
        new_chunk->prev = insert_chunk;
        if (insert_chunk->next != NULL) {
            insert_chunk->next->prev = new_chunk;
        } else {
            buf->rbegin = new_chunk;  // New chunk is now the last one
        }
        insert_chunk->next = new_chunk;

        // Move data from insertion point onwards to new_chunk
        int move_len = insert_chunk->size - insert_rel_i;
        if (move_len > 0) {
            memcpy(new_chunk->data, insert_chunk->data + insert_rel_i, move_len);
            new_chunk->size = move_len;
            insert_chunk->size = insert_rel_i;  // Truncate current chunk
        } else {
            // Inserting exactly at the end of the full chunk. Nothing to move.
            new_chunk->size = 0;
        }

        // Insert the new character into the *original* chunk (which now has 1 byte free at the end)
        // OR into the *new* chunk if insertion point was at the end.
        if (insert_rel_i == BUFCHUNK_SIZE) {  // Inserted at the very end, goes into new chunk
            new_chunk->data[0] = c;
            new_chunk->size++;
            // Move data originally copied to new_chunk one position over
            memmove(new_chunk->data + 1, new_chunk->data, new_chunk->size - 1);  // size already incremented

            // Cursor goes to start of new_chunk + 1
            buf->cursor_chunk = new_chunk;
            buf->cursor_rel_i = 1;

        } else {  // Inserted mid-chunk, goes into original chunk (after split)
            insert_chunk->data[insert_rel_i] = c;
            insert_chunk->size++;
            // Cursor stays in original chunk, after inserted char
            buf->cursor_chunk = insert_chunk;
            buf->cursor_rel_i = insert_rel_i + 1;
        }
    }

    buf->size++;
    buf->cursor_abs_i++;
    buf->dirty = 1;

    // Update abs_y, abs_x (expensive - full recalculation needed for accuracy)
    // bufclient_update_cursor_coords(buf); // Too slow for every insert
    // Simple update (less accurate but faster):
    if (c == '\n') {
        buf->cursor_abs_y++;
        buf->cursor_abs_x = 0;
    } else {
        // Recalculate visual X based on the new character
        buf->cursor_abs_x = calculate_visual_x(buf, buf->cursor_abs_y, buf->cursor_abs_i);
    }
    buf->cursor_goal_x = buf->cursor_abs_x;  // Update goal x on horizontal move/insert

    return RESULT_OK;
}

enum RESULT bufclient_delete_char(struct bufclient* buf) {  // Deletes char *before* cursor
    if (buf->cursor_abs_i == 0) {
        return RESULT_OK;  // Nothing to delete at the beginning
    }

    // Find the position *before* the cursor
    int del_abs_i = buf->cursor_abs_i - 1;
    struct bufchunk* del_chunk;
    int del_rel_i;

    if (bufclient_find_pos(buf, del_abs_i, &del_chunk, &del_rel_i) != RESULT_OK) {
        editorSetStatusMessage("Error finding delete position!");
        return RESULT_ERR;  // Should not happen if abs_i > 0
    }

    // Invalidate rowoff cache if deleting before its position
    if (buf->rowoff_chunk && del_abs_i < buf->rowoff_abs_i) {
        buf->rowoff_chunk = NULL;
    }

    char deleted_char = del_chunk->data[del_rel_i];

    // Shift data within the chunk to overwrite the deleted character
    memmove(del_chunk->data + del_rel_i, del_chunk->data + del_rel_i + 1, del_chunk->size - del_rel_i - 1);
    del_chunk->size--;
    buf->size--;
    buf->dirty = 1;

    // Update cursor position (moves one step back logically)
    buf->cursor_abs_i--;
    // Find the chunk/rel_i for the *new* cursor position. It's the same as the deletion position.
    buf->cursor_chunk = del_chunk;
    buf->cursor_rel_i = del_rel_i;

    // Recalculate abs_y, abs_x (expensive)
    bufclient_update_cursor_coords(buf);
    buf->cursor_goal_x = buf->cursor_abs_x;

    // --- Chunk Merging ---
    // Condition 1: Check if deleting the character emptied the chunk (and it wasn't the only chunk)
    if (del_chunk->size == 0 && del_chunk != buf->begin) {
        struct bufchunk* prev_chunk = del_chunk->prev;
        struct bufchunk* next_chunk = del_chunk->next;  // Can be NULL

        // Unlink the empty chunk
        prev_chunk->next = next_chunk;
        if (next_chunk != NULL) {
            next_chunk->prev = prev_chunk;
        } else {
            buf->rbegin = prev_chunk;  // prev_chunk is now the last one
        }

        // Cursor must have been at the start of the (now deleted) chunk,
        // so move it to the end of the previous chunk.
        buf->cursor_chunk = prev_chunk;
        buf->cursor_rel_i = prev_chunk->size;

        bufchunk_free(del_chunk);  // Free the empty chunk

        // After removing a chunk, try merging prev and next if possible
        del_chunk = prev_chunk;  // Point to the chunk before the freed one for potential next merge
        // Intentional fallthrough to check merge with next? No, handle separately.
    }

    // Condition 2: Check if we can merge del_chunk with the *next* chunk
    // This is useful if del_chunk is now small, OR if we deleted a newline
    // that previously separated del_chunk and next_chunk.
    if (del_chunk->next != NULL) {
        struct bufchunk* next_chunk = del_chunk->next;
        // Merge if combined size fits and either del_chunk is small or '\n' was deleted at boundary
        int should_merge = 0;
        if (del_chunk->size + next_chunk->size <= BUFCHUNK_SIZE) {
            // Always merge if combined fits? Or only if one is small? Let's merge if fits.
            should_merge = 1;
        }

        if (should_merge) {
            // Append next_chunk's data to del_chunk
            memcpy(del_chunk->data + del_chunk->size, next_chunk->data, next_chunk->size);

            // Update size and links
            del_chunk->size += next_chunk->size;
            del_chunk->next = next_chunk->next;
            if (next_chunk->next != NULL) {
                next_chunk->next->prev = del_chunk;
            } else {
                buf->rbegin = del_chunk;  // del_chunk is now the last chunk
            }

            // Cursor adjustment if it was in the merged (freed) chunk is not needed
            // because deletion happens *before* the cursor. If the cursor was in
            // next_chunk before deletion, it would have moved back into del_chunk
            // during the initial delete step.

            bufchunk_free(next_chunk);
        }
    }

    return RESULT_OK;
}

// Move cursor to a specific absolute index
void bufclient_move_cursor_to(struct bufclient* buf, int target_abs_i) {
    // Clamp target index within valid buffer range
    if (target_abs_i < 0)
        target_abs_i = 0;
    if (target_abs_i > buf->size)
        target_abs_i = buf->size;

    // Find the chunk and relative position for the target index
    if (bufclient_find_pos(buf, target_abs_i, &buf->cursor_chunk, &buf->cursor_rel_i) == RESULT_OK) {
        buf->cursor_abs_i = target_abs_i;
        // Update visual coordinates (inefficiently but accurately)
        bufclient_update_cursor_coords(buf);
        // Set goal x when explicitly moving cursor
        buf->cursor_goal_x = buf->cursor_abs_x;
    }
    // Handle error? Maybe set cursor to nearest valid pos? For now, it might fail silently.
}

// Move cursor based on ARROW_UP, DOWN, LEFT, RIGHT keys
void bufclient_move_cursor_relative(struct bufclient* buf, int key) {
    int current_abs_i = buf->cursor_abs_i;
    int target_abs_i = current_abs_i;
    struct bufchunk* current_chunk = buf->cursor_chunk;  // Use current chunk as starting point?
    int current_rel_i = buf->cursor_rel_i;

    switch (key) {
        case ARROW_LEFT:
            if (target_abs_i > 0) {
                target_abs_i--;
                // Update chunk/rel_i efficiently if possible
                if (current_rel_i > 0) {
                    current_rel_i--;
                } else if (current_chunk->prev != NULL) {
                    current_chunk = current_chunk->prev;
                    current_rel_i = current_chunk->size - 1;
                } else {
                    // At start of buffer, find_pos needed if logic fails
                    if (bufclient_find_pos(buf, target_abs_i, &current_chunk, &current_rel_i) != RESULT_OK) {
                        return;  // Error finding position
                    }
                }
            } else {
                return;
            }  // Already at start
            break;
        case ARROW_RIGHT:
            if (target_abs_i < buf->size) {
                target_abs_i++;
                // Update chunk/rel_i efficiently
                if (current_rel_i < current_chunk->size) {
                    current_rel_i++;
                } else if (current_chunk->next != NULL) {
                    current_chunk = current_chunk->next;
                    current_rel_i = (target_abs_i == buf->size) ? current_chunk->size : 1;  // Move to first char or end
                                                                                            // If moving right into the next chunk, rel_i should be 0 if target != size
                    if (target_abs_i != buf->size)
                        current_rel_i = 0;

                } else {
                    // At end of buffer, find_pos needed
                    if (bufclient_find_pos(buf, target_abs_i, &current_chunk, &current_rel_i) != RESULT_OK) {
                        return;  // Error finding position
                    }
                }
            } else {
                return;
            }  // Already at end
            break;
        case ARROW_UP: {
            if (buf->cursor_abs_y == 0)
                return;  // Already on first line

            // Find start of current line to know current X offset
            struct bufchunk* line_start_chunk;
            int line_start_rel_i, line_start_abs_i;
            if (bufclient_find_line_start(buf, buf->cursor_abs_y, &line_start_chunk, &line_start_rel_i, &line_start_abs_i) != RESULT_OK)
                return;

            // Find start of previous line
            struct bufchunk* prev_line_start_chunk;
            int prev_line_start_rel_i, prev_line_start_abs_i;
            if (bufclient_find_line_start(buf, buf->cursor_abs_y - 1, &prev_line_start_chunk, &prev_line_start_rel_i, &prev_line_start_abs_i) != RESULT_OK)
                return;

            // Iterate from start of prev line to find position matching goal_x
            int visual_x = 0;
            target_abs_i = prev_line_start_abs_i;  // Start searching from beginning of prev line
            current_chunk = prev_line_start_chunk;
            current_rel_i = prev_line_start_rel_i;

            while (current_chunk != NULL) {
                while (current_rel_i < current_chunk->size) {
                    char c = current_chunk->data[current_rel_i];
                    if (c == '\n') {
                        goto end_prev_line_search;  // Reached end of line before goal_x
                    }

                    int char_width = 0;
                    if (c == '\t') {
                        char_width = (TAB_STOP - (visual_x % TAB_STOP));
                    } else if (iscntrl((unsigned char)c)) {
                        char_width = 2;
                    } else {
                        char_width = 1;
                    }

                    // If adding this character *exceeds* goal_x, stop *before* it
                    if (visual_x + char_width > buf->cursor_goal_x) {
                        goto end_prev_line_search;
                    }
                    visual_x += char_width;
                    target_abs_i++;  // Advance target index
                    current_rel_i++;

                    // If we exactly hit the goal_x, stop *after* this char
                    if (visual_x == buf->cursor_goal_x) {
                        goto end_prev_line_search;
                    }
                }
                // Move to next chunk on the line
                current_chunk = current_chunk->next;
                current_rel_i = 0;
            }
        end_prev_line_search:;
            // Target abs_i now points to the closest position on the previous line
        } break;
        case ARROW_DOWN: {
            // Find start of next line
            struct bufchunk* next_line_start_chunk;
            int next_line_start_rel_i, next_line_start_abs_i;

            if (bufclient_find_line_start(buf, buf->cursor_abs_y + 1, &next_line_start_chunk, &next_line_start_rel_i, &next_line_start_abs_i) == RESULT_OK) {
                // Iterate from start of next line to find goal_x position
                int visual_x = 0;
                target_abs_i = next_line_start_abs_i;
                current_chunk = next_line_start_chunk;
                current_rel_i = next_line_start_rel_i;

                while (current_chunk != NULL) {
                    while (current_rel_i < current_chunk->size) {
                        char c = current_chunk->data[current_rel_i];
                        if (c == '\n') {
                            goto end_next_line_search;
                        }

                        int char_width = 0;
                        if (c == '\t') {
                            char_width = (TAB_STOP - (visual_x % TAB_STOP));
                        } else if (iscntrl((unsigned char)c)) {
                            char_width = 2;
                        } else {
                            char_width = 1;
                        }

                        if (visual_x + char_width > buf->cursor_goal_x) {
                            goto end_next_line_search;
                        }
                        visual_x += char_width;
                        target_abs_i++;
                        current_rel_i++;
                        if (visual_x == buf->cursor_goal_x) {
                            goto end_next_line_search;
                        }
                    }
                    current_chunk = current_chunk->next;
                    current_rel_i = 0;
                }
            end_next_line_search:;

                // Ensure target_abs_i does not exceed buffer size (can happen if last line is short)
                if (target_abs_i > buf->size)
                    target_abs_i = buf->size;

            } else {
                // Already on the last line (or line after last), don't move down
                return;
            }
        } break;
        default:  // Should not happen if called with arrow keys
            return;
    }

    // Move cursor to the calculated target_abs_i using the efficient chunk/rel_i if possible
    if (target_abs_i != buf->cursor_abs_i) {
        buf->cursor_abs_i = target_abs_i;
        buf->cursor_chunk = current_chunk;  // Use the efficiently tracked chunk
        buf->cursor_rel_i = current_rel_i;  // Use the efficiently tracked rel_i

        // Update visual coordinates (inefficiently but accurately)
        bufclient_update_cursor_coords(buf);
        // Only update goal_x on horizontal movement
        if (key == ARROW_LEFT || key == ARROW_RIGHT) {
            buf->cursor_goal_x = buf->cursor_abs_x;
        }
    }
}

// *** Terminal Handling Implementation ***
void die(const char* s) {
    // Try to clear screen and restore terminal before exiting
    write(STDOUT_FILENO, "\x1b[2J", 4);  // Clear screen
    write(STDOUT_FILENO, "\x1b[H", 3);   // Move cursor to top-left
    disableRawMode();
    perror(s);  // Print error message related to errno
    _exit(1);   // Use _exit to avoid stdio flushing or atexit handlers
}

void disableRawMode() {
    // Restore original terminal settings if termios was successfully read
    if (orig_termios.c_lflag != 0) {  // Basic check if termios was initialized
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    }
}

enum RESULT enableRawMode() {
    if (!isatty(STDIN_FILENO)) {  // Check if stdin is a terminal
        errno = ENOTTY;
        return RESULT_ERR;
    }
    if (tcgetattr(STDIN_FILENO, &orig_termios) == -1)
        return RESULT_ERR;

    struct termios raw = orig_termios;
    // Input flags: disable Break signal, CR-to-NL translation, Parity check, Input stripping, SW flow control
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    // Output flags: disable all output processing
    raw.c_oflag &= ~(OPOST);
    // Control flags: set character size to 8 bits/byte
    raw.c_cflag |= (CS8);
    // Local flags: disable Echoing, Canonical mode, Signal chars (Ctrl-C, Ctrl-Z), Extended input processing
    raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
    // Control characters: Set timeout for read()
    raw.c_cc[VMIN] = 0;   // read() returns as soon as data is available or timeout expires
    raw.c_cc[VTIME] = 1;  // 100ms timeout

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
        return RESULT_ERR;
    return RESULT_OK;
}

// Get terminal window size using ioctl or fallback escape codes
enum RESULT getWindowSize(int* rows, int* cols) {
    struct winsize ws;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        // Fallback: Move cursor far right/down and query position
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12)
            return RESULT_ERR;
        // Send Device Status Report (DSR) query for cursor position
        if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4)
            return RESULT_ERR;

        // Read response: Esc [ rows ; cols R
        char buf[32];
        unsigned int i = 0;
        // Read char by char until 'R' or buffer full/timeout
        while (i < sizeof(buf) - 1) {
            // Use read with timeout (set by enableRawMode)
            if (read(STDIN_FILENO, &buf[i], 1) != 1)
                break;
            if (buf[i] == 'R')
                break;
            i++;
        }
        buf[i] = '\0';  // Null-terminate

        // Check if response starts correctly
        if (i < 4 || buf[0] != '\x1b' || buf[1] != '[')
            return RESULT_ERR;

        // Parse rows and columns (simple atoi replacement)
        int r = 0, c = 0;
        char* ptr = &buf[2];
        char* semicolon = strchr(ptr, ';');
        if (!semicolon || semicolon == ptr)
            return RESULT_ERR;
        *semicolon = '\0';  // Split string at semicolon

        // Parse rows
        while (*ptr) {
            if (!isdigit((unsigned char)*ptr))
                return RESULT_ERR;
            r = r * 10 + (*ptr - '0');
            ptr++;
        }
        // Parse columns
        ptr = semicolon + 1;
        while (*ptr && *ptr != 'R') {  // Stop at 'R'
            if (!isdigit((unsigned char)*ptr))
                return RESULT_ERR;
            c = c * 10 + (*ptr - '0');
            ptr++;
        }

        if (r == 0 || c == 0)
            return RESULT_ERR;  // Invalid dimensions
        *rows = r;
        *cols = c;
    } else {
        // ioctl succeeded
        *cols = ws.ws_col;
        *rows = ws.ws_row;
    }

    if (*cols <= 0 || *rows <= 0)
        return RESULT_ERR;
    return RESULT_OK;
}

// Read a key, handling escape sequences for arrows, home, end etc.
enum editorKey editorReadKey() {
    int nread;
    char c;
    // Loop until a key is read or an error occurs (excluding timeout)
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        if (nread == -1 && errno != EAGAIN)
            die("read");
        // Handle signals or other non-EAGAIN errors if necessary
    }

    if (c == '\x1b') {  // Potential escape sequence
        char seq[3];
        // Check for immediate escape key press (no following sequence)
        if (read(STDIN_FILENO, &seq[0], 1) != 1)
            return '\x1b';  // Just ESC

        // Check for common sequence start '[' or 'O'
        if (seq[0] == '[') {
            if (read(STDIN_FILENO, &seq[1], 1) != 1)
                return '\x1b';  // Incomplete sequence

            if (seq[1] >= '0' && seq[1] <= '9') {
                // Extended sequence like Home, End, Del, PageUp/Down (e.g., Esc[3~)
                if (read(STDIN_FILENO, &seq[2], 1) != 1)
                    return '\x1b';  // Incomplete
                if (seq[2] == '~') {
                    switch (seq[1]) {
                        case '1':
                            return HOME_KEY;  // Often ^[[1~
                        case '3':
                            return DEL_KEY;  // Often ^[[3~
                        case '4':
                            return END_KEY;  // Often ^[[4~
                        case '5':
                            return PAGE_UP;
                        case '6':
                            return PAGE_DOWN;
                        case '7':
                            return HOME_KEY;  // Sometimes ^[[7~
                        case '8':
                            return END_KEY;  // Sometimes ^[[8~
                        default:
                            return '\x1b';  // Unrecognized number
                    }
                } else {
                    return '\x1b';
                }  // Malformed sequence
            } else {
                // Standard CSI sequences (e.g., arrow keys)
                switch (seq[1]) {
                    case 'A':
                        return ARROW_UP;  // Esc[A
                    case 'B':
                        return ARROW_DOWN;  // Esc[B
                    case 'C':
                        return ARROW_RIGHT;  // Esc[C
                    case 'D':
                        return ARROW_LEFT;  // Esc[D
                    case 'H':
                        return HOME_KEY;  // Sometimes Esc[H
                    case 'F':
                        return END_KEY;  // Sometimes Esc[F
                    default:
                        return '\x1b';  // Unrecognized letter
                }
            }
        } else if (seq[0] == 'O') {
            // Alternate sequences (e.g., from VT100 keypad)
            if (read(STDIN_FILENO, &seq[1], 1) != 1)
                return '\x1b';  // Incomplete
            switch (seq[1]) {
                case 'H':
                    return HOME_KEY;  // EscOH
                case 'F':
                    return END_KEY;  // EscOF
                // Arrows might be EscOA, EscOB, etc. Add if needed.
                default:
                    return '\x1b';
            }
        } else {
            // Escape followed by something other than [ or O
            return '\x1b';
        }
    } else {
        // Regular character (including Backspace, Enter, Tab etc.)
        return c;
    }
}

// *** Output / Rendering Implementation ***

// Append string to static screen buffer, handling overflow
void screenbuf_append(const char* s, int len) {
    if (len <= 0)
        return;
    if (screenbuf_len + len > SCREEN_BUF_SIZE) {
        // Buffer overflow! Truncate the appended string.
        len = SCREEN_BUF_SIZE - screenbuf_len;
        if (len <= 0)
            return;  // No space left at all
    }
    memcpy(screenbuf + screenbuf_len, s, len);
    screenbuf_len += len;
}

// Clear static screen buffer and add initial control sequences
void screenbuf_clear() {
    screenbuf_len = 0;
    // Start with hiding cursor (prevents flicker during redraw)
    screenbuf_append("\x1b[?25l", 6);
    // Go to home position (top-left) instead of clearing whole screen
    screenbuf_append("\x1b[H", 3);
}

// Adjust rowoff/coloff to ensure cursor is visible on screen
void editorScroll() {
    // Vertical scroll check
    if (textbuf.cursor_abs_y < textbuf.rowoff) {
        textbuf.rowoff = textbuf.cursor_abs_y;
        textbuf.rowoff_chunk = NULL;  // Invalidate cache when scrolling up
    }
    if (textbuf.cursor_abs_y >= textbuf.rowoff + screenrows) {
        textbuf.rowoff = textbuf.cursor_abs_y - screenrows + 1;
        textbuf.rowoff_chunk = NULL;  // Invalidate cache when scrolling down
    }

    // Horizontal scroll check (based on visual column)
    if (textbuf.cursor_abs_x < textbuf.coloff) {
        textbuf.coloff = textbuf.cursor_abs_x;
    }
    if (textbuf.cursor_abs_x >= textbuf.coloff + screencols) {
        textbuf.coloff = textbuf.cursor_abs_x - screencols + 1;
    }
}

// Draw the text buffer content onto the screen buffer
void editorDrawRows() {
    int y;
    struct bufchunk* current_chunk;
    int current_rel_i;
    int current_abs_i; // Absolute index corresponding to current_chunk/rel_i

    // Find the starting position for rendering (based on rowoff)
    // Use cache if valid, otherwise recalculate
    if (textbuf.rowoff_chunk == NULL) {
        if (bufclient_find_line_start(&textbuf, textbuf.rowoff, &textbuf.rowoff_chunk, &textbuf.rowoff_rel_i, &textbuf.rowoff_abs_i) != RESULT_OK) {
            // Error finding start row (e.g., rowoff beyond buffer end after delete?)
            // Reset to top of buffer as fallback
            textbuf.rowoff = 0;
            textbuf.rowoff_chunk = textbuf.begin;
            textbuf.rowoff_rel_i = 0;
            textbuf.rowoff_abs_i = 0;
            if (textbuf.begin == NULL) return; // Handle completely empty buffer case
        }
    }
     // Ensure initial state is valid even if rowoff was bad
    if (textbuf.rowoff_chunk == NULL && textbuf.begin != NULL) {
         textbuf.rowoff_chunk = textbuf.begin;
         textbuf.rowoff_rel_i = 0;
         textbuf.rowoff_abs_i = 0;
    } else if (textbuf.begin == NULL) {
         // Nothing to draw if buffer is completely empty (no initial chunk)
          for (y = 0; y < screenrows; y++) {
              if (y == screenrows / 3) { // Show welcome on empty
                 char welcome[80];
                 int welcome_len = snprintf(welcome, sizeof(welcome), "lkjsxceditor v%s -- %d chunks free", LKJSXCEDITOR_VERSION, BUFCHUNK_COUNT - bufchunk_pool_used);
                 if (welcome_len > screencols) welcome_len = screencols;
                 int padding = (screencols - welcome_len) / 2;
                 if (padding > 0) { screenbuf_append("~", 1); padding--; }
                 while (padding-- > 0) screenbuf_append(" ", 1);
                 screenbuf_append(welcome, welcome_len);
              } else {
                 screenbuf_append("~", 1);
              }
              screenbuf_append("\x1b[K", 3);
              screenbuf_append("\r\n", 2);
          }
          return; // Nothing more to do
    }


    current_chunk = textbuf.rowoff_chunk;
    current_rel_i = textbuf.rowoff_rel_i;
    current_abs_i = textbuf.rowoff_abs_i;

    // Iterate through each row of the terminal screen
    for (y = 0; y < screenrows; y++) {
        int file_line_abs_y = textbuf.rowoff + y; // The absolute line number we are trying to render

        // Optimization: If we know we are past the end of the buffer content, draw tildes
        if (current_chunk == NULL || (current_abs_i >= textbuf.size && textbuf.size > 0 && file_line_abs_y > textbuf.cursor_abs_y) ) {
            // Check if we really should be past the content based on cursor pos as heuristic
            // (This condition needs refinement, but aims to avoid unnecessary line scanning)
             if (textbuf.size == 0 && y == screenrows / 3) { // Show welcome message near middle on empty file
                char welcome[80];
                int welcome_len = snprintf(welcome, sizeof(welcome), "lkjsxceditor v%s -- %d chunks free", LKJSXCEDITOR_VERSION, BUFCHUNK_COUNT - bufchunk_pool_used);
                if (welcome_len > screencols) welcome_len = screencols;
                int padding = (screencols - welcome_len) / 2;
                if (padding > 0) { screenbuf_append("~", 1); padding--; }
                while (padding-- > 0) screenbuf_append(" ", 1);
                screenbuf_append(welcome, welcome_len);
            } else {
                screenbuf_append("~", 1);
            }
            screenbuf_append("\x1b[K", 3); // Clear rest of line
            screenbuf_append("\r\n", 2);
            continue; // Move to the next screen row
        }

        // Render the current file line char by char, handling horizontal scroll (coloff)
        int line_visual_col = 0; // Visual column within the *file* line
        int screen_x = 0;        // Current visual column position on the *screen* line
        int line_abs_start_i = current_abs_i; // Remember start index for this line


        struct bufchunk* line_chunk = current_chunk; // Temporary pointers for line traversal
        int line_rel_i = current_rel_i;
        int line_abs_i = current_abs_i;
        int line_drawn = 0; // Flag to check if we drew anything for this line

        while (line_chunk != NULL) {
            while (line_rel_i < line_chunk->size) {
                char c = line_chunk->data[line_rel_i];

                if (c == '\n') {
                    // End of the current file line found
                    // Advance main pointers past the newline for the next iteration
                    current_abs_i = line_abs_i + 1;
                    if (line_rel_i + 1 < line_chunk->size) {
                        current_chunk = line_chunk;
                        current_rel_i = line_rel_i + 1;
                    } else if (line_chunk->next != NULL) {
                        current_chunk = line_chunk->next;
                        current_rel_i = 0;
                    } else {
                        // Reached end of buffer exactly at newline
                        current_chunk = NULL; // Signal end for outer loop
                        current_rel_i = 0;
                    }
                    goto next_screen_row; // Break inner loops to move to next screen row
                }

                // Calculate width of current character and its representation
                int char_width = 0;
                char display_buf[TAB_STOP + 3]; // Max width is tab stop + ^ + char + null
                int display_len = 0;

                if (c == '\t') {
                    char_width = (TAB_STOP - (line_visual_col % TAB_STOP));
                    int i;
                    for (i = 0; i < char_width; i++) display_buf[i] = ' ';
                    display_len = char_width;
                } else if (iscntrl((unsigned char)c)) {
                    char_width = 2;
                    display_buf[0] = '^';
                    display_buf[1] = ((c & 0x1f) | '@'); // Map 0-31 to @, A, B...
                    display_len = 2;
                } else {
                    char_width = 1;
                    display_buf[0] = c;
                    display_len = 1;
                }
                display_buf[display_len] = '\0'; // Null terminate for safety


                // --- Horizontal Scrolling Logic ---
                int char_end_visual_col = line_visual_col + char_width;

                // Check if this character is *at least partially* visible
                if (char_end_visual_col > textbuf.coloff) {
                    int visible_start_col = (line_visual_col < textbuf.coloff) ? textbuf.coloff : line_visual_col;
                    int visible_width = char_end_visual_col - visible_start_col;

                    // Calculate screen position based on the visible start
                    screen_x = visible_start_col - textbuf.coloff;

                    // Truncate display string if char starts before coloff (e.g., tab)
                    // For simplicity, we won't truncate the display string itself here,
                    // but we will use visible_width to check screen bounds.
                    // A more complex render could show partial tabs.

                    // Check if the *visible part* fits on the remaining screen width
                    if (screen_x < screencols) {
                         int draw_width = (screen_x + visible_width > screencols) ? (screencols - screen_x) : visible_width;

                         // Append the character (or its representation)
                         // Simple approach: draw full representation if any part fits.
                         if (draw_width > 0) {
                             // If the char width > draw_width (e.g. tab cut off) draw spaces?
                             // For now, just draw the representation up to draw_width.
                             int append_len = (display_len < draw_width) ? display_len : draw_width;
                             screenbuf_append(display_buf, append_len);
                             // If append_len < draw_width, fill with spaces? No, just draw what fits.
                             line_drawn = 1; // Mark that we drew something
                        }
                        // If draw_width <= 0, char starts exactly at or after screen edge.
                    } else {
                         // Character starts beyond the right edge of the screen. Stop rendering this line.
                         // We need to advance the main pointers to the start of the next line
                         // This requires finding the next newline from line_abs_i
                          struct bufchunk* scan_chunk = line_chunk;
                          int scan_rel_i = line_rel_i;
                          int scan_abs_i = line_abs_i;
                          while(scan_chunk) {
                             while(scan_rel_i < scan_chunk->size) {
                                  if (scan_chunk->data[scan_rel_i] == '\n') {
                                      current_abs_i = scan_abs_i + 1;
                                      if (scan_rel_i + 1 < scan_chunk->size) {
                                          current_chunk = scan_chunk;
                                          current_rel_i = scan_rel_i + 1;
                                      } else if (scan_chunk->next) {
                                          current_chunk = scan_chunk->next;
                                          current_rel_i = 0;
                                      } else {
                                          current_chunk = NULL; current_rel_i = 0;
                                      }
                                      goto next_screen_row;
                                  }
                                  scan_rel_i++; scan_abs_i++;
                             }
                             scan_chunk = scan_chunk->next; scan_rel_i = 0;
                          }
                          // If no newline found, we reached end of buffer
                          current_chunk = NULL; current_rel_i = 0; current_abs_i = textbuf.size;
                          goto next_screen_row;
                    }
                }
                // --- End Horizontal Scrolling Logic ---

                line_visual_col += char_width; // Advance visual column on the file line
                line_rel_i++;
                line_abs_i++;

            } // end while current_rel_i < chunk->size

            // Move to the next chunk if needed for this line
            line_chunk = line_chunk->next;
            line_rel_i = 0;

        } // end while line_chunk != NULL (finished buffer while drawing line)

        // If we exit the loop because we ran out of buffer content before finding '\n'
        current_chunk = NULL; // Signal end for outer loop
        current_rel_i = 0;
        current_abs_i = textbuf.size;

    next_screen_row:
        // If we didn't draw anything (e.g., line entirely off-screen left),
        // and file_line_abs_y corresponds to a real line, add a marker? Optional.
        // if (!line_drawn && file_line_abs_y < total_lines) { }

        screenbuf_append("\x1b[K", 3); // Clear rest of the screen line from cursor onwards
        screenbuf_append("\r\n", 2);   // Move to the beginning of the next screen line
    } // end for each screen row y
}

// Draw the status bar at the bottom
void editorDrawStatusBar() {
    screenbuf_append("\x1b[7m", 4);  // Invert colors (enter reverse video mode)

    char status[128], rstatus[64];
    int len = 0, rlen = 0;

    // Left part: Mode, Filename, Dirty status
    const char* mode_str;
    switch (mode) {
        case MODE_NORMAL:
            mode_str = "-- NORMAL --";
            break;
        case MODE_INSERT:
            mode_str = "-- INSERT --";
            break;
        case MODE_COMMAND:
            mode_str = "-- COMMAND --";
            break;  // Maybe just show ':'? No, command line below.
        default:
            mode_str = "";
            break;
    }
    len = snprintf(status, sizeof(status), " %.15s %.40s%s",
                   mode_str,
                   textbuf.filename[0] ? textbuf.filename : "[No Name]",
                   textbuf.dirty ? " [+]" : "");

    // Right part: Line/TotalLines, Percentage
    int total_lines = 1;  // Inefficiently count total lines
    if (textbuf.size > 0) {
        struct bufchunk* ch = textbuf.begin;
        int i;
        while (ch) {
            for (i = 0; i < ch->size; ++i) {
                if (ch->data[i] == '\n')
                    total_lines++;
            }
            ch = ch->next;
        }
        // Handle file not ending with newline: last line isn't counted by '\n' scan
        if (textbuf.rbegin && textbuf.rbegin->size > 0 && textbuf.rbegin->data[textbuf.rbegin->size - 1] != '\n') {
            // This case is tricky, the last line exists but has no '\n'.
            // Our simple '\n' count might be off by one for the total.
            // Let's stick to the simple count for now.
        }
    } else {
        total_lines = 1;  // Empty buffer is considered 1 line
    }

    int percent = (textbuf.size > 0) ? (int)(((long long)(textbuf.cursor_abs_y + 1) * 100) / total_lines) : 100;
    if (percent > 100)
        percent = 100;  // Clamp if cursor_y > total_lines somehow

    rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d %3d%% ",
                    textbuf.cursor_abs_y + 1, total_lines, percent);

    // Render left status, truncated if necessary
    if (len > screencols)
        len = screencols;
    screenbuf_append(status, len);

    // Render right status with padding in between
    while (len < screencols) {
        if (screencols - len == rlen) {  // If right status fits exactly
            screenbuf_append(rstatus, rlen);
            len += rlen;
        } else {
            screenbuf_append(" ", 1);  // Add padding space
            len++;
        }
    }

    screenbuf_append("\x1b[m", 3);  // Reset colors (exit reverse video mode)
    // No \r\n needed here, editorDrawCommandLine will follow
}

// Draw the command/status message line below the status bar
void editorDrawCommandLine() {
    screenbuf_append("\x1b[K", 3);  // Clear the line first

    time_t current_time = time(NULL);
    if (mode == MODE_COMMAND) {
        // Show command prompt and current command buffer
        statusbuf[0] = '\0';  // Clear any timed status message
        int prompt_len = 1;   // Length of ":"
        screenbuf_append(":", prompt_len);
        int cmd_display_len = cmdbuf_len;
        // Handle command longer than screen width (basic truncation)
        if (prompt_len + cmd_display_len > screencols) {
            cmd_display_len = screencols - prompt_len;
        }
        screenbuf_append(cmdbuf, cmd_display_len);
        // Cursor will be positioned after the command text later in editorRefreshScreen
    } else if (statusbuf[0] != '\0' && current_time - statusbuf_time < 5) {
        // Display timed status message if active
        int msg_len = strlen(statusbuf);
        if (msg_len > screencols)
            msg_len = screencols;  // Truncate if needed
        screenbuf_append(statusbuf, msg_len);
    } else {
        // Clear status message if expired or not in command mode
        statusbuf[0] = '\0';
    }
}

// Refresh the entire screen content based on current editor state
void editorRefreshScreen() {
    editorScroll();     // Ensure cursor position is valid for scrolling offsets
    screenbuf_clear();  // Reset buffer and add initial control sequences

    editorDrawRows();         // Draw text content
    editorDrawStatusBar();    // Draw status bar
    editorDrawCommandLine();  // Draw command/message line

    // Calculate final cursor position on screen
    // Screen Y = Absolute Y - Row Offset + 1 (for 1-based terminal coords)
    int screen_cursor_y = textbuf.cursor_abs_y - textbuf.rowoff + 1;
    // Screen X = Visual X - Col Offset + 1 (for 1-based terminal coords)
    int screen_cursor_x = textbuf.cursor_abs_x - textbuf.coloff + 1;

    // Adjust cursor position if in command mode
    if (mode == MODE_COMMAND) {
        screen_cursor_y = screenrows + 2;      // Command line is row below status bar
        screen_cursor_x = 1 + cmdbuf_len + 1;  // ':' + command text length + 1
    }

    // Generate cursor positioning command
    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", screen_cursor_y, screen_cursor_x);
    screenbuf_append(buf, strlen(buf));

    screenbuf_append("\x1b[?25h", 6);  // Show cursor

    // Write the entire accumulated screen buffer to standard output in one go
    if (write(STDOUT_FILENO, screenbuf, screenbuf_len) == -1) {
        die("write to screen failed");  // Fatal error if writing fails
    }
}

// *** File I/O Implementation ***

// Open a file and load its content into the text buffer
enum RESULT editorOpen(const char* filename) {
    FILE* fp = fopen(filename, "r");
    if (!fp) {
        // File doesn't exist, treat as a new file
        if (errno == ENOENT) {
            strncpy(textbuf.filename, filename, sizeof(textbuf.filename) - 1);
            textbuf.filename[sizeof(textbuf.filename) - 1] = '\0';
            editorSetStatusMessage("New file");
            bufclient_clear(&textbuf);  // Ensure buffer is empty for new file
            textbuf.dirty = 0;          // New file isn't dirty yet
            return RESULT_OK;
        } else {
            // Other error opening file
            char err_msg[sizeof(textbuf.filename) + 32];
            snprintf(err_msg, sizeof(err_msg), "Error opening: %s", strerror(errno));
            editorSetStatusMessage(err_msg);
            return RESULT_ERR;
        }
    }

    // File exists, store filename and clear current buffer content
    strncpy(textbuf.filename, filename, sizeof(textbuf.filename) - 1);
    textbuf.filename[sizeof(textbuf.filename) - 1] = '\0';
    bufclient_clear(&textbuf);  // Clear existing buffer before loading

    char readbuf[FILE_BUF_SIZE];
    size_t nread;
    enum RESULT res = RESULT_OK;
    long long total_read = 0;

    // Read file in chunks and insert characters into buffer client
    while ((nread = fread(readbuf, 1, sizeof(readbuf), fp)) > 0) {
        size_t i;
        total_read += nread;
        for (i = 0; i < nread; i++) {
            // TODO: Handle potential CR/LF conversion? For simplicity, store as is.
            if (bufclient_insert_char(&textbuf, readbuf[i]) != RESULT_OK) {
                editorSetStatusMessage("Error loading file: Out of memory?");
                res = RESULT_ERR;
                goto cleanup;  // Exit loop and close file on memory error
            }
        }
    }

    // Check for read errors after loop
    if (ferror(fp)) {
        editorSetStatusMessage("Error reading file contents");
        res = RESULT_ERR;
    }

cleanup:
    fclose(fp);
    if (res == RESULT_OK) {
        textbuf.dirty = 0;                      // File just loaded is not dirty
        bufclient_move_cursor_to(&textbuf, 0);  // Move cursor to start of file
        char status[sizeof(textbuf.filename) + 32];
        snprintf(status, sizeof(status), "Opened \"%s\" (%lld bytes)", textbuf.filename, total_read);
        editorSetStatusMessage(status);
    }
    return res;
}

// Save the current text buffer content to the associated filename
enum RESULT editorSave() {
    if (textbuf.filename[0] == '\0') {
        // TODO: Implement prompting for filename if not set
        editorSetStatusMessage("No filename. Use :w <filename> or specify filename on open.");
        return RESULT_ERR;
    }

    // Open file for writing (truncates existing file or creates new)
    FILE* fp = fopen(textbuf.filename, "w");
    if (!fp) {
        char err_msg[sizeof(textbuf.filename) + 32];
        snprintf(err_msg, sizeof(err_msg), "Error saving: %s", strerror(errno));
        editorSetStatusMessage(err_msg);
        return RESULT_ERR;
    }

    // Iterate through buffer chunks and write data to file
    struct bufchunk* current = textbuf.begin;
    long long total_written = 0;
    enum RESULT res = RESULT_OK;

    while (current != NULL) {
        if (current->size > 0) {  // Only write if chunk has data
            if (fwrite(current->data, 1, current->size, fp) != (size_t)current->size) {
                char err_msg[64];
                snprintf(err_msg, sizeof(err_msg), "Write error: %s", strerror(errno));
                editorSetStatusMessage(err_msg);
                res = RESULT_ERR;
                break;  // Stop writing on error
            }
            total_written += current->size;
        }
        current = current->next;
    }

    // Check for close errors? fclose can fail.
    if (fclose(fp) != 0 && res == RESULT_OK) {
        editorSetStatusMessage("Error closing file after write.");
        res = RESULT_ERR;  // Consider save failed if close fails
    }

    if (res == RESULT_OK) {
        textbuf.dirty = 0;  // Mark buffer as clean after successful save
        char status[sizeof(textbuf.filename) + 32];
        snprintf(status, sizeof(status), "\"%s\" %lld bytes written", textbuf.filename, total_written);
        editorSetStatusMessage(status);
    }
    return res;
}

// *** Editor Operations Implementation ***

// Initialize editor state: terminal, screen size, buffers
void initEditor() {
    bufchunk_pool_init();  // Initialize memory pool first
    if (bufclient_init(&textbuf) != RESULT_OK) {
        write(STDERR_FILENO, "Failed to init text buffer\n", 27);
        _exit(1);  // Cannot continue without buffer
    }
    // cmdbuf and statusbuf are static arrays, already allocated.
    cmdbuf[0] = '\0';
    cmdbuf_len = 0;
    statusbuf[0] = '\0';
    statusbuf_time = 0;
    mode = MODE_NORMAL;

    // Get terminal dimensions
    int total_rows, total_cols;
    if (getWindowSize(&total_rows, &total_cols) == RESULT_ERR)
        die("getWindowSize failed");
    screencols = total_cols;
    // Reserve bottom 2 rows for status bar and command line
    screenrows = total_rows - 2;
    if (screenrows < 1) {  // Ensure at least one row for text area
        die("Terminal too small");
        screenrows = 1;
    }

    // Enable raw mode
    if (enableRawMode() == RESULT_ERR)
        die("enableRawMode failed");
}

// Set the status message displayed at the bottom
void editorSetStatusMessage(const char* msg) {
    if (msg == NULL) {
        statusbuf[0] = '\0';
        return;
    }
    strncpy(statusbuf, msg, STATUS_BUF_SIZE - 1);
    statusbuf[STATUS_BUF_SIZE - 1] = '\0';  // Ensure null termination
    statusbuf_time = time(NULL);            // Record time for timeout display
}

// Process the command entered in command mode
void editorProcessCommand() {
    cmdbuf[cmdbuf_len] = '\0';  // Null-terminate the received command

    // Trim trailing whitespace (simple version)
    while (cmdbuf_len > 0 && isspace((unsigned char)cmdbuf[cmdbuf_len - 1])) {
        cmdbuf_len--;
        cmdbuf[cmdbuf_len] = '\0';
    }

    if (cmdbuf_len == 0) {  // Empty command
        mode = MODE_NORMAL;
        return;
    }

    // --- Command Matching ---
    if (strcmp(cmdbuf, "q") == 0) {
        if (textbuf.dirty && QUIT_TIMES > 0) {  // Check dirty flag only if quit confirmation is needed
            static int quit_confirm = 0;        // Static counter for confirmation attempts
            quit_confirm++;
            if (quit_confirm >= QUIT_TIMES) {
                terminate_editor = 1;  // Confirmed quit despite dirty buffer
            } else {
                char msg[128];
                snprintf(msg, sizeof(msg), "Unsaved changes! Use :wq to save and quit, or :q! to discard (%d/%d)", quit_confirm, QUIT_TIMES);
                editorSetStatusMessage(msg);
                mode = MODE_NORMAL;  // Return to normal mode after failed quit attempt
                quit_confirm = 0;    // Reset count if user does something else
            }
        } else {
            terminate_editor = 1;  // Quit normally (buffer clean or no confirmation needed)
        }
    } else if (strcmp(cmdbuf, "q!") == 0) {
        terminate_editor = 1;  // Force quit, discard changes
    } else if (strcmp(cmdbuf, "w") == 0) {
        editorSave();  // Save to current filename
        mode = MODE_NORMAL;
    } else if (strcmp(cmdbuf, "wq") == 0) {
        if (editorSave() == RESULT_OK) {  // Try saving first
            terminate_editor = 1;         // Quit if save was successful
        } else {
            mode = MODE_NORMAL;  // Stay in editor if save failed
        }
    } else if (strncmp(cmdbuf, "w ", 2) == 0) {
        // Save As: :w <filename>
        char* filename = cmdbuf + 2;
        // Trim leading whitespace from filename
        while (*filename && isspace((unsigned char)*filename))
            filename++;

        if (filename[0] != '\0') {
            // Update buffer's filename before saving
            strncpy(textbuf.filename, filename, sizeof(textbuf.filename) - 1);
            textbuf.filename[sizeof(textbuf.filename) - 1] = '\0';
            editorSave();  // Save to the new filename
        } else {
            editorSetStatusMessage("Filename missing for :w command");
        }
        mode = MODE_NORMAL;
    } else if (strncmp(cmdbuf, "e ", 2) == 0) {
        // Edit file: :e <filename>
        // TODO: Check for dirty buffer first?
        char* filename = cmdbuf + 2;
        while (*filename && isspace((unsigned char)*filename))
            filename++;
        if (filename[0] != '\0') {
            if (textbuf.dirty) {
                editorSetStatusMessage("Unsaved changes! Save or use :e! to discard.");
            } else {
                editorOpen(filename);
            }
        } else {
            editorSetStatusMessage("Filename missing for :e command");
        }
        mode = MODE_NORMAL;
    } else if (strncmp(cmdbuf, "e! ", 3) == 0) {
        // Edit file force: :e! <filename>
        char* filename = cmdbuf + 3;
        while (*filename && isspace((unsigned char)*filename))
            filename++;
        if (filename[0] != '\0') {
            editorOpen(filename);  // Discard changes and open
        } else {
            editorSetStatusMessage("Filename missing for :e! command");
        }
        mode = MODE_NORMAL;
    } else {
        // Unknown command
        char err_msg[CMD_BUF_SIZE + 16];
        snprintf(err_msg, sizeof(err_msg), "Unknown command: %s", cmdbuf);
        editorSetStatusMessage(err_msg);
        mode = MODE_NORMAL;
    }

    // Clear command buffer for next time (unless staying in command mode)
    // cmdbuf[0] = '\0';
    // cmdbuf_len = 0;
}

// Process the next keypress from the user based on the current editor mode
void editorProcessKeypress() {
    enum editorKey c = editorReadKey();

    // Handle mode-specific key presses
    switch (mode) {
        case MODE_NORMAL:
            switch (c) {
                // --- Mode Changes ---
                case 'i':  // Enter Insert mode
                    mode = MODE_INSERT;
                    editorSetStatusMessage("-- INSERT --");
                    break;
                case ':':  // Enter Command mode
                    mode = MODE_COMMAND;
                    cmdbuf[0] = '\0';  // Clear previous command
                    cmdbuf_len = 0;
                    editorSetStatusMessage(":");  // Show initial prompt
                    break;

                // --- Movement ---
                case 'h':
                case ARROW_LEFT:
                    bufclient_move_cursor_relative(&textbuf, ARROW_LEFT);
                    break;
                case 'l':
                case ARROW_RIGHT:
                    bufclient_move_cursor_relative(&textbuf, ARROW_RIGHT);
                    break;
                case 'k':
                case ARROW_UP:
                    bufclient_move_cursor_relative(&textbuf, ARROW_UP);
                    break;
                case 'j':
                case ARROW_DOWN:
                    bufclient_move_cursor_relative(&textbuf, ARROW_DOWN);
                    break;
                case PAGE_UP:
                    // Move cursor up by one screen height
                    {
                        int i;
                        for (i = 0; i < screenrows; i++)
                            bufclient_move_cursor_relative(&textbuf, ARROW_UP);
                    }
                    // Scroll view instantly (handled by editorScroll before next draw)
                    break;
                case PAGE_DOWN:
                    // Move cursor down by one screen height
                    {
                        int i;
                        for (i = 0; i < screenrows; i++)
                            bufclient_move_cursor_relative(&textbuf, ARROW_DOWN);
                    }
                    // Scroll view instantly
                    break;
                case HOME_KEY:  // Move cursor to beginning of the current line (visual column 0)
                {
                    struct bufchunk* line_chunk;
                    int line_rel_i, line_abs_i;
                    if (bufclient_find_line_start(&textbuf, textbuf.cursor_abs_y, &line_chunk, &line_rel_i, &line_abs_i) == RESULT_OK) {
                        bufclient_move_cursor_to(&textbuf, line_abs_i);
                    }
                } break;
                case END_KEY:  // Move cursor to end of the current line
                {
                    struct bufchunk* next_line_chunk;
                    int next_line_rel_i, next_line_abs_i;
                    // Find start of *next* line
                    if (bufclient_find_line_start(&textbuf, textbuf.cursor_abs_y + 1, &next_line_chunk, &next_line_rel_i, &next_line_abs_i) == RESULT_OK) {
                        // Move to the character just before the newline (or start of next line)
                        bufclient_move_cursor_to(&textbuf, next_line_abs_i - 1);
                    } else {
                        // We are on the last line, move cursor to the very end of the buffer
                        bufclient_move_cursor_to(&textbuf, textbuf.size);
                    }
                } break;

                // --- Editing ---
                case 'x':  // Delete character under cursor (Vim 'x')
                    // Move right, delete backward, effectively deleting char at original position
                    if (textbuf.cursor_abs_i < textbuf.size) {
                        bufclient_move_cursor_relative(&textbuf, ARROW_RIGHT);
                        bufclient_delete_char(&textbuf);
                    }
                    break;
                case 'd':  // Potential start of 'dd' (delete line) - Not implemented
                    editorSetStatusMessage("dd not implemented");
                    break;
                case 'o':  // Insert newline below current and enter insert mode - Not implemented
                    editorSetStatusMessage("o not implemented");
                    break;
                case 'O':  // Insert newline above current and enter insert mode - Not implemented
                    editorSetStatusMessage("O not implemented");
                    break;

                case '\x1b':  // Escape key - already in normal mode, do nothing silently
                    break;

                default:
                    // Ignore other keys in normal mode for now
                    // Could beep or show message?
                    break;
            }
            break;  // End MODE_NORMAL

        case MODE_INSERT:
            switch (c) {
                case '\x1b':  // Escape: Return to Normal mode
                    mode = MODE_NORMAL;
                    // Vim behavior: move cursor left if possible after exiting insert
                    if (textbuf.cursor_abs_i > 0) {
                        // Check if cursor is at the start of the line (visual X = 0)
                        // If not at visual column 0, move left.
                        if (textbuf.cursor_abs_x > 0) {
                            bufclient_move_cursor_relative(&textbuf, ARROW_LEFT);
                        }
                    }
                    editorSetStatusMessage("-- NORMAL --");
                    break;
                case '\r':  // Enter key (treat same as newline)
                    bufclient_insert_char(&textbuf, '\n');
                    break;
                case BACKSPACE:  // Backspace key
                    bufclient_delete_char(&textbuf);
                    break;
                case DEL_KEY:  // Delete key (delete character *after* cursor)
                    // Move right, delete backward (if not at end of buffer)
                    if (textbuf.cursor_abs_i < textbuf.size) {
                        bufclient_move_cursor_relative(&textbuf, ARROW_RIGHT);
                        bufclient_delete_char(&textbuf);
                    }
                    break;

                // Ignore movement keys in insert mode for simplicity
                case ARROW_UP:
                case ARROW_DOWN:
                case ARROW_LEFT:
                case ARROW_RIGHT:
                case PAGE_UP:
                case PAGE_DOWN:
                case HOME_KEY:
                case END_KEY:
                    // Could potentially allow movement, but complicates state.
                    // editorSetStatusMessage("Use Esc to return to Normal mode for movement.");
                    break;

                default:  // Insert regular character if printable
                    // Check for standard printable ASCII range
                    if (c >= 32 && c <= 126) {
                        bufclient_insert_char(&textbuf, (char)c);
                    }
                    // Ignore other non-printable chars in insert mode
                    break;
            }
            break;  // End MODE_INSERT

        case MODE_COMMAND:
            switch (c) {
                case '\x1b':  // Escape: Cancel command, return to Normal mode
                    mode = MODE_NORMAL;
                    editorSetStatusMessage("");  // Clear command line/status
                    break;
                case '\r':  // Enter key: Execute the command
                    editorProcessCommand();
                    // Mode is usually set back to NORMAL by editorProcessCommand
                    break;
                case BACKSPACE:  // Backspace in command line
                    if (cmdbuf_len > 0) {
                        cmdbuf_len--;
                        cmdbuf[cmdbuf_len] = '\0';  // Keep null-terminated
                    }
                    break;

                // Ignore Del, Arrows, Page keys in command line for simplicity
                case DEL_KEY:
                case ARROW_UP:
                case ARROW_DOWN:
                case ARROW_LEFT:
                case ARROW_RIGHT:
                case PAGE_UP:
                case PAGE_DOWN:
                case HOME_KEY:
                case END_KEY:
                    break;

                default:                                  // Append typed character to command buffer if printable
                    if (cmdbuf_len < CMD_BUF_SIZE - 1) {  // Check buffer limit
                        if (isprint((unsigned char)c)) {  // Check if character is printable
                            cmdbuf[cmdbuf_len++] = (char)c;
                            cmdbuf[cmdbuf_len] = '\0';  // Keep null-terminated
                        }
                    }
                    break;
            }
            break;  // End MODE_COMMAND
    }
}

// *** Main Function ***
int main(int argc, char* argv[]) {
    // Initialization
    initEditor();

    // Open file specified on command line, if any
    if (argc >= 2) {
        editorOpen(argv[1]);
        // editorOpen sets status messages for success/failure/new file
    } else {
        // No file specified, show welcome/help message
        editorSetStatusMessage("lkjsxceditor | Help: :w [file] | :q | :q! | :wq | :e [file]");
    }

    // Main event loop
    while (!terminate_editor) {
        editorRefreshScreen();    // Update display
        editorProcessKeypress();  // Wait for and process user input
    }

    // Cleanup before exiting
    // Clear screen and move cursor to top-left for a clean exit
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    disableRawMode();          // Restore original terminal settings
    bufclient_free(&textbuf);  // Free buffer chunks back to pool

    return 0;  // Successful exit
}