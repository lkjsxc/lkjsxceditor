#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>  // for NULL, size_t
#include <stdio.h>
#include <stdlib.h> // For _exit, exit
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

enum RESULT bufclient_update_cursor_coords(struct bufclient* buf) {
    // Target absolute index we need to find coordinates for
    int target_abs_i = buf->cursor_abs_i;

    // Validate target index
    if (target_abs_i < 0 || target_abs_i > buf->size) {
        // editorSetStatusMessage("Error: Invalid cursor_abs_i %d (size %d)", target_abs_i, buf->size);
        // Optionally try to clamp it? Or just error out.
        // buf->cursor_abs_i = (target_abs_i < 0) ? 0 : buf->size;
        // target_abs_i = buf->cursor_abs_i;
        return RESULT_ERR; // Indicate error
    }

    // --- Optimization: Choose a better starting point ---
    struct bufchunk* start_chunk = buf->begin;
    int start_rel_i = 0;
    int start_abs_i = 0;
    int start_y = 0;
    int start_x = 0; // Visual X at the start of the scan

    // Check if using the rowoff cache is valid and potentially faster
    // We use it if the cache is valid AND the target is at or after the cache point.
    // We assume rowoff_abs_i marks the START of line rowoff.
    if (buf->rowoff_chunk != NULL && buf->rowoff_abs_i >= 0 && buf->rowoff_abs_i <= target_abs_i)
    {
        // Heuristic: Only use the cache if it saves a significant amount of scanning.
        // For simplicity, we'll use it whenever target_abs_i >= rowoff_abs_i.
        // A more complex heuristic could compare target_abs_i - rowoff_abs_i vs rowoff_abs_i.
        start_chunk = buf->rowoff_chunk;
        start_rel_i = buf->rowoff_rel_i;
        start_abs_i = buf->rowoff_abs_i;
        start_y = buf->rowoff; // Start counting lines from rowoff
        start_x = 0;           // Visual X is 0 at the start of any line
    }

    // --- Perform the scan from the chosen start point ---
    struct bufchunk* current_chunk = start_chunk;
    int current_rel_i = start_rel_i;
    int current_abs_i = start_abs_i;
    int current_y = start_y;
    int current_x = start_x; // Track visual column from the start point

    // Scan forward to the target_abs_i
    while (current_chunk != NULL && current_abs_i < target_abs_i) {
        // Process characters within the current chunk up to its size or until target found
        int limit = current_chunk->size;
        while (current_rel_i < limit && current_abs_i < target_abs_i) {
            char c = current_chunk->data[current_rel_i];

            if (c == '\n') {
                current_y++;
                current_x = 0; // Reset visual column for the new line
            } else if (c == '\t') {
                // Add spaces up to the next tab stop
                current_x += TAB_STOP - (current_x % TAB_STOP);
            } else if (c >= 0 && c < 32) {
                 // Handle other control characters (e.g., display as ^X or ignore)
                 // Simple approach: treat like a single character for position
                 current_x++; // Or current_x += 2 for ^X notation? Let's stick to 1.
            }
            else {
                // Regular printable character (assuming single byte / fixed width for now)
                // TODO: Handle multi-byte UTF-8 characters if needed
                current_x++;
            }

            current_rel_i++;
            current_abs_i++;
        }

        // If we haven't reached the target yet, move to the next chunk
        if (current_abs_i < target_abs_i) {
             current_chunk = current_chunk->next;
             current_rel_i = 0; // Start from the beginning of the next chunk
        }
    }

    // --- Final Check and Update ---

    // After the loop, current_abs_i should be == target_abs_i.
    // current_y and current_x should hold the calculated coordinates.
    if (current_abs_i != target_abs_i) {
        // This case should ideally not happen if target_abs_i is valid and <= buf->size.
        // It might indicate an issue with chunk linking or sizes.
        // Maybe the target_abs_i == buf->size case wasn't handled perfectly?
        // Let's double-check the loop condition. The loop stops *when* current_abs_i == target_abs_i.
        // So the values calculated *up to that point* are correct for the cursor *at* target_abs_i.

        // If target_abs_i == buf->size (cursor at the very end), the loop calculates
        // the position correctly based on the characters *before* it.

        // If we somehow didn't reach the target (e.g., current_chunk became NULL unexpectedly)
        // or overshot it (logic error), fall back or report error.
        // editorSetStatusMessage("Error: Cursor coord calculation failed internal check (abs_i %d != target %d)",
        //                        current_abs_i, target_abs_i);

        // Optional Fallback: Perform a full scan from the beginning as a safety measure
        // (This would essentially be the original algorithm but with integrated X calculation)
        // For now, just return error.
        return RESULT_ERR;
    }

    // Update the buffer's cursor coordinates
    buf->cursor_abs_y = current_y;
    buf->cursor_abs_x = current_x;
    // buf->cursor_goal_x = current_x; // Usually set separately when moving vertically

    // Optional: Update the cursor_chunk and cursor_rel_i based on final position
    // This requires finding the chunk containing target_abs_i if we didn't track it precisely.
    // The scan loop *ends* with current_chunk and current_rel_i pointing *at* the target position.
    // However, if target_abs_i is exactly at the end of a chunk, current_rel_i might be == chunk->size.
    // We need to handle the case where the cursor is *at* the start of the *next* chunk.
    if (current_rel_i == current_chunk->size && current_chunk->next != NULL && target_abs_i < buf->size ) {
         // Cursor is effectively at the beginning of the next chunk
         buf->cursor_chunk = current_chunk->next;
         buf->cursor_rel_i = 0;
    } else {
         // Cursor is within the current_chunk or at the very end of the buffer
         buf->cursor_chunk = current_chunk;
         buf->cursor_rel_i = current_rel_i;
    }
     // Safety check for cursor chunk if buffer is empty
    if (buf->size == 0) {
        buf->cursor_chunk = buf->begin; // Should be NULL or an empty chunk
        buf->cursor_rel_i = 0;
    }


    return RESULT_OK;
}

// Helper to calculate visual X column for a given absolute index on a given line
// (Inefficient: requires scan from line start)
int calculate_visual_x(struct bufclient* buf, int target_abs_y, int target_abs_i) {
    struct bufchunk* line_chunk;
    int line_rel_i, line_start_abs_i;
    int visual_x = 0;

    if (bufclient_find_line_start(buf, target_abs_y, &line_chunk, &line_rel_i, &line_start_abs_i) != RESULT_OK) {
        return 0;  // Error finding line start or line doesn't exist
    }

    // Handle case where target_abs_i is exactly at the start of the line
    if (target_abs_i == line_start_abs_i) {
        return 0;
    }


    struct bufchunk* current_chunk = line_chunk;
    int current_rel_i = line_rel_i;
    int current_abs_i = line_start_abs_i;

    while (current_chunk != NULL && current_abs_i < target_abs_i) {
        while (current_rel_i < current_chunk->size && current_abs_i < target_abs_i) {
            char c = current_chunk->data[current_rel_i];
            if (c == '\n') {
                // Should not happen if target_abs_i is on target_abs_y and before the end
                return visual_x; // Reached end of line before target_abs_i
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
        // Move to the next chunk if needed
        if (current_abs_i < target_abs_i) {
             current_chunk = current_chunk->next;
             current_rel_i = 0;
        } else {
             // We reached the target_abs_i exactly at the end of a chunk
             break;
        }
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
    int filename_len = strlen(buf->filename); // Get len before memset in bufclient_free
    if (filename_len > 0 && filename_len < sizeof(old_filename)) {
        memcpy(old_filename, buf->filename, filename_len + 1); // Include null terminator
    } else {
        old_filename[0] = '\0';
    }

    bufclient_free(buf);
    if (bufclient_init(buf) != RESULT_OK) { // Re-initialize to a single empty chunk
         die("Failed to re-initialize buffer after clear"); // Should not happen if alloc worked once
    }
    if(old_filename[0] != '\0') {
       strncpy(buf->filename, old_filename, sizeof(buf->filename) - 1); // Restore filename
       buf->filename[sizeof(buf->filename) - 1] = '\0';
    }
    buf->dirty = 1;                                               // Clearing makes it dirty unless it was already empty
}

enum RESULT bufclient_insert_char(struct bufclient* buf, char c) {
    // Find the effective insertion chunk/offset (handles cursor at end of chunk)
    struct bufchunk* insert_chunk = buf->cursor_chunk;
    int insert_rel_i = buf->cursor_rel_i;

    // If cursor is exactly at the end of a non-last chunk, insertion logically happens
    // at the start of the next chunk. This simplifies splitting logic.
    if (insert_chunk != NULL && insert_rel_i == insert_chunk->size && insert_chunk->next != NULL) {
        insert_chunk = insert_chunk->next;
        insert_rel_i = 0;
    }
    // Handle insertion into a completely empty buffer (begin is NULL)
    if (insert_chunk == NULL) {
        if (buf->begin != NULL) { // Should have at least one chunk after init/clear
            insert_chunk = buf->begin;
            insert_rel_i = 0;
        } else {
             // This case should ideally not happen if init worked.
             editorSetStatusMessage("Error: Buffer in inconsistent state during insert.");
             return RESULT_ERR;
        }
    }


    // Invalidate rowoff cache if inserting before its position
    if (buf->rowoff_chunk && buf->cursor_abs_i < buf->rowoff_abs_i) {
        buf->rowoff_chunk = NULL;
    }

    // Case 1: Current chunk has space
    if (insert_chunk->size < BUFCHUNK_SIZE) {
        if (insert_rel_i < insert_chunk->size) {  // Shift data only if inserting mid-chunk
            memmove(insert_chunk->data + insert_rel_i + 1, insert_chunk->data + insert_rel_i, insert_chunk->size - insert_rel_i);
        }
        insert_chunk->data[insert_rel_i] = c;
        insert_chunk->size++;

        // Update cursor position: stays logically after the inserted char
        buf->cursor_chunk = insert_chunk;
        buf->cursor_rel_i = insert_rel_i + 1;

    } else {
        // Case 2: Target chunk is full, need to allocate a new chunk *after* it
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

        // Determine if we need to split the current chunk or if insertion is exactly at the end.
        // If insert_rel_i is BUFCHUNK_SIZE, it means the cursor was at the end of the full chunk.
        if (insert_rel_i == BUFCHUNK_SIZE) {
            // Insertion is logically at the beginning of the new chunk.
            new_chunk->data[0] = c;
            new_chunk->size = 1;
            // Cursor goes to new_chunk, relative index 1 (after inserted char)
            buf->cursor_chunk = new_chunk;
            buf->cursor_rel_i = 1;
        } else {
            // Insertion is mid-chunk, requiring a split.
            // Move data from insertion point onwards to new_chunk
            int move_len = insert_chunk->size - insert_rel_i;
            if (move_len > 0) { // Should always be true if insert_rel_i < BUFCHUNK_SIZE
                memcpy(new_chunk->data, insert_chunk->data + insert_rel_i, move_len);
                new_chunk->size = move_len;
                insert_chunk->size = insert_rel_i;  // Truncate current chunk
            } else {
                 // Should not happen in this branch. If it did, new chunk is empty.
                 new_chunk->size = 0;
            }


            // Insert the new character into the *original* chunk (which now has space).
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

    // Update abs_y, abs_x (can be expensive, but necessary for accurate display/movement)
    // Use the full update for correctness, especially with newlines/tabs.
    // if (bufclient_update_cursor_coords(buf) != RESULT_OK) {
         // Handle error? Maybe try simpler update as fallback?
         // Simple update (less accurate but faster):
         if (c == '\n') {
             buf->cursor_abs_y++;
             buf->cursor_abs_x = 0;
         } else {
              // This simple update is often wrong with tabs/control chars.
              buf->cursor_abs_x++; // Very naive
              // Keeping the full update is safer.
         }
    // }

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

    //char deleted_char = del_chunk->data[del_rel_i]; // Keep track if needed for undo later

    // Shift data within the chunk to overwrite the deleted character
    // Make sure not to read past the end if deleting the last char
    if (del_rel_i + 1 < del_chunk->size) {
        memmove(del_chunk->data + del_rel_i, del_chunk->data + del_rel_i + 1, del_chunk->size - del_rel_i - 1);
    }
    // Else: deleting the last char, no move needed, just decrement size.

    del_chunk->size--;
    buf->size--;
    buf->dirty = 1;

    // Update cursor position (moves one step back logically)
    buf->cursor_abs_i--;
    // Find the chunk/rel_i for the *new* cursor position. It's the same as the deletion position.
    // No need to call find_pos again, just update fields directly.
    buf->cursor_chunk = del_chunk;
    buf->cursor_rel_i = del_rel_i;

    // Recalculate abs_y, abs_x (expensive but necessary)
    if (bufclient_update_cursor_coords(buf) != RESULT_OK) {
       editorSetStatusMessage("Warning: Cursor coordinate update failed after delete.");
    }
    buf->cursor_goal_x = buf->cursor_abs_x;

    // --- Chunk Merging ---
    // Condition 1: Check if deleting the character emptied the chunk (and it wasn't the only chunk)
    if (del_chunk->size == 0 && del_chunk != buf->begin) {
        struct bufchunk* prev_chunk = del_chunk->prev;
        struct bufchunk* next_chunk = del_chunk->next;  // Can be NULL

        // Unlink the empty chunk
        if (prev_chunk) { // Should always be true since del_chunk != buf->begin
           prev_chunk->next = next_chunk;
        } else {
            // This should not happen if del_chunk != buf->begin
            editorSetStatusMessage("Error: Buffer inconsistency during chunk merge (empty).");
            return RESULT_ERR;
        }
        if (next_chunk != NULL) {
            next_chunk->prev = prev_chunk;
        } else {
            buf->rbegin = prev_chunk;  // prev_chunk is now the last one
        }

        // Cursor must have been at the start of the (now deleted) chunk,
        // so move it to the end of the previous chunk.
        buf->cursor_chunk = prev_chunk;
        buf->cursor_rel_i = prev_chunk->size; // Cursor is now at the original deletion point

        bufchunk_free(del_chunk);  // Free the empty chunk

        // Point del_chunk to the previous chunk for the next merge check
        del_chunk = prev_chunk;
        // Now check if this 'prev_chunk' (now pointed to by del_chunk) can be merged with its next
    }

    // Condition 2: Check if we can merge the current chunk (del_chunk) with the *next* chunk
    // This check happens regardless of whether the previous condition was met.
    // If condition 1 was met, del_chunk now points to the chunk *before* the freed one.
    if (del_chunk != NULL && del_chunk->next != NULL) {
        struct bufchunk* next_chunk = del_chunk->next;
        // Merge if combined size fits
        if (del_chunk->size + next_chunk->size <= BUFCHUNK_SIZE) {

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

            // Cursor adjustment:
            // If the cursor ended up in the chunk that's about to be freed (next_chunk),
            // it needs to be moved back into del_chunk. This *shouldn't* happen
            // because deletion moves the cursor backward. But as a safeguard:
             if (buf->cursor_chunk == next_chunk) {
                 buf->cursor_chunk = del_chunk;
                 // The relative position needs adjustment: it's now old_del_chunk_size + old_cursor_rel_i
                 buf->cursor_rel_i += (del_chunk->size - next_chunk->size); // Size before merge
             }

            // Invalidate rowoff cache if it points to the merged chunk
            if (buf->rowoff_chunk == next_chunk) {
                 buf->rowoff_chunk = NULL; // Force recalc on next draw
            }


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
        if (bufclient_update_cursor_coords(buf) != RESULT_OK) {
             editorSetStatusMessage("Warning: Cursor coordinate update failed after move.");
        }
        // Set goal x when explicitly moving cursor
        buf->cursor_goal_x = buf->cursor_abs_x;
    } else {
         editorSetStatusMessage("Error: Failed to find position for cursor move.");
         // Optional: move to nearest valid position? (e.g., end of buffer)
         // buf->cursor_abs_i = buf->size;
         // bufclient_find_pos(buf, buf->cursor_abs_i, &buf->cursor_chunk, &buf->cursor_rel_i);
         // bufclient_update_cursor_coords(buf);
    }
}

// Move cursor based on ARROW_UP, DOWN, LEFT, RIGHT keys
void bufclient_move_cursor_relative(struct bufclient* buf, int key) {
    int current_abs_i = buf->cursor_abs_i;
    int target_abs_i = current_abs_i;
    // Use current cursor chunk/rel_i as potential starting points for efficiency
    struct bufchunk* target_chunk = buf->cursor_chunk;
    int target_rel_i = buf->cursor_rel_i;
    int need_full_update = 0; // Flag if efficient update fails

    switch (key) {
        case ARROW_LEFT:
            if (target_abs_i > 0) {
                target_abs_i--;
                // Efficiently update chunk/rel_i if possible
                if (target_rel_i > 0) {
                    target_rel_i--;
                } else if (target_chunk && target_chunk->prev != NULL) {
                    target_chunk = target_chunk->prev;
                    target_rel_i = target_chunk->size - 1; // Move to last char of prev chunk
                } else {
                    // At start of buffer or inconsistent state, need full find_pos
                    need_full_update = 1;
                }
            } else {
                return; // Already at start
            }
            break; // ARROW_LEFT

        case ARROW_RIGHT:
            if (target_abs_i < buf->size) {
                target_abs_i++;
                // Efficiently update chunk/rel_i
                if (target_chunk && target_rel_i < target_chunk->size) {
                    target_rel_i++;
                } else if (target_chunk && target_chunk->next != NULL) {
                    target_chunk = target_chunk->next;
                    target_rel_i = (target_chunk->size > 0) ? 0 : 0; // Move to first char of next chunk
                     // Correction: If target_abs_i is the very end, rel_i should be size
                     if (target_abs_i == buf->size) {
                          target_rel_i = target_chunk ? target_chunk->size : 0;
                     } else {
                          target_rel_i = 0; // Moving into next chunk, not at end
                     }

                } else {
                    // At end of buffer or inconsistent state, need full find_pos
                     // If moving right to the absolute end, handle specially
                     if (target_abs_i == buf->size) {
                         target_chunk = buf->rbegin;
                         target_rel_i = target_chunk ? target_chunk->size : 0;
                     } else {
                         need_full_update = 1;
                     }
                }
            } else {
                return; // Already at end
            }
            break; // ARROW_RIGHT

        case ARROW_UP: {
            if (buf->cursor_abs_y == 0)
                return; // Already on first line

            // Find start of previous line
            struct bufchunk* prev_line_start_chunk;
            int prev_line_start_rel_i, prev_line_start_abs_i;
            if (bufclient_find_line_start(buf, buf->cursor_abs_y - 1, &prev_line_start_chunk, &prev_line_start_rel_i, &prev_line_start_abs_i) != RESULT_OK)
                return; // Error finding previous line

            // Iterate from start of prev line to find position matching goal_x
            int visual_x = 0;
            target_abs_i = prev_line_start_abs_i; // Start searching from beginning of prev line
            struct bufchunk* search_chunk = prev_line_start_chunk;
            int search_rel_i = prev_line_start_rel_i;
            int search_done = 0; // Flag to break outer loop

            while (search_chunk != NULL && !search_done) {
                while (search_rel_i < search_chunk->size) {
                    char c = search_chunk->data[search_rel_i];
                    if (c == '\n') {
                        search_done = 1; // Reached end of line before goal_x
                        break; // Exit inner loop
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
                        search_done = 1;
                        break; // Exit inner loop
                    }
                    visual_x += char_width;
                    target_abs_i++;  // Advance target index
                    search_rel_i++;

                    // Update target chunk/rel_i as we iterate for efficiency later
                    target_chunk = search_chunk;
                    target_rel_i = search_rel_i;

                    // If we exactly hit the goal_x, stop *after* this char
                    if (visual_x == buf->cursor_goal_x) {
                        search_done = 1;
                        break; // Exit inner loop
                    }
                } // end inner while (search_rel_i < search_chunk->size)

                if (search_done) {
                    break; // Exit outer loop if flag is set
                }

                // Move to next chunk on the line
                search_chunk = search_chunk->next;
                search_rel_i = 0;
            } // end outer while (search_chunk != NULL && !search_done)
            // Target abs_i now points to the closest position on the previous line
            // Target chunk/rel_i *might* be correct if loop finished normally after update
            // If loop broke early, or hit edge cases, might need full update.
            need_full_update = 1; // Assume full update needed for simplicity/robustness here.

        } break; // ARROW_UP

        case ARROW_DOWN: {
            // Find start of next line
            struct bufchunk* next_line_start_chunk;
            int next_line_start_rel_i, next_line_start_abs_i;

            if (bufclient_find_line_start(buf, buf->cursor_abs_y + 1, &next_line_start_chunk, &next_line_start_rel_i, &next_line_start_abs_i) == RESULT_OK) {
                // Iterate from start of next line to find goal_x position
                int visual_x = 0;
                target_abs_i = next_line_start_abs_i;
                struct bufchunk* search_chunk = next_line_start_chunk;
                int search_rel_i = next_line_start_rel_i;
                int search_done = 0; // Flag to break outer loop

                while (search_chunk != NULL && !search_done) {
                    while (search_rel_i < search_chunk->size) {
                        char c = search_chunk->data[search_rel_i];
                        if (c == '\n') {
                            search_done = 1; // Reached end of line
                            break; // Exit inner loop
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
                             search_done = 1; // Stop before this char
                             break; // Exit inner loop
                        }
                        visual_x += char_width;
                        target_abs_i++;
                        search_rel_i++;

                        // Update target chunk/rel_i
                        target_chunk = search_chunk;
                        target_rel_i = search_rel_i;

                        if (visual_x == buf->cursor_goal_x) {
                            search_done = 1; // Stop after this char
                            break; // Exit inner loop
                        }
                    } // end inner while
                     if (search_done) {
                         break; // Exit outer loop
                     }
                    // Move to next chunk on the line
                    search_chunk = search_chunk->next;
                    search_rel_i = 0;
                } // end outer while

                // Ensure target_abs_i does not exceed buffer size
                if (target_abs_i > buf->size) {
                    target_abs_i = buf->size;
                    // Update target chunk/rel_i to end of buffer
                    target_chunk = buf->rbegin;
                    target_rel_i = target_chunk ? target_chunk->size : 0;
                } else {
                    // Need full update unless target_chunk/rel_i were perfectly updated
                     need_full_update = 1; // Assume full update needed
                }

            } else {
                // Already on the last line (or line after last), don't move down
                return;
            }
        } break; // ARROW_DOWN

        default: // Should not happen if called with arrow keys
            return;
    } // end switch(key)


    // --- Update Cursor State ---
    if (target_abs_i != buf->cursor_abs_i) {
        buf->cursor_abs_i = target_abs_i;

        // Use the efficiently tracked chunk/rel_i if possible, otherwise recalculate
        if (need_full_update || target_chunk == NULL) {
            if (bufclient_find_pos(buf, target_abs_i, &buf->cursor_chunk, &buf->cursor_rel_i) != RESULT_OK) {
                editorSetStatusMessage("Error: Failed to find position after relative move.");
                // Reset to old position? Or move to known good state (e.g., start/end)?
                buf->cursor_abs_i = current_abs_i; // Revert absolute index
                bufclient_find_pos(buf, buf->cursor_abs_i, &buf->cursor_chunk, &buf->cursor_rel_i); // Try to restore chunk/rel_i
                return; // Don't proceed with potentially bad state
            }
        } else {
            // Use the calculated target chunk/rel_i
            buf->cursor_chunk = target_chunk;
            buf->cursor_rel_i = target_rel_i;
        }

        // Update visual coordinates accurately
        if (bufclient_update_cursor_coords(buf) != RESULT_OK) {
            editorSetStatusMessage("Warning: Cursor coordinate update failed after relative move.");
        }

        // Only update goal_x on horizontal movement
        if (key == ARROW_LEFT || key == ARROW_RIGHT) {
            buf->cursor_goal_x = buf->cursor_abs_x;
        }
        // Note: Goal X is *not* updated on vertical movement, preserving the desired column.
    }
}


// *** Terminal Handling Implementation ***
void die(const char* s) {
    // Try to clear screen and restore terminal before exiting
    write(STDOUT_FILENO, "\x1b[2J", 4);  // Clear screen
    write(STDOUT_FILENO, "\x1b[H", 3);   // Move cursor to top-left
    disableRawMode();
    perror(s);  // Print error message related to errno
    exit(1);   // Use exit() for normal cleanup (if possible) vs _exit()
}

void disableRawMode() {
    // Restore original terminal settings if termios was successfully read
    // Check if c_lflag has meaningful bits set (basic check if termios was initialized)
    if (orig_termios.c_lflag != 0 || orig_termios.c_iflag != 0 || orig_termios.c_oflag != 0 || orig_termios.c_cflag != 0) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    }
}

enum RESULT enableRawMode() {
    if (!isatty(STDIN_FILENO)) {  // Check if stdin is a terminal
        errno = ENOTTY;
        fprintf(stderr, "Error: Standard input is not a terminal.\n");
        return RESULT_ERR;
    }
    if (tcgetattr(STDIN_FILENO, &orig_termios) == -1) {
        perror("tcgetattr failed");
        return RESULT_ERR;
    }

    // Ensure disableRawMode is called on exit
    atexit(disableRawMode);


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
    raw.c_cc[VTIME] = 1;  // 100ms timeout (1 decisecond)

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) {
        perror("tcsetattr failed");
        return RESULT_ERR;
    }
    return RESULT_OK;
}

// Get terminal window size using ioctl or fallback escape codes
enum RESULT getWindowSize(int* rows, int* cols) {
    struct winsize ws;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0 || ws.ws_row == 0) {
        // Fallback: Move cursor far right/down and query position
        // Ensure raw mode is enabled for this to work reliably with read() timeout
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) {
            fprintf(stderr, "Error: Failed write for window size fallback (1).\n");
            return RESULT_ERR;
        }
        // Send Device Status Report (DSR) query for cursor position (CPR - Cursor Position Report)
        if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) {
             fprintf(stderr, "Error: Failed write for window size fallback (2).\n");
             return RESULT_ERR;
        }

        // Read response: Esc [ rows ; cols R
        char buf[32];
        unsigned int i = 0;
        // Read char by char until 'R' or buffer full/timeout
        while (i < sizeof(buf) - 1) {
            // Use read with timeout (set by enableRawMode)
            if (read(STDIN_FILENO, &buf[i], 1) != 1) {
                 // Check if read timed out or failed
                 if (errno == EAGAIN) { // Timeout likely means no response
                      fprintf(stderr, "Error: Timeout reading window size response.\n");
                      return RESULT_ERR;
                 } else { // Other read error
                      perror("read window size");
                      return RESULT_ERR;
                 }
            }
            if (buf[i] == 'R')
                break; // Found end of response
            i++;
        }
        buf[i] = '\0';  // Null-terminate

        // Check if response starts correctly: Esc [
        if (i < 4 || buf[0] != '\x1b' || buf[1] != '[') {
             fprintf(stderr, "Error: Invalid window size response format: %s\n", buf);
             return RESULT_ERR;
        }

        // Parse rows and columns (simple sscanf)
        int r_parsed = 0, c_parsed = 0;
        if (sscanf(&buf[2], "%d;%d", &r_parsed, &c_parsed) != 2) {
            fprintf(stderr, "Error: Failed to parse window size response: %s\n", buf);
            return RESULT_ERR;
        }


        if (r_parsed <= 0 || c_parsed <= 0) {
             fprintf(stderr, "Error: Invalid dimensions from fallback: %d x %d\n", r_parsed, c_parsed);
             return RESULT_ERR; // Invalid dimensions
        }
        *rows = r_parsed;
        *cols = c_parsed;

         // Move cursor back to home after query
         write(STDOUT_FILENO, "\x1b[H", 3);

    } else {
        // ioctl succeeded
        *cols = ws.ws_col;
        *rows = ws.ws_row;
    }

    if (*cols <= 0 || *rows <= 0) {
         fprintf(stderr, "Error: Final window dimensions invalid: %d x %d\n", *rows, *cols);
         return RESULT_ERR;
    }
    return RESULT_OK;
}

// Read a key, handling escape sequences for arrows, home, end etc.
enum editorKey editorReadKey() {
    int nread;
    char c;
    // Loop until a key is read or an error occurs (excluding timeout/EAGAIN)
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        if (nread == -1 && errno != EAGAIN)
            die("read keypress");
        // Handle signals or other non-EAGAIN errors if necessary?
        // For now, just retry on EAGAIN (timeout).
    }

    if (c == '\x1b') {  // Potential escape sequence start
        char seq[3];
        // Try reading the next character with immediate timeout (non-blocking check)
        // Set VTIME = 0 briefly? No, rely on the 100ms default timeout.
        if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b'; // Just ESC pressed

        // Check for common sequence start '[' or 'O'
        if (seq[0] == '[') {
            if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b'; // Incomplete sequence ESC [

            if (seq[1] >= '0' && seq[1] <= '9') {
                // Extended sequence like Home, End, Del, PageUp/Down (e.g., Esc[3~)
                if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b'; // Incomplete ESC [ digit
                if (seq[2] == '~') {
                    switch (seq[1]) {
                        case '1': return HOME_KEY;  // Often ^[[1~
                        case '3': return DEL_KEY;   // Often ^[[3~
                        case '4': return END_KEY;   // Often ^[[4~
                        case '5': return PAGE_UP;
                        case '6': return PAGE_DOWN;
                        case '7': return HOME_KEY;  // Sometimes ^[[7~
                        case '8': return END_KEY;   // Sometimes ^[[8~
                        default: return '\x1b';    // Unrecognized number
                    }
                } else {
                     // Possibly other sequences like ESC [ 1 ; 5 C for Ctrl+Right? Ignore for now.
                    return '\x1b'; // Malformed or unrecognized sequence
                }
            } else {
                // Standard CSI sequences (e.g., arrow keys)
                switch (seq[1]) {
                    case 'A': return ARROW_UP;    // Esc[A
                    case 'B': return ARROW_DOWN;  // Esc[B
                    case 'C': return ARROW_RIGHT; // Esc[C
                    case 'D': return ARROW_LEFT;  // Esc[D
                    case 'H': return HOME_KEY;    // Sometimes Esc[H (xterm)
                    case 'F': return END_KEY;     // Sometimes Esc[F (linux console)
                    default: return '\x1b';      // Unrecognized letter
                }
            }
        } else if (seq[0] == 'O') {
            // Alternate sequences (e.g., from VT100 keypad/linux console)
            if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b'; // Incomplete ESC O
            switch (seq[1]) {
                case 'H': return HOME_KEY; // EscOH
                case 'F': return END_KEY;  // EscOF
                // Arrows might be EscOA, EscOB, etc. Add if needed for specific terminals.
                // case 'A': return ARROW_UP;
                // case 'B': return ARROW_DOWN;
                // case 'C': return ARROW_RIGHT;
                // case 'D': return ARROW_LEFT;
                default: return '\x1b';
            }
        } else {
            // Escape followed by something other than [ or O (e.g., Alt+key might send ESC char)
             // We could potentially handle Alt sequences here if needed.
            return '\x1b'; // Treat as plain ESC for now
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
        if (len <= 0) {
             // Optionally log an error here, screen updates will be incomplete.
             // fprintf(stderr, "Screen buffer overflow!\n");
             return; // No space left at all
        }
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
    // Clearing line-by-line (using \x1b[K) during draw prevents full screen clear flicker.
    screenbuf_append("\x1b[H", 3);
}

// Adjust rowoff/coloff to ensure cursor is visible on screen
void editorScroll() {
    // Ensure cursor coordinates are up-to-date before scrolling
    // This might have already been done by the operation causing the scroll,
    // but doing it again ensures correctness if multiple operations happened.
    // bufclient_update_cursor_coords(&textbuf); // Can be slow, assume coords are current

    // Check if cursor Y is valid relative to rowoff
    if (textbuf.cursor_abs_y < textbuf.rowoff) {
        textbuf.rowoff = textbuf.cursor_abs_y;
        textbuf.rowoff_chunk = NULL; // Invalidate cache when scrolling up
    }
    if (textbuf.cursor_abs_y >= textbuf.rowoff + screenrows) {
        textbuf.rowoff = textbuf.cursor_abs_y - screenrows + 1;
        textbuf.rowoff_chunk = NULL; // Invalidate cache when scrolling down
    }


    // Check if cursor X is valid relative to coloff
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

    // --- Find starting position for rendering (based on rowoff) ---
    // Use cache if valid, otherwise recalculate
    if (textbuf.rowoff_chunk == NULL && textbuf.rowoff > 0) { // Only recalc if not row 0
        if (bufclient_find_line_start(&textbuf, textbuf.rowoff, &textbuf.rowoff_chunk, &textbuf.rowoff_rel_i, &textbuf.rowoff_abs_i) != RESULT_OK) {
            // Error finding start row (e.g., rowoff beyond buffer end after delete?)
            // Reset to top of buffer as fallback
            textbuf.rowoff = 0;
            textbuf.rowoff_chunk = textbuf.begin;
            textbuf.rowoff_rel_i = 0;
            textbuf.rowoff_abs_i = 0;
        }
    } else if (textbuf.rowoff == 0) {
         // If rowoff is 0, start at the beginning of the buffer
         textbuf.rowoff_chunk = textbuf.begin;
         textbuf.rowoff_rel_i = 0;
         textbuf.rowoff_abs_i = 0;
    }
    // Ensure initial state is valid even if rowoff was bad or buffer empty
    if (textbuf.begin == NULL) { // Handle completely empty buffer case
         for (y = 0; y < screenrows; y++) {
             if (y == screenrows / 3) { // Show welcome on empty
                char welcome[80];
                int welcome_len = snprintf(welcome, sizeof(welcome), "lkjsxceditor v%s -- %d chunks free", LKJSXCEDITOR_VERSION, BUFCHUNK_COUNT - bufchunk_pool_used);
                if (welcome_len > screencols) welcome_len = screencols;
                int padding = (screencols - welcome_len) / 2;
                if (padding > 0) { screenbuf_append("~", 1); padding--; }
                while (padding-- > 0) screenbuf_append(" ", 1);
                if (welcome_len > 0) screenbuf_append(welcome, welcome_len);
             } else {
                screenbuf_append("~", 1);
             }
             screenbuf_append("\x1b[K", 3); // Clear rest of line
             screenbuf_append("\r\n", 2);
         }
         return; // Nothing more to draw
    } else if (textbuf.rowoff_chunk == NULL) {
         // If cache was invalid and recalc failed or wasn't needed (rowoff=0), default to start
         textbuf.rowoff_chunk = textbuf.begin;
         textbuf.rowoff_rel_i = 0;
         textbuf.rowoff_abs_i = 0;
    }

    // --- Set initial pointers for rendering loop ---
    current_chunk = textbuf.rowoff_chunk;
    current_rel_i = textbuf.rowoff_rel_i;
    current_abs_i = textbuf.rowoff_abs_i;


    // --- Iterate through each row of the terminal screen ---
    for (y = 0; y < screenrows; y++) {
        int file_line_abs_y = textbuf.rowoff + y; // The absolute line number we are trying to render
        int line_render_finished = 0; // Flag to signal moving to next screen row early

        // Check if we are trying to draw past the actual content
        // A simple check: if current_abs_i >= textbuf.size, we are done.
        if (current_abs_i >= textbuf.size && textbuf.size > 0) {
             // Draw tildes for remaining screen rows
             if (textbuf.size == 0 && y == screenrows / 3) { // Welcome message if buffer became empty
                 char welcome[80];
                 int welcome_len = snprintf(welcome, sizeof(welcome), "lkjsxceditor v%s -- %d chunks free", LKJSXCEDITOR_VERSION, BUFCHUNK_COUNT - bufchunk_pool_used);
                 if (welcome_len > screencols) welcome_len = screencols;
                 int padding = (screencols - welcome_len) / 2;
                 if (padding > 0) { screenbuf_append("~", 1); padding--; }
                 while (padding-- > 0) screenbuf_append(" ", 1);
                 if (welcome_len > 0) screenbuf_append(welcome, welcome_len);
             } else {
                 screenbuf_append("~", 1);
             }
             line_render_finished = 1; // No more content to draw for this or subsequent rows
        } else {
            // --- Render the current file line char by char ---
            int line_visual_col = 0; // Visual column within the *file* line
            // Temporary pointers for line traversal, starting from current position
            struct bufchunk* line_chunk = current_chunk;
            int line_rel_i = current_rel_i;
            int line_abs_i = current_abs_i;

            while (line_chunk != NULL && !line_render_finished) {
                while (line_rel_i < line_chunk->size) {
                    char c = line_chunk->data[line_rel_i];

                    if (c == '\n') {
                        // End of the current file line found
                        // Advance main pointers past the newline for the *next* screen row iteration
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
                        line_render_finished = 1; // Mark line as done
                        break; // Exit inner char loop
                    }

                    // Calculate width of current character and its representation
                    int char_width = 0;
                    char display_buf[TAB_STOP + 3]; // Max width for tab or ^X
                    int display_len = 0;

                    if (c == '\t') {
                        char_width = (TAB_STOP - (line_visual_col % TAB_STOP));
                        memset(display_buf, ' ', char_width); // Fill with spaces
                        display_len = char_width;
                    } else if (iscntrl((unsigned char)c)) {
                        char_width = 2;
                        display_buf[0] = '^';
                        display_buf[1] = ((c & 0x1f) + '@'); // Map 0-31 to @, A, B...
                        display_len = 2;
                    } else {
                        char_width = 1;
                        display_buf[0] = c;
                        display_len = 1;
                    }
                    //display_buf[display_len] = '\0'; // Not needed for screenbuf_append

                    // --- Horizontal Scrolling Logic ---
                    int char_end_visual_col = line_visual_col + char_width;
                    int screen_x = line_visual_col - textbuf.coloff; // Screen col where char *starts*

                    // Check if this character is *at least partially* visible on screen
                    if (char_end_visual_col > textbuf.coloff && screen_x < screencols) {
                        int append_len = display_len;
                        const char* append_ptr = display_buf;

                        // Adjust if character starts before coloff
                        if (screen_x < 0) {
                            // Character starts off-screen left, clip its representation
                            int clip_amount = -screen_x; // How many visual columns to clip
                            if (clip_amount < char_width) {
                                // Partial clip (e.g. part of a tab visible)
                                // Simple approach: Draw spaces for the visible part of tab/ctrl
                                if (c == '\t' || iscntrl((unsigned char)c)) {
                                     memset(display_buf, ' ', char_width - clip_amount);
                                     append_ptr = display_buf;
                                     append_len = char_width - clip_amount;
                                } else { // Normal char shouldn't be partially clipped this way
                                     append_len = 0; // Don't draw clipped single char
                                }
                                screen_x = 0; // Starts at screen column 0 now
                            } else {
                                append_len = 0; // Fully clipped
                            }
                        }

                        // Adjust if character ends after screencols
                        if (screen_x + append_len > screencols) {
                             append_len = screencols - screen_x;
                        }

                        // Append the visible part if any length remains
                        if (append_len > 0) {
                             screenbuf_append(append_ptr, append_len);
                        }
                    } else if (screen_x >= screencols) {
                         // Character starts beyond the right edge. Stop rendering this line.
                         // We need to advance the main pointers to the start of the next line.
                          struct bufchunk* scan_chunk = line_chunk;
                          int scan_rel_i = line_rel_i;
                          int scan_abs_i = line_abs_i;
                          int found_newline = 0;
                          while(scan_chunk && !found_newline) {
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
                                      found_newline = 1;
                                      break; // Exit inner scan loop
                                  }
                                  scan_rel_i++; scan_abs_i++;
                             }
                             if (found_newline) break; // Exit outer scan loop
                             scan_chunk = scan_chunk->next; scan_rel_i = 0;
                          }
                          // If no newline found, we reached end of buffer
                          if (!found_newline) {
                              current_chunk = NULL; current_rel_i = 0; current_abs_i = textbuf.size;
                          }
                          line_render_finished = 1; // Mark line as done
                          break; // Exit the inner char loop (while line_rel_i < size)
                    }
                    // --- End Horizontal Scrolling Logic ---

                    line_visual_col += char_width; // Advance visual column on the file line
                    line_rel_i++;
                    line_abs_i++;

                } // end while (line_rel_i < line_chunk->size)

                if (line_render_finished) {
                    break; // Exit outer chunk loop if line finished early
                }

                // Move to the next chunk if needed for this line
                line_chunk = line_chunk->next;
                line_rel_i = 0;

            } // end while (line_chunk != NULL)

            // If we exit the loop because we ran out of buffer content before finding '\n'
            // (i.e., last line doesn't end with newline)
            if (!line_render_finished) {
                 current_chunk = NULL; // Signal end for next screen row iteration
                 current_rel_i = 0;
                 current_abs_i = textbuf.size;
                 line_render_finished = 1; // Mark this line as finished too
            }
        } // end else (not past end of content)

        // --- Finish the screen row ---
        screenbuf_append("\x1b[K", 3); // Clear rest of the screen line from cursor onwards
        screenbuf_append("\r\n", 2);   // Move to the beginning of the next screen line
    } // end for each screen row y
}


// Draw the status bar at the bottom
void editorDrawStatusBar() {
    screenbuf_append("\x1b[7m", 4);  // Invert colors (enter reverse video mode)

    char status[128], rstatus[64];
    int len = 0, rlen = 0;
    int status_max_len = sizeof(status) -1;
    int rstatus_max_len = sizeof(rstatus) -1;


    // Left part: Mode, Filename, Dirty status
    const char* mode_str;
    switch (mode) {
        case MODE_NORMAL:   mode_str = "-- NORMAL --"; break;
        case MODE_INSERT:   mode_str = "-- INSERT --"; break;
        case MODE_COMMAND:  mode_str = "-- COMMAND --"; break;
        default:            mode_str = "?? UNKNOWN ??"; break;
    }
    // Use snprintf carefully to avoid overflow
    len = snprintf(status, status_max_len + 1, " %.15s %.40s%s",
                   mode_str,
                   textbuf.filename[0] ? textbuf.filename : "[No Name]",
                   textbuf.dirty ? " [+]" : "");
     if (len < 0) len = 0; // Handle snprintf error
     if (len > status_max_len) len = status_max_len; // Ensure truncation


    // Right part: Line/TotalLines, Percentage
    // Count total lines (inefficiently - consider caching this if performance is an issue)
    int total_lines = 0;
    if (textbuf.size > 0) {
        struct bufchunk* ch = textbuf.begin;
        int i;
        total_lines = 1; // Start with 1 line even if empty or no newlines
        int last_char_was_newline = 0;
        while (ch) {
            for (i = 0; i < ch->size; ++i) {
                if (ch->data[i] == '\n') {
                    total_lines++;
                    last_char_was_newline = 1;
                } else {
                     last_char_was_newline = 0;
                }
            }
            ch = ch->next;
        }
         // If the file ends without a newline, the last line wasn't counted by the loop increment.
         // However, our logic already counts it implicitly unless the file is truly empty.
         // Let's adjust: If the file is non-empty and doesn't end with \n, total_lines is correct.
         // If file is empty, total_lines should be 1.
         // If file ends with \n, total_lines is correct.
         // It seems the count is okay as is. Let's refine the calculation for empty/single line.

         if (textbuf.size == 0) {
             total_lines = 1; // Show 1/1 for empty file
         }

    } else {
        total_lines = 1; // Empty buffer is considered 1 line
    }

    // Calculate percentage (handle division by zero)
    int percent = 100;
    if (total_lines > 0) { // Use the calculated total_lines
         // Calculate cursor Y + 1 based on current Y coord
         int display_line = textbuf.cursor_abs_y + 1;
         // Prevent division by zero if total_lines somehow became 0
         if (total_lines > 0) {
            percent = (int)(((long long)display_line * 100) / total_lines);
         } else {
             percent = 100; // Or 0? 100 seems reasonable for "end" of empty file
         }
    }
    if (percent > 100) percent = 100; // Clamp top
    if (percent < 0) percent = 0; // Clamp bottom (shouldn't happen)


    rlen = snprintf(rstatus, rstatus_max_len + 1, "%d/%d %3d%% ",
                    textbuf.cursor_abs_y + 1, total_lines, percent);
     if (rlen < 0) rlen = 0;
     if (rlen > rstatus_max_len) rlen = rstatus_max_len;


    // Render left status, truncated if necessary by screen width
    if (len > screencols) len = screencols;
    if (len > 0) screenbuf_append(status, len);

    // Render right status with padding in between
    int current_col = len;
    while (current_col < screencols) {
        if (screencols - current_col == rlen) {  // If right status fits exactly
            if (rlen > 0) screenbuf_append(rstatus, rlen);
            current_col += rlen;
            break; // Done filling
        } else {
            screenbuf_append(" ", 1);  // Add padding space
            current_col++;
        }
    }
    // Ensure the line is filled if rstatus didn't fit
    while(current_col < screencols) {
         screenbuf_append(" ", 1);
         current_col++;
    }


    screenbuf_append("\x1b[m", 3);  // Reset colors (exit reverse video mode)
    // No \r\n needed here, editorDrawCommandLine will follow on the next line
}

// Draw the command/status message line below the status bar
void editorDrawCommandLine() {
    screenbuf_append("\x1b[K", 3);  // Clear the line first

    time_t current_time = time(NULL);
    if (mode == MODE_COMMAND) {
        // Show command prompt and current command buffer
        statusbuf[0] = '\0';      // Clear any timed status message when entering command mode
        statusbuf_time = 0;
        int prompt_len = 1;       // Length of ":"
        screenbuf_append(":", prompt_len);
        int cmd_display_len = cmdbuf_len;

        // Handle command longer than screen width (basic truncation from left?)
        // Simple: just truncate from the right for now.
        int max_cmd_display = screencols - prompt_len;
        if (max_cmd_display < 0) max_cmd_display = 0; // Handle tiny screens
        if (cmd_display_len > max_cmd_display) {
            cmd_display_len = max_cmd_display;
        }
        if (cmd_display_len > 0) {
             screenbuf_append(cmdbuf, cmd_display_len);
        }
        // Cursor will be positioned after the command text later in editorRefreshScreen
    } else if (statusbuf[0] != '\0' && statusbuf_time > 0 && current_time - statusbuf_time < 5) {
        // Display timed status message if active and not expired (5 seconds)
        int msg_len = strlen(statusbuf);
        if (msg_len > screencols) msg_len = screencols; // Truncate if needed
        if (msg_len > 0) screenbuf_append(statusbuf, msg_len);
    } else {
        // Clear status message if expired or not in command mode and no message set
        if (statusbuf_time > 0) { // Only clear if it was a timed message
             statusbuf[0] = '\0';
             statusbuf_time = 0;
        }
         // Leave persistent messages (statusbuf_time == 0) alone unless command mode overwrites.
    }
}

// Refresh the entire screen content based on current editor state
void editorRefreshScreen() {
    editorScroll();     // Ensure cursor position is valid for scrolling offsets
    screenbuf_clear();  // Reset buffer and add initial control sequences (\x1b[?25l \x1b[H)

    editorDrawRows();         // Draw text content
    editorDrawStatusBar();    // Draw status bar (includes \x1b[7m ... \x1b[m)
    editorDrawCommandLine();  // Draw command/message line (includes \x1b[K)

    // Calculate final cursor position on screen (1-based)
    int screen_cursor_y = textbuf.cursor_abs_y - textbuf.rowoff + 1;
    int screen_cursor_x = textbuf.cursor_abs_x - textbuf.coloff + 1;

    // Clamp cursor position to screen boundaries if something went wrong
    if (screen_cursor_y < 1) screen_cursor_y = 1;
    if (screen_cursor_y > screenrows) screen_cursor_y = screenrows;
    if (screen_cursor_x < 1) screen_cursor_x = 1;
    if (screen_cursor_x > screencols) screen_cursor_x = screencols;


    // Adjust cursor position if in command mode
    if (mode == MODE_COMMAND) {
        screen_cursor_y = screenrows + 2; // Command line is row below status bar
        // Calculate X position based on prompt + command length, respecting screen width
        int cmd_cursor_x = 1 + cmdbuf_len + 1; // 1-based: ':' + text + cursor
        if (cmd_cursor_x > screencols) cmd_cursor_x = screencols;
        screen_cursor_x = cmd_cursor_x;
    }

    // Generate cursor positioning command (Move cursor)
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "\x1b[%d;%dH", screen_cursor_y, screen_cursor_x);
    if (len > 0 && len < sizeof(buf)) {
         screenbuf_append(buf, len);
    }

    // Show cursor again after positioning it
    screenbuf_append("\x1b[?25h", 6);

    // Write the entire accumulated screen buffer to standard output in one go
    if (write(STDOUT_FILENO, screenbuf, screenbuf_len) == -1) {
        // Avoid die() here as it might try writing again. Exit directly.
        perror("Fatal: write to screen failed");
        disableRawMode(); // Try to restore terminal
        exit(1);
    }
    // Reset buffer length for next refresh cycle
    // screenbuf_len = 0; // Done by screenbuf_clear() at the start
}

// *** File I/O Implementation ***

// Open a file and load its content into the text buffer
enum RESULT editorOpen(const char* filename) {
    if (!filename || filename[0] == '\0') {
         editorSetStatusMessage("Error: No filename specified for open.");
         return RESULT_ERR;
    }

    FILE* fp = fopen(filename, "r");
    if (!fp) {
        // File doesn't exist, treat as a new file
        if (errno == ENOENT) {
            strncpy(textbuf.filename, filename, sizeof(textbuf.filename) - 1);
            textbuf.filename[sizeof(textbuf.filename) - 1] = '\0';
            bufclient_clear(&textbuf);  // Ensure buffer is empty for new file
             // bufclient_clear sets dirty=1, which is correct for a new *unsaved* file buffer.
             // Let's reset dirty=0, as the file itself (non-existent) isn't modified.
             textbuf.dirty = 0;
            editorSetStatusMessage("New file"); // Overwrite status from clear
            // Reset cursor etc. just in case clear didn't fully reset
            bufclient_move_cursor_to(&textbuf, 0);
            return RESULT_OK;
        } else {
            // Other error opening file
            char err_msg[sizeof(textbuf.filename) + 64];
            snprintf(err_msg, sizeof(err_msg), "Error opening '%s': %s", filename, strerror(errno));
            editorSetStatusMessage(err_msg);
            // Don't change current buffer if open fails
            return RESULT_ERR;
        }
    }

    // File exists, store filename and clear current buffer content *before* loading
    strncpy(textbuf.filename, filename, sizeof(textbuf.filename) - 1);
    textbuf.filename[sizeof(textbuf.filename) - 1] = '\0';
    bufclient_clear(&textbuf);  // Clear existing buffer before loading

    char readbuf[FILE_BUF_SIZE];
    size_t nread;
    enum RESULT res = RESULT_OK;
    long long total_read = 0;
    int io_error = 0;

    // Read file in chunks and insert characters into buffer client
    while ((nread = fread(readbuf, 1, sizeof(readbuf), fp)) > 0) {
        size_t i;
        total_read += nread;
        for (i = 0; i < nread; i++) {
            // TODO: Handle potential CR/LF conversion? For simplicity, store as is.
            if (bufclient_insert_char(&textbuf, readbuf[i]) != RESULT_OK) {
                editorSetStatusMessage("Error loading file: Out of memory?");
                res = RESULT_ERR;
                io_error = 1; // Mark error to stop loop
                break; // Exit inner loop
            }
        }
        if (io_error) break; // Exit outer loop if memory error occurred
    }

    // Check for read errors after loop (if not already memory error)
    if (!io_error && ferror(fp)) {
        char err_msg[sizeof(textbuf.filename) + 64];
        snprintf(err_msg, sizeof(err_msg), "Error reading '%s': %s", filename, strerror(errno));
        editorSetStatusMessage(err_msg);
        res = RESULT_ERR;
    }

    fclose(fp);

    if (res == RESULT_OK) {
        textbuf.dirty = 0;                      // File just loaded is not dirty
        bufclient_move_cursor_to(&textbuf, 0);  // Move cursor to start of file
        char status[sizeof(textbuf.filename) + 32];
        snprintf(status, sizeof(status), "Opened \"%s\" (%lld bytes)", textbuf.filename, total_read);
        editorSetStatusMessage(status);
    } else {
         // If loading failed (memory or read error), buffer might be partially loaded.
         // It's already marked dirty by clear/insert. Keep it that way.
         // Cursor position might be arbitrary. Resetting to 0 is safest.
         bufclient_move_cursor_to(&textbuf, 0);
    }
    return res;
}

// Save the current text buffer content to the associated filename
enum RESULT editorSave() {
    if (textbuf.filename[0] == '\0') {
        // Need to prompt for filename in command mode before calling save again
        editorSetStatusMessage("No filename. Use :w <filename>");
        return RESULT_ERR;
    }

    // Open file for writing (truncates existing file or creates new)
    // Use "wb" for binary mode to avoid CR/LF translation issues on Windows if ported
    FILE* fp = fopen(textbuf.filename, "wb");
    if (!fp) {
        char err_msg[sizeof(textbuf.filename) + 64];
        snprintf(err_msg, sizeof(err_msg), "Error saving '%s': %s", textbuf.filename, strerror(errno));
        editorSetStatusMessage(err_msg);
        return RESULT_ERR;
    }

    // Iterate through buffer chunks and write data to file
    struct bufchunk* current = textbuf.begin;
    long long total_written = 0;
    enum RESULT res = RESULT_OK;
    int write_error = 0;

    while (current != NULL) {
        if (current->size > 0) {  // Only write if chunk has data
            size_t written = fwrite(current->data, 1, current->size, fp);
            if (written != (size_t)current->size) {
                char err_msg[64];
                 // ferror() or feof() might give more info, but strerror(errno) is often useful
                snprintf(err_msg, sizeof(err_msg), "Write error: %s", strerror(errno));
                editorSetStatusMessage(err_msg);
                res = RESULT_ERR;
                write_error = 1;
                break;  // Stop writing on error
            }
            total_written += written;
        }
        current = current->next;
    }

    // Check for close errors (e.g., disk full on final flush)
    if (fclose(fp) != 0) {
        // If no write error occurred yet, report the close error
        if (!write_error) {
            char err_msg[64];
            snprintf(err_msg, sizeof(err_msg), "Error closing file after write: %s", strerror(errno));
            editorSetStatusMessage(err_msg);
            res = RESULT_ERR;
        }
        // If a write error already happened, the close error is secondary.
    }

    if (res == RESULT_OK) {
        textbuf.dirty = 0;  // Mark buffer as clean after successful save and close
        char status[sizeof(textbuf.filename) + 32];
        snprintf(status, sizeof(status), "\"%s\" %lld bytes written", textbuf.filename, total_written);
        editorSetStatusMessage(status);
    } else {
        // If save failed, the buffer remains dirty.
        textbuf.dirty = 1;
    }
    return res;
}

// *** Editor Operations Implementation ***

// Initialize editor state: terminal, screen size, buffers
void initEditor() {
    // Setup exit handler for terminal restoration *before* enabling raw mode
    // atexit(disableRawMode); // Moved inside enableRawMode to ensure orig_termios is set first

    bufchunk_pool_init();  // Initialize memory pool first
    if (bufclient_init(&textbuf) != RESULT_OK) {
        // Use fprintf directly as die() might rely on editor state not yet set
        fprintf(stderr, "Fatal: Failed to initialize text buffer memory.\n");
        exit(1); // Cannot continue without buffer
    }

    // Initialize static buffers
    cmdbuf[0] = '\0';
    cmdbuf_len = 0;
    statusbuf[0] = '\0';
    statusbuf_time = 0;
    mode = MODE_NORMAL;

    // Enable raw mode (includes getting original termios and setting atexit handler)
    if (enableRawMode() == RESULT_ERR) {
         // Error message printed by enableRawMode or tcgetattr/tcsetattr
         exit(1);
    }


    // Get terminal dimensions *after* raw mode potentially needed for fallback
    int total_rows = 0, total_cols = 0;
    if (getWindowSize(&total_rows, &total_cols) == RESULT_ERR) {
         // Error messages printed by getWindowSize or its helpers
         // Still try to disable raw mode before dying
         disableRawMode();
         fprintf(stderr, "Fatal: Could not determine terminal size.\n");
         exit(1);
    }

    screencols = total_cols;
    // Reserve bottom 2 rows for status bar and command line
    screenrows = total_rows - 2;
    if (screenrows < 1) {
         disableRawMode();
         fprintf(stderr, "Fatal: Terminal too small (need at least 3 rows total).\n");
         exit(1);
    }

     // Ensure screen buffer is large enough (optional check)
     if (SCREEN_BUF_SIZE < (size_t)(screencols * total_rows * 4)) { // Estimate worst case VT100 sequences
          // This isn't fatal, but drawing might truncate.
          // fprintf(stderr, "Warning: Screen buffer might be small for terminal size.\n");
     }
}

// Set the status message displayed at the bottom line
void editorSetStatusMessage(const char* msg) {
    if (msg == NULL) {
        statusbuf[0] = '\0';
        statusbuf_time = 0; // Clear timed message flag
        return;
    }
    strncpy(statusbuf, msg, STATUS_BUF_SIZE - 1);
    statusbuf[STATUS_BUF_SIZE - 1] = '\0';  // Ensure null termination
    statusbuf_time = time(NULL);            // Record time for timeout display (5 seconds)
}

// Process the command entered in command mode (: line)
void editorProcessCommand() {
    cmdbuf[cmdbuf_len] = '\0';  // Null-terminate the received command

    // Trim trailing whitespace (simple version)
    char* end = cmdbuf + cmdbuf_len - 1;
    while (end >= cmdbuf && isspace((unsigned char)*end)) {
        *end = '\0';
        end--;
        cmdbuf_len--;
    }

    // Trim leading whitespace
    char* start = cmdbuf;
    while (*start && isspace((unsigned char)*start)) {
        start++;
    }
    // If trimming occurred, move the command content
    if (start > cmdbuf) {
         memmove(cmdbuf, start, strlen(start) + 1); // Include null terminator
         cmdbuf_len = strlen(cmdbuf);
    }


    if (cmdbuf_len == 0) {  // Empty command after trimming
        mode = MODE_NORMAL;
        editorSetStatusMessage(""); // Clear ":" prompt
        return;
    }

    // --- Command Matching ---
    if (strcmp(cmdbuf, "q") == 0) {
        if (textbuf.dirty && QUIT_TIMES > 0) {
            // Static quit_confirm counter is problematic if user tries :q, then :w, then :q again.
            // Better: Just check dirty flag each time.
            editorSetStatusMessage("Unsaved changes! Use :wq to save and quit, or :q! to discard.");
             mode = MODE_NORMAL; // Return to normal mode after failed quit attempt
        } else {
            terminate_editor = 1;  // Quit normally (buffer clean or no confirmation needed)
        }
    } else if (strcmp(cmdbuf, "q!") == 0) {
        terminate_editor = 1;  // Force quit, discard changes
    } else if (strcmp(cmdbuf, "w") == 0) {
        if (editorSave() == RESULT_OK) {
            // Status set by editorSave
        } // Else: status set by editorSave on error
        mode = MODE_NORMAL;
    } else if (strcmp(cmdbuf, "wq") == 0) {
        if (editorSave() == RESULT_OK) {
            terminate_editor = 1; // Quit if save was successful
        } else {
            // Save failed, status message set by editorSave
            mode = MODE_NORMAL; // Stay in editor
        }
    } else if (strncmp(cmdbuf, "w ", 2) == 0) {
        // Save As: :w <filename>
        char* filename = cmdbuf + 2;
        // Trim leading whitespace from filename (already done globally, but safe to repeat)
        while (*filename && isspace((unsigned char)*filename)) filename++;

        if (filename[0] != '\0') {
            // Update buffer's filename before saving
            strncpy(textbuf.filename, filename, sizeof(textbuf.filename) - 1);
            textbuf.filename[sizeof(textbuf.filename) - 1] = '\0';
            editorSave();  // Save to the new filename (status set by save)
        } else {
            editorSetStatusMessage("Filename missing for :w command");
        }
        mode = MODE_NORMAL;
    } else if (strncmp(cmdbuf, "e ", 2) == 0) {
        // Edit file: :e <filename>
        char* filename = cmdbuf + 2;
        while (*filename && isspace((unsigned char)*filename)) filename++;

        if (filename[0] != '\0') {
            if (textbuf.dirty) {
                editorSetStatusMessage("Unsaved changes! Save or use :e! to discard.");
            } else {
                editorOpen(filename); // Status set by open
            }
        } else {
            editorSetStatusMessage("Filename missing for :e command");
        }
        mode = MODE_NORMAL;
    } else if (strncmp(cmdbuf, "e!", 2) == 0 && isspace((unsigned char)cmdbuf[2])) { // Check for space after e!
        // Edit file force: :e! <filename>
        char* filename = cmdbuf + 3; // Start after "e! "
        while (*filename && isspace((unsigned char)*filename)) filename++;

        if (filename[0] != '\0') {
            editorOpen(filename);  // Discard changes and open (status set by open)
        } else {
            editorSetStatusMessage("Filename missing for :e! command");
        }
        mode = MODE_NORMAL;
    }
    // --- Add other commands here ---
    // Example: Go to line number
    else if (isdigit((unsigned char)cmdbuf[0])) {
         int line_num = atoi(cmdbuf);
         if (line_num > 0) {
             // Find absolute index for start of target line (1-based input -> 0-based internal)
             struct bufchunk* target_chunk;
             int target_rel_i, target_abs_i;
             if (bufclient_find_line_start(&textbuf, line_num - 1, &target_chunk, &target_rel_i, &target_abs_i) == RESULT_OK) {
                 bufclient_move_cursor_to(&textbuf, target_abs_i);
             } else {
                 // Line number out of range, move to end? Or start? Or stay put?
                 // Let's move to the end of the buffer.
                 bufclient_move_cursor_to(&textbuf, textbuf.size);
                 editorSetStatusMessage("Line number out of range");
             }
         } else {
             editorSetStatusMessage("Invalid line number");
         }
         mode = MODE_NORMAL;
    }
    // --- End other commands ---
    else {
        // Unknown command
        char err_msg[CMD_BUF_SIZE + 32];
        snprintf(err_msg, sizeof(err_msg), "Unknown command: '%.*s'", CMD_BUF_SIZE, cmdbuf);
        editorSetStatusMessage(err_msg);
        mode = MODE_NORMAL;
    }

    // Clear command buffer for next time if returning to normal mode
    if (mode == MODE_NORMAL) {
         cmdbuf[0] = '\0';
         cmdbuf_len = 0;
    }
}

// Process the next keypress from the user based on the current editor mode
void editorProcessKeypress() {
    enum editorKey c = editorReadKey();

    // --- Global Keybinds (if any, e.g., resize handling) ---
    // None implemented here yet.

    // --- Mode-Specific Key Presses ---
    switch (mode) {
        case MODE_NORMAL:
            switch (c) {
                // --- Mode Changes ---
                case 'i':  // Enter Insert mode (at cursor)
                    mode = MODE_INSERT;
                    editorSetStatusMessage("-- INSERT --");
                    break;
                case 'a': // Enter Insert mode (after cursor)
                    // Move right if possible before entering insert mode
                    if (textbuf.cursor_abs_i < textbuf.size) {
                         bufclient_move_cursor_relative(&textbuf, ARROW_RIGHT);
                    }
                    mode = MODE_INSERT;
                    editorSetStatusMessage("-- INSERT --");
                    break;
                case 'I': // Enter Insert mode (at start of line)
                     {
                        struct bufchunk* line_chunk;
                        int line_rel_i, line_abs_i;
                        if (bufclient_find_line_start(&textbuf, textbuf.cursor_abs_y, &line_chunk, &line_rel_i, &line_abs_i) == RESULT_OK) {
                            bufclient_move_cursor_to(&textbuf, line_abs_i);
                        } // else: stay where we are if error
                     }
                     mode = MODE_INSERT;
                     editorSetStatusMessage("-- INSERT --");
                     break;
                case 'A': // Enter Insert mode (at end of line)
                     {
                        struct bufchunk* next_line_chunk;
                        int next_line_rel_i, next_line_abs_i;
                        // Find start of *next* line to find end of current
                        if (bufclient_find_line_start(&textbuf, textbuf.cursor_abs_y + 1, &next_line_chunk, &next_line_rel_i, &next_line_abs_i) == RESULT_OK) {
                            // Move to the character just before the newline
                            bufclient_move_cursor_to(&textbuf, next_line_abs_i - 1);
                        } else {
                            // We are on the last line, move cursor to the very end of the buffer
                            bufclient_move_cursor_to(&textbuf, textbuf.size);
                        }
                    }
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
                case 'h': case ARROW_LEFT:
                    bufclient_move_cursor_relative(&textbuf, ARROW_LEFT);
                    break;
                case 'l': case ARROW_RIGHT:
                    bufclient_move_cursor_relative(&textbuf, ARROW_RIGHT);
                    break;
                case 'k': case ARROW_UP:
                    bufclient_move_cursor_relative(&textbuf, ARROW_UP);
                    break;
                case 'j': case ARROW_DOWN:
                    bufclient_move_cursor_relative(&textbuf, ARROW_DOWN);
                    break;
                case PAGE_UP:
                    // Move cursor view 'up' by one screen height, then position cursor
                     textbuf.cursor_abs_y -= screenrows;
                     if (textbuf.cursor_abs_y < 0) textbuf.cursor_abs_y = 0;
                     // Now find the abs_i for this new Y and goal X
                     {
                         struct bufchunk* target_line_chunk;
                         int target_line_rel_i, target_line_abs_i;
                         if (bufclient_find_line_start(&textbuf, textbuf.cursor_abs_y, &target_line_chunk, &target_line_rel_i, &target_line_abs_i) == RESULT_OK) {
                              // Use find_line_start to find the line, then reuse ARROW_DOWN logic's goal X seeking
                              // (or just move to start of line for simplicity)
                              bufclient_move_cursor_to(&textbuf, target_line_abs_i); // Simple version: move to start
                              // TODO: Implement proper goal-X seeking for PgUp/PgDn if desired
                         } else { // Should not happen if y>=0
                              bufclient_move_cursor_to(&textbuf, 0);
                         }
                         // Ensure view scrolls up too (editorScroll will handle this)
                         textbuf.rowoff = textbuf.cursor_abs_y;
                         textbuf.rowoff_chunk = NULL; // Invalidate cache
                     }
                    break;
                case PAGE_DOWN:
                    // Move cursor view 'down' by one screen height
                     {
                         // Estimate total lines (very inefficiently)
                         int total_lines = 0;
                         struct bufchunk* ch = textbuf.begin; int i;
                         if(textbuf.size > 0) total_lines = 1;
                         while(ch) { for(i=0; i<ch->size; ++i) if(ch->data[i] == '\n') total_lines++; ch=ch->next; }
                         if(textbuf.size == 0) total_lines = 1;

                         textbuf.cursor_abs_y += screenrows;
                         if (textbuf.cursor_abs_y >= total_lines) textbuf.cursor_abs_y = total_lines - 1;
                         if (textbuf.cursor_abs_y < 0) textbuf.cursor_abs_y = 0; // Safety check

                         struct bufchunk* target_line_chunk;
                         int target_line_rel_i, target_line_abs_i;
                         if (bufclient_find_line_start(&textbuf, textbuf.cursor_abs_y, &target_line_chunk, &target_line_rel_i, &target_line_abs_i) == RESULT_OK) {
                             bufclient_move_cursor_to(&textbuf, target_line_abs_i); // Simple version: move to start
                              // TODO: Implement goal-X seeking
                         } else {
                              bufclient_move_cursor_to(&textbuf, textbuf.size); // Move to end if line not found
                         }
                          // Ensure view scrolls down too
                         textbuf.rowoff = textbuf.cursor_abs_y - screenrows + 1;
                         if (textbuf.rowoff < 0) textbuf.rowoff = 0;
                         textbuf.rowoff_chunk = NULL; // Invalidate cache
                     }
                    break;
                case HOME_KEY: case '0': // Move cursor to beginning of the current line (visual column 0)
                {
                    struct bufchunk* line_chunk;
                    int line_rel_i, line_abs_i;
                    if (bufclient_find_line_start(&textbuf, textbuf.cursor_abs_y, &line_chunk, &line_rel_i, &line_abs_i) == RESULT_OK) {
                        bufclient_move_cursor_to(&textbuf, line_abs_i);
                    }
                } break;
                case END_KEY: case '$': // Move cursor to end of the current line
                {
                    struct bufchunk* next_line_chunk;
                    int next_line_rel_i, next_line_abs_i;
                    // Find start of *next* line
                    if (bufclient_find_line_start(&textbuf, textbuf.cursor_abs_y + 1, &next_line_chunk, &next_line_rel_i, &next_line_abs_i) == RESULT_OK) {
                        // Move to the character just before the newline (or start of next line)
                        // Handle empty line case where next_line_abs_i == current line start + 1
                        if (next_line_abs_i > 0) {
                             bufclient_move_cursor_to(&textbuf, next_line_abs_i - 1);
                        } else { // If next line starts at 0, current must be empty line 0
                             bufclient_move_cursor_to(&textbuf, 0);
                        }
                    } else {
                        // We are on the last line, move cursor to the very end of the buffer
                        bufclient_move_cursor_to(&textbuf, textbuf.size);
                    }
                } break;

                // --- Editing ---
                case 'x':  // Delete character under cursor (Vim 'x')
                    if (textbuf.cursor_abs_i < textbuf.size) { // Ensure not at EOF
                        bufclient_move_cursor_relative(&textbuf, ARROW_RIGHT); // Move onto char
                        bufclient_delete_char(&textbuf); // Delete char before new cursor pos
                    }
                    break;
                case 'd':  // Potential start of 'dd' (delete line)
                     // Requires peeking at next key, complex state.
                     // For now, just implement single 'd' as no-op or beep?
                     // Let's make 'D' delete to end of line for simplicity.
                     editorSetStatusMessage("'dd' not implemented. Use 'D' for delete-to-end.");
                    break;
                case 'D': // Delete from cursor to end of line
                    {
                        int original_cursor_pos = textbuf.cursor_abs_i;
                        // Find end of current line (same logic as END_KEY / $)
                        int end_of_line_pos = original_cursor_pos;
                        struct bufchunk* next_line_chunk;
                        int next_line_rel_i, next_line_abs_i;
                        if (bufclient_find_line_start(&textbuf, textbuf.cursor_abs_y + 1, &next_line_chunk, &next_line_rel_i, &next_line_abs_i) == RESULT_OK) {
                             // End is just before the newline of the next line
                             end_of_line_pos = next_line_abs_i -1;
                        } else {
                            // On last line, end is the end of the buffer
                            end_of_line_pos = textbuf.size;
                        }

                        // Delete characters one by one from end back to original position
                        while (textbuf.cursor_abs_i < end_of_line_pos) {
                            // Move cursor to the end of the part to delete
                            bufclient_move_cursor_to(&textbuf, end_of_line_pos);
                            if (textbuf.cursor_abs_i == 0) break; // Safety check
                            if (bufclient_delete_char(&textbuf) != RESULT_OK) break; // Stop on error
                            // Deleting moves cursor back one, end_of_line_pos is now one smaller too
                            end_of_line_pos--;
                        }
                        // Ensure cursor is back where 'D' was pressed if possible
                         bufclient_move_cursor_to(&textbuf, original_cursor_pos > textbuf.size ? textbuf.size : original_cursor_pos);
                    }
                    break;
                case 'o':  // Open line below and enter insert mode
                     {
                         // Go to end of current line first
                         editorProcessKeypress(); // Fake an END_KEY press
                         // Then insert a newline
                         bufclient_insert_char(&textbuf, '\n');
                         // Enter insert mode
                         mode = MODE_INSERT;
                         editorSetStatusMessage("-- INSERT --");
                     }
                    break;
                case 'O':  // Open line above and enter insert mode
                     {
                         // Go to start of current line
                         editorProcessKeypress(); // Fake a HOME_KEY press
                         // Insert newline (pushes current line down)
                         bufclient_insert_char(&textbuf, '\n');
                         // Cursor is now at start of the original line, move up to new empty line
                         bufclient_move_cursor_relative(&textbuf, ARROW_LEFT); // Actually moves to end of prev line
                         // Enter insert mode
                         mode = MODE_INSERT;
                         editorSetStatusMessage("-- INSERT --");
                     }
                    break;

                case '\x1b':  // Escape key - already in normal mode, do nothing silently
                    break;

                default:
                    // Optional: Beep or show "Unknown key" message
                    // editorSetStatusMessage("Unknown key in normal mode");
                    break;
            }
            break;  // End MODE_NORMAL

        case MODE_INSERT:
            switch (c) {
                case '\x1b':  // Escape: Return to Normal mode
                    mode = MODE_NORMAL;
                    // Vim behavior: move cursor left if possible after exiting insert,
                    // unless it was already at the beginning of the line.
                    if (textbuf.cursor_abs_i > 0) {
                         // Find start of current line
                         struct bufchunk* line_chunk; int line_rel_i, line_abs_i;
                         if (bufclient_find_line_start(&textbuf, textbuf.cursor_abs_y, &line_chunk, &line_rel_i, &line_abs_i) == RESULT_OK) {
                              // Only move left if cursor is not already at the line start
                              if (textbuf.cursor_abs_i > line_abs_i) {
                                   bufclient_move_cursor_relative(&textbuf, ARROW_LEFT);
                              }
                         }
                    }
                    editorSetStatusMessage(""); // Clear "-- INSERT --"
                    break;
                case '\r':  // Enter key (treat same as newline)
                    if (bufclient_insert_char(&textbuf, '\n') != RESULT_OK) {
                        // Status message set by insert_char on error
                    }
                    break;
                case BACKSPACE:  // Backspace key
                    if (bufclient_delete_char(&textbuf) != RESULT_OK) {
                         // Status message set by delete_char on error
                    }
                    break;
                case DEL_KEY:  // Delete key (delete character *after* cursor)
                    // Move right, delete backward (if not at end of buffer)
                    if (textbuf.cursor_abs_i < textbuf.size) {
                        bufclient_move_cursor_relative(&textbuf, ARROW_RIGHT);
                        bufclient_delete_char(&textbuf);
                    }
                    break;

                // Allow movement keys in insert mode? Common request.
                case ARROW_UP:    bufclient_move_cursor_relative(&textbuf, ARROW_UP); break;
                case ARROW_DOWN:  bufclient_move_cursor_relative(&textbuf, ARROW_DOWN); break;
                case ARROW_LEFT:  bufclient_move_cursor_relative(&textbuf, ARROW_LEFT); break;
                case ARROW_RIGHT: bufclient_move_cursor_relative(&textbuf, ARROW_RIGHT); break;
                case PAGE_UP:   // Simplified PgUp - move cursor only
                     { int i; for(i=0; i<screenrows; ++i) bufclient_move_cursor_relative(&textbuf, ARROW_UP); }
                     break;
                case PAGE_DOWN: // Simplified PgDn - move cursor only
                     { int i; for(i=0; i<screenrows; ++i) bufclient_move_cursor_relative(&textbuf, ARROW_DOWN); }
                     break;
                case HOME_KEY: // Simplified Home - move cursor only
                     { struct bufchunk* lch; int lri, lai;
                       if(bufclient_find_line_start(&textbuf, textbuf.cursor_abs_y, &lch, &lri, &lai)==RESULT_OK)
                           bufclient_move_cursor_to(&textbuf, lai); }
                     break;
                case END_KEY: // Simplified End - move cursor only
                      { struct bufchunk* nlch; int nlri, nlai;
                        if(bufclient_find_line_start(&textbuf, textbuf.cursor_abs_y + 1, &nlch, &nlri, &nlai)==RESULT_OK)
                            bufclient_move_cursor_to(&textbuf, nlai > 0 ? nlai - 1 : 0);
                        else bufclient_move_cursor_to(&textbuf, textbuf.size); }
                      break;


                default:  // Insert regular character if printable
                    // Check for standard printable ASCII range and Tab
                    if ((c >= 32 && c <= 126) || c == '\t') {
                        if(bufclient_insert_char(&textbuf, (char)c) != RESULT_OK) {
                            // Status set on error
                        }
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
                    cmdbuf[0] = '\0'; // Clear buffer too
                    cmdbuf_len = 0;
                    break;
                case '\r':  // Enter key: Execute the command
                    editorProcessCommand();
                    // Mode is usually set back to NORMAL by editorProcessCommand
                    // If command fails and stays in command mode, need different handling?
                    // Assume processCommand always returns to normal or quits.
                    break;
                case BACKSPACE:  // Backspace in command line
                    if (cmdbuf_len > 0) {
                        cmdbuf_len--;
                        cmdbuf[cmdbuf_len] = '\0';  // Keep null-terminated
                    }
                    break;

                // Basic command line editing? (Arrows, Del, Home, End) - Ignored for now
                case DEL_KEY:     // Could implement delete char under cursor
                case ARROW_UP:    // Could implement command history
                case ARROW_DOWN:  // Could implement command history
                case ARROW_LEFT:  // Could implement cursor movement
                case ARROW_RIGHT: // Could implement cursor movement
                case PAGE_UP:
                case PAGE_DOWN:
                case HOME_KEY:    // Could implement move to start of command
                case END_KEY:     // Could implement move to end of command
                    // Ignore for simplicity
                    break;

                default: // Append typed character to command buffer if printable
                    if (cmdbuf_len < CMD_BUF_SIZE - 1) {  // Check buffer limit
                        // Allow any printable character in command
                        if (isprint((unsigned char)c)) {
                            cmdbuf[cmdbuf_len++] = (char)c;
                            cmdbuf[cmdbuf_len] = '\0';  // Keep null-terminated
                        }
                    } else {
                         // Optional: Beep when command buffer is full
                    }
                    break;
            }
            break;  // End MODE_COMMAND
    } // end switch(mode)
}


// *** Main Function ***
int main(int argc, char* argv[]) {
    // Initialization (terminal, screen size, buffers, raw mode, exit handler)
    initEditor();

    // Open file specified on command line, if any
    if (argc >= 2) {
        editorOpen(argv[1]);
        // editorOpen sets status messages for success/failure/new file
    } else {
        // No file specified, show welcome message
        editorSetStatusMessage("lkjsxceditor | Version " LKJSXCEDITOR_VERSION " | Press : for command");
    }

    // Main event loop
    while (!terminate_editor) {
        editorRefreshScreen();    // Update display based on current state
        editorProcessKeypress();  // Wait for and process one keypress
    }

    // Cleanup is handled by atexit(disableRawMode)
    // Optional: explicit free?
    // bufclient_free(&textbuf); // Free buffer chunks back to pool

    // Clear screen finally for a clean exit (disableRawMode might already do this)
    // write(STDOUT_FILENO, "\x1b[2J", 4); // Already in die() and maybe atexit handler
    // write(STDOUT_FILENO, "\x1b[H", 3);

    return 0;  // Successful exit
}
