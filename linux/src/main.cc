#include "mainwindow.hh"

#include <gtkmm/application.h>

int main(int argc, char *argv[])
	{
	auto app = Gtk::Application::create(GD_APP_ID);
	return app->make_window_and_run<MainWindow>(argc, argv);
	}
