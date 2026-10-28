/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 */
#include "test_framework.h"
#include "server/OperatorCli.h"

#include <QJsonDocument>
#include <QTextStream>

using namespace mw::cli;

namespace {

// A running server in a box: canned GET answers, every admin POST recorded.
struct FakeServer
{
    bool running = true;
    int connects = 0;
    QMap<QString, Reply> gets;
    QMap<QString, QList<Reply>> posts; // answered in order, last one repeats
    QList<QPair<QString, QJsonObject>> sent;
    QStringList getsAsked;

    Instance instance()
    {
        Instance i;
        i.connect = [this] {
            ++connects;
            return running;
        };
        i.get = [this](const QString& path) {
            getsAsked << path;
            return gets.value(path, Reply{false, 404, QJsonObject{{"error", "not found"}}, {}});
        };
        i.adminPost = [this](const QString& path, const QByteArray& body) {
            sent.append({path, QJsonDocument::fromJson(body).object()});
            QList<Reply>& queue = posts[path];
            if (queue.isEmpty()) return Reply{false, 404, {}, {}};
            return queue.size() > 1 ? queue.takeFirst() : queue.first();
        };
        return i;
    }
};

Reply okJson(const QJsonObject& obj)
{
    return Reply{true, 200, obj, {}};
}

// Ids as AuthManager::hashToken makes them: SHA-256 in base64url, 43 characters.
const QString kIdA = QStringLiteral("Ab3dEf_hIj-lMnOpQrStUvWxYz0123456789abcdEFG");
const QString kIdB = QStringLiteral("Ab3dEf_hQQ-lMnOpQrStUvWxYz0123456789abcdEFG");
const QString kIdC = QStringLiteral("zZ9-yY8_xX7wW6vV5uU4tT3sS2rR1qQ0pPoOnNmMlLk");

QJsonArray sampleSessions()
{
    QJsonObject a{{"token", kIdA},         {"ip", "192.0.2.10"},   {"machine_name", "Laptop"},
                  {"created_at", 1000000}, {"last_seen", 1003600}, {"streaming", true},
                  {"is_host", false},      {"is_admin", true},     {"via_tunnel", false},
                  {"ephemeral", false},    {"current", false}};
    QJsonObject b{{"token", kIdB},         {"ip", "198.51.100.7"}, {"machine_name", ""},
                  {"created_at", 1000000}, {"last_seen", 1000000}, {"streaming", false},
                  {"is_host", true},       {"is_admin", false},    {"via_tunnel", true},
                  {"ephemeral", true},     {"current", false}};
    QJsonObject c{{"token", kIdC},           {"ip", "192.0.2.20"},    {"machine_name", ""},
                  {"display_name", "alice"}, {"auth_method", "oidc"}, {"oidc_admin", true},
                  {"created_at", 1000000},   {"last_seen", 1003590},  {"current", true}};
    return QJsonArray{a, b, c};
}

struct Streams
{
    QString outText, errText;
    QTextStream out{&outText};
    QTextStream err{&errText};
    void flush()
    {
        out.flush();
        err.flush();
    }
};

} // namespace

static void test_operator_args()
{
    SECTION("OperatorCli: which argv entries are operator commands");
    for (const char* flag :
         {"--status", "--new-pin", "--set-admin-password", "--clear-admin-password",
          "--enable-internet", "--list-sessions", "--revoke-session", "--set", "--get"})
        CHECK(isOperatorCommandArg(flag));
    // The = spelling QCommandLineParser accepts for a valued option.
    CHECK(isOperatorCommandArg("--set=transport_mode=wss"));
    CHECK(isOperatorCommandArg("--get=https_port"));
    CHECK(isOperatorCommandArg("--revoke-session=abcd1234"));
    // A longer flag that merely starts with one is not it.
    CHECK(!isOperatorCommandArg("--settings"));
    CHECK(!isOperatorCommandArg("--getter"));
    CHECK(!isOperatorCommandArg("--status-all"));
    CHECK(!isOperatorCommandArg("--list-sessionsx"));
    // Neither the server's own flags nor modifiers of a command.
    CHECK(!isOperatorCommandArg("--port"));
    CHECK(!isOperatorCommandArg("--json"));
    CHECK(!isOperatorCommandArg("--yes"));
    CHECK(!isOperatorCommandArg("--stream-worker"));
    CHECK(!isOperatorCommandArg("set"));
    CHECK(!isOperatorCommandArg("-set"));
    CHECK(!isOperatorCommandArg(""));
    CHECK(!isOperatorCommandArg(nullptr));
}

static void test_setting_keys()
{
    SECTION("OperatorCli: the allow-list");
    // The keys the brief asks for that main's routes accept.
    for (const char* k : {"transport_mode", "cert_auth_enabled", "packet_marking", "instance_name",
                          "https_port", "debug_mode", "verbose_logs", "stream_notifications",
                          "upnp_enabled", "video_codec", "native_video_pipeline"})
        CHECK(findSettingKey(QString::fromLatin1(k)) != nullptr);
    // Not on main's routes (a WS-3 branch adds it), so not offered.
    CHECK(findSettingKey("allowed_transports") == nullptr);
    // Never offered: secrets, and what has its own command.
    CHECK(findSettingKey("admin_password") == nullptr);
    CHECK(findSettingKey("local_key") == nullptr);
    CHECK(findSettingKey("internet_access_enabled") == nullptr);
    CHECK(findSettingKey("") == nullptr);
    // Exact names only.
    CHECK(findSettingKey("Transport_Mode") == nullptr);
    CHECK(findSettingKey(" transport_mode") == nullptr);

    // Each key maps to the route the admin page writes it through.
    CHECK(findSettingKey("transport_mode")->group == SettingGroup::Streaming);
    CHECK(findSettingKey("upnp_enabled")->group == SettingGroup::Streaming);
    CHECK(findSettingKey("video_codec")->group == SettingGroup::Streaming);
    CHECK(findSettingKey("native_video_pipeline")->group == SettingGroup::Streaming);
    CHECK(findSettingKey("cert_auth_enabled")->group == SettingGroup::Admin);
    CHECK(findSettingKey("https_port")->group == SettingGroup::Admin);
    CHECK(findSettingKey("instance_name")->group == SettingGroup::Admin);
    CHECK(findSettingKey("debug_mode")->group == SettingGroup::Admin);
    CHECK(findSettingKey("verbose_logs")->group == SettingGroup::Admin);
    CHECK(findSettingKey("packet_marking")->group == SettingGroup::Admin);
    CHECK(findSettingKey("stream_notifications")->group == SettingGroup::Admin);

    CHECK_EQ(settingsPostPath(SettingGroup::Admin), QStringLiteral("/api/admin/settings"));
    CHECK_EQ(settingsGetPath(SettingGroup::Admin), QStringLiteral("/api/admin/settings"));
    CHECK_EQ(settingsPostPath(SettingGroup::Streaming), QStringLiteral("/api/settings/streaming"));
    CHECK_EQ(settingsGetPath(SettingGroup::Streaming), QStringLiteral("/api/settings/streaming"));

    // Types.
    CHECK(findSettingKey("cert_auth_enabled")->type == ValueType::Bool);
    CHECK(findSettingKey("https_port")->type == ValueType::Int);
    CHECK(findSettingKey("instance_name")->type == ValueType::String);
    CHECK(findSettingKey("transport_mode")->type == ValueType::Enum);

    // Every key has a summary, the list names every key once, and keys are unique.
    QStringList seen;
    const QString list = supportedKeysText();
    for (const SettingKey& k : settingKeys()) {
        CHECK(!k.summary.isEmpty());
        CHECK(!seen.contains(k.key));
        seen << k.key;
        CHECK(list.contains(k.key));
        CHECK(list.contains(describeValues(k)));
        if (k.type == ValueType::Enum) CHECK(!k.choices.isEmpty());
        if (k.type == ValueType::Int) CHECK(k.min <= k.max);
    }

    CHECK_EQ(describeValues(*findSettingKey("debug_mode")), QStringLiteral("true|false"));
    CHECK_EQ(describeValues(*findSettingKey("https_port")), QStringLiteral("1-65535"));
    CHECK(describeValues(*findSettingKey("transport_mode")).startsWith("auto|all|"));
    CHECK(describeValues(*findSettingKey("instance_name")).contains("32"));
}

static void test_parse_value()
{
    SECTION("OperatorCli: values, by type");
    const SettingKey& b = *findSettingKey("cert_auth_enabled");
    for (const char* t : {"true", "TRUE", "on", "yes", "1", "True", " true "}) {
        const ValueResult r = parseValue(b, QString::fromLatin1(t));
        CHECK(r.ok);
        CHECK(r.value == QJsonValue(true));
    }
    for (const char* f : {"false", "off", "no", "0", "OFF"}) {
        const ValueResult r = parseValue(b, QString::fromLatin1(f));
        CHECK(r.ok);
        CHECK(r.value == QJsonValue(false));
    }
    for (const char* bad : {"", "maybe", "2", "tru", "enabled", "-1"}) {
        const ValueResult r = parseValue(b, QString::fromLatin1(bad));
        CHECK(!r.ok);
        CHECK(r.error.contains("cert_auth_enabled"));
        CHECK(r.error.contains("true or false"));
    }

    const SettingKey& port = *findSettingKey("https_port");
    {
        ValueResult r = parseValue(port, "8443");
        CHECK(r.ok);
        CHECK(r.value == QJsonValue(8443));
        CHECK(parseValue(port, "1").ok);
        CHECK(parseValue(port, "65535").ok);
        CHECK(parseValue(port, " 443 ").ok);
    }
    for (const char* bad :
         {"0", "65536", "-443", "abc", "", "44 3", "8443.0", "0x1bb", "1e3", "99999999999"}) {
        const ValueResult r = parseValue(port, QString::fromLatin1(bad));
        CHECK(!r.ok);
        CHECK(r.error.contains("https_port"));
    }
    CHECK(parseValue(port, "70000").error.contains("between 1 and 65535"));
    CHECK(parseValue(port, "abc").error.contains("a number"));

    const SettingKey& name = *findSettingKey("instance_name");
    {
        ValueResult r = parseValue(name, "Living room");
        CHECK(r.ok);
        CHECK(r.value == QJsonValue(QStringLiteral("Living room")));
        // Empty is the reset to the machine's own name, not an error.
        r = parseValue(name, "");
        CHECK(r.ok);
        CHECK(r.value == QJsonValue(QString()));
        // 32 characters fit, as the server counts them (collapsed spaces).
        CHECK(parseValue(name, QString(32, QLatin1Char('x'))).ok);
        CHECK(parseValue(name, QStringLiteral("  ") + QString(32, QLatin1Char('x')) + "  ").ok);
        // Unicode counts as characters.
        CHECK(parseValue(name, QString(32, QChar(0x00e9))).ok);
        r = parseValue(name, QString(33, QLatin1Char('x')));
        CHECK(!r.ok);
        CHECK(r.error.contains("instance_name"));
        CHECK(r.error.contains("32"));
    }

    const SettingKey& tm = *findSettingKey("transport_mode");
    for (const char* v : {"auto", "all", "webrtc-media-udp", "webrtc-dc-udp", "webrtc-media-tcp",
                          "webrtc-dc-tcp", "wss"}) {
        const ValueResult r = parseValue(tm, QString::fromLatin1(v));
        CHECK(r.ok);
        CHECK(r.value == QJsonValue(QString::fromLatin1(v)));
    }
    {
        // Case and padding are forgiven; the stored value is canonical.
        const ValueResult r = parseValue(tm, " WSS ");
        CHECK(r.ok);
        CHECK(r.value == QJsonValue(QStringLiteral("wss")));
    }
    for (const char* bad : {"", "webrtc", "tcp", "wss,auto", "webrtc-dc"}) {
        const ValueResult r = parseValue(tm, QString::fromLatin1(bad));
        CHECK(!r.ok);
        CHECK(r.error.contains("transport_mode"));
        CHECK(r.error.contains("auto, all, webrtc-media-udp"));
    }
    CHECK(parseValue(*findSettingKey("video_codec"), "AV1").value == QJsonValue("av1"));
    CHECK(!parseValue(*findSettingKey("video_codec"), "auto").ok);
    CHECK(!parseValue(*findSettingKey("video_codec"), "vp9").ok);
    CHECK(parseValue(*findSettingKey("native_video_pipeline"), "vulkan").ok);
    CHECK(!parseValue(*findSettingKey("native_video_pipeline"), "metal").ok);
}

static void test_split_assignment()
{
    SECTION("OperatorCli: key=value");
    Assignment a;
    QString error;
    CHECK(splitAssignment("transport_mode=wss", &a, &error));
    CHECK_EQ(a.key, QStringLiteral("transport_mode"));
    CHECK_EQ(a.value, QStringLiteral("wss"));
    // The value keeps everything after the first '=', spaces included.
    CHECK(splitAssignment("instance_name=a=b c", &a, &error));
    CHECK_EQ(a.key, QStringLiteral("instance_name"));
    CHECK_EQ(a.value, QStringLiteral("a=b c"));
    CHECK(splitAssignment("instance_name=", &a, &error));
    CHECK(a.value.isEmpty());
    // Spaces around the key are a typing accident, not part of the name.
    CHECK(splitAssignment(" https_port =8443", &a, &error));
    CHECK_EQ(a.key, QStringLiteral("https_port"));

    CHECK(!splitAssignment("transport_mode", &a, &error));
    CHECK(error.contains("key=value"));
    CHECK(!splitAssignment("=wss", &a, &error));
    CHECK(error.contains("key=value"));
    CHECK(!splitAssignment("", &a, &error));
}

static void test_plan_set()
{
    SECTION("OperatorCli: --set plans");
    {
        const SetPlan p = planSet({"transport_mode=wss"});
        CHECK(p.ok);
        CHECK_EQ(p.requests.size(), 1);
        CHECK(p.requests[0].group == SettingGroup::Streaming);
        CHECK(p.requests[0].body == QJsonObject({{"transport_mode", "wss"}}));
        CHECK_EQ(p.requests[0].keys, QStringList{"transport_mode"});
    }
    {
        // Repeated --set: one body per route, streaming first whatever the
        // order given, keys in the order given.
        const SetPlan p = planSet({"https_port=8443", "cert_auth_enabled=on", "upnp_enabled=off",
                                   "debug_mode=1", "transport_mode=auto"});
        CHECK(p.ok);
        CHECK_EQ(p.requests.size(), 2);
        CHECK(p.requests[0].group == SettingGroup::Streaming);
        CHECK(p.requests[0].body ==
              QJsonObject({{"upnp_enabled", false}, {"transport_mode", "auto"}}));
        CHECK_EQ(p.requests[0].keys, (QStringList{"upnp_enabled", "transport_mode"}));
        CHECK(p.requests[1].group == SettingGroup::Admin);
        CHECK(
            p.requests[1].body ==
            QJsonObject({{"https_port", 8443}, {"cert_auth_enabled", true}, {"debug_mode", true}}));
        CHECK_EQ(p.requests[1].keys,
                 (QStringList{"https_port", "cert_auth_enabled", "debug_mode"}));
    }
    {
        const SetPlan p = planSet({"verbose_logs=true"});
        CHECK(p.ok);
        CHECK_EQ(p.requests.size(), 1);
        CHECK(p.requests[0].group == SettingGroup::Admin);
    }
    {
        // An unknown key says so and lists the keys there are.
        const SetPlan p = planSet({"transport_mode=wss", "allowed_transports=wss"});
        CHECK(!p.ok);
        CHECK(p.unknownKey);
        CHECK(p.error.contains("allowed_transports"));
        CHECK(p.requests.isEmpty());
    }
    {
        // One bad value refuses the lot: nothing is half-applied.
        const SetPlan p = planSet({"cert_auth_enabled=true", "https_port=70000"});
        CHECK(!p.ok);
        CHECK(!p.unknownKey);
        CHECK(p.error.contains("https_port"));
        CHECK(p.requests.isEmpty());
    }
    {
        // The same key twice is a mistake to point out, not a last-one-wins.
        const SetPlan p = planSet({"debug_mode=on", "debug_mode=off"});
        CHECK(!p.ok);
        CHECK(p.error.contains("debug_mode"));
        CHECK(p.error.contains("twice"));
    }
    {
        const SetPlan p = planSet({"debug_mode"});
        CHECK(!p.ok);
        CHECK(p.error.contains("key=value"));
    }
    {
        const SetPlan p = planSet({});
        CHECK(!p.ok);
    }
}

static void test_plan_get()
{
    SECTION("OperatorCli: --get plans");
    {
        const GetPlan p = planGet({"https_port"});
        CHECK(p.ok);
        CHECK_EQ(p.keys, QStringList{"https_port"});
    }
    {
        const GetPlan p = planGet({"https_port", "transport_mode", "https_port"});
        CHECK(p.ok);
        CHECK_EQ(p.keys, (QStringList{"https_port", "transport_mode"}));
    }
    {
        const GetPlan p = planGet({"all"});
        CHECK(p.ok);
        CHECK_EQ(p.keys.size(), settingKeys().size());
        CHECK_EQ(p.keys.first(), settingKeys().first().key);
    }
    {
        const GetPlan p = planGet({"nope"});
        CHECK(!p.ok);
        CHECK(p.unknownKey);
        CHECK(p.error.contains("nope"));
    }
    CHECK(!planGet({}).ok);
}

static void test_format_value()
{
    SECTION("OperatorCli: values as printed");
    CHECK_EQ(formatValue(QJsonValue(true)), QStringLiteral("true"));
    CHECK_EQ(formatValue(QJsonValue(false)), QStringLiteral("false"));
    CHECK_EQ(formatValue(QJsonValue(8443)), QStringLiteral("8443"));
    CHECK_EQ(formatValue(QJsonValue(8443.0)), QStringLiteral("8443"));
    CHECK_EQ(formatValue(QJsonValue(QStringLiteral("wss"))), QStringLiteral("wss"));
    CHECK_EQ(formatValue(QJsonValue(QString())), QString());
    CHECK_EQ(formatValue(QJsonValue()), QString());
}

static void test_match_session()
{
    SECTION("OperatorCli: finding a session by id");
    const QJsonArray s = sampleSessions();
    {
        const SessionMatch m = matchSession(s, kIdC);
        CHECK(m.kind == MatchKind::Found);
        CHECK_EQ(m.session.value("token").toString(), kIdC);
    }
    {
        // The short id the table prints, and any unique prefix.
        const SessionMatch m = matchSession(s, kIdC.left(kShortIdLength));
        CHECK(m.kind == MatchKind::Found);
        CHECK(matchSession(s, "zZ9-").kind == MatchKind::Found);
        CHECK(matchSession(s, "Ab3dEf_hI").kind == MatchKind::Found);
        CHECK_EQ(matchSession(s, "Ab3dEf_hI").session.value("token").toString(), kIdA);
        // Padding is forgiven; case is not: base64url is case-sensitive, and
        // "ab3d" is another id than "Ab3d".
        CHECK(matchSession(s, " zZ9-yY8_ ").kind == MatchKind::Found);
        CHECK(matchSession(s, "ab3dEf_hI").kind == MatchKind::NotFound);
        CHECK(matchSession(s, "ZZ9-").kind == MatchKind::NotFound);
    }
    {
        const SessionMatch m = matchSession(s, "Ab3dEf_h");
        CHECK(m.kind == MatchKind::Ambiguous);
        CHECK_EQ(m.candidates.size(), 2);
        CHECK(m.candidates.contains(kIdA));
        CHECK(m.candidates.contains(kIdB));
    }
    CHECK(matchSession(s, "ffff9999").kind == MatchKind::NotFound);
    CHECK(matchSession(s, "zzzzzz").kind == MatchKind::NotFound);
    CHECK(matchSession(QJsonArray(), "zZ9-").kind == MatchKind::NotFound);
    // Too short to mean one session, outside the base64url alphabet, or
    // longer than any id could be.
    CHECK(matchSession(s, "zZ9").kind == MatchKind::Malformed);
    CHECK(matchSession(s, "").kind == MatchKind::Malformed);
    CHECK(matchSession(s, "    ").kind == MatchKind::Malformed);
    CHECK(matchSession(s, "zZ9-+456").kind == MatchKind::Malformed);
    CHECK(matchSession(s, "zZ9/yY8").kind == MatchKind::Malformed);
    CHECK(matchSession(s, "zZ9= ").kind == MatchKind::Malformed);
    CHECK(matchSession(s, "zZ 9-yY").kind == MatchKind::Malformed);
    CHECK(matchSession(s, QString(129, QLatin1Char('a'))).kind == MatchKind::Malformed);
    // A whole id is never too long.
    CHECK(matchSession(s, kIdC).kind == MatchKind::Found);
}

static void test_session_table()
{
    SECTION("OperatorCli: the sessions table");
    const QJsonArray s = sampleSessions();
    CHECK_EQ(sessionFlags(s[0].toObject()), (QStringList{"admin", "streaming"}));
    CHECK_EQ(sessionFlags(s[1].toObject()), (QStringList{"host", "tunnel", "ephemeral"}));
    CHECK_EQ(sessionFlags(s[2].toObject()), (QStringList{"oidc", "oidc-admin", "current"}));
    CHECK(sessionFlags(QJsonObject()).isEmpty());

    CHECK_EQ(formatAge(0), QStringLiteral("just now"));
    CHECK_EQ(formatAge(59), QStringLiteral("just now"));
    CHECK_EQ(formatAge(-30), QStringLiteral("just now")); // a clock a little ahead
    CHECK_EQ(formatAge(60), QStringLiteral("1 min ago"));
    CHECK_EQ(formatAge(3599), QStringLiteral("59 min ago"));
    CHECK_EQ(formatAge(3600), QStringLiteral("1 h ago"));
    CHECK_EQ(formatAge(86399), QStringLiteral("23 h ago"));
    CHECK_EQ(formatAge(86400), QStringLiteral("1 d ago"));
    CHECK_EQ(formatAge(86400 * 40), QStringLiteral("40 d ago"));

    const QString t = formatSessionsTable(s, 1003600, QTimeZone::utc());
    const QStringList lines = t.split('\n', Qt::SkipEmptyParts);
    CHECK_EQ(lines.size(), 4);
    CHECK(lines[0].startsWith("ID"));
    for (const char* h : {"DEVICE", "ADDRESS", "CREATED", "LAST SEEN", "FLAGS"})
        CHECK(lines[0].contains(QString::fromLatin1(h)));
    // Short ids, never the whole hash.
    CHECK(lines[1].startsWith(kIdA.left(kShortIdLength) + " "));
    CHECK(!t.contains(kIdA));
    CHECK(lines[1].contains("Laptop"));
    CHECK(lines[1].contains("192.0.2.10"));
    CHECK(lines[1].contains("1970-01-12 13:46")); // 1000000 s, UTC
    CHECK(lines[1].contains("just now"));
    CHECK(lines[1].endsWith("admin, streaming"));
    // No device name: a dash, not an empty gap that shifts the columns.
    CHECK(lines[2].contains(" - "));
    CHECK(lines[2].contains("1 h ago"));
    // An OIDC session names the person.
    CHECK(lines[3].contains("alice"));
    // Columns line up: FLAGS starts at the same offset on every line.
    const int col = lines[0].indexOf("FLAGS");
    CHECK(lines[1].mid(col).startsWith("admin"));
    CHECK(lines[2].mid(col).startsWith("host"));

    CHECK_EQ(formatSessionsTable(QJsonArray(), 0, QTimeZone::utc()), QString());
}

static void test_reply_error()
{
    SECTION("OperatorCli: error text");
    CHECK_EQ(replyError(Reply{false, 400, QJsonObject{{"error", "bad thing"}}, {}}),
             QStringLiteral("bad thing"));
    CHECK_EQ(replyError(Reply{false, 0, {}, "Connection refused"}),
             QStringLiteral("Connection refused"));
    CHECK_EQ(replyError(Reply{false, 500, {}, {}}), QStringLiteral("HTTP 500"));
    // The API's message beats Qt's generic "server replied: Bad Request".
    CHECK_EQ(replyError(Reply{false, 400, QJsonObject{{"error", "why"}}, "replied: Bad"}),
             QStringLiteral("why"));
    CHECK(notRunningMessage().contains("not running"));
}

static void test_run_list_sessions()
{
    SECTION("OperatorCli: --list-sessions against a fake server");
    {
        FakeServer f;
        f.gets["/api/auth/sessions"] = okJson(QJsonObject{{"sessions", sampleSessions()}});
        Streams st;
        CHECK_EQ(runListSessions(f.instance(), false, 1003600, QTimeZone::utc(), st.out, st.err),
                 int(kExitOk));
        st.flush();
        CHECK(st.outText.contains("Laptop"));
        CHECK(st.outText.contains("3 sessions"));
        CHECK(st.errText.isEmpty());
        CHECK(f.sent.isEmpty()); // a query writes nothing
    }
    {
        // --json: the server's array, whole ids, nothing else on stdout.
        FakeServer f;
        f.gets["/api/auth/sessions"] = okJson(QJsonObject{{"sessions", sampleSessions()}});
        Streams st;
        CHECK_EQ(runListSessions(f.instance(), true, 0, QTimeZone::utc(), st.out, st.err),
                 int(kExitOk));
        st.flush();
        QJsonParseError pe;
        const QJsonDocument d = QJsonDocument::fromJson(st.outText.toUtf8(), &pe);
        CHECK(pe.error == QJsonParseError::NoError);
        CHECK_EQ(d.object().value("sessions").toArray().size(), 3);
        CHECK(st.outText.contains(kIdA));
    }
    {
        FakeServer f;
        f.gets["/api/auth/sessions"] = okJson(QJsonObject{{"sessions", QJsonArray()}});
        Streams st;
        CHECK_EQ(runListSessions(f.instance(), false, 0, QTimeZone::utc(), st.out, st.err),
                 int(kExitOk));
        st.flush();
        CHECK(st.outText.contains("No sessions"));
    }
    {
        FakeServer f;
        f.running = false;
        Streams st;
        CHECK_EQ(runListSessions(f.instance(), false, 0, QTimeZone::utc(), st.out, st.err),
                 int(kExitFailed));
        st.flush();
        CHECK(st.errText.contains("not running"));
        CHECK(st.outText.isEmpty());
        CHECK(f.getsAsked.isEmpty());
    }
    {
        FakeServer f;
        f.gets["/api/auth/sessions"] =
            Reply{false, 403, QJsonObject{{"error", "Only available from localhost"}}, {}};
        Streams st;
        CHECK_EQ(runListSessions(f.instance(), false, 0, QTimeZone::utc(), st.out, st.err),
                 int(kExitFailed));
        st.flush();
        CHECK(st.errText.contains("Only available from localhost"));
    }
}

static void test_run_revoke()
{
    SECTION("OperatorCli: --revoke-session against a fake server");
    const Reply list = okJson(QJsonObject{{"sessions", sampleSessions()}});
    {
        FakeServer f;
        f.gets["/api/auth/sessions"] = list;
        f.posts["/api/auth/sessions/revoke"] = {okJson(QJsonObject{{"status", "revoked"}})};
        Streams st;
        CHECK_EQ(runRevokeSession(f.instance(), kIdA.left(12), st.out, st.err), int(kExitOk));
        st.flush();
        // The whole id goes to the server, as the admin page sends it.
        CHECK_EQ(f.sent.size(), 1);
        CHECK_EQ(f.sent[0].first, QStringLiteral("/api/auth/sessions/revoke"));
        CHECK(f.sent[0].second == QJsonObject({{"token", kIdA}}));
        CHECK(st.outText.contains("Revoked"));
        CHECK(st.outText.contains("Laptop"));
        CHECK(st.outText.contains("stream")); // it was streaming: say it stopped
    }
    {
        // A host session: say what that browser loses.
        FakeServer f;
        f.gets["/api/auth/sessions"] = list;
        f.posts["/api/auth/sessions/revoke"] = {okJson(QJsonObject{{"status", "revoked"}})};
        Streams st;
        CHECK_EQ(runRevokeSession(f.instance(), kIdB, st.out, st.err), int(kExitOk));
        st.flush();
        CHECK(st.outText.contains("host key"));
    }
    {
        // The server's guard against revoking the caller itself.
        FakeServer f;
        f.gets["/api/auth/sessions"] = list;
        f.posts["/api/auth/sessions/revoke"] = {Reply{
            false, 409, QJsonObject{{"status", "error"}, {"error", "cannot_revoke_self"}}, {}}};
        Streams st;
        CHECK_EQ(runRevokeSession(f.instance(), kIdC, st.out, st.err), int(kExitFailed));
        st.flush();
        CHECK(st.errText.contains("own session"));
    }
    {
        FakeServer f;
        f.gets["/api/auth/sessions"] = list;
        Streams st;
        CHECK_EQ(runRevokeSession(f.instance(), "ffff9999", st.out, st.err), int(kExitFailed));
        st.flush();
        CHECK(st.errText.contains("No session"));
        CHECK(f.sent.isEmpty());
    }
    {
        FakeServer f;
        f.gets["/api/auth/sessions"] = list;
        Streams st;
        CHECK_EQ(runRevokeSession(f.instance(), "Ab3dEf_h", st.out, st.err), int(kExitUsage));
        st.flush();
        CHECK(st.errText.contains(kIdA.left(kShortIdLength)));
        CHECK(st.errText.contains(kIdB.left(kShortIdLength)));
        CHECK(f.sent.isEmpty());
    }
    {
        // A malformed id is refused before the server is even looked for.
        FakeServer f;
        Streams st;
        CHECK_EQ(runRevokeSession(f.instance(), "xyz", st.out, st.err), int(kExitUsage));
        CHECK_EQ(f.connects, 0);
    }
    {
        FakeServer f;
        f.running = false;
        Streams st;
        CHECK_EQ(runRevokeSession(f.instance(), kIdA, st.out, st.err), int(kExitFailed));
        st.flush();
        CHECK(st.errText.contains("not running"));
    }
    {
        FakeServer f;
        f.gets["/api/auth/sessions"] = list;
        f.posts["/api/auth/sessions/revoke"] = {Reply{false, 0, {}, "timed out after 15 s"}};
        Streams st;
        CHECK_EQ(runRevokeSession(f.instance(), kIdA, st.out, st.err), int(kExitFailed));
        st.flush();
        CHECK(st.errText.contains("timed out"));
    }
}

static void test_run_set()
{
    SECTION("OperatorCli: --set against a fake server");
    {
        FakeServer f;
        f.posts["/api/settings/streaming"] = {
            okJson(QJsonObject{{"transport_mode", "wss"}, {"status", "saved"}})};
        f.posts["/api/admin/settings"] = {okJson(QJsonObject{{"cert_auth_enabled", true}})};
        Streams st;
        CHECK_EQ(
            runSet(f.instance(), {"cert_auth_enabled=yes", "transport_mode=WSS"}, st.out, st.err),
            int(kExitOk));
        st.flush();
        CHECK_EQ(f.sent.size(), 2);
        CHECK_EQ(f.sent[0].first, QStringLiteral("/api/settings/streaming"));
        CHECK(f.sent[0].second == QJsonObject({{"transport_mode", "wss"}}));
        CHECK_EQ(f.sent[1].first, QStringLiteral("/api/admin/settings"));
        CHECK(f.sent[1].second == QJsonObject({{"cert_auth_enabled", true}}));
        CHECK(st.outText.contains("transport_mode = wss"));
        CHECK(st.outText.contains("cert_auth_enabled = true"));
        CHECK(st.errText.isEmpty());
    }
    {
        // What the server kept is what is printed — and a difference is named
        // (debug_mode held on by --debug for this run).
        FakeServer f;
        f.posts["/api/admin/settings"] = {
            okJson(QJsonObject{{"debug_mode", true}, {"instance_name", "Den"}})};
        Streams st;
        CHECK_EQ(runSet(f.instance(), {"debug_mode=off", "instance_name=  Den  "}, st.out, st.err),
                 int(kExitOk));
        st.flush();
        CHECK(st.outText.contains("debug_mode = true"));
        CHECK(st.outText.contains("asked for false"));
        CHECK(st.outText.contains("instance_name = Den"));
    }
    {
        FakeServer f;
        f.posts["/api/admin/settings"] = {
            okJson(QJsonObject{{"https_port", 8443}, {"port_changed", true}})};
        Streams st;
        CHECK_EQ(runSet(f.instance(), {"https_port=8443"}, st.out, st.err), int(kExitOk));
        st.flush();
        CHECK(st.outText.contains("https_port = 8443"));
        CHECK(st.outText.contains("8443")); // where the listener moves
        CHECK(st.outText.contains("--config") || st.outText.contains("port"));
    }
    {
        // Usage errors: exit 2, nothing sent, server not even looked for.
        for (const QStringList& bad :
             {QStringList{"bogus=1"}, QStringList{"https_port=0"}, QStringList{"debug_mode"},
              QStringList{"debug_mode=on", "debug_mode=on"}, QStringList{}}) {
            FakeServer f;
            Streams st;
            CHECK_EQ(runSet(f.instance(), bad, st.out, st.err), int(kExitUsage));
            st.flush();
            CHECK(f.sent.isEmpty());
            CHECK_EQ(f.connects, 0);
            CHECK(!st.errText.isEmpty());
            CHECK(st.outText.isEmpty());
        }
        // An unknown key lists the supported ones.
        FakeServer f;
        Streams st;
        runSet(f.instance(), {"bogus=1"}, st.out, st.err);
        st.flush();
        CHECK(st.errText.contains("bogus"));
        CHECK(st.errText.contains("transport_mode"));
        CHECK(st.errText.contains("https_port"));
    }
    {
        FakeServer f;
        f.running = false;
        Streams st;
        CHECK_EQ(runSet(f.instance(), {"debug_mode=on"}, st.out, st.err), int(kExitFailed));
        st.flush();
        CHECK(st.errText.contains("not running"));
        CHECK(f.sent.isEmpty());
    }
    {
        // The server refuses the first request: exit 1, the second never sent.
        FakeServer f;
        f.posts["/api/settings/streaming"] = {Reply{
            false,
            400,
            QJsonObject{{"error", "native_video_pipeline must be auto, d3d11, d3d12, vaapi or "
                                  "vulkan"}},
            {}}};
        Streams st;
        CHECK_EQ(
            runSet(f.instance(), {"native_video_pipeline=vulkan", "debug_mode=on"}, st.out, st.err),
            int(kExitFailed));
        st.flush();
        CHECK_EQ(f.sent.size(), 1);
        CHECK(st.errText.contains("native_video_pipeline must be"));
        CHECK(st.errText.contains("Nothing was changed"));
    }
    {
        // The second refuses after the first applied: say what did change.
        FakeServer f;
        f.posts["/api/settings/streaming"] = {okJson(QJsonObject{{"upnp_enabled", false}})};
        f.posts["/api/admin/settings"] = {Reply{false, 403, QJsonObject{{"error", "nope"}}, {}}};
        Streams st;
        CHECK_EQ(runSet(f.instance(), {"upnp_enabled=off", "debug_mode=on"}, st.out, st.err),
                 int(kExitFailed));
        st.flush();
        CHECK(st.outText.contains("upnp_enabled = false"));
        CHECK(st.errText.contains("nope"));
        CHECK(st.errText.contains("debug_mode"));
        CHECK(!st.errText.contains("Nothing was changed"));
    }
}

static void test_run_get()
{
    SECTION("OperatorCli: --get against a fake server");
    const Reply admin = okJson(QJsonObject{{"https_port", 8443},
                                           {"cert_auth_enabled", false},
                                           {"instance_name", "Den"},
                                           {"debug_mode", false},
                                           {"verbose_logs", true},
                                           {"packet_marking", true},
                                           {"stream_notifications", true}});
    const Reply streaming = okJson(QJsonObject{{"transport_mode", "auto"},
                                               {"upnp_enabled", true},
                                               {"video_codec", "hevc"},
                                               {"native_video_pipeline", "auto"}});
    {
        // One key: the bare value, for $(moonlightweb --get https_port).
        FakeServer f;
        f.gets["/api/admin/settings"] = admin;
        Streams st;
        CHECK_EQ(runGet(f.instance(), {"https_port"}, false, st.out, st.err), int(kExitOk));
        st.flush();
        CHECK_EQ(st.outText, QStringLiteral("8443\n"));
        CHECK_EQ(f.getsAsked, QStringList{"/api/admin/settings"});
        CHECK(f.sent.isEmpty());
    }
    {
        // Several: key=value lines, each route read once.
        FakeServer f;
        f.gets["/api/admin/settings"] = admin;
        f.gets["/api/settings/streaming"] = streaming;
        Streams st;
        CHECK_EQ(runGet(f.instance(), {"transport_mode", "https_port", "upnp_enabled"}, false,
                        st.out, st.err),
                 int(kExitOk));
        st.flush();
        CHECK_EQ(st.outText,
                 QStringLiteral("transport_mode=auto\nhttps_port=8443\nupnp_enabled=true\n"));
        CHECK_EQ(f.getsAsked.size(), 2);
    }
    {
        FakeServer f;
        f.gets["/api/admin/settings"] = admin;
        f.gets["/api/settings/streaming"] = streaming;
        Streams st;
        CHECK_EQ(runGet(f.instance(), {"all"}, true, st.out, st.err), int(kExitOk));
        st.flush();
        const QJsonObject o = QJsonDocument::fromJson(st.outText.toUtf8()).object();
        CHECK_EQ(o.size(), settingKeys().size());
        CHECK(o.value("https_port") == QJsonValue(8443));
        CHECK(o.value("transport_mode") == QJsonValue("auto"));
    }
    {
        FakeServer f;
        Streams st;
        CHECK_EQ(runGet(f.instance(), {"nope"}, false, st.out, st.err), int(kExitUsage));
        st.flush();
        CHECK(st.errText.contains("transport_mode"));
        CHECK_EQ(f.connects, 0);
    }
    {
        FakeServer f;
        f.running = false;
        Streams st;
        CHECK_EQ(runGet(f.instance(), {"https_port"}, false, st.out, st.err), int(kExitFailed));
    }
    {
        // An older server that does not report the key: say so, exit 1.
        FakeServer f;
        f.gets["/api/admin/settings"] = okJson(QJsonObject{{"https_port", 443}});
        Streams st;
        CHECK_EQ(runGet(f.instance(), {"packet_marking"}, false, st.out, st.err), int(kExitFailed));
        st.flush();
        CHECK(st.errText.contains("packet_marking"));
    }
    {
        FakeServer f;
        f.gets["/api/admin/settings"] = Reply{false, 500, {}, {}};
        Streams st;
        CHECK_EQ(runGet(f.instance(), {"https_port"}, false, st.out, st.err), int(kExitFailed));
        st.flush();
        CHECK(st.errText.contains("HTTP 500"));
    }
}

void run_operator_cli_tests()
{
    test_operator_args();
    test_setting_keys();
    test_parse_value();
    test_split_assignment();
    test_plan_set();
    test_plan_get();
    test_format_value();
    test_match_session();
    test_session_table();
    test_reply_error();
    test_run_list_sessions();
    test_run_revoke();
    test_run_set();
    test_run_get();
}
