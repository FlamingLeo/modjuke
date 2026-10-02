#include "filterdialog.h"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHash>
#include <QVBoxLayout>
#include <algorithm>

namespace {

// "90", "1:30", "1:30.5", "" -> seconds; ok=false on garbage.
double parseSeconds(const QString &text, bool *okOut)
{
    const QString t = text.trimmed();
    if (t.isEmpty()) {
        if (okOut)
            *okOut = true;
        return 0.0;
    }
    double total = 0.0;
    bool ok = true;
    for (const QString &part : t.split(QLatin1Char(':'))) {
        bool partOk = false;
        const double v = part.trimmed().toDouble(&partOk);
        if (!partOk) {
            ok = false;
            break;
        }
        total = total * 60.0 + v;
    }
    if (okOut)
        *okOut = ok;
    return ok ? total : -1.0;
}

QString secondsText(double seconds)
{
    if (seconds <= 0.0)
        return QString();
    const int total = int(seconds);
    return QString::asprintf("%d:%02d", total / 60, total % 60);
}

// formats_in(): (format, count) sorted by count desc, then name.
QVector<QPair<QString, int>> formatsIn(const QVector<Track> &tracks)
{
    // unanalyzed tracks count under "" (shown as "unknown"): the filter
    // compares against the track's format, which is "" for them
    QHash<QString, int> counts;
    for (const Track &track : tracks)
        counts[track.fmt.toLower().trimmed()] += 1;
    QVector<QPair<QString, int>> out;
    for (auto it = counts.constBegin(); it != counts.constEnd(); ++it)
        out.append({it.key(), it.value()});
    std::sort(out.begin(), out.end(), [](const auto &a, const auto &b) {
        return a.second != b.second ? a.second > b.second : a.first < b.first;
    });
    return out;
}

}   // namespace

FilterDialog::FilterDialog(QWidget *parent, const QueueFilter &current,
                           const QVector<Track> &tracks)
    : QDialog(parent), tracks_(tracks)
{
    setWindowTitle(tr("Filter the queue"));
    auto *root = new QVBoxLayout(this);

    auto *formatBox = new QGroupBox(tr("Formats"), this);
    auto *grid = new QGridLayout(formatBox);
    const auto formats = formatsIn(tracks);
    int col = 0;
    for (const auto &pair : formats) {
        auto *check = new QCheckBox(QStringLiteral("%1 (%2)")
                                        .arg(pair.first.isEmpty() ? tr("unknown") : pair.first)
                                        .arg(pair.second),
                                    formatBox);
        check->setProperty("format", pair.first);
        // "all formats selected" means no restriction (keeps unknown too)
        check->setChecked(current.formats.isEmpty() || current.formats.contains(pair.first)
                          || (pair.first.isEmpty() && current.formats.contains(QLatin1String("unknown"))));
        connect(check, &QCheckBox::toggled, this, [this] { updatePreview(); });
        grid->addWidget(check, col / 3, col % 3);
        formatChecks_.append(check);
        ++col;
    }
    if (formats.isEmpty())
        grid->addWidget(new QLabel(tr("No analyzed tracks yet - run Analyze first."), formatBox));
    root->addWidget(formatBox);

    auto *range = new QGroupBox(tr("Length (seconds or m:ss)"), this);
    auto *form = new QFormLayout(range);
    minEdit_ = new QLineEdit(secondsText(current.minSeconds), range);
    maxEdit_ = new QLineEdit(secondsText(current.maxSeconds), range);
    minEdit_->setPlaceholderText(tr("no minimum"));
    maxEdit_->setPlaceholderText(tr("no maximum"));
    connect(minEdit_, &QLineEdit::textChanged, this, [this] { updatePreview(); });
    connect(maxEdit_, &QLineEdit::textChanged, this, [this] { updatePreview(); });
    form->addRow(tr("Shortest"), minEdit_);
    form->addRow(tr("Longest"), maxEdit_);
    root->addWidget(range);

    hideBroken_ = new QCheckBox(tr("Hide modules that cannot be played"), this);
    hideBroken_->setChecked(current.hideBroken);
    connect(hideBroken_, &QCheckBox::toggled, this, [this] { updatePreview(); });
    root->addWidget(hideBroken_);

    countLabel_ = new QLabel(this);
    root->addWidget(countLabel_);

    auto *buttons = new QDialogButtonBox(this);
    auto *clearBtn = buttons->addButton(tr("Clear filter"), QDialogButtonBox::ResetRole);
    buttons->addButton(tr("Cancel"), QDialogButtonBox::RejectRole);
    auto *applyBtn = buttons->addButton(tr("Apply"), QDialogButtonBox::AcceptRole);
    applyBtn_ = applyBtn;
    connect(clearBtn, &QPushButton::clicked, this, &FilterDialog::clear);
    connect(applyBtn, &QPushButton::clicked, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(buttons);

    updatePreview();
    resize(440, 300);
}

QueueFilter FilterDialog::criteria() const
{
    QueueFilter out;
    const bool all = std::all_of(formatChecks_.begin(), formatChecks_.end(),
                                 [](QCheckBox *c) { return c->isChecked(); });
    if (!all) {
        for (QCheckBox *check : formatChecks_) {
            if (check->isChecked()) {
                // not-analyzed tracks are stored as "unknown" (the settings
                // loader drops empty format names)
                const QString f = check->property("format").toString();
                out.formats << (f.isEmpty() ? QStringLiteral("unknown") : f);
            }
        }
    }
    // ok is never checked: a garbled length counts as no limit (-1 -> 0)
    bool ok = true;
    out.minSeconds = std::max(0.0, parseSeconds(minEdit_->text(), &ok));
    out.maxSeconds = std::max(0.0, parseSeconds(maxEdit_->text(), &ok));
    out.hideBroken = hideBroken_->isChecked();
    return out;
}

void FilterDialog::updatePreview()
{
    // nothing checked would be stored as "no restriction" and show everything
    const bool none = !formatChecks_.isEmpty()
                      && std::none_of(formatChecks_.begin(), formatChecks_.end(),
                                      [](QCheckBox *c) { return c->isChecked(); });
    if (applyBtn_)
        applyBtn_->setEnabled(!none);
    if (none) {
        countLabel_->setText(tr("Check at least one format."));
        return;
    }
    const int kept = criteria().count(tracks_);
    QString text = tr("%1 of %2 tracks match").arg(kept).arg(tracks_.size());
    int unknown = 0;
    for (const Track &t : tracks_) {
        if (!t.analyzed)
            ++unknown;
    }
    if (unknown)
        text += tr("  (%1 not analyzed yet)").arg(unknown);
    countLabel_->setText(text);
}

void FilterDialog::clear()
{
    for (QCheckBox *check : formatChecks_)
        check->setChecked(true);
    minEdit_->clear();
    maxEdit_->clear();
    hideBroken_->setChecked(false);
    updatePreview();
}
