#pragma once

// Python's str.casefold() is more aggressive than a Unicode toLower(). The
// port keys case-insensitive identities (playlist names, shuffle sources) with
// this helper so "Straße" and "STRASSE" line up with what the Tk app does.
// Covers the full-fold mappings that differ from toLower for script-ordinary
// text; the rare Turkish-Azeri and sign-role exceptions are left as-is on both
// sides (they agree because both use one simple rule).

#include <QChar>
#include <QString>

inline QString caseFold(QString text)
{
    text = text.toLower();
    text.replace(QChar(0x00DF), QStringLiteral("ss"));    // ß
    text.replace(QChar(0x1E9E), QStringLiteral("ss"));     // ẞ (if toLower left it)
    text.replace(QChar(0x03C2), QChar(0x03C3));            // final sigma ς -> σ
    text.replace(QChar(0x0130), QStringLiteral("i\u0307"));   // İ -> i + combining dot
    text.replace(QChar(0x0149), QStringLiteral("\u02BCn"));   // ŉ
    text.replace(QChar(0xFB00), QStringLiteral("ff"));
    text.replace(QChar(0xFB01), QStringLiteral("fi"));
    text.replace(QChar(0xFB02), QStringLiteral("fl"));
    text.replace(QChar(0xFB03), QStringLiteral("ffi"));
    text.replace(QChar(0xFB04), QStringLiteral("ffl"));
    text.replace(QChar(0xFB05), QStringLiteral("st"));
    text.replace(QChar(0xFB06), QStringLiteral("st"));
    // characters that are already lower case but fold to another letter
    text.replace(QChar(0x00B5), QChar(0x03BC));            // µ micro sign -> μ
    text.replace(QChar(0x017F), QLatin1Char('s'));         // ſ long s
    text.replace(QChar(0x03D0), QChar(0x03B2));            // ϐ -> β
    text.replace(QChar(0x03D1), QChar(0x03B8));            // ϑ -> θ
    text.replace(QChar(0x03D5), QChar(0x03C6));            // ϕ -> φ
    text.replace(QChar(0x03D6), QChar(0x03C0));            // ϖ -> π
    text.replace(QChar(0x03F0), QChar(0x03BA));            // ϰ -> κ
    text.replace(QChar(0x03F1), QChar(0x03C1));            // ϱ -> ρ
    text.replace(QChar(0x03F5), QChar(0x03B5));            // ϵ -> ε
    text.replace(QChar(0x1E9B), QChar(0x1E61));            // ẛ -> ṡ
    text.replace(QChar(0x1FBE), QChar(0x03B9));            // ι (prosgegrammeni) -> ι
    return text;
}
