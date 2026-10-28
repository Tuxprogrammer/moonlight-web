/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * The page's diagnostics on the input channel. A codec fallback is announced
 * just before the page quits to relaunch in another codec: every relay must
 * recognise it (and log it at info, not as an unknown input), and what it logs
 * comes from a page that can write anything, so only short plain tokens of it
 * reach the log.
 */
#include "test_framework.h"
#include "streaming/ClientDiagnostic.h"

#include <QJsonObject>
#include <QString>

void run_client_diagnostic_tests()
{
    std::fprintf(stderr, "[client_diagnostic]\n");

    using InputMsg::describeClientDiagnostic;
    using InputMsg::isClientDiagnostic;

    // codec_fallback is a diagnostic; input and housekeeping types are not.
    {
        CHECK(isClientDiagnostic(QStringLiteral("codec_fallback")));
        for (const char* other :
             {"keydown", "mousemove", "gamepad", "ping", "requestidr", "linkstats", "codecfallback",
              "codec_fallback ", "CODEC_FALLBACK", ""})
            CHECK(!isClientDiagnostic(QString::fromLatin1(other)));
    }

    // The fields the client sends (StreamView._requestCodecFallback).
    {
        const QJsonObject msg{{QStringLiteral("type"), QStringLiteral("codec_fallback")},
                              {QStringLiteral("from"), QStringLiteral("hevc")},
                              {QStringLiteral("to"), QStringLiteral("h264")},
                              {QStringLiteral("hdr"), false}};
        CHECK_EQ(describeClientDiagnostic(QStringLiteral("codec_fallback"), msg),
                 QStringLiteral("Client codec fallback: from=hevc to=h264 hdr=false"));
    }
    {
        const QJsonObject msg{{QStringLiteral("from"), QStringLiteral("av1")},
                              {QStringLiteral("to"), QStringLiteral("hevc")},
                              {QStringLiteral("hdr"), true}};
        CHECK_EQ(describeClientDiagnostic(QStringLiteral("codec_fallback"), msg),
                 QStringLiteral("Client codec fallback: from=av1 to=hevc hdr=true"));
    }

    // Missing or mistyped fields read as '?', never as a guess.
    {
        CHECK_EQ(describeClientDiagnostic(QStringLiteral("codec_fallback"), QJsonObject()),
                 QStringLiteral("Client codec fallback: from=? to=? hdr=?"));
        const QJsonObject msg{{QStringLiteral("from"), 42},
                              {QStringLiteral("to"), QJsonObject()},
                              {QStringLiteral("hdr"), QStringLiteral("yes")}};
        CHECK_EQ(describeClientDiagnostic(QStringLiteral("codec_fallback"), msg),
                 QStringLiteral("Client codec fallback: from=? to=? hdr=?"));
        const QJsonObject empty{{QStringLiteral("from"), QString()},
                                {QStringLiteral("to"), QStringLiteral("h264")}};
        CHECK_EQ(describeClientDiagnostic(QStringLiteral("codec_fallback"), empty),
                 QStringLiteral("Client codec fallback: from=? to=h264 hdr=?"));
    }

    // A page can write anything: no line break, space or markup reaches the
    // log, and a long value is cut short.
    {
        const QJsonObject msg{
            {QStringLiteral("from"), QStringLiteral("hevc\n[Auth] admin unlocked")},
            {QStringLiteral("to"), QString(500, QLatin1Char('x'))},
            {QStringLiteral("hdr"), false}};
        const QString line = describeClientDiagnostic(QStringLiteral("codec_fallback"), msg);
        CHECK(!line.contains(QLatin1Char('\n')));
        CHECK(!line.contains(QLatin1Char('[')));
        CHECK_EQ(line, QStringLiteral("Client codec fallback: from=hevcAuthadminunlocked to=") +
                           QString(24, QLatin1Char('x')) + QStringLiteral(" hdr=false"));
    }
    {
        // Codec names with their usual punctuation survive.
        const QJsonObject msg{{QStringLiteral("from"), QStringLiteral("hevc-main10")},
                              {QStringLiteral("to"), QStringLiteral("avc1.42E01F")},
                              {QStringLiteral("hdr"), false}};
        CHECK_EQ(
            describeClientDiagnostic(QStringLiteral("codec_fallback"), msg),
            QStringLiteral("Client codec fallback: from=hevc-main10 to=avc1.42E01F hdr=false"));
    }

    // Anything that is not a known diagnostic is described as nothing.
    CHECK(describeClientDiagnostic(QStringLiteral("keydown"), QJsonObject()).isEmpty());
}
