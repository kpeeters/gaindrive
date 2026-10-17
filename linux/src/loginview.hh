#pragma once

#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/entry.h>
#include <gtkmm/label.h>
#include <gtkmm/spinner.h>

class LoginView : public Gtk::Box {
	public:
		LoginView();

		void prefill(const std::string& url, const std::string& user);
		void set_busy(bool busy);
		void set_error(const std::string& text);

		sigc::signal<void(const std::string& url, const std::string& user,
		                  const std::string& password)> signal_login;

	private:
		void submit();

		Gtk::Box           form_;
		Gtk::Label         title_;
		// A plain Entry: gtkmm 4.10's PasswordEntry cannot report Enter.
		Gtk::Entry         url_, user_, password_;
		Gtk::Button        login_;
		Gtk::Spinner       spinner_;
		Gtk::Label         error_;
};
