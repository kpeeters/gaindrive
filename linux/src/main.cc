#include "mainwindow.hh"

#include <gtkmm/application.h>
#include <gst/gst.h>

int main(int argc, char *argv[])
	{
	gst_init(&argc, &argv);
	auto app = Gtk::Application::create(GD_APP_ID);
	app->signal_startup().connect([app]() {
		app->set_accel_for_action("win.search", "<Control>f");
		// Closing rather than app->quit(), so the window's close handler
		// still reports the stop and saves its size.
		app->add_action("quit", [app]() {
			for(auto w: app->get_windows())
				w->close();
			});
		app->set_accel_for_action("app.quit", "<Control>q");
		});
	return app->make_window_and_run<MainWindow>(argc, argv);
	}
