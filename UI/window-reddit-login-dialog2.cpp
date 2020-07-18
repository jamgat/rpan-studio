#include <qt-wrappers.hpp>

#include "window-reddit-login-dialog2.hpp"

#include <QDesktopServices>
#include <json11.hpp>

#include "api-reddit.hpp"
#include "window-basic-main.hpp"

using namespace std;
using namespace json11;

#define PAGE_SIGNIN 0
#define PAGE_OTP 1
#define PAGE_SPINNER 2

RedditLoginDialog2::RedditLoginDialog2(QWidget *parent)
	: QDialog(parent),
	  ui(new Ui::RedditLoginDialog2)
{
	ui->setupUi(this);

	setWindowFlag(Qt::WindowContextHelpButtonHint, false);

	ui->forgotPasswordLink->setText(
		"<a href='https://www.reddit.com/password'>" + QTStr(
			"Reddit.Login.Links.ForgotPassword") + "</a>");
	ui->forgotUsernameLink->setText(
		"<a href='https://www.reddit.com/username'>" + QTStr(
			"Reddit.Login.Links.ForgotUsername") + "</a>");
	ui->signupLink->setText(
		"<a href='https://www.reddit.com/register/'>" + QTStr(
			"Reddit.Login.Links.Signup") + "</a>");

	ui->errorLabel->setText("");
	ui->otpErrorLabel->setText("");
	ui->errorLabel->setProperty("themeID", "error");
	ui->otpErrorLabel->setProperty("themeID", "error");
	ui->stack->setCurrentIndex(PAGE_SIGNIN);

	connect(ui->signinButton, SIGNAL(clicked()), this, SLOT(SignIn()));
}

void RedditLoginDialog2::SetPage(int page)
{
	ui->stack->setCurrentIndex(page);

	switch (page) {
	case PAGE_SIGNIN:
		ui->subtitle->setText(Str("Reddit.Login.SignIn"));
		ui->signinButton->setEnabled(true);
		ui->signinButton->setText(Str("Reddit.Login.SignIn"));
		break;
	case PAGE_OTP:
		ui->subtitle->setText(Str("Reddit.Login.2FA.Subtitle"));
		ui->signinButton->setEnabled(true);
		ui->signinButton->setText(Str("Reddit.Login.2FA.Button.Check"));
		break;
	case PAGE_SPINNER:
		ui->signinButton->setEnabled(false);
		break;
	}
}

void RedditLoginDialog2::SignIn()
{
	const string newUsername = ui->usernameEdit->text().toStdString();
	const string password = ui->passwordEdit->text().toStdString();

	if (newUsername.empty()) {
		ui->errorLabel->setText(Str("Reddit.Login.Error.Username"));
		return;
	}
	if (password.empty()) {
		ui->errorLabel->setText(Str("Reddit.Login.Error.Password"));
		return;
	}
	string otpCode;
	if (needsOtp) {
		otpCode = ui->otpEdit->text().trimmed().toStdString();
		if (otpCode.empty() || otpCode.length() < 6) {
			ui->otpErrorLabel->setText(
				Str("Reddit.Login.2FA.Error.Required"));
			return;
		}
	}

	switch (ui->stack->currentIndex()) {
	case PAGE_SIGNIN:
		ui->subtitle->setText(QTStr("Reddit.Login.Subtitle.SigningIn"));
		break;
	case PAGE_OTP:
		ui->subtitle->setText(
			Str("Reddit.Login.Subtitle.CheckingCode"));
		break;
	}
	ui->stack->setCurrentIndex(PAGE_SPINNER);
	ui->errorLabel->setText("");
	ui->otpErrorLabel->setText("");

	username = make_shared<string>(newUsername);

	// Step 1: Login
	auto *thread = RedditApi::AuthLogin(newUsername, password, otpCode);
	connect(thread, &RemoteTextThread::Result, this,
	        &RedditLoginDialog2::LoginResult);

	loginThread.reset(thread);
	loginThread->start();
}

void RedditLoginDialog2::LoginResult(const QString &text,
                                     const QString &errorText,
                                     const QStringList &responseHeaders)
{
	string error;
	const Json loginJson = Json::parse(text.toStdString(), error);
	const Json loginJsonData = loginJson["json"]["data"];
	if (!loginJsonData.is_object()) {
		const Json errors = loginJson["json"]["errors"];
		if (errors.is_array() && errors.array_items().size() > 0) {
			for (const Json item : errors.array_items()) {
				string type = item.array_items()[0]
					.string_value();
				string msg = item.array_items()[1]
					.string_value();

				blog(LOG_INFO, "Reddit: Login failure: %s", text.toStdString().c_str());

				string err = Str(
					             "Reddit.Login.Error.Generic.Message")
				             + msg;

				if (needsOtp) {
					ui->otpErrorLabel->setText(
						QString::fromStdString(err)
						);
					SetPage(PAGE_OTP);
				} else {
					ui->errorLabel->setText(
						QString::fromStdString(err)
						);
					SetPage(PAGE_SIGNIN);
				}
				return;
			}
		}
	}
	const Json details = loginJsonData["details"];
	if (details.is_string() &&
	    details.string_value() == "TWO_FA_REQUIRED") {
		needsOtp = true;
		blog(LOG_INFO, "Reddit: Account has 2FA enabled");
		SetPage(PAGE_OTP);
		return;
	}

	const string modhash = loginJsonData["modhash"].string_value();

	QString newCookie;
	for (const QString &header : responseHeaders) {
		if (header.left(12) == "set-cookie: ") {
			auto val = header.mid(12);
			if (!newCookie.isEmpty()) {
				newCookie += ";";
			}
			newCookie += val.left(val.indexOf(';'));
		}
	}

	cookie = make_shared<string>(newCookie.toStdString());

	// Step 2: Authorize
	auto *thread = RedditApi::AuthAuthorize(modhash,
	                                        newCookie.toStdString());
	connect(thread, &RemoteTextThread::Result, this,
	        &RedditLoginDialog2::AuthorizeResult);

	authThread.reset(thread);
	authThread->start();
}

void RedditLoginDialog2::AuthorizeResult(const QString &text,
                                         const QString &,
                                         const QStringList &responseHeaders)
{
	string error;
	Json authJson = Json::parse(text.toStdString(), error);

	QString newCode;
	for (const QString &header : responseHeaders) {
		if (header.left(10) == "location: ") {
			QString val = header.mid(10);
			int codeIdx = val.indexOf("code=");
			if (codeIdx < 0) {
				SetPage(PAGE_SIGNIN);
				return;
			}
			int codeEndIdx = val.indexOf('&', codeIdx);
			if (codeEndIdx < 0) {
				newCode = val.mid(codeIdx + 5);
			} else {
				newCode = val.mid(codeIdx + 5,
				                  codeIdx + 5 - codeEndIdx);
			}
			break;
		}
	}

	code = make_shared<string>(newCode.toStdString());

	accept();
}
