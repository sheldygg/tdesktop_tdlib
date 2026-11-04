/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "ui/countryinput.h"
#include "intro/intro_step.h"
#include "tdb/tdb_sender.h"

namespace Ui {
class PhonePartInput;
class CountryCodeInput;
class RoundButton;
class FlatLabel;
} // namespace Ui

namespace Intro {
namespace details {

class PhoneWidget final : public Step {
public:
	PhoneWidget(
		QWidget *parent,
		not_null<Main::Account*> account,
		not_null<Data*> data);

	QString accessibilityName() override;

	void selectCountry(const QString &country);

	void setInnerFocus() override;
	void activate() override;
	void finished() override;
	void cancelled() override;
	void submit() override;

	bool hasBack() const override {
		return true;
	}

protected:
	void resizeEvent(QResizeEvent *e) override;

private:
	void setupQrLogin();
	void phoneChanged();
	void advanceTdbState();
	void countryChanged();

	void phoneSubmitFail(const Tdb::Error &error);

	QString fullNumber() const;

	void showPhoneError(rpl::producer<QString> text);
	void hidePhoneError();

	bool _changed = false;

	object_ptr<CountryInput> _country;
	object_ptr<Ui::CountryCodeInput> _code;
	object_ptr<Ui::PhonePartInput> _phone;

	QString _sentPhone;
	Tdb::Sender _tdbApi;
	Tdb::RequestId _sentRequest = 0;
	bool _active = false;
	bool _allowTdbForward = true;

};

} // namespace details
} // namespace Intro
