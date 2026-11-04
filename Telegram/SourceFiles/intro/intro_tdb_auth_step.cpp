/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "intro/intro_tdb_auth_step.h"

#include "intro/intro_phone.h"
#include "intro/intro_widget.h"
#include "main/main_account.h"
#include "main/main_tdb_authorization.h"
#include "lang/lang_keys.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/fields/password_input.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/boxes/confirm_box.h"
#include "ui/text/format_values.h"
#include "boxes/abstract_box.h"
#include "styles/style_intro.h"

namespace Intro::details {
namespace {

using namespace Tdb;

} // namespace

TdbAuthStep::TdbAuthStep(
	QWidget *parent,
	not_null<Main::Account*> account,
	not_null<Data*> data)
: Step(parent, account, data)
, _auth(&account->tdbAuthorization())
, _sender(&_auth->tdb().sender())
, _field(this, st::introName, tr::lng_code_ph())
, _lastName(this, st::introName, tr::lng_signup_lastname())
, _password(this, st::introPassword, tr::lng_signin_password())
, _passwordConfirm(this, st::introPassword, tr::lng_cloud_password_confirm_new())
, _hint(this, st::introPasswordHint)
, _resend(this, tr::lng_cloud_password_resend(tr::now))
, _recover(this, tr::lng_signin_recover(tr::now))
, _resendTimer([=] { _resend->setEnabled(true); }) {
	_field->submits() | rpl::on_next([=] { submit(); }, lifetime());
	_lastName->submits() | rpl::on_next([=] { submit(); }, lifetime());
	_field->changes() | rpl::on_next([=] { hideError(); }, lifetime());
	connect(_password, &Ui::PasswordInput::submitted, this, [=] { submit(); });
	connect(_password, &Ui::PasswordInput::changed, this, [=] { hideError(); });
	connect(_passwordConfirm, &Ui::PasswordInput::submitted, this, [=] { submit(); });
	connect(_passwordConfirm, &Ui::PasswordInput::changed, this, [=] { hideError(); });
	_resend->setClickedCallback([=] { resend(); });
	_recover->setClickedCallback([=] { recover(); });
	_auth->stateValue() | rpl::on_next([=] {
		crl::on_main(this, [=] { applyState(); });
	}, lifetime());
	_auth->errors() | rpl::on_next([=](const QString &error) {
		fail(error);
	}, lifetime());
	_auth->needsLegacyPasswordValue() | rpl::on_next([=](bool needed) {
		_legacyPassword = needed;
		crl::on_main(this, [=] { applyState(); });
	}, lifetime());
	_auth->authorizationValue() | rpl::on_next([=](const auto &authorization) {
		_pendingAuthorization = authorization;
		crl::on_main(this, [=] { finishIfReady(); });
	}, lifetime());
	setMode(Mode::Connecting);
}

TdbAuthStep::~TdbAuthStep() = default;

bool TdbAuthStep::hasBack() const {
	return true;
}

void TdbAuthStep::activate() {
	_active = true;
	Step::activate();
	applyState();
	setInnerFocus();
	crl::on_main(this, [=] { finishIfReady(); });
}

void TdbAuthStep::finished() {
	_active = false;
	Step::finished();
}

void TdbAuthStep::finishIfReady() {
	if (_active && _auth->state()
		&& _auth->state()->type() == id_authorizationStateReady) {
		if (const auto authorization = base::take(_pendingAuthorization)) {
			finish(*authorization);
		}
	}
}

void TdbAuthStep::cancelled() {
	_active = false;
	apiClear();
	_sender.request(base::take(_request)).cancel();
	_busy = false;
	_password->clear();
	_passwordConfirm->clear();
}

void TdbAuthStep::setInnerFocus() {
	if (!_password->isHidden()) {
		_password->setFocusFast();
	} else if (!_field->isHidden()) {
		_field->setFocusFast();
	}
}

void TdbAuthStep::resizeEvent(QResizeEvent *e) {
	Step::resizeEvent(e);
	updateGeometry();
}

void TdbAuthStep::updateGeometry() {
	const auto top = contentTop() + st::introStepFieldTop;
	_field->moveToLeft(contentLeft(), top);
	_password->moveToLeft(contentLeft(), top);
	_passwordConfirm->moveToLeft(contentLeft(),
		top + _password->height() + st::introPhoneTop);
	_lastName->moveToLeft(contentLeft(), top + _field->height() + st::introPhoneTop);
	_hint->moveToLeft(contentLeft(), top + _password->height() + st::introPhoneTop);
	const auto linkTop = top + _field->height() + st::introLinkTop;
	_resend->moveToLeft(contentLeft(), linkTop);
	_recover->moveToLeft(contentLeft(), linkTop);
}

void TdbAuthStep::setMode(Mode mode) {
	const auto changed = _mode != mode;
	_mode = mode;
	_field->setVisible(mode == Mode::Code || mode == Mode::Email
		|| mode == Mode::EmailCode || mode == Mode::RecoveryCode
		|| mode == Mode::Registration);
	_lastName->setVisible(mode == Mode::Registration);
	_password->setVisible(mode == Mode::Password || mode == Mode::RecoveryPassword);
	_passwordConfirm->setVisible(mode == Mode::RecoveryPassword);
	_password->setPlaceholder(mode == Mode::RecoveryPassword
		? tr::lng_cloud_password_enter_new()
		: tr::lng_signin_password());
	_hint->hide();
	_resend->setVisible(mode == Mode::Code || mode == Mode::EmailCode);
	_recover->hide();
	if (changed) {
		_field->setText(QString());
		_lastName->setText(QString());
		_password->clear();
		_passwordConfirm->clear();
		_passportConfirmed = false;
		hideError();
		_busy = false;
	}
	_field->setInputMethodHints(mode == Mode::Email
		? Qt::ImhEmailCharactersOnly | Qt::ImhNoAutoUppercase | Qt::ImhNoPredictiveText
		: mode == Mode::Registration
		? Qt::ImhNone
		: Qt::ImhDigitsOnly | Qt::ImhSensitiveData | Qt::ImhNoPredictiveText);
	_field->setPlaceholder(mode == Mode::Registration
		? tr::lng_signup_firstname()
		: mode == Mode::Email
		? tr::lng_settings_cloud_login_email_placeholder()
		: tr::lng_code_ph());
	updateGeometry();
	if (changed) {
		setInnerFocus();
	}
}

void TdbAuthStep::applyState() {
	if (!_active) {
		return;
	}
	const auto &state = _auth->state();
	if (!state) {
		return;
	}
	if (_legacyPassword) {
		setMode(Mode::Password);
		setTitleText(tr::lng_signin_title());
		setDescriptionText(tr::lng_signin_desc());
		return;
	}
	state->match([&](const TLDauthorizationStateWaitPhoneNumber &) {
		goReplace<PhoneWidget>(Animate::Back);
	}, [&](const TLDauthorizationStateWaitCode &data) {
		const auto &info = data.vcode_info().data();
		setMode(Mode::Code);
		setTitleText(rpl::single(Ui::FormatPhone(info.vphone_number().v)));
		setDescriptionText(info.vtype().type() == id_authenticationCodeTypeTelegramMessage
			? tr::lng_code_from_telegram(tr::rich)
			: tr::lng_code_desc(tr::marked));
		_resend->setVisible(info.vnext_type().has_value());
		_resend->setEnabled(info.vtimeout().v <= 0);
		if (info.vtimeout().v > 0) {
			_resendTimer.callOnce(info.vtimeout().v * crl::time(1000));
		}
	}, [&](const TLDauthorizationStateWaitEmailAddress &) {
		setMode(Mode::Email);
		setTitleText(tr::lng_intro_email_setup_title());
		setDescriptionText(tr::lng_settings_cloud_login_email_about());
	}, [&](const TLDauthorizationStateWaitEmailCode &data) {
		setMode(Mode::EmailCode);
		setTitleText(tr::lng_signin_code());
		setDescriptionText(tr::lng_intro_email_confirm_subtitle(
			lt_email, rpl::single(data.vcode_info().data().vemail_address_pattern().v)));
	}, [&](const TLDauthorizationStateWaitPassword &data) {
		if (_mode == Mode::RecoveryCode) {
			setDescriptionText(tr::lng_signin_recover_desc(lt_email,
				rpl::single(data.vrecovery_email_address_pattern().v)));
		}
		if (_mode != Mode::RecoveryCode && _mode != Mode::RecoveryPassword) {
			setMode(Mode::Password);
			setTitleText(tr::lng_signin_title());
			setDescriptionText(tr::lng_signin_desc());
			_recover->setVisible(data.vhas_recovery_email_address().v);
			if (!data.vpassword_hint().v.isEmpty()) {
				_hint->setText(tr::lng_signin_hint(tr::now,
					lt_password_hint, data.vpassword_hint().v));
				_hint->show();
				_recover->moveToLeft(contentLeft(),
					_hint->y() + _hint->height() + st::introPhoneTop);
			}
		}
	}, [&](const TLDauthorizationStateWaitRegistration &) {
		setMode(Mode::Registration);
		setTitleText(tr::lng_signup_title());
		setDescriptionText(tr::lng_bad_name());
	}, [&](const TLDauthorizationStateReady &) {
		setMode(Mode::Connecting);
		setTitleText(tr::lng_connecting());
		setDescriptionText(rpl::single(QString()));
	}, [&](const TLDauthorizationStateWaitPremiumPurchase &) {
		setMode(Mode::Connecting);
		setTitleText(tr::lng_phone_title());
		showError(rpl::single(Lang::Hard::ServerError()));
	}, [&](const auto &) {
		setMode(Mode::Connecting);
		setTitleText(tr::lng_connecting());
	});
}

void TdbAuthStep::fail(const QString &error) {
	_busy = false;
	if (!_auth->state()
		|| _auth->state()->type() != id_authorizationStateReady) {
		apiClear();
		_pendingAuthorization = std::nullopt;
	}
	LOG(("TDLib login failed: %1").arg(error));
	const auto text = error == u"PASSWORD_HASH_INVALID"_q
		? tr::lng_signin_bad_password()
		: (error == u"PHONE_CODE_INVALID"_q || error == u"EMAIL_CODE_INVALID"_q)
		? tr::lng_bad_code()
		: (error.startsWith(u"Too Many Requests"_q)
			|| error.startsWith(u"FLOOD"_q))
		? tr::lng_flood_error()
		: error == u"PROXY_TYPE_UNSUPPORTED"_q
		? tr::lng_proxy_unsupported()
		: rpl::single(Lang::Hard::ServerError());
	showError(text);
	setInnerFocus();
}

void TdbAuthStep::submit() {
	if (_busy || isHidden()) {
		return;
	}
	hideError();
	const auto text = _field->getLastText().trimmed();
	switch (_mode) {
	case Mode::Code:
		sendRequest(TLcheckAuthenticationCode(tl_string(text)));
		break;
	case Mode::Email:
		sendRequest(TLsetAuthenticationEmailAddress(tl_string(text)));
		break;
	case Mode::EmailCode:
		sendRequest(TLcheckAuthenticationEmailCode(
			tl_emailAddressAuthenticationCode(tl_string(text))));
		break;
	case Mode::Password:
		_busy = true;
		_auth->checkPassword(_password->getLastText());
		break;
	case Mode::RecoveryCode:
		_busy = true;
		_request = _sender.request(TLcheckAuthenticationPasswordRecoveryCode(
			tl_string(text)
		)).done([=] {
			_request = 0;
			if (!_auth->state()
				|| _auth->state()->type() != id_authorizationStateWaitPassword) {
				_busy = false;
				return;
			}
			_recoveryCode = text;
			setMode(Mode::RecoveryPassword);
			setTitleText(tr::lng_signin_recover_title());
			setDescriptionText(tr::lng_cloud_password_about());
		}).fail([=](const Error &error) { fail(error.message); }).send();
		break;
	case Mode::RecoveryPassword:
		if (_password->getLastText().isEmpty()) {
			_password->showError();
			return;
		}
		if (_password->getLastText() != _passwordConfirm->getLastText()) {
			_passwordConfirm->showError();
			showError(tr::lng_cloud_password_differ());
			return;
		}
		if (!_passportConfirmed && _auth->state()
			&& _auth->state()->type() == id_authorizationStateWaitPassword
			&& _auth->state()->c_authorizationStateWaitPassword().vhas_passport_data().v) {
			Ui::show(Ui::MakeConfirmBox({
				.text = tr::lng_cloud_password_passport_losing(),
				.confirmed = crl::guard(this, [=](Fn<void()> close) {
					close();
					_passportConfirmed = true;
					submit();
				}),
				.confirmText = tr::lng_signin_reset(),
			}));
			return;
		}
		_busy = true;
		_auth->recoverPassword(_recoveryCode, _password->getLastText());
		break;
	case Mode::Registration:
		registerUser();
		break;
	case Mode::Connecting:
		_auth->retry();
		break;
	}
}

void TdbAuthStep::resend() {
	if (!_busy && _resend->isEnabled()) {
		sendRequest(TLresendAuthenticationCode(std::nullopt));
	}
}

void TdbAuthStep::recover() {
	if (_busy) {
		return;
	}
	_busy = true;
	_request = _sender.request(TLrequestAuthenticationPasswordRecovery()).done([=] {
		_request = 0;
		if (!_auth->state()
			|| _auth->state()->type() != id_authorizationStateWaitPassword) {
			_busy = false;
			return;
		}
		setMode(Mode::RecoveryCode);
		setTitleText(tr::lng_signin_code());
		const auto &data = _auth->state()->c_authorizationStateWaitPassword();
		setDescriptionText(tr::lng_signin_recover_desc(lt_email,
			rpl::single(data.vrecovery_email_address_pattern().v)));
	}).fail([=](const Error &error) { fail(error.message); }).send();
}

void TdbAuthStep::registerUser() {
	if (!_auth->state()
		|| _auth->state()->type() != id_authorizationStateWaitRegistration) {
		return;
	}
	if (_field->getLastText().trimmed().isEmpty()) {
		_field->showError();
		return;
	}
	if (!_termsAccepted) {
		const auto &terms = _auth->state()->c_authorizationStateWaitRegistration(
			).vterms_of_service().data();
		auto text = terms.vtext().data().vtext().v;
		if (terms.vmin_user_age().v > 0) {
			text += u"\n\n"_q + tr::lng_terms_age(
				tr::now, lt_count, terms.vmin_user_age().v);
		}
		Ui::show(Ui::MakeConfirmBox({
			.text = text,
			.confirmed = crl::guard(this, [=](Fn<void()> close) {
				close();
				_termsAccepted = true;
				registerUser();
			}),
			.confirmText = tr::lng_terms_agree(),
		}));
		return;
	}
	sendRequest(TLregisterUser(tl_string(_field->getLastText().trimmed()),
		tl_string(_lastName->getLastText().trimmed()), tl_bool(false)));
}

} // namespace Intro::details
