/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "tdb/tdb_account.h"
#include "tdb/tdb_tl_scheme.h"
#include "mtproto/sender.h"
#include "base/timer.h"

namespace Main {

class Account;

class TdbAuthorization final {
public:
	explicit TdbAuthorization(not_null<Account*> account);
	~TdbAuthorization();
	[[nodiscard]] static QString DatabasePath(not_null<Account*> account);

	[[nodiscard]] Tdb::Account &tdb();
	[[nodiscard]] auto stateValue() const
		-> rpl::producer<std::optional<Tdb::TLauthorizationState>>;
	[[nodiscard]] const std::optional<Tdb::TLauthorizationState> &state() const;
	[[nodiscard]] auto authorizationValue() const
		-> rpl::producer<std::optional<MTPUser>>;
	[[nodiscard]] rpl::producer<QString> errors() const;
	[[nodiscard]] rpl::producer<bool> needsLegacyPasswordValue() const;
	void checkPassword(const QString &password);
	void recoverPassword(const QString &code, const QString &password);
	void retry();
	void bindLegacy(MTP::Instance *instance);
	void logout(Fn<void()> done);
	[[nodiscard]] bool closed() const;

private:
	void applyState(const Tdb::TLauthorizationState &state);
	void applyProxy();
	void refreshToken();
	void restoreLegacySession();
	void handleToken(const MTPauth_LoginToken &result);
	void finish(const MTPauth_Authorization &authorization);
	void finishUser(const MTPUser &user);
	void setPaired(bool paired);
	void fail(const QString &error);
	void checkLegacyPassword();
	void legacyFailed(const MTP::Error &error);

	const not_null<Account*> _account;
	Tdb::Account _tdb;
	Tdb::Sender _sender;
	std::optional<MTP::Sender> _legacy;
	base::Timer _refreshTimer;
	rpl::variable<std::optional<Tdb::TLauthorizationState>> _state;
	rpl::variable<std::optional<MTPUser>> _authorization;
	rpl::variable<bool> _needsLegacyPassword = false;
	rpl::event_stream<QString> _errors;
	Tdb::RequestId _meRequest = 0;
	Tdb::RequestId _confirmRequest = 0;
	mtpRequestId _legacyRequest = 0;
	int64 _userId = 0;
	QByteArray _acceptedToken;
	QString _password;
	Fn<void()> _logoutDone;
	bool _bridging = false;
	bool _passwordRequest = false;
	bool _paired = false;
	rpl::lifetime _lifetime;

};

} // namespace Main
