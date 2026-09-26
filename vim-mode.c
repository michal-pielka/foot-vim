#include "vim-mode.h"

#include <xkbcommon/xkbcommon-compose.h>

#define LOG_MODULE "vim-mode"
#define LOG_ENABLE_DBG 0
#include "log.h"

#include "char32.h"
#include "commands.h"
#include "config.h"
#include "grid.h"
#include "misc.h"
#include "render.h"
#include "search.h"
#include "selection.h"
#include "util.h"

/*
 * The cursor (term->vim.cursor) is stored in absolute grid
 * coordinates. This means it stays glued to the content when the
 * scrollback scrolls.
 *
 * Motions, however, operate on scrollback relative row numbers, where
 * row 0 is the top-most (oldest) allocated scrollback row, and
 * 'sb_max' is the bottom-most screen row. This gives us a linear
 * coordinate space, without any ring-buffer wrap-arounds, to do
 * arithmetic and comparisons in.
 */
struct vim_ctx {
    struct terminal *term;
    struct grid *grid;
    int sb_start;      /* Absolute row number of the first allocated row */
    int sb_max;        /* Bottom-most screen row, scrollback relative */
    struct coord pos;  /* Cursor, row is scrollback relative */
};

static int
abs_to_sb(const struct vim_ctx *ctx, int abs_row)
{
    return grid_row_abs_to_sb_precalc_sb_start(ctx->grid, ctx->sb_start, abs_row);
}

static int
sb_to_abs(const struct vim_ctx *ctx, int sb_row)
{
    return (ctx->sb_start + sb_row) & (ctx->grid->num_rows - 1);
}

/* Place the cursor at the terminal cursor if it is inside the
 * current viewport, otherwise at the top-left of the viewport */
static void
cursor_to_start_position(struct terminal *term)
{
    struct grid *grid = term->grid;
    const int cursor_abs =
        (grid->offset + grid->cursor.point.row) & (grid->num_rows - 1);
    const int view_rel =
        (cursor_abs - grid->view + grid->num_rows) & (grid->num_rows - 1);

    if (view_rel < term->rows) {
        term->vim.cursor.row = cursor_abs;
        term->vim.cursor.col = min(grid->cursor.point.col, term->cols - 1);
    } else {
        term->vim.cursor.row = grid->view;
        term->vim.cursor.col = 0;
    }
}

void
vim_mode_validate_cursor(struct terminal *term)
{
    if (!term->vim.active)
        return;

    struct grid *grid = term->grid;

    term->vim.cursor.row &= grid->num_rows - 1;
    term->vim.cursor.col = max(min(term->vim.cursor.col, term->cols - 1), 0);

    if (likely(grid->rows[term->vim.cursor.row] != NULL))
        return;

    /* The row under the cursor has been freed, e.g. by the client
     * erasing the scrollback (CSI 3 J), or by a reverse scroll */
    LOG_DBG("vim mode: cursor row has been freed, resetting cursor");
    cursor_to_start_position(term);
}

static struct vim_ctx
ctx_for_term(struct terminal *term)
{
    vim_mode_validate_cursor(term);

    struct grid *grid = term->grid;
    struct vim_ctx ctx = {.term = term, .grid = grid};

    ctx.sb_start = grid_sb_start_ignore_uninitialized(grid, term->rows);
    ctx.sb_max = abs_to_sb(
        &ctx, (grid->offset + term->rows - 1) & (grid->num_rows - 1));

    /* The cursor may point at a row that has since been recycled, or
     * scrolled out - clamp it back into the valid range */
    ctx.pos.row = min(
        abs_to_sb(&ctx, term->vim.cursor.row & (grid->num_rows - 1)),
        ctx.sb_max);
    ctx.pos.col = max(min(term->vim.cursor.col, term->cols - 1), 0);

    return ctx;
}

static int
view_sb(const struct vim_ctx *ctx)
{
    return abs_to_sb(ctx, ctx->grid->view);
}

/* Scroll the viewport, if needed, to make the cursor visible */
static void
scroll_to_cursor(struct vim_ctx *ctx)
{
    const int view = view_sb(ctx);

    if (ctx->pos.row < view)
        cmd_scrollback_up(ctx->term, view - ctx->pos.row);
    else if (ctx->pos.row >= view + ctx->term->rows)
        cmd_scrollback_down(
            ctx->term, ctx->pos.row - (view + ctx->term->rows - 1));
}

/* Update an ongoing selection to follow the cursor */
static void
update_selection(struct vim_ctx *ctx)
{
    struct terminal *term = ctx->term;

    if (term->selection.coords.start.row < 0 || !term->selection.ongoing)
        return;

    /* selection_update() takes view relative coordinates */
    selection_update(term, ctx->pos.col, ctx->pos.row - view_sb(ctx));
}

static void
apply_cursor(struct vim_ctx *ctx)
{
    struct terminal *term = ctx->term;

    xassert(ctx->pos.row >= 0);
    xassert(ctx->pos.row <= ctx->sb_max);
    xassert(ctx->pos.col >= 0);
    xassert(ctx->pos.col < term->cols);

    term->vim.cursor.row = sb_to_abs(ctx, ctx->pos.row);
    term->vim.cursor.col = ctx->pos.col;

    scroll_to_cursor(ctx);
    update_selection(ctx);
    render_refresh(term);
}

static const struct row *
row_at(const struct vim_ctx *ctx, int sb_row)
{
    const struct row *row = ctx->grid->rows[sb_to_abs(ctx, sb_row)];
    xassert(row != NULL);
    return row;
}

static bool
is_spacer(const struct row *row, int col)
{
    return row->cells[col].wc >= CELL_SPACER;
}

/* Does the row continue onto the next row? */
static bool
row_wraps(const struct vim_ctx *ctx, int sb_row)
{
    return !row_at(ctx, sb_row)->linebreak;
}

/* True if 'pos' is on the last column of a row that wraps */
static bool
is_wrap(const struct vim_ctx *ctx, struct coord pos)
{
    return pos.col == ctx->term->cols - 1 && row_wraps(ctx, pos.row);
}

static char32_t
base_char(const struct vim_ctx *ctx, const struct row *row, int col)
{
    char32_t wc = row->cells[col].wc;
    if (wc >= CELL_COMB_CHARS_LO && wc <= CELL_COMB_CHARS_HI)
        wc = composed_lookup(ctx->term->composed, wc - CELL_COMB_CHARS_LO)->chars[0];
    return wc;
}

static bool
is_space(const struct vim_ctx *ctx, struct coord pos)
{
    const struct row *row = row_at(ctx, pos.row);
    if (is_spacer(row, pos.col))
        return false;

    const char32_t wc = base_char(ctx, row, pos.col);
    return wc == U'\0' || wc == U' ' || wc == U'\t';
}

/* Column of the first non-empty cell in the row, or -1 */
static int
first_occupied_in_row(const struct vim_ctx *ctx, int sb_row)
{
    for (int col = 0; col < ctx->term->cols; col++) {
        if (!is_space(ctx, (struct coord){col, sb_row}))
            return col;
    }
    return -1;
}

/* Column of the last non-empty cell in the row, or -1 */
static int
last_occupied_in_row(const struct vim_ctx *ctx, int sb_row)
{
    for (int col = ctx->term->cols - 1; col >= 0; col--) {
        if (!is_space(ctx, (struct coord){col, sb_row}))
            return col;
    }
    return -1;
}

enum vim_direction {VIM_LEFT, VIM_RIGHT};

/* Move 'pos' off wide character spacer cells: to the base character
 * when moving left, to the last spacer when moving right */
static struct coord
expand_wide(const struct vim_ctx *ctx, struct coord pos,
            enum vim_direction direction)
{
    const struct row *row = row_at(ctx, pos.row);

    if (direction == VIM_LEFT) {
        while (pos.col > 0 && is_spacer(row, pos.col))
            pos.col--;
    } else {
        while (pos.col < ctx->term->cols - 1 && is_spacer(row, pos.col + 1))
            pos.col++;
    }

    return pos;
}

static void
motion_up(struct vim_ctx *ctx)
{
    if (ctx->pos.row > 0)
        ctx->pos.row--;
}

static void
motion_down(struct vim_ctx *ctx)
{
    if (ctx->pos.row < ctx->sb_max)
        ctx->pos.row++;
}

static void
motion_left(struct vim_ctx *ctx)
{
    ctx->pos = expand_wide(ctx, ctx->pos, VIM_LEFT);

    if (ctx->pos.col == 0 && ctx->pos.row > 0 &&
        row_wraps(ctx, ctx->pos.row - 1))
    {
        /* Wrap around to the end of the previous row */
        ctx->pos.row--;
        ctx->pos.col = ctx->term->cols - 1;
    } else
        ctx->pos.col = max(ctx->pos.col - 1, 0);
}

static void
motion_right(struct vim_ctx *ctx)
{
    ctx->pos = expand_wide(ctx, ctx->pos, VIM_RIGHT);

    if (is_wrap(ctx, ctx->pos)) {
        if (ctx->pos.row < ctx->sb_max) {
            ctx->pos.row++;
            ctx->pos.col = 0;
        }
    } else
        ctx->pos.col = min(ctx->pos.col + 1, ctx->term->cols - 1);
}

/* Is 'pos' at a grid corner, in the direction of movement? */
static bool
is_boundary(const struct vim_ctx *ctx, struct coord pos,
            enum vim_direction direction)
{
    if (direction == VIM_LEFT)
        return pos.row <= 0 && pos.col == 0;
    return pos.row >= ctx->sb_max && pos.col + 1 >= ctx->term->cols;
}

/* One cell in the direction of movement, crossing row boundaries, and
 * clamped at the grid corners */
static struct coord
advance(const struct vim_ctx *ctx, struct coord pos,
        enum vim_direction direction)
{
    if (direction == VIM_LEFT) {
        if (--pos.col < 0) {
            if (pos.row > 0) {
                pos.row--;
                pos.col = ctx->term->cols - 1;
            } else
                pos.col = 0;
        }
    } else {
        if (++pos.col >= ctx->term->cols) {
            if (pos.row < ctx->sb_max) {
                pos.row++;
                pos.col = 0;
            } else
                pos.col = ctx->term->cols - 1;
        }
    }

    return pos;
}

/* Expand 'point' to the word boundary, using the configured word
 * delimiters. Only expands within words - delimiters and whitespace
 * are boundaries of their own */
static struct coord
expand_semantic(const struct vim_ctx *ctx, struct coord point,
                enum vim_direction direction)
{
    const struct row *row = row_at(ctx, point.row);

    if (!is_spacer(row, point.col) &&
        !isword(base_char(ctx, row, point.col), false,
                ctx->term->conf->word_delimiters))
    {
        return point;
    }

    struct coord abs = {point.col, sb_to_abs(ctx, point.row)};
    if (direction == VIM_LEFT)
        selection_find_word_boundary_left(ctx->term, &abs, false);
    else
        selection_find_word_boundary_right(ctx->term, &abs, false, true);

    return (struct coord){abs.col, abs_to_sb(ctx, abs.row)};
}

/* Move by delimiter separated word, like w/b/e/ge in vi. 'side' is
 * the side of the word to stop at: VIM_LEFT for the beginning of the
 * word, VIM_RIGHT for the end */
static void
motion_semantic(struct vim_ctx *ctx, enum vim_direction direction,
                enum vim_direction side)
{
    struct coord point = ctx->pos;

    /* Move to word boundary */
    if (direction != side && !is_boundary(ctx, point, direction))
        point = expand_semantic(ctx, point, direction);

    /* Make sure we jump above wide chars */
    point = expand_wide(ctx, point, direction);

    /* Skip whitespace */
    struct coord next = advance(ctx, point, direction);
    while (!is_boundary(ctx, point, direction) && is_space(ctx, next)) {
        point = next;
        next = advance(ctx, point, direction);
    }

    /* Assure minimum movement of one cell */
    if (!is_boundary(ctx, point, direction)) {
        point = advance(ctx, point, direction);

        /* Skip over wide cell spacers */
        if (direction == VIM_LEFT)
            point = expand_wide(ctx, point, direction);
    }

    /* Move to word boundary */
    if (direction == side && !is_boundary(ctx, point, direction))
        point = expand_semantic(ctx, point, direction);

    ctx->pos = point;
}

/* Move by whitespace separated word, like W/B/E/gE in vi */
static void
motion_word(struct vim_ctx *ctx, enum vim_direction direction,
            enum vim_direction side)
{
    /* Make sure we jump above wide chars */
    struct coord point = expand_wide(ctx, ctx->pos, direction);

    if (direction == side) {
        /* Skip whitespace until right before a word */
        struct coord next = advance(ctx, point, direction);
        while (!is_boundary(ctx, point, direction) && is_space(ctx, next)) {
            point = next;
            next = advance(ctx, point, direction);
        }

        /* Skip non-whitespace until right inside word boundary */
        next = advance(ctx, point, direction);
        while (!is_boundary(ctx, point, direction) && !is_space(ctx, next)) {
            point = next;
            next = advance(ctx, point, direction);
        }
    } else {
        /* Skip non-whitespace until just beyond word */
        while (!is_boundary(ctx, point, direction) && !is_space(ctx, point))
            point = advance(ctx, point, direction);

        /* Skip whitespace until right inside word boundary */
        while (!is_boundary(ctx, point, direction) && is_space(ctx, point))
            point = advance(ctx, point, direction);
    }

    ctx->pos = point;
}

static void
motion_semantic_left(struct vim_ctx *ctx)
{
    motion_semantic(ctx, VIM_LEFT, VIM_LEFT);
}

static void
motion_semantic_right(struct vim_ctx *ctx)
{
    motion_semantic(ctx, VIM_RIGHT, VIM_LEFT);
}

static void
motion_semantic_left_end(struct vim_ctx *ctx)
{
    motion_semantic(ctx, VIM_LEFT, VIM_RIGHT);
}

static void
motion_semantic_right_end(struct vim_ctx *ctx)
{
    motion_semantic(ctx, VIM_RIGHT, VIM_RIGHT);
}

static void
motion_word_left(struct vim_ctx *ctx)
{
    motion_word(ctx, VIM_LEFT, VIM_LEFT);
}

static void
motion_word_right(struct vim_ctx *ctx)
{
    motion_word(ctx, VIM_RIGHT, VIM_LEFT);
}

static void
motion_word_left_end(struct vim_ctx *ctx)
{
    motion_word(ctx, VIM_LEFT, VIM_RIGHT);
}

static void
motion_word_right_end(struct vim_ctx *ctx)
{
    motion_word(ctx, VIM_RIGHT, VIM_RIGHT);
}

/* Move to the matching bracket, if the cursor is on one */
static void
motion_bracket(struct vim_ctx *ctx)
{
    static const struct {
        char32_t open;
        char32_t close;
    } pairs[] = {
        {U'(', U')'}, {U'[', U']'}, {U'{', U'}'}, {U'<', U'>'},
    };

    const char32_t start_char = base_char(
        ctx, row_at(ctx, ctx->pos.row), ctx->pos.col);

    /* Find the matching bracket we're looking for */
    bool forward = false;
    char32_t end_char = U'\0';

    for (size_t i = 0; i < ALEN(pairs); i++) {
        if (pairs[i].open == start_char) {
            forward = true;
            end_char = pairs[i].close;
            break;
        }
        if (pairs[i].close == start_char) {
            forward = false;
            end_char = pairs[i].open;
            break;
        }
    }

    if (end_char == U'\0')
        return;

    const enum vim_direction direction = forward ? VIM_RIGHT : VIM_LEFT;
    struct coord pos = ctx->pos;

    /* For every character match that equals the starting bracket, we
     * ignore one bracket of the opposite type */
    int skip_pairs = 0;

    while (!is_boundary(ctx, pos, direction)) {
        pos = advance(ctx, pos, direction);

        const struct row *row = row_at(ctx, pos.row);
        if (is_spacer(row, pos.col))
            continue;

        const char32_t wc = base_char(ctx, row, pos.col);

        if (wc == end_char && skip_pairs == 0) {
            ctx->pos = pos;
            return;
        } else if (wc == start_char)
            skip_pairs++;
        else if (wc == end_char)
            skip_pairs--;
    }
}

static bool
row_is_clear(const struct vim_ctx *ctx, int sb_row)
{
    return last_occupied_in_row(ctx, sb_row) < 0;
}

/* Move above the current paragraph */
static void
motion_paragraph_up(struct vim_ctx *ctx)
{
    int row = ctx->pos.row - 1;

    /* Skip empty rows until we find the next paragraph, then skip
     * over the paragraph until we reach the next empty row */
    while (row > 0 && row_is_clear(ctx, row))
        row--;
    while (row > 0 && !row_is_clear(ctx, row))
        row--;

    ctx->pos.row = max(row, 0);
    ctx->pos.col = 0;
}

/* Move below the current paragraph */
static void
motion_paragraph_down(struct vim_ctx *ctx)
{
    int row = ctx->pos.row;

    while (row < ctx->sb_max && row_is_clear(ctx, row))
        row++;
    while (row < ctx->sb_max && !row_is_clear(ctx, row))
        row++;

    ctx->pos.row = min(row, ctx->sb_max);
    ctx->pos.col = 0;
}

/* First column, or beginning of the logical line when already there */
static void
motion_first(struct vim_ctx *ctx)
{
    ctx->pos = expand_wide(ctx, ctx->pos, VIM_LEFT);

    while (ctx->pos.col == 0 && ctx->pos.row > 0 &&
           row_wraps(ctx, ctx->pos.row - 1))
    {
        ctx->pos.row--;
    }

    ctx->pos.col = 0;
}

/* Last non-empty cell, or last column, across soft line breaks */
static void
motion_last(struct vim_ctx *ctx)
{
    struct coord pos = expand_wide(ctx, ctx->pos, VIM_RIGHT);
    int occupied = last_occupied_in_row(ctx, pos.row);

    if (pos.col < occupied) {
        /* Jump to last occupied cell when not already at or beyond it */
        pos.col = occupied;
    } else if (is_wrap(ctx, pos)) {
        /* Jump to last occupied cell across soft line breaks */
        while (pos.row < ctx->sb_max && is_wrap(ctx, pos))
            pos.row++;

        occupied = last_occupied_in_row(ctx, pos.row);
        if (occupied >= 0)
            pos.col = occupied;
    } else {
        /* Jump to last column when beyond the last occupied cell */
        pos.col = ctx->term->cols - 1;
    }

    ctx->pos = pos;
}

/* First non-empty cell of the row, or of the logical line when
 * already there */
static void
motion_first_occupied(struct vim_ctx *ctx)
{
    const int last_col = ctx->term->cols - 1;
    struct coord pos = expand_wide(ctx, ctx->pos, VIM_LEFT);

    int occupied_col = first_occupied_in_row(ctx, pos.row);
    if (occupied_col < 0)
        occupied_col = last_col;

    if (pos.col != occupied_col) {
        ctx->pos.col = occupied_col;
        return;
    }

    /* Already at the row's first occupied cell - jump across soft
     * line breaks, to the logical line's first occupied cell */
    struct coord found = {-1, -1};

    for (int row = pos.row - 1; row >= 0; row--) {
        if (!row_wraps(ctx, row))
            break;

        int col = first_occupied_in_row(ctx, row);
        if (col >= 0)
            found = (struct coord){col, row};
    }

    if (found.row < 0) {
        /* Fallback to the next non-empty cell */
        int row = pos.row;

        while (true) {
            int col = first_occupied_in_row(ctx, row);
            if (col >= 0) {
                found = (struct coord){col, row};
                break;
            }

            if (row >= ctx->sb_max ||
                !is_wrap(ctx, (struct coord){last_col, row}))
            {
                found = (struct coord){last_col, row};
                break;
            }

            row++;
        }
    }

    ctx->pos = found;
}

static void
move_to_view_row(struct vim_ctx *ctx, int rel_row)
{
    const int row = min(view_sb(ctx) + rel_row, ctx->sb_max);
    const int col = first_occupied_in_row(ctx, row);

    ctx->pos = (struct coord){col >= 0 ? col : 0, row};
}

static void
motion_middle(struct vim_ctx *ctx)
{
    move_to_view_row(ctx, max(ctx->term->rows / 2 - 1, 0));
}

/* Scroll the viewport 'lines' rows (negative is up), dragging the
 * cursor along to keep its on-screen position */
static bool
scroll_action(struct terminal *term, int lines)
{
    struct vim_ctx ctx = ctx_for_term(term);

    const int row = min(max(ctx.pos.row + lines, 0), ctx.sb_max);
    const int col = first_occupied_in_row(&ctx, row);
    ctx.pos = (struct coord){col >= 0 ? col : 0, row};

    if (lines < 0)
        cmd_scrollback_up(term, -lines);
    else
        cmd_scrollback_down(term, lines);

    /* The viewport may not have been able to scroll the full amount */
    const int view = view_sb(&ctx);
    ctx.pos.row = min(max(ctx.pos.row, view), view + term->rows - 1);

    apply_cursor(&ctx);
    return true;
}

static bool
scroll_home_action(struct terminal *term)
{
    struct vim_ctx ctx = ctx_for_term(term);

    cmd_scrollback_up(term, ctx.grid->num_rows);

    ctx.pos = (struct coord){0, 0};
    motion_first_occupied(&ctx);
    apply_cursor(&ctx);
    return true;
}

static bool
scroll_end_action(struct terminal *term)
{
    struct vim_ctx ctx = ctx_for_term(term);

    cmd_scrollback_down(term, ctx.grid->num_rows);

    ctx.pos = (struct coord){0, ctx.sb_max};

    /* Twice, to jump across soft line breaks */
    motion_first_occupied(&ctx);
    motion_first_occupied(&ctx);
    apply_cursor(&ctx);
    return true;
}

/* Scroll the viewport so that the cursor ends up on view row
 * 'view_row' (zt, zz, zb in vi). The cursor stays on its content */
static bool
scroll_cursor_to_view_row(struct terminal *term, int view_row)
{
    struct vim_ctx ctx = ctx_for_term(term);
    const int target_view = ctx.pos.row - view_row;
    const int delta = view_sb(&ctx) - target_view;

    if (delta > 0)
        cmd_scrollback_up(term, delta);
    else if (delta < 0)
        cmd_scrollback_down(term, -delta);

    return true;
}

static bool
motion(struct terminal *term, void (*fn)(struct vim_ctx *ctx))
{
    struct vim_ctx ctx = ctx_for_term(term);
    fn(&ctx);
    apply_cursor(&ctx);
    return true;
}

/* Repeat a motion 'count' times, stopping early when it no longer
 * moves the cursor (e.g. at the top of the scrollback) */
static bool
motion_n(struct terminal *term, void (*fn)(struct vim_ctx *ctx), int count)
{
    struct vim_ctx ctx = ctx_for_term(term);

    for (int i = 0; i < count; i++) {
        const struct coord before = ctx.pos;
        fn(&ctx);

        if (ctx.pos.row == before.row && ctx.pos.col == before.col)
            break;
    }

    apply_cursor(&ctx);
    return true;
}

/* H/L with a count: move to the count:th row from the top/bottom of
 * the viewport */
static bool
motion_view_row(struct terminal *term, int view_row)
{
    struct vim_ctx ctx = ctx_for_term(term);
    move_to_view_row(&ctx, max(min(view_row, term->rows - 1), 0));
    apply_cursor(&ctx);
    return true;
}

static void
pending_reset(struct terminal *term)
{
    term->vim.pending.count = 0;
    term->vim.pending.prefix = 0;
    term->vim.pending.char_cmd = 0;
}

void
vim_mode_begin(struct terminal *term)
{
    if (term->vim.active)
        return;

    LOG_DBG("vim mode: begin");

    selection_cancel(term);
    cursor_to_start_position(term);

    term->vim.active = true;
    term->vim.inline_search.character = U'\0';
    term->vim.inline_search.char_pending = false;
    term->vim.inline_search.backward = false;
    term->vim.inline_search.stop_short = false;
    pending_reset(term);

    /* Keyboard input is never sent to the client while in vim mode */
    if (term_ime_is_enabled(term)) {
        term->vim.reenable_ime = true;
        term_ime_disable(term);
    }

    render_refresh(term);
}

void
vim_mode_cancel(struct terminal *term)
{
    if (!term->vim.active)
        return;

    LOG_DBG("vim mode: cancel");

    term->vim.active = false;
    term->vim.inline_search.char_pending = false;
    pending_reset(term);

    if (term->vim.reenable_ime) {
        term->vim.reenable_ime = false;
        term_ime_enable(term);
    }

    render_refresh(term);
}

void
vim_mode_goto(struct terminal *term, struct coord pos)
{
    if (!term->vim.active)
        return;

    struct vim_ctx ctx = ctx_for_term(term);

    ctx.pos.row = min(
        abs_to_sb(&ctx, pos.row & (ctx.grid->num_rows - 1)), ctx.sb_max);
    ctx.pos.col = max(min(pos.col, term->cols - 1), 0);

    apply_cursor(&ctx);
}

void
vim_mode_view_changed(struct terminal *term)
{
    if (!term->vim.active || term->is_searching)
        return;

    /* Drag the cursor along with the viewport, like Alacritty does
     * when scrolling the display in vi mode */
    struct vim_ctx ctx = ctx_for_term(term);
    const int view = view_sb(&ctx);
    const int clamped =
        max(min(ctx.pos.row, view + term->rows - 1), view);

    if (clamped == ctx.pos.row &&
        sb_to_abs(&ctx, ctx.pos.row) == term->vim.cursor.row)
    {
        return;
    }

    ctx.pos.row = clamped;
    term->vim.cursor.row = sb_to_abs(&ctx, ctx.pos.row);
    term->vim.cursor.col = ctx.pos.col;
    update_selection(&ctx);
    render_refresh(term);
}

void
vim_mode_resized(struct terminal *term)
{
    if (!term->vim.active)
        return;

    /* Ensure the cursor is not in the unallocated part of the ring.
     * ctx_for_term() resets it if its row has been freed */
    struct vim_ctx ctx = ctx_for_term(term);
    term->vim.cursor.row = sb_to_abs(&ctx, ctx.pos.row);
    term->vim.cursor.col = ctx.pos.col;
}

/* Find the next (or previous) occurrence of 'needle' after (before)
 * '*pos', within the current logical line. Updates '*pos' on success */
static bool
inline_find(const struct vim_ctx *ctx, struct coord *pos_io,
            char32_t needle, bool backward)
{
    const int last_col = ctx->term->cols - 1;
    struct coord pos = *pos_io;

    if (!backward) {
        /* Immediately stop if the starting point is on a line break */
        if (pos.col == last_col && !row_wraps(ctx, pos.row))
            return false;

        while (!is_boundary(ctx, pos, VIM_RIGHT)) {
            pos = advance(ctx, pos, VIM_RIGHT);

            const struct row *row = row_at(ctx, pos.row);
            if (!is_spacer(row, pos.col) &&
                base_char(ctx, row, pos.col) == needle)
            {
                *pos_io = pos;
                return true;
            }

            if (pos.col == last_col && !row_wraps(ctx, pos.row)) {
                /* Hard line break - stop */
                break;
            }
        }
    } else {
        while (!is_boundary(ctx, pos, VIM_LEFT)) {
            struct coord prev = advance(ctx, pos, VIM_LEFT);

            if (prev.col == last_col && !row_wraps(ctx, prev.row)) {
                /* Crossed a hard line break - stop */
                break;
            }

            pos = prev;

            const struct row *row = row_at(ctx, pos.row);
            if (!is_spacer(row, pos.col) &&
                base_char(ctx, row, pos.col) == needle)
            {
                *pos_io = pos;
                return true;
            }
        }
    }

    return false;
}

/* Search for the last inline search character, within the current
 * logical line, like f/F/t/T/;/, in vi. Moves to the count:th
 * occurrence; like vi, the cursor does not move at all if there are
 * fewer than 'count' occurrences. 'repeat' is true for ; and , */
static void
inline_search_exec(struct terminal *term, bool backward, int count,
                   bool repeat)
{
    const char32_t needle = term->vim.inline_search.character;
    if (needle == U'\0')
        return;

    struct vim_ctx ctx = ctx_for_term(term);
    const enum vim_direction back = backward ? VIM_RIGHT : VIM_LEFT;
    struct coord pos = ctx.pos;

    for (int i = 0; i < count; i++) {
        if (!inline_find(&ctx, &pos, needle, backward))
            return;
    }

    if (term->vim.inline_search.stop_short) {
        struct coord target = advance(&ctx, pos, back);

        if (repeat &&
            target.row == ctx.pos.row && target.col == ctx.pos.col)
        {
            /* Repeating t/T from right next to the match would not
             * move the cursor - skip to the next match, like vim */
            if (!inline_find(&ctx, &pos, needle, backward))
                return;
            target = advance(&ctx, pos, back);
        }

        pos = target;
    }

    ctx.pos = pos;
    apply_cursor(&ctx);
}

static bool
inline_search_start(struct terminal *term, bool backward, bool stop_short)
{
    term->vim.inline_search.backward = backward;
    term->vim.inline_search.stop_short = stop_short;
    term->vim.inline_search.char_pending = true;
    term->vim.inline_search.character = U'\0';
    return true;
}

/* The next typed character is the inline search target */
static void
inline_search_capture(struct seat *seat, struct terminal *term, uint32_t key)
{
    term->vim.inline_search.char_pending = false;

    enum xkb_compose_status compose_status =
        seat->kbd.xkb_compose_state != NULL
            ? xkb_compose_state_get_status(seat->kbd.xkb_compose_state)
            : XKB_COMPOSE_NOTHING;

    uint8_t buf[64] = {0};
    int count = 0;

    if (compose_status == XKB_COMPOSE_COMPOSED) {
        count = xkb_compose_state_get_utf8(
            seat->kbd.xkb_compose_state, (char *)buf, sizeof(buf));
        xkb_compose_state_reset(seat->kbd.xkb_compose_state);
    } else if (compose_status == XKB_COMPOSE_CANCELLED ||
               compose_status == XKB_COMPOSE_COMPOSING) {
        count = 0;
    } else {
        count = xkb_state_key_get_utf8(
            seat->kbd.xkb_state, key, (char *)buf, sizeof(buf));
    }

    if (count == 0) {
        /* Not a character (e.g. a modifier key) - keep waiting */
        term->vim.inline_search.char_pending = true;
        return;
    }

    char32_t c32s[64];
    size_t chars = mbsntoc32(c32s, (const char *)buf, count, ALEN(c32s) - 1);

    if (chars == (size_t)-1 || chars == 0 || !isc32print(c32s[0])) {
        /* Control character (e.g. escape) - abort */
        return;
    }

    term->vim.inline_search.character = c32s[0];
    inline_search_exec(
        term, term->vim.inline_search.backward,
        max(term->vim.pending.count, 1), false);
}

/* Start a new selection at the cursor, toggle an existing one of the
 * same kind off, or switch the kind of an existing one */
static bool
toggle_selection(struct terminal *term, enum selection_kind kind)
{
    struct vim_ctx ctx = ctx_for_term(term);

    scroll_to_cursor(&ctx);

    const bool have_selection =
        term->selection.coords.start.row >= 0 &&
        term->selection.coords.end.row >= 0;

    if (have_selection && term->selection.kind == kind) {
        selection_cancel(term);
        return true;
    }

    const int view = view_sb(&ctx);
    struct coord anchor = ctx.pos;

    if (have_selection) {
        /* Switch selection kind, keeping the anchor point when it is
         * still inside the viewport */
        const int pivot_row =
            abs_to_sb(&ctx, term->selection.pivot.start.row &
                      (ctx.grid->num_rows - 1));

        if (pivot_row >= view && pivot_row < view + term->rows) {
            anchor = (struct coord){
                min(term->selection.pivot.start.col, term->cols - 1),
                pivot_row,
            };
        }
    }

    selection_start(term, anchor.col, anchor.row - view, kind, false);

    /* Extend to the cursor. This also ensures the selection is never
     * empty, so that a subsequent copy yanks at least one cell */
    selection_update(term, ctx.pos.col, ctx.pos.row - view);
    return true;
}

static bool
yank(struct seat *seat, struct terminal *term, uint32_t serial)
{
    if (term->selection.coords.start.row < 0 ||
        term->selection.coords.end.row < 0)
    {
        return true;
    }

    selection_finalize(seat, term, serial);
    selection_to_clipboard(seat, term, serial);
    selection_cancel(term);
    return true;
}

/* Jump to the count:th next/previous match of the last search query */
static bool
search_jump(struct terminal *term, bool backward, int count)
{
    struct vim_ctx ctx = ctx_for_term(term);
    struct coord pos = ctx.pos;
    bool moved = false;

    for (int i = 0; i < count; i++) {
        /* Start the search at the cell next to the cursor */
        struct coord start =
            advance(&ctx, pos, backward ? VIM_LEFT : VIM_RIGHT);
        start.row = sb_to_abs(&ctx, start.row);

        struct range match;
        if (!search_find_last_query(
                term, start, backward ? SEARCH_BACKWARD : SEARCH_FORWARD,
                &match))
        {
            break;
        }

        const struct coord next = {
            .col = max(min(match.start.col, term->cols - 1), 0),
            .row = min(abs_to_sb(&ctx, match.start.row), ctx.sb_max),
        };

        if (next.row == pos.row && next.col == pos.col) {
            /* The only match - no point in searching again */
            break;
        }

        pos = next;
        moved = true;
    }

    if (moved) {
        ctx.pos = pos;
        apply_cursor(&ctx);
    }
    return true;
}

/* Scale a scroll amount by a count, without overflowing */
static int
scroll_amount(const struct terminal *term, int lines, int count)
{
    const long long total = (long long)lines * count;
    return total > term->grid->num_rows ? term->grid->num_rows : (int)total;
}

/* Execute a vim mode action. 'count' is the typed count prefix (e.g.
 * 5 in 5j), or 0 when none was typed. Actions where a count makes no
 * sense ignore it */
static bool
execute_action(struct seat *seat, struct terminal *term,
               enum bind_action_vim action, int count, uint32_t serial)
{
    const int n = max(count, 1);

    switch (action) {
    case BIND_ACTION_VIM_NONE:
        return false;

    case BIND_ACTION_VIM_CANCEL:
        term_reset_view(term);
        vim_mode_cancel(term);
        return true;

    case BIND_ACTION_VIM_UP:
        return motion_n(term, &motion_up, n);

    case BIND_ACTION_VIM_DOWN:
        return motion_n(term, &motion_down, n);

    case BIND_ACTION_VIM_LEFT:
        return motion_n(term, &motion_left, n);

    case BIND_ACTION_VIM_RIGHT:
        return motion_n(term, &motion_right, n);

    case BIND_ACTION_VIM_FIRST:
        return motion(term, &motion_first);

    case BIND_ACTION_VIM_LAST:
        return motion(term, &motion_last);

    case BIND_ACTION_VIM_FIRST_OCCUPIED:
        return motion(term, &motion_first_occupied);

    case BIND_ACTION_VIM_HIGH:
        return motion_view_row(term, n - 1);

    case BIND_ACTION_VIM_MIDDLE:
        return motion(term, &motion_middle);

    case BIND_ACTION_VIM_LOW:
        return motion_view_row(term, term->rows - n);

    case BIND_ACTION_VIM_SEMANTIC_LEFT:
        return motion_n(term, &motion_semantic_left, n);

    case BIND_ACTION_VIM_SEMANTIC_RIGHT:
        return motion_n(term, &motion_semantic_right, n);

    case BIND_ACTION_VIM_SEMANTIC_LEFT_END:
        return motion_n(term, &motion_semantic_left_end, n);

    case BIND_ACTION_VIM_SEMANTIC_RIGHT_END:
        return motion_n(term, &motion_semantic_right_end, n);

    case BIND_ACTION_VIM_WORD_LEFT:
        return motion_n(term, &motion_word_left, n);

    case BIND_ACTION_VIM_WORD_RIGHT:
        return motion_n(term, &motion_word_right, n);

    case BIND_ACTION_VIM_WORD_LEFT_END:
        return motion_n(term, &motion_word_left_end, n);

    case BIND_ACTION_VIM_WORD_RIGHT_END:
        return motion_n(term, &motion_word_right_end, n);

    case BIND_ACTION_VIM_BRACKET:
        return motion(term, &motion_bracket);

    case BIND_ACTION_VIM_PARAGRAPH_UP:
        return motion_n(term, &motion_paragraph_up, n);

    case BIND_ACTION_VIM_PARAGRAPH_DOWN:
        return motion_n(term, &motion_paragraph_down, n);

    case BIND_ACTION_VIM_SCROLLBACK_UP_PAGE:
        return scroll_action(term, -scroll_amount(term, term->rows, n));

    case BIND_ACTION_VIM_SCROLLBACK_UP_HALF_PAGE:
        return scroll_action(
            term, -scroll_amount(term, max(term->rows / 2, 1), n));

    case BIND_ACTION_VIM_SCROLLBACK_UP_LINE:
        /* vim_mode_view_changed() drags the cursor along */
        cmd_scrollback_up(term, scroll_amount(term, 1, n));
        return true;

    case BIND_ACTION_VIM_SCROLLBACK_DOWN_PAGE:
        return scroll_action(term, scroll_amount(term, term->rows, n));

    case BIND_ACTION_VIM_SCROLLBACK_DOWN_HALF_PAGE:
        return scroll_action(
            term, scroll_amount(term, max(term->rows / 2, 1), n));

    case BIND_ACTION_VIM_SCROLLBACK_DOWN_LINE:
        cmd_scrollback_down(term, scroll_amount(term, 1, n));
        return true;

    case BIND_ACTION_VIM_SCROLLBACK_HOME:
        return scroll_home_action(term);

    case BIND_ACTION_VIM_SCROLLBACK_END:
        return scroll_end_action(term);

    case BIND_ACTION_VIM_CENTER_CURSOR:
        return scroll_cursor_to_view_row(term, term->rows / 2 - 1);

    case BIND_ACTION_VIM_SCROLL_CURSOR_TO_TOP:
        return scroll_cursor_to_view_row(term, 0);

    case BIND_ACTION_VIM_SCROLL_CURSOR_TO_BOTTOM:
        return scroll_cursor_to_view_row(term, term->rows - 1);

    case BIND_ACTION_VIM_TOGGLE_NORMAL_SELECTION:
        return toggle_selection(term, SELECTION_CHAR_WISE);

    case BIND_ACTION_VIM_TOGGLE_LINE_SELECTION:
        return toggle_selection(term, SELECTION_LINE_WISE);

    case BIND_ACTION_VIM_TOGGLE_BLOCK_SELECTION:
        return toggle_selection(term, SELECTION_BLOCK);

    case BIND_ACTION_VIM_TOGGLE_SEMANTIC_SELECTION:
        return toggle_selection(term, SELECTION_WORD_WISE);

    case BIND_ACTION_VIM_CLEAR_SELECTION:
        selection_cancel(term);
        return true;

    case BIND_ACTION_VIM_COPY:
        return yank(seat, term, serial);

    case BIND_ACTION_VIM_COPY_TO_END_OF_LINE:
        toggle_selection(term, SELECTION_CHAR_WISE);
        motion(term, &motion_last);
        return yank(seat, term, serial);

    /* The count is kept pending until the target character is typed,
     * see vim_mode_input() */
    case BIND_ACTION_VIM_INLINE_SEARCH_FORWARD:
        return inline_search_start(term, false, false);

    case BIND_ACTION_VIM_INLINE_SEARCH_BACKWARD:
        return inline_search_start(term, true, false);

    case BIND_ACTION_VIM_INLINE_SEARCH_FORWARD_SHORT:
        return inline_search_start(term, false, true);

    case BIND_ACTION_VIM_INLINE_SEARCH_BACKWARD_SHORT:
        return inline_search_start(term, true, true);

    case BIND_ACTION_VIM_INLINE_SEARCH_NEXT:
        inline_search_exec(term, term->vim.inline_search.backward, n, true);
        return true;

    case BIND_ACTION_VIM_INLINE_SEARCH_PREVIOUS:
        inline_search_exec(term, !term->vim.inline_search.backward, n, true);
        return true;

    case BIND_ACTION_VIM_SEARCH_START:
        term->vim.search_backward = false;
        search_begin(term);
        return true;

    case BIND_ACTION_VIM_SEARCH_START_BACKWARD:
        term->vim.search_backward = true;
        search_begin(term);
        return true;

    case BIND_ACTION_VIM_SEARCH_NEXT:
        return search_jump(term, term->vim.search_backward, n);

    case BIND_ACTION_VIM_SEARCH_PREVIOUS:
        return search_jump(term, !term->vim.search_backward, n);

    case BIND_ACTION_VIM_COUNT:
        BUG("Invalid action type");
        return true;
    }

    BUG("Unhandled action type");
    return false;
}

/*
 * Built-in two-key commands, like in vi.
 *
 * A prefix key (g, z) only starts a command when it is not bound to
 * an action itself; binding e.g. 'g' in [vim-bindings] disables all
 * g-commands. The actions are regular actions, and can additionally
 * be bound to single keys.
 */
static const struct {
    xkb_keysym_t prefix;
    xkb_keysym_t key;
    enum bind_action_vim action;
} sequences[] = {
    {XKB_KEY_g, XKB_KEY_g, BIND_ACTION_VIM_SCROLLBACK_HOME},
    {XKB_KEY_g, XKB_KEY_e, BIND_ACTION_VIM_SEMANTIC_LEFT_END},
    {XKB_KEY_g, XKB_KEY_E, BIND_ACTION_VIM_WORD_LEFT_END},
    {XKB_KEY_z, XKB_KEY_z, BIND_ACTION_VIM_CENTER_CURSOR},
    {XKB_KEY_z, XKB_KEY_t, BIND_ACTION_VIM_SCROLL_CURSOR_TO_TOP},
    {XKB_KEY_z, XKB_KEY_b, BIND_ACTION_VIM_SCROLL_CURSOR_TO_BOTTOM},
};

/* Largest count prefix accepted (further digits are ignored) */
#define VIM_MAX_COUNT 9999

static bool
is_prefix_key(xkb_keysym_t sym)
{
    for (size_t i = 0; i < ALEN(sequences); i++) {
        if (sequences[i].prefix == sym)
            return true;
    }
    return false;
}

static enum bind_action_vim
sequence_action(xkb_keysym_t prefix, xkb_keysym_t key)
{
    for (size_t i = 0; i < ALEN(sequences); i++) {
        if (sequences[i].prefix == prefix && sequences[i].key == key)
            return sequences[i].action;
    }
    return BIND_ACTION_VIM_NONE;
}

/* The value of a digit key, or -1 */
static int
digit_value(xkb_keysym_t sym)
{
    if (sym >= XKB_KEY_0 && sym <= XKB_KEY_9)
        return sym - XKB_KEY_0;
    if (sym >= XKB_KEY_KP_0 && sym <= XKB_KEY_KP_9)
        return sym - XKB_KEY_KP_0;
    return -1;
}

/* Copied from input.c (which copied it from libxkbcommon) */
static bool
keysym_is_modifier(xkb_keysym_t keysym)
{
    return
        (keysym >= XKB_KEY_Shift_L && keysym <= XKB_KEY_Hyper_R) ||
        (keysym >= XKB_KEY_ISO_Lock && keysym <= XKB_KEY_ISO_Last_Group_Lock) ||
        keysym == XKB_KEY_Mode_switch ||
        keysym == XKB_KEY_Num_Lock;
}

static const struct key_binding *
find_binding(const struct key_binding_set *bindings, uint32_t key,
             xkb_keysym_t sym, xkb_mod_mask_t mods, xkb_mod_mask_t consumed,
             const xkb_keysym_t *raw_syms, size_t raw_count)
{
    /* Match untranslated symbols */
    tll_foreach(bindings->vim, it) {
        const struct key_binding *bind = &it->item;

        if (bind->mods != mods || bind->mods == 0)
            continue;

        for (size_t i = 0; i < raw_count; i++) {
            if (bind->k.sym == raw_syms[i])
                return bind;
        }
    }

    /* Match translated symbol */
    tll_foreach(bindings->vim, it) {
        const struct key_binding *bind = &it->item;

        if (bind->k.sym == sym && bind->mods == (mods & ~consumed))
            return bind;
    }

    /* Match raw key code */
    tll_foreach(bindings->vim, it) {
        const struct key_binding *bind = &it->item;

        if (bind->mods != mods || bind->mods == 0)
            continue;

        tll_foreach(bind->k.key_codes, code) {
            if (code->item == key)
                return bind;
        }
    }

    return NULL;
}

void
vim_mode_pending_keys(const struct terminal *term, char *buf, size_t size)
{
    const int count = term->vim.pending.count;
    const xkb_keysym_t sym = term->vim.pending.prefix != 0
        ? term->vim.pending.prefix
        : term->vim.inline_search.char_pending
            ? term->vim.pending.char_cmd
            : 0;

    char key[16] = {0};
    if (sym != 0 && xkb_keysym_to_utf8(sym, key, sizeof(key)) <= 0)
        key[0] = '\0';

    if (count > 0)
        snprintf(buf, size, "%d%s ", count, key);
    else if (key[0] != '\0')
        snprintf(buf, size, "%s ", key);
    else if (size > 0)
        buf[0] = '\0';
}

void
vim_mode_input(struct seat *seat, struct terminal *term,
               const struct key_binding_set *bindings, uint32_t key,
               xkb_keysym_t sym, xkb_mod_mask_t mods, xkb_mod_mask_t consumed,
               const xkb_keysym_t *raw_syms, size_t raw_count,
               uint32_t serial)
{
    LOG_DBG("vim mode: input: sym=%d/0x%x, mods=0x%08x, consumed=0x%08x",
            sym, sym, mods, consumed);

    if (term->vim.inline_search.char_pending) {
        /* Uses, and then clears, the pending count */
        inline_search_capture(seat, term, key);

        if (!term->vim.inline_search.char_pending) {
            pending_reset(term);
            render_refresh(term);
        }
        return;
    }

    /* Pressing e.g. shift, to type the 'E' in 'gE', must not abort a
     * pending command */
    if (keysym_is_modifier(sym))
        return;

    /* Modifiers beyond those consumed by the key itself. Zero for a
     * plain key press, including shifted characters like 'E' */
    const xkb_mod_mask_t plain_mods =
        mods & ~consumed & seat->kbd.legacy_significant;

    /* Second key of a two-key command */
    if (term->vim.pending.prefix != 0) {
        const xkb_keysym_t prefix = term->vim.pending.prefix;
        const int count = term->vim.pending.count;
        pending_reset(term);

        /* Anything that doesn't complete a command aborts it, like in vi */
        const enum bind_action_vim action = plain_mods == 0
            ? sequence_action(prefix, sym)
            : BIND_ACTION_VIM_NONE;

        if (action != BIND_ACTION_VIM_NONE &&
            execute_action(seat, term, action, count, serial))
        {
            seat->kbd.last_shortcut_sym = sym;
        }

        render_refresh(term);
        return;
    }

    /* Count prefix. '0' is only part of a count when it isn't the
     * first digit; on its own, it is a regular key (first column) */
    if (plain_mods == 0) {
        const int digit = digit_value(sym);

        if (digit > 0 || (digit == 0 && term->vim.pending.count > 0)) {
            term->vim.pending.count =
                min(term->vim.pending.count * 10 + digit, VIM_MAX_COUNT);
            render_refresh(term);
            return;
        }
    }

    /* Escape only aborts a pending count */
    if (sym == XKB_KEY_Escape && term->vim.pending.count > 0) {
        pending_reset(term);
        render_refresh(term);
        return;
    }

    const struct key_binding *bind = find_binding(
        bindings, key, sym, mods, consumed, raw_syms, raw_count);

    if (bind != NULL) {
        const int count = term->vim.pending.count;

        if (execute_action(seat, term, bind->action, count, serial))
            seat->kbd.last_shortcut_sym = sym;

        if (term->vim.active && term->vim.inline_search.char_pending) {
            /* f/F/t/T: keep the count until the character is typed */
            term->vim.pending.char_cmd = sym;
        } else
            pending_reset(term);

        render_refresh(term);
        return;
    }

    /* First key of a two-key command */
    if (plain_mods == 0 && is_prefix_key(sym)) {
        term->vim.pending.prefix = sym;
        render_refresh(term);
        return;
    }

    /* All other input is swallowed while in vim mode, and aborts any
     * pending count */
    if (term->vim.pending.count > 0) {
        pending_reset(term);
        render_refresh(term);
    }
}
