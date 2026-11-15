#ifndef RUNNER_WINDOW_CONTROLS_H_
#define RUNNER_WINDOW_CONTROLS_H_

#include <flutter_linux/flutter_linux.h>
#include <gtk/gtk.h>

// Lets Dart drag, minimise, maximise and close the window.
//
// The window is undecorated — see `my_application.cc` — so the title bar is
// drawn in Flutter, and everything the system bar used to do has to be asked
// for explicitly. This is the whole of that bridge; there is no plugin and no
// pub dependency behind it.
//
// Must be called after the view is realized and the engine is running.
void window_controls_register(FlView* view, GtkWindow* window);

#endif  // RUNNER_WINDOW_CONTROLS_H_
