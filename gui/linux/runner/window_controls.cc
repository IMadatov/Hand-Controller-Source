#include "window_controls.h"

static constexpr char kChannelName[] = "dev.handcontroller/window";

// Starts a window move from the pointer's current position.
//
// GTK owns the drag once this is called: it talks to the compositor and the
// window follows the pointer until the button is released. Tracking the
// pointer ourselves and calling gtk_window_move would fight the compositor
// and does not work on Wayland at all.
static void begin_drag(GtkWindow* window) {
  GdkDisplay* display = gtk_widget_get_display(GTK_WIDGET(window));
  GdkSeat* seat = gdk_display_get_default_seat(display);
  GdkDevice* pointer = gdk_seat_get_pointer(seat);
  if (pointer == nullptr) return;

  gint x = 0, y = 0;
  gdk_device_get_position(pointer, nullptr, &x, &y);

  // Button 1 and time 0: the press has already happened on the Flutter side,
  // so this continues the gesture rather than starting a new one.
  gtk_window_begin_move_drag(window, 1, x, y, GDK_CURRENT_TIME);
}

static void method_call_cb(FlMethodChannel* channel,
                           FlMethodCall* method_call,
                           gpointer user_data) {
  GtkWindow* window = GTK_WINDOW(user_data);
  const gchar* method = fl_method_call_get_name(method_call);

  g_autoptr(FlMethodResponse) response = nullptr;

  if (g_strcmp0(method, "startDrag") == 0) {
    begin_drag(window);
    response = FL_METHOD_RESPONSE(fl_method_success_response_new(nullptr));
  } else if (g_strcmp0(method, "minimize") == 0) {
    gtk_window_iconify(window);
    response = FL_METHOD_RESPONSE(fl_method_success_response_new(nullptr));
  } else if (g_strcmp0(method, "toggleMaximize") == 0) {
    if (gtk_window_is_maximized(window)) {
      gtk_window_unmaximize(window);
    } else {
      gtk_window_maximize(window);
    }
    // Reports the state AFTER the toggle, so the button can draw the right
    // icon without asking again.
    g_autoptr(FlValue) maximized =
        fl_value_new_bool(gtk_window_is_maximized(window));
    response = FL_METHOD_RESPONSE(fl_method_success_response_new(maximized));
  } else if (g_strcmp0(method, "isMaximized") == 0) {
    g_autoptr(FlValue) maximized =
        fl_value_new_bool(gtk_window_is_maximized(window));
    response = FL_METHOD_RESPONSE(fl_method_success_response_new(maximized));
  } else if (g_strcmp0(method, "close") == 0) {
    gtk_window_close(window);
    response = FL_METHOD_RESPONSE(fl_method_success_response_new(nullptr));
  } else {
    response = FL_METHOD_RESPONSE(fl_method_not_implemented_response_new());
  }

  g_autoptr(GError) error = nullptr;
  if (!fl_method_call_respond(method_call, response, &error)) {
    g_warning("Failed to respond to %s: %s", method, error->message);
  }
}

void window_controls_register(FlView* view, GtkWindow* window) {
  FlEngine* engine = fl_view_get_engine(view);
  FlBinaryMessenger* messenger = fl_engine_get_binary_messenger(engine);

  g_autoptr(FlStandardMethodCodec) codec = fl_standard_method_codec_new();
  FlMethodChannel* channel = fl_method_channel_new(
      messenger, kChannelName, FL_METHOD_CODEC(codec));

  // The channel outlives this call and is owned by the engine; the window
  // outlives both. No ref is taken, deliberately — a ref here would keep the
  // window alive past its own close.
  fl_method_channel_set_method_call_handler(channel, method_call_cb, window,
                                            nullptr);
}
