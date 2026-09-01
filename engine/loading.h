/*
 * q1psx — boot / level loading screen with a progress bar.
 *
 * The boot sequence and (later) level loads do a lot of slow CD I/O. Without
 * feedback a long load looks like a hang. Wrap the work in:
 *
 *     load_begin(N);
 *     load_step("reading pak0.pak");   ... work ...
 *     load_step("loading progs.dat");  ... work ...
 *     load_done();
 *
 * Each step redraws the bar and flips, so the screen is always current.
 */

#pragma once

void load_begin(int total_steps);
void load_step(const char *label);   /* advance one step, then redraw */
void load_note(const char *label);   /* redraw with a new label, no advance */
void load_done(void);
