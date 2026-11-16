#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 youkorr
# Part of ppa-alternative: https://github.com/youkorr/ppa-alternative-
"""The PPA tests, checked against copies of lv_draw_ortho with a wait taken out.

The fake PPA runs a queued transaction only when the caller waits, so a wait
lv_draw_ortho forgets leaves the frame buffer -- or a tile -- as it was, and a
picture comes out wrong. Each mutant below takes one wait out; `ortho_test
--ppa` must fail on every one, or it is not testing that wait. Run after
`make`:

    python3 ppa_mutants.py LVGL_BUILD_DIR      (default: build)

`--only WORDS` runs only the mutants whose name holds WORDS.

With `--cores BUILD_DIR` (an LVGL built with LV_USE_OS=1, see test/Makefile),
it checks the second core instead: `ortho_test --cores` must fail on each of
CORES_MUTANTS.
"""

import pathlib
import shutil
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
ORTHO = HERE.parent / "components" / "lvgl_ortho"
MUTANTS = {
    # A tile drawn on while the PPA is still turning it out.
    "a tile reused before its turn finished": ("            wait_ticket(&u->ticket[k]);\n", ""),
    # A tile drawn on before the PPA has read the frame buffer back into it.
    "a tile drawn before it was read back": ("        if(!wait_ticket(&t))   /* it is drawn on next */\n",
                                             "        if(0)\n"),
    # The layer handed back to LVGL with turns still running.
    "a run of tasks left running": ("    fence();\n    for(int i = 0; i < 4; i++) u->ticket[i] = 0;\n",
                                     "    for(int i = 0; i < 4; i++) u->ticket[i] = 0;\n"),
    # A camera frame handed back to the application mid-turn.
    "a picture given back mid-turn": (
        "    fence();   /* the picture is the application's again when the task is finished */\n", ""),
    # DIG-734: a block the P4 never finishes sent whole (esp-idf #19023).
    "DIG-734 blocks sent whole": (
        "    if(!cut(0, 0, bw, bh, rot, px * 8, pieces, &n, 0)) return 0;\n",
        "    pieces[n++] = (piece_t) {0, 0, bw, bh};\n"),
    # The drivers' own test taken for the need: a last macro block of exactly
    # the FIFO's depth sent without the bypass, which a Tab5 stalled on
    # (464x70 at 90).
    "the FIFO's own depth taken for enough": ("    return several && (needed ? left < 2 * FIFO_BITS : left < FIFO_BITS);\n",
                                              "    return several && left < FIFO_BITS;\n"),
    # The margin of a whole FIFO dropped: the exact depth kept, nothing above.
    "no margin above the FIFO's depth": ("    return several && (needed ? left < 2 * FIFO_BITS : left < FIFO_BITS);\n",
                                         "    return several && (needed ? left <= FIFO_BITS : left < FIFO_BITS);\n"),
    # Cuts anywhere, as before: strips as thin as one pixel.
    "slivers cut": ("#define SIDE 16\n", "#define SIDE 1\n"),
    # A wait with no bound: a stuck PPA freezes the screen (the driver's own
    # blocking mode). The test would hang, so the run has a timeout.
    "a wait with no bound": ("        else if(idle_us >= limit) {",
                             "        else if(0) {"),
    # A wait that never counts the time it slept: the bound is never reached.
    "a sleep not counted": ("        idle_us += pause_for_ppa(limit - LV_MIN(idle_us, limit));",
                            "        pause_for_ppa(limit - LV_MIN(idle_us, limit));"),
    # A stalled PPA not given up on for good: the video tries it again, and
    # waits out the timeout every frame.
    "a stuck PPA used again": ("    if(g_stuck) return ESP_ERR_INVALID_STATE;\n    esp_err_t err = ensure_client();",
                               "    esp_err_t err = ensure_client();"),
    # A fill given the colour at 8 bits a channel: a PPA that rounds when it
    # narrows to RGB565 would put a colour one step off LVGL's. The fake
    # truncates, so the mutant is the fill colour narrowed the other way.
    "a fill colour off by a step": ("    const uint32_t r = cf == LV_COLOR_FORMAT_RGB565 ? (color.red & 0xF8u) : color.red;",
                                    "    const uint32_t r = cf == LV_COLOR_FORMAT_RGB565 ? ((color.red + 4u > 255u ? 255u : color.red + 4u) & 0xF8u) : color.red;"),
    # A fill left undrawn because some later picture exists, covering it or not.
    "a fill skipped that a picture does not cover": ("        if(lv_area_is_in(todo, &covered, 0)) return true;",
                                                     "        return true;"),
    # A picture turned once, found again at another rotation: the copy turned
    # for 90 shown at 270.
    "the picture cache blind to the rotation": (" && c->rot == rot) return c;", ") return c;"),
    # A picture changed in place and dropped, still drawn from its old copy.
    "a dropped picture kept": ("        if(data == NULL || g_cache[i].data == data) cache_forget(&g_cache[i]);",
                               "        if(data == NULL) cache_forget(&g_cache[i]);"),
    # The CPU copying out of the cache before the PPA has turned into it.
    "a copy out of the cache before its turn finished": (
        "    fence();\n    const uint8_t * sp = src", "    const uint8_t * sp = src"),
    # The copy out of the cache placed by the upright area, not the native one.
    "the cache copied from the upright place": (
        "                        lv_area_get_width(&whole) * px, &whole, &parts[i], true);",
        "                        lv_area_get_width(&whole) * px, &t->area, &parts[i], true);"),
    # A partial band sent to the panel with the copy out of the cache still
    # running.
    "a band sent mid-copy": ("    /* Drawn alone, so nothing else waits for these before LVGL takes the\n"
                             "     * buffer back -- in partial mode, to send it to the panel. */\n    fence();\n", ""),
    # A canvas -- a draw buffer LVGL changes -- kept turned.
    "a modifiable picture kept": ("    if(img->header.flags & LV_IMAGE_FLAGS_MODIFIABLE) return false;\n", ""),
    # tile_width taken and never used.
    "the tile width ignored": ("    if(g_tile_width > 0 && tw > g_tile_width) tw = g_tile_width;\n", ""),
    # A split cut between two rows that share a cache line.
    "a split cut inside a cache line": (
        "    return last / LV_DRAW_ORTHO_SPLIT_LINE != first / LV_DRAW_ORTHO_SPLIT_LINE;\n", "    return true;\n"),
    # The turner's band one row longer than the cut: one row turned by both.
    "the turner's band past the cut": ("    first.y2 = r1 - 1;\n", "    first.y2 = r1;\n"),
    # This core turning the whole picture over the turner's band.
    "this core's band over the turner's": ("    mine.y1 = helped ? r2 : r1;\n", "    mine.y1 = rect->y1;\n"),
    # The share asked for, ignored.
    "the split share ignored": ("    const float share = g_split_fixed ? g_split_fixed / 1000.0f\n",
                                "    const float share = 0 ? g_split_fixed / 1000.0f\n"),
    # A picture in the flash not recognised: handed to the PPA, as before,
    # which reads the flash more slowly than the CPU.
    "the flash not recognised": ("    if(g_flash_cb) return g_flash_cb(img);\n#if defined(ESP_PLATFORM)\n    return esp_ptr_in_drom",
                                 "    if(0) return g_flash_cb(img);\n#if defined(ESP_PLATFORM)\n    return esp_ptr_in_drom"),
    # The picture cache filled by the PPA straight out of the flash.
    "the cache filled from the flash by the PPA": ("    if(in_flash(img)) {\n        /* Read once",
                                                   "    if(0) {\n        /* Read once"),
    # LVGL's copies between frame buffers handed back to it as they come.
    "LVGL's copies made at once": ("            sync_behind(d, &a);\n", "            g_sync.orig[d]->buf_copy_cb(dest, dest_area, src, src_area);\n"),
    # A copy LVGL makes in a refresh with nothing to draw, dropped.
    "an empty refresh's copies dropped": ("            sync_behind(d, &a);\n", "            if(g_sync.disp->inv_p) sync_behind(d, &a);\n"),
    # What a frame draws subtracted where the UI has it, not the panel.
    "the frame's areas not turned to the panel": (
        "        if(lv_display_get_matrix_rotation(disp)) lv_display_rotate_area(disp, &drawn[nd]);\n", ""),
    # Brought up to date from itself instead of the buffer shown last.
    "a buffer repaired from itself": ("    sync_repair(a, sync_on_screen(disp), drawn, nd);",
                                      "    sync_repair(a, disp->buf_act, drawn, nd);"),
    # The repair left running while LVGL draws.
    "the repair left running": ("    sync_repair(a, sync_on_screen(disp), drawn, nd);\n    /* Done before anything is drawn: LVGL's own units, and the PPA's fills,\n     * are not in the turner's queue. */\n    fence();\n",
                                "    sync_repair(a, sync_on_screen(disp), drawn, nd);\n"),
    # Given back to LVGL with copies still written down.
    "given back with copies owed": ("    if(g_sync.disp) sync_settle_and_release();\n    sync_unhook();\n",
                                    "    if(g_sync.disp) sync_release();\n    sync_unhook();\n"),
    # The adapter's copy at the frame's start taken for one of LVGL's: it is
    # written down, and the frame is drawn over the buffer it left behind.
    "another copy written down": (
        "    if(same_place && disp && lv_refr_get_disp_refreshing() == disp && sync_area_of_lvgl(disp, dest_area)) {",
        "    if(same_place && disp && lv_refr_get_disp_refreshing() == disp && !disp->rendering_in_progress) {"),
    # That copy queued for the PPA and not waited for.
    "another copy left queued": ("            fence();\n            g_stats.sync_others++;", "            g_stats.sync_others++;"),
    # Buffers set again, and the copies left to LVGL from then on.
    "buffers set again, never taken again": ("        if(!sync_install(disp)) return;\n", "        return;\n"),
    # A card's shadow taken as hidden only where its background is the same
    # rectangle, as before: a card with a border has its background a pixel
    # inside, so every tile in it was read back first.
    "a shadow hidden only under a background of its own size": (
        "            if(f->opa >= LV_OPA_MAX && f->grad.dir == LV_GRAD_DIR_NONE) {",
        "            if(f->opa >= LV_OPA_MAX && f->grad.dir == LV_GRAD_DIR_NONE && lv_area_is_in(&t->area, &t->next->area, 0)) {"),
    # The rounded corners of that background counted as painted.
    "a background's rounded corners counted as covering the shadow": (
        "                cross(j, fa, 0, r, &t->next->clip_area);", "                cross(j, fa, 0, 0, &t->next->clip_area);"),
    # Only one arm of the cross: a run along a card, close to both its edges,
    # read back.
    "one arm of the cross": ("    for(int k = 0; k < 2; k++) {\n        lv_area_t h = *a;",
                             "    for(int k = 1; k < 2; k++) {\n        lv_area_t h = *a;"),
    # A rounded border's corners counted as clear of it.
    "a border's rounded corners counted as clear": (
        "        cross(j, &t->area, d->width, LV_MAX(d->width, d->radius), &t->area);",
        "        cross(j, &t->area, d->width, d->width, &t->area);"),
    # A camera canvas smaller than the screen: its background filled whole
    # under the frame every time, as before, instead of the ring round it.
    "a fill under a picture drawn whole": (
        "    if(covered_later(t, todo, &covered)) {",
        "    if(0 && covered_later(t, todo, &covered)) {"),
    # ...and the ring round it left unfilled.
    "the ring round a picture left unfilled": (
        "        nk = area_minus(todo, &covered, keep);",
        "        nk = 0;"),
    # ...or one band of it.
    "a band of the ring left unfilled": (
        "    if(b->y2 < a->y2) lv_area_set(&out[n++], a->x1, b->y2 + 1, a->x2, a->y2);\n", ""),
    # NOT here: a picture with alpha counted as covering a fill. plain_image()
    # already wants the layer's own format, none of which has alpha, so
    # covered_later()'s check is a guard that cannot fail.
    # Through internal RAM: a band copied in while the PPA still reads it.
    "a band copied over one the PPA still reads": (
        "            if(g_stage_reader[(i + 1) & 1] && !wait(g_stage_reader[(i + 1) & 1])) return g_queued;\n", ""),
    # ...a band turned before its copy has landed.
    "a band turned before it arrived": ("        if(!copies_done()) {", "        if(0) {"),
    # ...the next band's rows taken from the wrong place in the picture.
    "the next band copied from this band's rows": (
        "            if(!copy_rows(src, (uint32_t)block->y1 + row + k, next_rows, g_band[(i + 1) & 1])) {",
        "            if(!copy_rows(src, (uint32_t)block->y1 + row, next_rows, g_band[(i + 1) & 1])) {"),
    # ...every band put where the first one goes.
    "every band placed as the first": (
        "        place(rot, bw, bh, p->x, row + p->y, p->w, p->h, &ox, &oy);",
        "        place(rot, bw, bh, p->x, p->y, p->w, p->h, &ox, &oy);"),
    # ...the last, shorter band cut as a full one.
    "the last band cut as a full one": (
        "        const piece_t * pieces = rows == k ? full : last;", "        const piece_t * pieces = full;"),
    # ...a refused copy left without the rest turned straight.
    "a refused copy leaving the rest unturned": (
        "                g_stage_stats.declined++;\n                for(uint32_t r = row + k; r < bh; r += k) {",
        "                g_stage_stats.declined++;\n                for(uint32_t r = bh; r < bh; r += k) {"),
    # The first tile lent as bands: copied into over lines the CPU may still
    # have dirty in the cache...
    "a lent tile left in the cache": (
        "        if(esp_cache_msync(g_loan, 2 * half, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE) != ESP_OK)\n"
        "            return false;\n", ""),
    # ...its two halves made one band...
    "the lent tile's two bands the same": ("    g_band[1] = g_loan + half;", "    g_band[1] = g_loan;"),
    # ...and the tile never lent at all.
    "no tile lent": ("    if(lend) g_loan(tile_aligned(g_unit->tile_alloc[0]), LV_DRAW_ORTHO_TILE_BYTES);",
                     "    if(0) g_loan(tile_aligned(g_unit->tile_alloc[0]), LV_DRAW_ORTHO_TILE_BYTES);"),
    # NOT here: the wait for the lent tile's own turns before the first copy.
    # lv_draw_ortho takes a picture alone, and the run of tasks before it ends
    # with a fence, so nothing is queued when the tile is lent: the wait
    # returns at once, a guard for a caller that lends without one.
    # NOT here: the dedicated bands written back out of the cache once taken
    # (set_stage), and the wait for an earlier picture still reading the
    # first band. The first is outside every frame a test can watch, and
    # every picture lv_draw_ortho turns is fenced before the next: both
    # guards are for the board, where a dirty line or an interrupted picture
    # (a part outside a video window) can meet them.
    # The PPA's angle the wrong way round.
    "the PPA turning the other way": (
        "        case LV_DISPLAY_ROTATION_90:\n            return PPA_SRM_ROTATION_ANGLE_90;",
        "        case LV_DISPLAY_ROTATION_90:\n            return PPA_SRM_ROTATION_ANGLE_270;"),
}
# The second core: each must make `ortho_test --cores` fail. The third item
# is extra compiler flags: LV_DRAW_ORTHO_TEST_TILES=2 leaves no room for more
# than two blocks, so the four tiles are halves of those.
CORES_MUTANTS = {
    # The other core's tile handed to it while the PPA still turns it out.
    "the other core's tile reused mid-turn": ("    wait_ticket(&u->ticket[base + 1]);\n", "", []),
    # The other core's tile turned out before it has finished painting it.
    "turned out before the other core finished": (
        "    if(handed) {\n        helper_join();\n        g_stats.tiles_other_core++;\n    }\n    else if(ok[1]) paint_tile(&w[1]);\n"
        "    if(ok[0]) finish_tile(o, &w[0], &u->ticket[base]);\n    if(ok[1]) finish_tile(o, &w[1], &u->ticket[base + 1]);\n",
        "    if(ok[0]) finish_tile(o, &w[0], &u->ticket[base]);\n    if(ok[1]) finish_tile(o, &w[1], &u->ticket[base + 1]);\n"
        "    if(handed) {\n        helper_join();\n        g_stats.tiles_other_core++;\n    }\n    else if(ok[1]) paint_tile(&w[1]);\n", []),
    # Tiles cut from one block all at its start: the cores paint over each other.
    "tiles cut from a block overlapping": (
        "        u->tile[j] = j < want ? tile_aligned(u->tile_alloc[j / per]) + (size_t)(j % per) * bytes : NULL;\n",
        "        u->tile[j] = j < want ? tile_aligned(u->tile_alloc[j / per]) : NULL;\n",
        ["-DLV_DRAW_ORTHO_TEST_TILES=2"]),
    # Text on both cores at once: a font engine that is not safe there (as
    # ESPHome's is not) hands out the wrong glyph.
    "text on both cores at once": (
        "            if(text) lv_mutex_lock(&g_unit->text_mutex);\n", "", []),
    # An area that fits in one tile left to the first core alone, as it was:
    # a dashboard then runs on one core.
    "one-tile areas left to one core": (
        "    if(two && !cut_shows(jobs, n)) {\n        const int32_t h = lv_area_get_height(&box);",
        "    if(0) {\n        const int32_t h = lv_area_get_height(&box);", []),
    # The pair's first tile drawn into the other core's tile.
    "both cores in one tile": (
        "    ok[0] = prep_tile(jobs, n, o, &rect[0], u->tile[base], u->tile_bytes, &w[0]);",
        "    ok[0] = prep_tile(jobs, n, o, &rect[0], u->tile[base + 1], u->tile_bytes, &w[0]);", []),
}

# NOT here: the fence before the CPU turns what the PPA declined. The two never
# write the same pixel; what they can share is a cache line at the edge of
# their rectangles, and the fake PPA has no cache to share it in. It stays in
# lv_draw_ortho for the board, as lv_draw_ortho_video.c's write-back does.


def main():
    args = sys.argv[1:]
    only = None
    if "--only" in args:
        at = args.index("--only")
        only = args[at + 1]
        args = args[:at] + args[at + 2:]
    cores = bool(args) and args[0] == "--cores"
    if cores:
        args = args[1:]
    mutants = CORES_MUTANTS if cores else {k: (o, n, []) for k, (o, n) in MUTANTS.items()}
    if only:
        mutants = {k: v for k, v in mutants.items() if only in k}
    build = HERE / (args[0] if args else ("build-mt" if cores else "build"))
    lvgl = build / "lvgl.a"
    src = build / "lvgl-src"
    if not lvgl.exists():
        sys.exit(f"{lvgl} is missing: run make first")
    failed = 0
    with tempfile.TemporaryDirectory() as tmp:
        for name, (old, new, flags) in mutants.items():
            copy = pathlib.Path(tmp) / "c"
            shutil.rmtree(copy, ignore_errors=True)
            shutil.copytree(ORTHO, copy)
            hit = 0
            for f in ("lv_draw_ortho.c", "lv_draw_ortho_ppa.c"):
                p = copy / f
                text = p.read_text()
                hit += text.count(old)
                p.write_text(text.replace(old, new))
            assert hit == 1, f"mutant '{name}' no longer applies ({hit} matches)"
            exe = pathlib.Path(tmp) / "t"
            os_flags = ["-DLV_USE_OS=1"] if cores else []
            cmd = ["cc", "-O2", "-DLV_CONF_INCLUDE_SIMPLE", *os_flags, *flags, f"-I{HERE}", f"-I{copy}", f"-I{src}",
                   f"-I{HERE}/p4stub", "-w", str(HERE / "main.c"), str(copy / "lv_draw_ortho.c"),
                   str(copy / "lv_draw_ortho_video.c"), str(copy / "lv_draw_ortho_ppa.c"),
                   str(HERE / "p4stub" / "p4fake.c"), str(lvgl), "-lm", "-lpthread", "-o", str(exe)]
            subprocess.run(cmd, check=True)
            try:
                r = subprocess.run([str(exe), "--cores" if cores else "--ppa"], capture_output=True, text=True,
                                   timeout=300)
            except subprocess.TimeoutExpired:
                print(f"  ok    caught: {name} (hung, as a board would)")
                continue
            caught = r.returncode != 0
            fails = sum(1 for l in r.stdout.splitlines() if "FAIL" in l)
            # A queued turn reading a picture already freed is a crash here,
            # as it would be memory read by DMA after its owner let it go.
            how = (f"{fails} failing case(s)" if r.returncode > 0 else
                   f"crashed, signal {-r.returncode}" if r.returncode < 0 else "every case passed")
            print(f"  {'ok   ' if caught else 'MISSED'} caught: {name} ({how})")
            failed += not caught
    print(f"{len(mutants) - failed} of {len(mutants)} mutants caught")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
