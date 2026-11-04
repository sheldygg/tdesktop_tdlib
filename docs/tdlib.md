# TDLib foundation

This checkout starts from tdesktop `3a2a9a3` (7.3.5). The `Tdb` adapter is
ported from telegramdesktop/tdx `e2a90df232bcb47efb24b0a342557c12ff9aafdc`.
Its RPL subscriptions use the current `rpl::on_next` API.

TDLib is the `Telegram/ThirdParty/td` submodule, pinned to
`51743dfd01dff6179e2d8f7095729caa4e2222e9` (1.8.49). This is also the revision
used for tde2e by the upstream tdesktop base. The application builds both
`tdclient` and `tde2e` from this source tree, sharing their `tdutils` dependency
instead of linking a second, separately prepared copy of tde2e.

## Build

Initialize the checkout's submodules before following the normal platform
build instructions:

```sh
git submodule update --init --recursive
```

TDLib needs gperf on `PATH`, or its path passed to CMake with
`-DGPERF_EXECUTABLE=/path/to/gperf`. Python 3 generates the adapter wrappers.
OpenSSL and zlib come from the same CMake targets as the rest of tdesktop.
TDLib is built in the application's build tree; a separate TDLib installation
is not required.

For an already configured Debug tree:

```sh
cmake --build out --config Debug --target td_tdb tdclient td_tde2e
cmake --build out --config Debug --target Telegram
```

`Telegram/cmake/lib_td.cmake` adds native TDLib targets. The previous
standalone tde2e dependency preparation is redundant for this checkout;
existing prepared libraries can remain in place.

## Adapter

- `Telegram/cmake/td_tdb.cmake` defines the `tdesktop::td_tdb` object library,
  linked into Telegram. Clients start when entering phone login, or at startup
  when an account already has a TDLib database.
- `Telegram/cmake/generate_tdb_tl.cmake` runs
  `tdb/details/tdb_tl_generate.py` through the existing `lib_tl` generator.
  It reads `ThirdParty/td/td/generate/scheme/td_api.tl` directly. Native TDLib
  and the adapter therefore always use the same schema.
- `Tdb::details::Instance` owns client lifecycle, request IDs, initialization,
  the receive thread and delivery to the Qt main thread.
- `Tdb::Sender` supplies `.request().done().fail().send()` and cancels callback
  delivery when the sender or request is destroyed.
- `Tdb::Account` exposes updates, options, proxy configuration and generated
  file handling. File helpers include byte uploads, URL downloads and a
  Windows file reader that permits TDLib to rename open files.

## Authorization

Phone login uses TDLib's `setAuthenticationPhoneNumber`,
`checkAuthenticationCode` and `checkAuthenticationPassword`. The authorization
step also handles login email, code resending, registration after accepting
the terms of service, and password recovery with a new password.

`Main::TdbAuthorization` belongs to `Main::Account`. It consumes
`updateAuthorizationState`, retrieves the current user with `getMe`, and
restores TDLib's authorization from its database on the next launch. The
phone/country form is retained; code and password states use the new
`Intro::details::TdbAuthStep`. Existing QR and passkey login remain on the
upstream path.

The database and files live in separate `tdlib/db` and `tdlib/files`
directories beside that account's media cache, under `tdata/user_.../`.
The database uses the account's existing local encryption key. The pairing flag uses encrypted account preferences; the sequential settings
format is unchanged. Plaintext credentials are not written.

The rest of the application still requires an authorized native MTProto
session. After TDLib reaches Ready, the authorization controller exports a
login token from the native client and confirms it through TDLib. It handles
DC migration and verifies that both clients report the same user ID before
passing the native authorization to the existing session creation path.
The exchange follows [Telegram QR login](https://core.telegram.org/api/qr-login).
This transition uses two Telegram server sessions. A restored native session
is reused instead of creating another one on every launch. When local user
metadata is missing, the controller first checks the native self user before
requesting a new login token.

When the native client also requires 2FA, the password entered for TDLib is
used to compute the native SRP proof, then cleared after success or failure.
It stays in memory only. Revocation of the paired TDLib authorization also
logs out the native client when the revocation is received. If only the TDLib session survived and no password
was entered in this run, the password step can be shown for the native
client. Logout waits for TDLib to close before resetting local account data.

SOCKS5, HTTP and MTProto proxy settings are forwarded to TDLib. The custom
Web proxy transport is not supported by this TDLib revision; TDLib networking
is disabled for that proxy type rather than connecting directly.

## Remaining migration

Messages, chats and feature requests still use the original `Main::Session`
and MTProto implementation. Existing accounts created through native login
are not automatically imported into TDLib. Their migration and replacement
of feature requests are separate steps.
