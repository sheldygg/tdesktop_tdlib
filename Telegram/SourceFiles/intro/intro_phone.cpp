/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "intro/intro_phone.h"

#include "lang/lang_keys.h"
#include "intro/intro_tdb_auth_step.h"
#include "intro/intro_widget.h"
#include "main/main_tdb_authorization.h"
#include "intro/intro_qr.h"
#include "styles/style_intro.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/fade_wrap.h"
#include "ui/widgets/fields/special_fields.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_app_config.h"
#include "main/main_session.h"
#include "data/data_user.h"
#include "ui/boxes/confirm_box.h"
#include "boxes/abstract_box.h"
#include "boxes/phone_banned_box.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"
#include "countries/countries_instance.h" // Countries::Groups

namespace Intro {
namespace details {
namespace {

[[nodiscard]] bool AllowPhoneAttempt(const QString &phone) {
	const auto digits = ranges::count_if(
		phone,
		[](QChar ch) { return ch.isNumber(); });
	return (digits > 1);
}

[[nodiscard]] QString DigitsOnly(QString value) {
	static const auto RegExp = QRegularExpression("[^0-9]");
	return value.replace(RegExp, QString());
}

} // namespace

PhoneWidget::PhoneWidget(
	QWidget *parent,
	not_null<Main::Account*> account,
	not_null<Data*> data)
: Step(parent, account, data)
, _country(
	this,
	getData()->controller->uiShow(),
	st::introCountry)
, _code(this, st::introCountryCode)
, _phone(
	this,
	st::introPhone,
	[](const QString &s) { return Countries::Groups(s); })
, _tdbApi(&account->tdbAuthorization().tdb().sender()) {
	_code->setAccessibleName(tr::lng_country_code(tr::now));
	_phone->setAccessibleName(tr::lng_phone_number(tr::now));
	_phone->frontBackspaceEvent(
	) | rpl::on_next([=](not_null<QKeyEvent*> e) {
		_code->startErasing(e);
	}, _code->lifetime());

	_country->codeChanged(
	) | rpl::on_next([=](const QString &code) {
		_code->codeSelected(code);
		_phone->chooseCode(code);
	}, _country->lifetime());
	_code->codeChanged(
	) | rpl::on_next([=](const QString &code) {
		_country->onChooseCode(code);
		_phone->chooseCode(code);
	}, _code->lifetime());
	_code->addedToNumber(
	) | rpl::on_next([=](const QString &added) {
		_phone->addedToNumber(added);
	}, _phone->lifetime());
	_code->spacePressed() | rpl::on_next([=] {
		submit();
	}, _code->lifetime());
	connect(_phone, &Ui::PhonePartInput::changed, [=] { phoneChanged(); });
	connect(_code, &Ui::CountryCodeInput::changed, [=] { phoneChanged(); });

	setTitleText(tr::lng_phone_title());
	setDescriptionText(tr::lng_phone_desc());
	getData()->updated.events(
	) | rpl::on_next([=] {
		countryChanged();
	}, lifetime());
	setErrorCentered(true);
	setupQrLogin();

	if (!_country->chooseCountry(getData()->country)) {
		_country->chooseCountry(u"US"_q);
	}
	_changed = false;
	account->tdbAuthorization().stateValue(
	) | rpl::on_next([=] {
		crl::on_main(this, [=] { advanceTdbState(); });
	}, lifetime());
}

QString PhoneWidget::accessibilityName() {
	return tr::lng_phone_title(tr::now);
}

void PhoneWidget::setupQrLogin() {
	const auto qrLogin = Ui::CreateChild<Ui::LinkButton>(
		this,
		tr::lng_phone_to_qr(tr::now));
	qrLogin->show();

	DEBUG_LOG(("PhoneWidget.qrLogin link created and shown."));

	rpl::combine(
		sizeValue(),
		qrLogin->widthValue()
	) | rpl::on_next([=](QSize size, int qrLoginWidth) {
		qrLogin->moveToLeft(
			(size.width() - qrLoginWidth) / 2,
			contentTop() + st::introQrLoginLinkTop);
	}, qrLogin->lifetime());

	qrLogin->setClickedCallback([=] {
		goNextOrBack<QrWidget>();
	});
}

void PhoneWidget::resizeEvent(QResizeEvent *e) {
	Step::resizeEvent(e);
	_country->moveToLeft(contentLeft(), contentTop() + st::introStepFieldTop);
	auto phoneTop = _country->y() + _country->height() + st::introPhoneTop;
	_code->moveToLeft(contentLeft(), phoneTop);
	_phone->moveToLeft(contentLeft() + _country->width() - st::introPhone.width, phoneTop);
}

void PhoneWidget::showPhoneError(rpl::producer<QString> text) {
	_phone->showError();
	showError(std::move(text));
}

void PhoneWidget::hidePhoneError() {
	hideError();
}

void PhoneWidget::countryChanged() {
	if (!_changed) {
		selectCountry(getData()->country);
	}
}

void PhoneWidget::phoneChanged() {
	_changed = true;
	hidePhoneError();
}

void PhoneWidget::submit() {
	if (_sentRequest || isHidden()) {
		return;
	}
	const auto &proxy = Core::App().settings().proxy();
	if (proxy.isEnabled() && proxy.selected().type == MTP::ProxyData::Type::Web) {
		showPhoneError(tr::lng_proxy_unsupported());
		return;
	}

	{
		const auto hasCodeButWaitingPhone = _code->hasFocus()
			&& (_code->getLastText().size() > 1)
			&& _phone->getLastText().isEmpty();
		if (hasCodeButWaitingPhone) {
			_phone->hideError();
			_phone->setFocus();
			return;
		}
	}
	const auto phone = fullNumber();
	if (!AllowPhoneAttempt(phone)) {
		showPhoneError(tr::lng_bad_phone());
		_phone->setFocus();
		return;
	}

	cancelNearestDcRequest();

	// Check if such account is authorized already.
	const auto phoneDigits = DigitsOnly(phone);
	for (const auto &[index, existing] : Core::App().domain().accounts()) {
		const auto raw = existing.get();
		if (const auto session = raw->maybeSession()) {
			if (raw->mtp().environment() == account().mtp().environment()
				&& DigitsOnly(session->user()->phone()) == phoneDigits) {
				crl::on_main(raw, [=] {
					Core::App().domain().activate(raw);
				});
				return;
			}
		}
	}

	hidePhoneError();

	_sentPhone = phone;
	getData()->phone = DigitsOnly(phone);
	_allowTdbForward = true;
	_sentRequest = _tdbApi.request(Tdb::TLsetAuthenticationPhoneNumber(
		Tdb::tl_string(phone), std::nullopt
	)).done([=] {
		_sentRequest = 0;
		advanceTdbState();
	}).fail([=](const Tdb::Error &error) {
		phoneSubmitFail(error);
	}).send();
}

void PhoneWidget::advanceTdbState() {
	if (!_active || !_allowTdbForward || _sentRequest) {
		return;
	}
	const auto &state = account().tdbAuthorization().state();
	if (!state
		|| state->type() == Tdb::id_authorizationStateWaitTdlibParameters
		|| state->type() == Tdb::id_authorizationStateWaitPhoneNumber
		|| state->type() == Tdb::id_authorizationStateClosed) {
		return;
	}
	_allowTdbForward = false;
	_tdbApi.request(base::take(_sentRequest)).cancel();
	goNext<TdbAuthStep>();
}

void PhoneWidget::phoneSubmitFail(const Tdb::Error &error) {
	_sentRequest = 0;
	_allowTdbForward = false;
	const auto &err = error.message;
	if (Tdb::IsFloodError(error) || err == u"PHONE_NUMBER_FLOOD"_q) {
		showPhoneError(tr::lng_flood_error());
	} else if (err == u"PHONE_NUMBER_INVALID"_q) {
		showPhoneError(tr::lng_bad_phone());
	} else if (err == u"PHONE_NUMBER_BANNED"_q) {
		Ui::ShowPhoneBannedError(getData()->controller, _sentPhone);
	} else {
		showPhoneError(rpl::single(Lang::Hard::ServerError()));
	}
}

QString PhoneWidget::fullNumber() const {
	return _code->getLastText() + _phone->getLastText();
}

void PhoneWidget::selectCountry(const QString &country) {
	_country->chooseCountry(country);
}

void PhoneWidget::setInnerFocus() {
	_phone->setFocusFast();
}

void PhoneWidget::activate() {
	_active = true;
	crl::on_main(this, [=] { advanceTdbState(); });
	Step::activate();
	showChildren();
	setInnerFocus();
}

void PhoneWidget::finished() {
	Step::finished();
	cancelled();
}

void PhoneWidget::cancelled() {
	_active = false;
	_allowTdbForward = false;
	_tdbApi.request(base::take(_sentRequest)).cancel();
}

} // namespace details
} // namespace Intro
