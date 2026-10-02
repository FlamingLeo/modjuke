#include "trackerdata.h"

namespace {

PatternDataReply::Cell formatCell(const OpenMPTModule &module, int pattern, int row, int channel)
{
    using CT = OpenMPTModule::CommandType;
    auto take = [&](CT type, const char *fallback) {
        QString value = module.formatCommand(pattern, row, channel, type).trimmed();
        if (value.isEmpty() || value.startsWith(QLatin1Char('.')))
            return QString::fromLatin1(fallback);
        return value;
    };
    PatternDataReply::Cell cell;
    cell.note = take(CT::Note, "...");
    cell.instrument = take(CT::Instrument, "");
    const QString volLetter = take(CT::VolColEffect, "");
    const QString volValue = take(CT::Volume, "");
    if (!volValue.isEmpty() || !volLetter.isEmpty())
        cell.volume = (volLetter.isEmpty() ? QStringLiteral("v") : volLetter)
                      + (volValue.isEmpty() ? QStringLiteral("..") : volValue);
    const QString effLetter = take(CT::Effect, "");
    const QString effParam = take(CT::Parameter, "");
    if (!effLetter.isEmpty())
        cell.effect = effLetter + (effParam.isEmpty() ? QStringLiteral("00") : effParam);
    return cell;
}

}  // namespace

MetadataWorker::MetadataWorker(const QAtomicInt *currentToken, const QAtomicInteger<quint64> *currentLoadEpoch)
    : currentToken_(currentToken), currentLoadEpoch_(currentLoadEpoch)
{
}

bool MetadataWorker::prepare(int token, const QString &path, int subsong, quint64 loadEpoch)
{
    if (!current(token)) return false;
    if (path_ != path || loadEpoch_ != loadEpoch || !module_.isOpen()) {
        path_ = path;
        loadEpoch_ = loadEpoch;
        module_.close();
        if (auto *lib = OpenMPTLib::instance())
            module_ = lib->openFile(path_, {{"load.skip_samples", "1"}}, nullptr);
    }
    if (module_.isOpen() && !module_.selectSubsong(subsong)) module_.close();
    return current(token) && loadEpoch == currentLoadEpoch_->loadAcquire();
}

void MetadataWorker::requestSong(int token, const QString &path, int subsong, quint64 generation, quint64 loadEpoch)
{
    if (!prepare(token, path, subsong, loadEpoch)) return;
    SongDataReply reply;
    reply.token = token;
    reply.path = path_;
    reply.generation = generation;
    reply.subsong = subsong;
    if (module_.isOpen()) {
        const int orders = module_.numOrders();
        for (int i = 0; i < orders; ++i) {
            const int pattern = module_.orderPattern(i);
            reply.orders << pattern;
            if (pattern >= 0 && !reply.rows.contains(pattern))
                reply.rows.insert(pattern, module_.patternRows(pattern));
        }
        reply.channels = module_.numChannels();
        reply.numPatterns = module_.numPatterns();
    }
    emit songReady(reply);
}

void MetadataWorker::requestPattern(int token, const QString &path, int subsong, quint64 loadEpoch,
                                    int pattern, int channelLimit)
{
    if (!prepare(token, path, subsong, loadEpoch)) return;
    PatternDataReply reply;
    reply.token = token;
    reply.pattern = pattern;
    if (!module_.isOpen()) {
        reply.error = QStringLiteral("no module");
    } else {
        reply.rows = module_.patternRows(pattern);
        reply.channels = channelLimit > 0 ? channelLimit : module_.numChannels();
        reply.cells.resize(reply.rows);
        for (int row = 0; row < reply.rows; ++row) {
            if (!current(token)) return;
            auto &cells = reply.cells[row];
            cells.resize(reply.channels);
            for (int ch = 0; ch < reply.channels; ++ch)
                cells[ch] = formatCell(module_, pattern, row, ch);
        }
    }
    emit patternReady(reply, loadEpoch);
}
