#include "loginview.hh"

LoginView::LoginView()
	: Gtk::Box(Gtk::Orientation::VERTICAL),
	  form_(Gtk::Orientation::VERTICAL, 10),
	  title_("GainDrive"),
	  login_("Log in")
	{
	form_.set_halign(Gtk::Align::CENTER);
	form_.set_valign(Gtk::Align::CENTER);
	form_.set_vexpand(true);
	form_.set_size_request(320, -1);

	title_.add_css_class("title-1");
	form_.append(title_);

	url_.set_placeholder_text("Server address");
	url_.set_input_purpose(Gtk::InputPurpose::URL);
	user_.set_placeholder_text("User name");
	password_.set_placeholder_text("Password");
	password_.set_visibility(false);
	password_.set_input_purpose(Gtk::InputPurpose::PASSWORD);
	form_.append(url_);
	form_.append(user_);
	form_.append(password_);

	login_.add_css_class("suggested-action");
	form_.append(login_);
	form_.append(spinner_);

	error_.set_wrap(true);
	error_.set_max_width_chars(40);
	error_.add_css_class("error");
	error_.set_visible(false);
	form_.append(error_);

	append(form_);

	login_.signal_clicked().connect(sigc::mem_fun(*this, &LoginView::submit));
	url_.signal_activate().connect([this]() { user_.grab_focus(); });
	user_.signal_activate().connect([this]() { password_.grab_focus(); });
	password_.signal_activate().connect(sigc::mem_fun(*this, &LoginView::submit));
	}

void LoginView::prefill(const std::string& url, const std::string& user)
	{
	url_.set_text(url);
	user_.set_text(user);
	password_.set_text("");
	if(url.empty())       url_.grab_focus();
	else if(user.empty()) user_.grab_focus();
	else                  password_.grab_focus();
	}

void LoginView::set_busy(bool busy)
	{
	login_.set_sensitive(!busy);
	spinner_.set_spinning(busy);
	if(busy) error_.set_visible(false);
	}

void LoginView::set_error(const std::string& text)
	{
	error_.set_text(text);
	error_.set_visible(!text.empty());
	}

void LoginView::submit()
	{
	if(!login_.get_sensitive()) return;
	if(url_.get_text().empty() || user_.get_text().empty()) {
		set_error("Please enter the server address and your user name.");
		return;
		}
	signal_login.emit(url_.get_text(), user_.get_text(), password_.get_text());
	}
