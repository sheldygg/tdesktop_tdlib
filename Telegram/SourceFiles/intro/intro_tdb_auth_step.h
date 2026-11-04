/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "intro/intro_step.h"
#include "tdb/tdb_sender.h"
#include "base/timer.h"

namespace Ui {
class InputField;
class PasswordInput;
class FlatLabel;
class LinkButton;
} // namespace Ui

namespace Main {
class TdbAuthorization;
} // namespace Main

namespace Intro::details {

class TdbAuthStep final : public Step {
public:
	TdbAuthStep(
		QWidget *parent,
		not_null<Main::Account*> account,
		not_null<Data*> data);
	~TdbAuthStep();
	void activate() override;
	void finished() override;
	void cancelled() override;
	void setInnerFocus() override;
	void submit() override;
	bool hasBack() const override;

protected:
	void resizeEvent(QResizeEvent *e) override;

private:
	enum class Mode {
		Connecting,
		Code,
		Email,
		EmailCode,
		Password,
		RecoveryCode,
		RecoveryPassword,
		Registration,
	};
	void applyState();
	void finishIfReady();
	void setMode(Mode mode);
	void updateGeometry();
	void fail(const QString &error);
	void resend();
	void recover();
	void registerUser();
	template <typename Request>
	void sendRequest(Request &&request) {
		_busy = true;
		_request = _sender.request(std::move(request)).done([=] {
			_request = 0;
			_busy = false;
		}).fail([=](const Tdb::Error &error) {
			_request = 0;
			fail(error.message);
		}).send();
	}

	const not_null<Main::TdbAuthorization*> _auth;
	Tdb::Sender _sender;
	object_ptr<Ui::InputField> _field;
	object_ptr<Ui::InputField> _lastName;
	object_ptr<Ui::PasswordInput> _password;
	object_ptr<Ui::PasswordInput> _passwordConfirm;
	object_ptr<Ui::FlatLabel> _hint;
	object_ptr<Ui::LinkButton> _resend;
	object_ptr<Ui::LinkButton> _recover;
	base::Timer _resendTimer;
	Tdb::RequestId _request = 0;
	Mode _mode = Mode::Connecting;
	bool _busy = false;
	bool _active = false;
	bool _legacyPassword = false;
	bool _termsAccepted = false;
	bool _passportConfirmed = false;
	QString _recoveryCode;
	std::optional<MTPUser> _pendingAuthorization;

};

} // namespace Intro::details
