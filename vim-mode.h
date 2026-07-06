#pragma once

#include <xkbcommon/xkbcommon.h>

#include "key-binding.h"
#include "terminal.h"

void vim_mode_begin(struct terminal *term);
void vim_mode_cancel(struct terminal *term);
void vim_mode_input(
    struct seat *seat, struct terminal *term,
    const struct key_binding_set *bindings, uint32_t key,
    xkb_keysym_t sym, xkb_mod_mask_t mods, xkb_mod_mask_t consumed,
    const xkb_keysym_t *raw_syms, size_t raw_count,
    uint32_t serial);

/* Drag the cursor along when the viewport has been scrolled */
void vim_mode_view_changed(struct terminal *term);

/* Re-clamp the cursor after the grid has been resized */
void vim_mode_resized(struct terminal *term);

static inline bool
vim_mode_is_active(const struct terminal *term)
{
    return term->vim.active;
}
