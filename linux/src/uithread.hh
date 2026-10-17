#pragma once

#include <functional>

// Running code on the GTK main loop from any thread. GTK may only be touched
// from the main thread, and every network reply arrives on a worker.

// Call once, from the main thread, before anything posts.
void ui_init();

// Queue `fn` to run on the main loop. A no-op after ui_close().
void on_ui(std::function<void()> fn);

// Drops everything queued and refuses further posts. Called while the window
// is being torn down, so a late reply cannot reach a destroyed widget.
void ui_close();
