/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "main/main_tdb_authorization.h"

#include "main/main_account.h"
#include "main/main_session.h"
#include "storage/storage_account.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "core/core_cloud_password.h"
#include "core/version.h"
#include "lang/lang_instance.h"
#include "base/platform/base_platform_info.h"

#include <QtCore/QFileInfo>
#include <QtCore/QDir>

namespace Main {
namespace {

using namespace Tdb;

constexpr auto kPairedPref = "tdlib_authorization_paired";

[[nodiscard]] AccountConfig Config(not_null<Account*> account) {
	const auto key = account->local().peekLegacyLocalKey();
	Expects(key != nullptr);
	const auto bytes = key->data();
	const auto database = TdbAuthorization::DatabasePath(account);
	return {
		.apiId = ApiId,
		.apiHash = ApiHash,
		.systemLanguageCode = Lang::GetInstance().systemLangCode(),
		.deviceModel = Platform::DeviceModelPretty(),
		.systemVersion = Platform::SystemVersionPretty(),
		.applicationVersion = QString::fromLatin1(AppVersionStr),
		.databaseDirectory = database,
		.filesDirectory = QDir::cleanPath(database + u"/../files"_q),
		.langpackDirectory = database,
		.encryptionKey = QByteArray(
			reinterpret_cast<const char*>(bytes.data()), bytes.size()),
		.testDc = account->mtp().isTestMode(),
	};
}

} // namespace

TdbAuthorization::TdbAuthorization(not_null<Account*> account)
: _account(account)
, _tdb(Config(account))
, _sender(&_tdb.sender())
, _legacy(std::in_place, &account->mtp())
, _refreshTimer([=] { refreshToken(); })
, _paired(account->local().readPref<bool>(kPairedPref)) {
	_tdb.updates() | rpl::on_next([=](const TLupdate &update) {
		update.match([&](const TLDupdateAuthorizationState &data) {
			applyState(data.vauthorization_state());
		}, [](const auto &) {
		});
	}, _lifetime);
	Core::App().proxyChanges() | rpl::on_next([=] {
		applyProxy();
	}, _lifetime);
	_account->sessionChanges() | rpl::on_next([=](Session *session) {
		if (!session) {
			return;
		}
		_bridging = false;
		_refreshTimer.cancel();
		if (_legacy) {
			_legacy->request(base::take(_legacyRequest)).cancel();
		}
		_password.clear();
		if (_userId && session->userId().bare != _userId) {
			setPaired(false);
			fail(u"AUTH_USER_MISMATCH"_q);
			_tdb.logout();
		} else if (_userId) {
			setPaired(true);
		}
	}, _lifetime);
	applyProxy();
}

TdbAuthorization::~TdbAuthorization() = default;

QString TdbAuthorization::DatabasePath(not_null<Account*> account) {
	return QFileInfo(account->local().cachePath()).dir().absoluteFilePath(
		u"tdlib/db"_q);
}

Tdb::Account &TdbAuthorization::tdb() {
	return _tdb;
}

auto TdbAuthorization::stateValue() const
-> rpl::producer<std::optional<TLauthorizationState>> {
	return _state.value();
}

auto TdbAuthorization::state() const
-> const std::optional<TLauthorizationState> & {
	return _state.current();
}

auto TdbAuthorization::authorizationValue() const
-> rpl::producer<std::optional<MTPUser>> {
	return _authorization.value();
}

rpl::producer<QString> TdbAuthorization::errors() const {
	return _errors.events();
}

rpl::producer<bool> TdbAuthorization::needsLegacyPasswordValue() const {
	return _needsLegacyPassword.value();
}

void TdbAuthorization::applyProxy() {
	const auto &settings = Core::App().settings().proxy();
	const auto proxy = settings.selected();
	const auto enabled = settings.isEnabled() && proxy;
	if (enabled && proxy.type == MTP::ProxyData::Type::Web) {
		_sender.request(TLsetNetworkType(tl_networkTypeNone())).send();
		_errors.fire_copy(u"PROXY_TYPE_UNSUPPORTED"_q);
		return;
	}
	if (enabled) {
		const auto type = [&]() -> TLproxyType {
			if (proxy.type == MTP::ProxyData::Type::Socks5) {
				return tl_proxyTypeSocks5(
					tl_string(proxy.user), tl_string(proxy.password));
			} else if (proxy.type == MTP::ProxyData::Type::Http) {
				return tl_proxyTypeHttp(
					tl_string(proxy.user),
					tl_string(proxy.password),
					tl_bool(false));
			}
			return tl_proxyTypeMtproto(tl_string(proxy.password));
		}();
		_tdb.setProxy(TLaddProxy(
			tl_string(proxy.host), tl_int32(proxy.port), tl_bool(true), type));
	} else {
		_tdb.setProxy(TLdisableProxy());
	}
	_sender.request(TLsetNetworkType(tl_networkTypeOther())).send();
}

void TdbAuthorization::applyState(const TLauthorizationState &state) {
	_state = state;
	if (_paired && !_logoutDone
		&& (state.type() == id_authorizationStateLoggingOut
			|| state.type() == id_authorizationStateClosed
			|| state.type() == id_authorizationStateWaitPhoneNumber)) {
		setPaired(false);
		_authorization = std::nullopt;
		fail(u"SESSION_REVOKED"_q);
		const auto account = _account;
		const auto userId = _userId;
		crl::on_main(account, [=] {
			const auto session = account->maybeSession();
			if (!session || session->userId().bare == userId) {
				account->logOut();
			}
		});
	}
	if (state.type() == id_authorizationStateReady) {
		retry();
	} else if (state.type() == id_authorizationStateClosed) {
		_password.clear();
		if (auto done = base::take(_logoutDone)) {
			crl::on_main(_account, std::move(done));
		}
	}
}

void TdbAuthorization::retry() {
	if (!_legacy || _meRequest || _bridging || _authorization.current() || _logoutDone
		|| !state() || state()->type() != id_authorizationStateReady) {
		return;
	}
	_meRequest = _sender.request(TLgetMe()).done([=](const TLDuser &user) {
		_meRequest = 0;
		_userId = user.vid().v;
		if (const auto session = _account->maybeSession()) {
			if (session->userId().bare != _userId) {
				setPaired(false);
				fail(u"AUTH_USER_MISMATCH"_q);
				_tdb.logout();
			} else {
				setPaired(true);
			}
			_password.clear();
			return;
		}
		_acceptedToken.clear();
		_passwordRequest = false;
		_needsLegacyPassword = false;
		_bridging = true;
		restoreLegacySession();
	}).fail([=](const Error &error) {
		_meRequest = 0;
		fail(error.message);
	}).send();
}

void TdbAuthorization::setPaired(bool paired) {
	_paired = paired;
	_account->local().writePref<bool>(kPairedPref, paired);
}

void TdbAuthorization::restoreLegacySession() {
	_legacyRequest = _legacy->request(MTPusers_GetUsers(
		MTP_vector<MTPInputUser>(1, MTP_inputUserSelf())
	)).done([=](const MTPVector<MTPUser> &users) {
		_legacyRequest = 0;
		if (users.v.size() == 1 && users.v.front().type() == mtpc_user) {
			finishUser(users.v.front());
		} else {
			refreshToken();
		}
	}).fail([=](const MTP::Error &error) {
		_legacyRequest = 0;
		if (error.type() == u"SESSION_PASSWORD_NEEDED"_q) {
			legacyFailed(error);
		} else if (error.code() == 401) {
			refreshToken();
		} else {
			fail(error.type());
		}
	}).handleAllErrors().send();
}

void TdbAuthorization::refreshToken() {
	if (!_legacy || !_bridging || _legacyRequest || _passwordRequest) {
		return;
	}
	_legacyRequest = _legacy->request(MTPauth_ExportLoginToken(
		MTP_int(ApiId), MTP_string(ApiHash), MTP_vector<MTPlong>()
	)).done([=](const MTPauth_LoginToken &result) {
		_legacyRequest = 0;
		handleToken(result);
	}).fail([=](const MTP::Error &error) {
		legacyFailed(error);
	}).handleAllErrors().send();
}

void TdbAuthorization::handleToken(const MTPauth_LoginToken &result) {
	if (!_bridging || _logoutDone) {
		return;
	}
	result.match([&](const MTPDauth_loginToken &data) {
		const auto token = data.vtoken().v;
		if (_acceptedToken != token) {
			_acceptedToken = token;
			const auto encoded = token.toBase64(
				QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
			const auto link = u"tg://login?token="_q + QString::fromLatin1(encoded);
			_confirmRequest = _sender.request(TLconfirmQrCodeAuthentication(
				tl_string(link)
			)).done([=] {
				_confirmRequest = 0;
				refreshToken();
			}).fail([=](const Error &error) {
				_confirmRequest = 0;
				if (error.message.startsWith(u"AUTH_TOKEN_EXPIRED"_q)) {
					_acceptedToken.clear();
				} else {
					fail(error.message);
				}
			}).send();
		}
		_refreshTimer.callOnce(1000);
	}, [&](const MTPDauth_loginTokenMigrateTo &data) {
		const auto dc = data.vdc_id().v;
		_account->mtp().setMainDcId(dc);
		_legacyRequest = _legacy->request(MTPauth_ImportLoginToken(
			data.vtoken()
		)).done([=](const MTPauth_LoginToken &result) {
			_legacyRequest = 0;
			handleToken(result);
		}).fail([=](const MTP::Error &error) {
			legacyFailed(error);
		}).toDC(dc).handleAllErrors().send();
	}, [&](const MTPDauth_loginTokenSuccess &data) {
		finish(data.vauthorization());
	});
}

void TdbAuthorization::legacyFailed(const MTP::Error &error) {
	_legacyRequest = 0;
	if (error.type() == u"SESSION_PASSWORD_NEEDED"_q) {
		_passwordRequest = true;
		_refreshTimer.cancel();
		if (_password.isEmpty()) {
			_needsLegacyPassword = true;
		} else {
			checkLegacyPassword();
		}
	} else {
		fail(error.type());
	}
}

void TdbAuthorization::checkPassword(const QString &password) {
	_password = password;
	if (_passwordRequest) {
		checkLegacyPassword();
		return;
	}
	_sender.request(TLcheckAuthenticationPassword(
		tl_string(password)
	)).fail([=](const Error &error) {
		_password.clear();
		_errors.fire_copy(error.message);
	}).send();
}

void TdbAuthorization::checkLegacyPassword() {
	if (!_legacy || _legacyRequest) {
		return;
	}
	_legacyRequest = _legacy->request(
		MTPaccount_GetPassword()
	).done([=](const MTPaccount_Password &result) {
		_legacyRequest = 0;
		const auto request = Core::ParseCloudPasswordCheckRequest(result.data());
		if (!request) {
			fail(u"PASSWORD_ALGORITHM_INVALID"_q);
			return;
		}
		const auto password = _password.toUtf8();
		const auto hash = Core::ComputeCloudPasswordHash(
			request.algo, bytes::make_span(password));
		const auto check = Core::ComputeCloudPasswordCheck(request, hash);
		if (!check) {
			fail(u"PASSWORD_ALGORITHM_INVALID"_q);
			return;
		}
		_legacyRequest = _legacy->request(MTPauth_CheckPassword(
			check.result
		)).done([=](const MTPauth_Authorization &result) {
			_legacyRequest = 0;
			finish(result);
		}).fail([=](const MTP::Error &error) {
			_legacyRequest = 0;
			_password.clear();
			_needsLegacyPassword = true;
			_errors.fire_copy(error.type());
		}).handleAllErrors().send();
	}).fail([=](const MTP::Error &error) {
		_legacyRequest = 0;
		fail(error.type());
	}).handleAllErrors().send();
}

void TdbAuthorization::recoverPassword(
		const QString &code,
		const QString &password) {
	_password = password;
	_sender.request(TLrecoverAuthenticationPassword(
		tl_string(code), tl_string(password), tl_string()
	)).fail([=](const Error &error) {
		_password.clear();
		_errors.fire_copy(error.message);
	}).send();
}

void TdbAuthorization::finish(const MTPauth_Authorization &authorization) {
	if (authorization.type() != mtpc_auth_authorization) {
		fail(u"AUTH_USER_MISMATCH"_q);
		return;
	}
	finishUser(authorization.c_auth_authorization().vuser());
}

void TdbAuthorization::finishUser(const MTPUser &user) {
	if (user.type() != mtpc_user || !user.c_user().is_self()
		|| user.c_user().vid().v != _userId) {
		fail(u"AUTH_USER_MISMATCH"_q);
		return;
	}
	_bridging = false;
	setPaired(true);
	_passwordRequest = false;
	_password.clear();
	_refreshTimer.cancel();
	_sender.request(base::take(_confirmRequest)).cancel();
	_needsLegacyPassword = false;
	_authorization = user;
}

void TdbAuthorization::fail(const QString &error) {
	_bridging = false;
	_passwordRequest = false;
	_refreshTimer.cancel();
	_sender.request(base::take(_confirmRequest)).cancel();
	if (_legacy) {
		_legacy->request(base::take(_legacyRequest)).cancel();
	}
	_authorization = std::nullopt;
	_password.clear();
	_errors.fire_copy(error);
}

bool TdbAuthorization::closed() const {
	return state() && state()->type() == id_authorizationStateClosed;
}

void TdbAuthorization::bindLegacy(MTP::Instance *instance) {
	_bridging = false;
	_paired = false;
	_refreshTimer.cancel();
	_sender.request(base::take(_meRequest)).cancel();
	_sender.request(base::take(_confirmRequest)).cancel();
	_legacyRequest = 0;
	_legacy.reset();
	_password.clear();
	_userId = 0;
	_passwordRequest = false;
	_needsLegacyPassword = false;
	_authorization = std::nullopt;
	if (instance) {
		_legacy.emplace(instance);
		retry();
	}
}

void TdbAuthorization::logout(Fn<void()> done) {
	if (_logoutDone) {
		return;
	}
	_logoutDone = std::move(done);
	_bridging = false;
	setPaired(false);
	_authorization = std::nullopt;
	_needsLegacyPassword = false;
	_passwordRequest = false;
	_refreshTimer.cancel();
	_sender.request(base::take(_meRequest)).cancel();
	_sender.request(base::take(_confirmRequest)).cancel();
	if (_legacy) {
		_legacy->request(base::take(_legacyRequest)).cancel();
	}
	_password.clear();
	_userId = 0;
	if (state() && (state()->type() == id_authorizationStateLoggingOut
		|| state()->type() == id_authorizationStateClosing)) {
		return;
	} else if (state() && state()->type() == id_authorizationStateReady) {
		_tdb.logout();
	} else {
		_tdb.reset();
	}
}

} // namespace Main
