#include "mainwindow.hh"

#include <gtkmm/application.h>
#include "stamp.hh"

#include <gst/gst.h>

#include <filesystem>
#include <iostream>

namespace {
	// Our own gtk4paintablesink: beside the installed binary, or in the
	// build tree. Found from the executable, so it works whatever the prefix.
	void scan_bundled_plugins()
		{
		std::error_code ec;
		auto exe = std::filesystem::read_symlink("/proc/self/exe", ec);
		if(ec) {
			std::cout << stamp() << "Cannot locate the executable: " << ec.message() << std::endl;
			return;
			}
		auto dir = exe.parent_path();
		for(const auto& p: {dir/".."/GD_PLUGIN_SUBDIR, dir/"gst-plugins"})
			if(std::filesystem::is_directory(p, ec))
				gst_registry_scan_path(gst_registry_get(), p.c_str());
		}
}

int main(int argc, char *argv[])
	{
	gst_init(&argc, &argv);
	scan_bundled_plugins();
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
