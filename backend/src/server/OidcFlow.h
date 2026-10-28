/*
 * MoonlightWeb — browser-based Sunshine/GameStream client.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <functional>
#include <optional>

/**
 * OpenID Connect login — the decisions, with no network and no server state.
 *
 * What the settings mean and when they are usable, which URLs may be talked to,
 * the single-use login attempts that bind a callback to the browser that
 * started it, and what an ID token's groups grant. The flow itself lives in
 * OidcLogin, the HTTP exchanges in OidcClient, and the token check in
 * JwtVerifier. See docs/design/oidc-login.md.
 */
namespace Oidc {

/// The oidc_* keys of settings.json, plus public_url.
struct Settings
{
    bool enabled = false;
    QString issuer;
    QString clientId;
    QString clientSecret; ///< as stored; see effectiveClientSecret() for the env
    QString scopes = QStringLiteral("openid profile email");
    QString groupsClaim = QStringLiteral("groups");
    QStringList allowedGroups; ///< empty: anyone the provider authenticates
    QStringList adminGroups;
    QString buttonLabel;
    bool disablePin = false;
    int sessionTtlHours = 24;
    /// The origins the login is offered on, e.g. https://mw.example.com — one per
    /// name the instance is reached under (a proxy's, a LAN address...). Each one's
    /// <origin>/auth/oidc/callback is registered with the provider.
    QStringList publicUrls;

    static Settings fromJson(const QJsonObject& settings);
    /// Write every key into @p settings, leaving the others untouched.
    void writeTo(QJsonObject& settings) const;
};

inline constexpr int MIN_TTL_HOURS = 1;
inline constexpr int MAX_TTL_HOURS = 720; // 30 days
inline constexpr int MAX_GROUPS = 64;
inline constexpr int MAX_LABEL = 40;

/// Where the client secret in force comes from.
enum class SecretSource
{
    None,     ///< public client: PKCE alone
    Settings, ///< oidc_client_secret in settings.json
    Env,      ///< MW_OIDC_CLIENT_SECRET
    EnvFile,  ///< the file named by MW_OIDC_CLIENT_SECRET_FILE (Docker secrets)
};

/// Reads an environment variable; injectable so tests do not touch the process.
using EnvReader = std::function<QString(const char*)>;
EnvReader processEnv();

/// The secret in force: the environment wins over settings.json, so a secret
/// can be kept out of the file entirely. @p source, when given, says which.
QString effectiveClientSecret(const Settings& s, const EnvReader& env,
                              SecretSource* source = nullptr);

/// An identity provider URL we may talk to: https, or plain http to a loopback
/// host only (a provider on this very machine, where there is no wire to read).
bool isAllowedProviderUrl(const QUrl& url);

/// A usable public_url: an https origin and nothing else (no path, query,
/// fragment or credentials). The session cookie is Secure, so http cannot work.
bool isAllowedPublicUrl(const QUrl& url);

/// "https://host[:port]" of @p url, lowercased, default port dropped. Empty
/// when @p url is not an allowed public URL.
QString originOf(const QString& url);

/// The configured origins, normalised (originOf), invalid entries left out.
QStringList publicOrigins(const Settings& s);

/// The configured origin a request addressed to @p hostHeader is on, compared
/// the way RequestGuard::normalizeAuthority compares authorities; empty when it
/// is on none, and the login is then not offered there.
QString originFor(const Settings& s, const QString& hostHeader);

/// <origin>/auth/oidc/callback — what to register with the provider.
QString redirectUriFor(const QString& origin);

/// Why the settings cannot run a login, or "" when they can.
QString configProblem(const Settings& s);

/// Enabled and complete: the login routes answer and the button is offered.
inline bool isUsable(const Settings& s)
{
    return configProblem(s).isEmpty();
}

/// oidc_disable_pin in force. Only ever true while OIDC itself is usable, so a
/// half-configured provider can never shut every door at once.
inline bool pinDisabled(const Settings& s)
{
    return s.disablePin && isUsable(s);
}

/// A list setting as the admin page may send it: a JSON array, or one string
/// separated by commas or newlines. Trimmed, de-duplicated, capped at MAX_GROUPS.
QStringList parseList(const QJsonValue& v);

/// What an admin page update did.
struct UpdateResult
{
    bool ok = false;
    QString error;   ///< "invalid_<key>", or "incomplete" with problem set
    QString problem; ///< configProblem() of the result, for "incomplete"
    /// Who may sign in changed (login switched off, another provider or client,
    /// other groups): the sessions the old rules admitted must end.
    bool accessChanged = false;
};

/// Apply the keys of a POST /api/admin/oidc body to @p s. Nothing is changed
/// unless the whole body is valid; enabling an incomplete login is refused.
/// client_secret sets the secret when non-empty, clear_client_secret: true
/// clears it — the field is write-only, so an absent one means "unchanged".
UpdateResult applyUpdate(Settings& s, const QJsonObject& body);

// ── The authorization request ──────────────────────────────────────────────

/// @p bytes of CSPRNG output, base64url without padding.
QString randomToken(int bytes = 32);

/// The PKCE S256 challenge of @p verifier (RFC 7636 §4.2).
QString pkceChallenge(const QString& verifier);

QUrl authorizationUrl(const QString& authorizationEndpoint, const Settings& s,
                      const QString& redirectUri, const QString& state, const QString& nonce,
                      const QString& challenge);

// ── Login attempts ──────────────────────────────────────────────────────────

/// What /auth/oidc/start leaves behind for its callback.
struct Attempt
{
    QByteArray bindingHash; ///< SHA-256 of the mw_oidc cookie value
    QString nonce;
    QString verifier;    ///< PKCE code_verifier
    QString redirectUri; ///< the origin's, as sent: the callback must be on it too
    qint64 createdAt = 0;
};

enum class TakeError
{
    None,
    UnknownState, ///< never issued, already used, or evicted
    Expired,
    WrongBrowser, ///< mw_oidc missing or not the one start set
};

/**
 * The attempts in flight, keyed by state. In memory only: a restart abandons
 * every login in progress, which costs the user one more click.
 *
 * Single use: take() removes the state whatever it then decides, so a callback
 * URL can never be replayed — not even after a refusal.
 */
class PendingLogins
{
public:
    static constexpr qint64 TTL_SECS = 600;
    static constexpr int MAX_PENDING = 256;

    /// Start an attempt on @p redirectUri. Returns its state; @p binding
    /// receives the value the mw_oidc cookie must carry back, @p attempt what
    /// the callback will need.
    QString begin(qint64 now, const QString& redirectUri, QString* binding, Attempt* attempt);

    /// Spend @p state. The attempt comes back only for the browser that began
    /// it, within TTL_SECS.
    std::optional<Attempt> take(const QString& state, const QString& binding, qint64 now,
                                TakeError* error = nullptr);

    int size() const { return static_cast<int>(m_attempts.size()); }

private:
    void prune(qint64 now);
    QHash<QString, Attempt> m_attempts;
};

// ── What the ID token grants ───────────────────────────────────────────────

struct Access
{
    bool allowed = false;
    bool admin = false;
    QStringList groups;
};

/// The groups in @p claims under @p claimName: an array of strings, or a single
/// string. Anything else counts as no group.
QStringList groupsFromClaims(const QJsonObject& claims, const QString& claimName);

/// Allowed when no allow-list is set or a group is on it; an admin group also
/// admits. Admin when a group is in oidc_admin_groups. Exact, case-sensitive.
Access decideAccess(const QJsonObject& claims, const Settings& s);

/// A name to show for the session: name, preferred_username, email, else sub.
/// Control characters dropped, at most 64 characters.
QString displayName(const QJsonObject& claims);

} // namespace Oidc
