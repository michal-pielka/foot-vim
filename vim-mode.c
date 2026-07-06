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
