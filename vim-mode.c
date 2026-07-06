#include "vim-mode.h"

#define LOG_MODULE "vim-mode"
#define LOG_ENABLE_DBG 0
#include "log.h"

#include "commands.h"
#include "config.h"
#include "grid.h"
#include "render.h"
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

static struct vim_ctx
ctx_for_term(struct terminal *term)
{
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
motion_high(struct vim_ctx *ctx)
{
    move_to_view_row(ctx, 0);
}

static void
motion_middle(struct vim_ctx *ctx)
{
    move_to_view_row(ctx, max(ctx->term->rows / 2 - 1, 0));
}

static void
motion_low(struct vim_ctx *ctx)
{
    move_to_view_row(ctx, ctx->term->rows - 1);
}

static bool
motion(struct terminal *term, void (*fn)(struct vim_ctx *ctx))
{
    struct vim_ctx ctx = ctx_for_term(term);
    fn(&ctx);
    apply_cursor(&ctx);
    return true;
}

void
vim_mode_begin(struct terminal *term)
{
    if (term->vim.active)
        return;

    LOG_DBG("vim mode: begin");

    selection_cancel(term);

    /* Place the cursor at the terminal cursor if it is inside the
     * current viewport, otherwise at the top-left of the viewport */
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

    term->vim.active = true;

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

    if (term->vim.reenable_ime) {
        term->vim.reenable_ime = false;
        term_ime_enable(term);
    }

    render_refresh(term);
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
    render_refresh(term);
}

void
vim_mode_resized(struct terminal *term)
{
    if (!term->vim.active)
        return;

    struct grid *grid = term->grid;

    term->vim.cursor.row &= grid->num_rows - 1;
    term->vim.cursor.col = max(min(term->vim.cursor.col, term->cols - 1), 0);

    if (grid->rows[term->vim.cursor.row] == NULL) {
        term->vim.cursor = (struct coord){.col = 0, .row = grid->view};
        return;
    }

    /* Ensure the cursor is not in the unallocated part of the ring */
    struct vim_ctx ctx = ctx_for_term(term);
    term->vim.cursor.row = sb_to_abs(&ctx, ctx.pos.row);
    term->vim.cursor.col = ctx.pos.col;
}

static bool
execute_binding(struct seat *seat, struct terminal *term,
                const struct key_binding *binding, uint32_t serial)
{
    const enum bind_action_vim action = binding->action;

    switch (action) {
    case BIND_ACTION_VIM_NONE:
        return false;

    case BIND_ACTION_VIM_CANCEL:
        term_reset_view(term);
        vim_mode_cancel(term);
        return true;

    case BIND_ACTION_VIM_UP:
        return motion(term, &motion_up);

    case BIND_ACTION_VIM_DOWN:
        return motion(term, &motion_down);

    case BIND_ACTION_VIM_LEFT:
        return motion(term, &motion_left);

    case BIND_ACTION_VIM_RIGHT:
        return motion(term, &motion_right);

    case BIND_ACTION_VIM_FIRST:
        return motion(term, &motion_first);

    case BIND_ACTION_VIM_LAST:
        return motion(term, &motion_last);

    case BIND_ACTION_VIM_FIRST_OCCUPIED:
        return motion(term, &motion_first_occupied);

    case BIND_ACTION_VIM_HIGH:
        return motion(term, &motion_high);

    case BIND_ACTION_VIM_MIDDLE:
        return motion(term, &motion_middle);

    case BIND_ACTION_VIM_LOW:
        return motion(term, &motion_low);

    case BIND_ACTION_VIM_COUNT:
        BUG("Invalid action type");
        return true;
    }

    BUG("Unhandled action type");
    return false;
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

    /* Match untranslated symbols */
    tll_foreach(bindings->vim, it) {
        const struct key_binding *bind = &it->item;

        if (bind->mods != mods || bind->mods == 0)
            continue;

        for (size_t i = 0; i < raw_count; i++) {
            if (bind->k.sym == raw_syms[i]) {
                if (execute_binding(seat, term, bind, serial))
                    seat->kbd.last_shortcut_sym = sym;
                return;
            }
        }
    }

    /* Match translated symbol */
    tll_foreach(bindings->vim, it) {
        const struct key_binding *bind = &it->item;

        if (bind->k.sym == sym &&
            bind->mods == (mods & ~consumed))
        {
            if (execute_binding(seat, term, bind, serial))
                seat->kbd.last_shortcut_sym = sym;
            return;
        }
    }

    /* Match raw key code */
    tll_foreach(bindings->vim, it) {
        const struct key_binding *bind = &it->item;

        if (bind->mods != mods || bind->mods == 0)
            continue;

        tll_foreach(bind->k.key_codes, code) {
            if (code->item == key) {
                if (execute_binding(seat, term, bind, serial))
                    seat->kbd.last_shortcut_sym = sym;
                return;
            }
        }
    }

    /* All other input is swallowed while in vim mode */
}
