#include "openmptapi.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>

#include <algorithm>
#include <cmath>
#include <type_traits>

// ---------------------------------------------------------------------------
// C API prototypes + resolved pointers
// ---------------------------------------------------------------------------

struct OpenMPTLib::Api {
    uint32_t (*get_library_version)(void) = nullptr;
    int (*is_extension_supported)(const char *) = nullptr;
    char *(*get_supported_extensions)(void) = nullptr;
    void (*free_string)(void *) = nullptr;

    void *(*module_create_from_memory2)(const void *, size_t, void (*)(const char *, void *),
                                        void *, void *, void *, int *, char **,
                                        const void *) = nullptr;
    void (*module_destroy)(void *) = nullptr;

    size_t (*read_interleaved_float_stereo)(void *, uint32_t, size_t, float *) = nullptr;
    double (*get_duration_seconds)(void *) = nullptr;
    double (*get_position_seconds)(void *) = nullptr;
    double (*set_position_seconds)(void *, double) = nullptr;
    double (*set_position_order_row)(void *, int32_t, int32_t) = nullptr;
    int32_t (*get_current_order)(void *) = nullptr;
    int32_t (*get_current_pattern)(void *) = nullptr;
    int32_t (*get_current_row)(void *) = nullptr;
    int32_t (*get_current_speed)(void *) = nullptr;
    int32_t (*get_current_tempo)(void *) = nullptr;
    int32_t (*get_current_playing_channels)(void *) = nullptr;
    float (*get_channel_vu_mono)(void *, int32_t) = nullptr;
    int (*select_subsong)(void *, int32_t) = nullptr;

    int32_t (*get_num_channels)(void *) = nullptr;
    int32_t (*get_num_orders)(void *) = nullptr;
    int32_t (*get_num_patterns)(void *) = nullptr;
    int32_t (*get_num_instruments)(void *) = nullptr;
    int32_t (*get_num_samples)(void *) = nullptr;
    int32_t (*get_num_subsongs)(void *) = nullptr;

    char *(*get_metadata_keys)(void *) = nullptr;
    char *(*get_metadata)(void *, const char *) = nullptr;
    char *(*get_instrument_name)(void *, int32_t) = nullptr;
    char *(*get_sample_name)(void *, int32_t) = nullptr;
    char *(*get_subsong_name)(void *, int32_t) = nullptr;

    int32_t (*get_order_pattern)(void *, int32_t) = nullptr;
    int32_t (*get_pattern_num_rows)(void *, int32_t) = nullptr;
    char *(*format_pattern_row_channel_command)(void *, int32_t, int32_t, int32_t, int) = nullptr;

    int (*set_repeat_count)(void *, int32_t) = nullptr;
    int (*ctl_set_text)(void *, const char *, const char *) = nullptr;
    int (*ctl_set_floatingpoint)(void *, const char *, double) = nullptr;
    int (*set_render_param)(void *, uint32_t, int32_t) = nullptr;
};

namespace {

// libopenmpt_module_initial_ctl (the C struct the create ctls argument wants)
struct InitialCtl {
    const char *ctl;
    const char *value;
};

// RENDER_INTERPOLATIONFILTER_LENGTH (openmpt_render_param enum)
constexpr uint32_t kRenderInterpolationFilterLength = 3;

const char *const kKnownExtensions[] = {
    "mod", "s3m", "xm", "it", "mptm", "stm", "nst", "m15", "ulm", "mtm", "itp",
    "669", "psm", "psm16", "amf", "ams", "dsym", "dmf", "dsm", "far", "imf",
    "j2b", "mms", "okt", "plm", "umx", "wow", "xmf", "c67", "cba", "digi",
    "dtm", "gdm", "ice", "mo3", "mpm", "mt2", "mus", "pat", "pt36", "ptm",
    "puma", "sfx", "sfx2", "st26", "stp", "ult", "uax", "liq", "dbm", "hmn",
    "jbm", "s3mod", "it1603", "xm104", "mdz", nullptr
};

// Loader warnings would go to stderr through libopenmpt's default log
// function; errors still come back through the create call's message.
void silentLog(const char *, void *) {}

// Real modules are a few MB (rarely a few hundred with huge samples); a
// misnamed multi-GB file must not be read into memory whole.
constexpr qint64 kMaxModuleBytes = qint64(1) << 30;

}  // namespace

// ---------------------------------------------------------------------------
// OpenMPTLib
// ---------------------------------------------------------------------------

OpenMPTLib::OpenMPTLib()
{
    // Resolve through QLibrary: explicit env override first, then a copy
    // bundled with the program (AppImage, Windows zip, macOS app), then the
    // system's library.
    const QByteArray env = qgetenv("MODJUKE_LIBOPENMPT");
    QStringList candidates;
    if (!env.isEmpty())
        candidates << QString::fromLocal8Bit(env);
    const QString appDir = QCoreApplication::applicationDirPath();
#if defined(Q_OS_WIN)
    if (!appDir.isEmpty())
        candidates << appDir + QStringLiteral("/libopenmpt.dll");
    candidates << QStringLiteral("libopenmpt") << QStringLiteral("openmpt");
#elif defined(Q_OS_MACOS)
    if (!appDir.isEmpty())
        candidates << appDir + QStringLiteral("/../Frameworks/libopenmpt.0.dylib")
                   << appDir + QStringLiteral("/libopenmpt.0.dylib");
    candidates << QStringLiteral("/opt/homebrew/lib/libopenmpt.0.dylib")   // Homebrew, Apple silicon
               << QStringLiteral("/usr/local/lib/libopenmpt.0.dylib")      // Homebrew, Intel
               << QStringLiteral("libopenmpt.0.dylib") << QStringLiteral("openmpt");
#else
    if (!appDir.isEmpty())
        candidates << appDir + QStringLiteral("/../lib/libopenmpt.so.0");
    // Runtime packages ship the versioned SONAME, not the development symlink.
    candidates << QStringLiteral("libopenmpt.so.0") << QStringLiteral("openmpt");
#endif
    for (const QString &cand : candidates) {
        library_.setFileName(cand);
        if (library_.load())
            break;
    }
    if (!library_.isLoaded())
        return;

    auto *api = new Api;
    // a missing symbol stays null (older libraries lack the ctl_set_* family)
    auto bind = [this](auto &fn, const char *symbol) {
        fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(library_.resolve(symbol));
    };
    bind(api->get_library_version, "openmpt_get_library_version");
    bind(api->is_extension_supported, "openmpt_is_extension_supported");
    bind(api->get_supported_extensions, "openmpt_get_supported_extensions");
    bind(api->free_string, "openmpt_free_string");
    bind(api->module_create_from_memory2, "openmpt_module_create_from_memory2");
    bind(api->module_destroy, "openmpt_module_destroy");
    bind(api->read_interleaved_float_stereo, "openmpt_module_read_interleaved_float_stereo");
    bind(api->get_duration_seconds, "openmpt_module_get_duration_seconds");
    bind(api->get_position_seconds, "openmpt_module_get_position_seconds");
    bind(api->set_position_seconds, "openmpt_module_set_position_seconds");
    bind(api->set_position_order_row, "openmpt_module_set_position_order_row");
    bind(api->get_current_order, "openmpt_module_get_current_order");
    bind(api->get_current_pattern, "openmpt_module_get_current_pattern");
    bind(api->get_current_row, "openmpt_module_get_current_row");
    bind(api->get_current_speed, "openmpt_module_get_current_speed");
    bind(api->get_current_tempo, "openmpt_module_get_current_tempo");
    bind(api->get_current_playing_channels, "openmpt_module_get_current_playing_channels");
    bind(api->get_channel_vu_mono, "openmpt_module_get_current_channel_vu_mono");
    bind(api->select_subsong, "openmpt_module_select_subsong");
    bind(api->get_num_channels, "openmpt_module_get_num_channels");
    bind(api->get_num_orders, "openmpt_module_get_num_orders");
    bind(api->get_num_patterns, "openmpt_module_get_num_patterns");
    bind(api->get_num_instruments, "openmpt_module_get_num_instruments");
    bind(api->get_num_samples, "openmpt_module_get_num_samples");
    bind(api->get_num_subsongs, "openmpt_module_get_num_subsongs");
    bind(api->get_metadata_keys, "openmpt_module_get_metadata_keys");
    bind(api->get_metadata, "openmpt_module_get_metadata");
    bind(api->get_instrument_name, "openmpt_module_get_instrument_name");
    bind(api->get_sample_name, "openmpt_module_get_sample_name");
    bind(api->get_subsong_name, "openmpt_module_get_subsong_name");
    bind(api->get_order_pattern, "openmpt_module_get_order_pattern");
    bind(api->get_pattern_num_rows, "openmpt_module_get_pattern_num_rows");
    bind(api->format_pattern_row_channel_command, "openmpt_module_format_pattern_row_channel_command");
    bind(api->set_repeat_count, "openmpt_module_set_repeat_count");
    bind(api->ctl_set_text, "openmpt_module_ctl_set_text");
    bind(api->ctl_set_floatingpoint, "openmpt_module_ctl_set_floatingpoint");
    bind(api->set_render_param, "openmpt_module_set_render_param");

    api_ = api;

    // libopenmpt encodes major << 24 | minor << 16 | patch (no pre-release byte)
    const uint32_t v = api->get_library_version ? api->get_library_version() : 0;
    versionString_ = QStringLiteral("%1.%2.%3")
                         .arg(int((v >> 24) & 0xFF)).arg(int((v >> 16) & 0xFF)).arg(int(v & 0xFFFF));

    // The library's own list covers every format it plays (med, mdl, stx,
    // symmod, packed containers, ...); the fixed table is only a fallback.
    if (api->get_supported_extensions && api->free_string) {
        if (char *list = api->get_supported_extensions()) {
            const QString all = QString::fromUtf8(list);
            api->free_string(list);
            for (const QString &ext : all.split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
                const QString lower = ext.trimmed().toLower();
                if (!lower.isEmpty() && !extensions_.contains(lower))
                    extensions_ << lower;
            }
        }
    }
    if (extensions_.isEmpty()) {
        for (const char *const *ext = kKnownExtensions; *ext; ++ext) {
            if (extensionSupported(QByteArray(*ext)))
                extensions_ << QString::fromLatin1(*ext);
        }
    }
}

OpenMPTLib::~OpenMPTLib()
{
    delete api_;
}

OpenMPTLib *OpenMPTLib::instance(QString *errorOut)
{
    static QMutex mutex;
    QMutexLocker lock(&mutex);
    static OpenMPTLib *singleton = nullptr;
    if (singleton)
        return singleton;
    auto *candidate = new OpenMPTLib;
    if (!candidate->library_.isLoaded() || !candidate->api_
        || !candidate->api_->module_create_from_memory2) {
        // (no #if inside the QStringLiteral macro: MSVC rejects that)
#if defined(Q_OS_WIN)
        const char *hint = "  Put libopenmpt.dll (and its openmpt-*.dll files) next to modjuke.exe.\n";
#elif defined(Q_OS_MACOS)
        const char *hint = "  macOS:  brew install libopenmpt\n";
#else
        const char *hint = "  Linux:  sudo apt install libopenmpt0t64   (older releases: libopenmpt0)\n";
#endif
        if (errorOut)
            *errorOut = QStringLiteral("libopenmpt could not be found or is too old.\n")
                        + QLatin1String(hint)
                        + QStringLiteral("You can also set MODJUKE_LIBOPENMPT to the library's full path");
        delete candidate;
        return nullptr;
    }
    singleton = candidate;
    return singleton;
}

bool OpenMPTLib::extensionSupported(const QByteArray &extLower) const
{
    if (!api_ || !api_->is_extension_supported)
        return false;
    QByteArray name = extLower;
    while (name.startsWith('.'))
        name.remove(0, 1);
    return api_->is_extension_supported(name.constData()) != 0;
}

QStringList OpenMPTLib::supportedExtensions() const { return extensions_; }

// take ownership of a returned string: decode + free
static QString takeString(const OpenMPTLib::Api &api, char *ptr)
{
    if (!ptr)
        return QString();
    QString text = QString::fromUtf8(ptr);
    if (api.free_string)
        api.free_string(ptr);
    return text;
}

OpenMPTModule OpenMPTLib::openMemory(const QByteArray &data,
                                     const QVector<QPair<QByteArray, QByteArray>> &ctls,
                                     QString *errorOut) const
{
    if (!api_ || data.isEmpty())
        return OpenMPTModule();
    QVector<InitialCtl> ctlArray;
    if (!ctls.isEmpty()) {
        for (const auto &pair : ctls)
            ctlArray.append(InitialCtl{pair.first.constData(), pair.second.constData()});
        ctlArray.append(InitialCtl{nullptr, nullptr});
    }
    int error = 0;
    char *message = nullptr;
    void *handle = api_->module_create_from_memory2(
        data.constData(), size_t(data.size()),
        silentLog, nullptr, nullptr, nullptr,
        &error, &message,
        ctlArray.isEmpty() ? nullptr : reinterpret_cast<const void *>(ctlArray.constData()));
    QString errorMessage = message ? takeString(*api_, message) : QString();
    if (!handle) {
        if (errorOut)
            *errorOut = errorMessage.isEmpty()
                            ? QStringLiteral("libopenmpt failed to open the module (error %1)").arg(error)
                            : errorMessage;
        return OpenMPTModule();
    }
    return OpenMPTModule(this, handle);
}

OpenMPTModule OpenMPTLib::openFile(const QString &path,
                                   const QVector<QPair<QByteArray, QByteArray>> &ctls,
                                   QString *errorOut) const
{
    // A FIFO or device would block or never end; a huge file would be read
    // into memory whole.
    const QFileInfo fi(path);
    if (fi.exists() && !fi.isFile()) {
        if (errorOut)
            *errorOut = QStringLiteral("Not a regular file");
        return OpenMPTModule();
    }
    if (fi.size() > kMaxModuleBytes) {
        if (errorOut)
            *errorOut = QStringLiteral("File too large (%1 MB)").arg(fi.size() >> 20);
        return OpenMPTModule();
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (errorOut)
            *errorOut = QStringLiteral("Cannot read file: %1").arg(file.errorString());
        return OpenMPTModule();
    }
    const QByteArray data = file.readAll();
    return openMemory(data, ctls, errorOut);
}

QStringList libraryExtensions()
{
    if (const OpenMPTLib *lib = OpenMPTLib::instance()) {
        const QStringList exts = lib->supportedExtensions();
        if (!exts.isEmpty())
            return exts;
    }
    return {QStringLiteral("669"), QStringLiteral("amf"), QStringLiteral("ams"),
            QStringLiteral("dbm"), QStringLiteral("dmf"), QStringLiteral("dsm"),
            QStringLiteral("dtm"), QStringLiteral("far"), QStringLiteral("gdm"),
            QStringLiteral("it"), QStringLiteral("j2b"), QStringLiteral("med"),
            QStringLiteral("mdl"), QStringLiteral("mod"), QStringLiteral("mptm"),
            QStringLiteral("mt2"), QStringLiteral("mtm"), QStringLiteral("noiser"),
            QStringLiteral("okta"), QStringLiteral("pt3"), QStringLiteral("s3m"),
            QStringLiteral("sfx"), QStringLiteral("stm"), QStringLiteral("stx"),
            QStringLiteral("ult"), QStringLiteral("wow"), QStringLiteral("xm")};
}

// ---------------------------------------------------------------------------
// OpenMPTModule
// ---------------------------------------------------------------------------

OpenMPTModule::~OpenMPTModule() { close(); }

OpenMPTModule::OpenMPTModule(OpenMPTModule &&other) noexcept
    : lib_(other.lib_), handle_(other.handle_)
{
    other.handle_ = nullptr;
    other.lib_ = nullptr;
}

OpenMPTModule &OpenMPTModule::operator=(OpenMPTModule &&other) noexcept
{
    if (this != &other) {
        close();
        lib_ = other.lib_;
        handle_ = other.handle_;
        other.handle_ = nullptr;
        other.lib_ = nullptr;
    }
    return *this;
}

void OpenMPTModule::close()
{
    if (handle_ && lib_->api_->module_destroy)
        lib_->api_->module_destroy(handle_);
    handle_ = nullptr;
}

// A handle only comes from OpenMPTLib::openMemory, so handle_ implies a
// library with a resolved API.
template<auto Fn, class R, class... A>
R OpenMPTModule::call(R fallback, A... args) const
{
    if (!handle_)
        return fallback;
    const auto fn = lib_->api_->*Fn;
    return fn ? R(fn(handle_, args...)) : fallback;
}

template<auto Fn, class... A>
QString OpenMPTModule::text(A... args) const
{
    char *ptr = call<Fn>(static_cast<char *>(nullptr), args...);
    return ptr ? takeString(*lib_->api_, ptr) : QString();
}

using Api = OpenMPTLib::Api;

size_t OpenMPTModule::readFloatStereo(int sampleRate, size_t frames, float *out) const
{
    return call<&Api::read_interleaved_float_stereo>(size_t(0), uint32_t(sampleRate), frames, out);
}

double OpenMPTModule::durationSeconds() const
{
    const double d = call<&Api::get_duration_seconds>(0.0);
    return std::isfinite(d) ? d : 1e18;    // >= 1 day = endless
}

double OpenMPTModule::positionSeconds() const { return call<&Api::get_position_seconds>(0.0); }

double OpenMPTModule::seekSeconds(double seconds) const
{
    return call<&Api::set_position_seconds>(0.0, std::max(0.0, seconds));
}

double OpenMPTModule::seekOrderRow(int order, int row) const
{
    return call<&Api::set_position_order_row>(0.0, int32_t(order), int32_t(row));
}

int OpenMPTModule::currentOrder() const { return call<&Api::get_current_order>(0); }
int OpenMPTModule::currentPattern() const { return call<&Api::get_current_pattern>(0); }
int OpenMPTModule::currentRow() const { return call<&Api::get_current_row>(0); }
int OpenMPTModule::currentSpeed() const { return call<&Api::get_current_speed>(0); }
int OpenMPTModule::currentTempo() const { return call<&Api::get_current_tempo>(0); }
int OpenMPTModule::playingChannels() const { return call<&Api::get_current_playing_channels>(0); }

float OpenMPTModule::channelVu(int channel) const
{
    const float v = call<&Api::get_channel_vu_mono>(0.0f, int32_t(channel));
    return std::isfinite(v) ? std::clamp(v, 0.0f, 1.0f) : 0.0f;
}

bool OpenMPTModule::selectSubsong(int index) const
{
    return call<&Api::select_subsong>(0, int32_t(index)) != 0;
}

int OpenMPTModule::numChannels() const { return call<&Api::get_num_channels>(0); }
int OpenMPTModule::numOrders() const { return call<&Api::get_num_orders>(0); }
int OpenMPTModule::numPatterns() const { return call<&Api::get_num_patterns>(0); }
int OpenMPTModule::numInstruments() const { return call<&Api::get_num_instruments>(0); }
int OpenMPTModule::numSamples() const { return call<&Api::get_num_samples>(0); }
int OpenMPTModule::numSubsongs() const { return call<&Api::get_num_subsongs>(1); }

int OpenMPTModule::orderPattern(int order) const
{
    // "+++" (skip) and "---" (end) order entries come back as 0xFFFE/0xFFFF
    const int pattern = call<&Api::get_order_pattern>(-1, int32_t(order));
    return pattern >= 0xFFFE ? -1 : pattern;
}

int OpenMPTModule::patternRows(int pattern) const
{
    return call<&Api::get_pattern_num_rows>(0, int32_t(pattern));
}

bool OpenMPTModule::setRepeatCount(int count) const
{
    return call<&Api::set_repeat_count>(0, int32_t(count)) != 0;
}

bool OpenMPTModule::setCtlText(const char *ctl, const QString &value) const
{
    const QByteArray bytes = value.toUtf8();
    return call<&Api::ctl_set_text>(0, ctl, bytes.constData()) != 0;
}

bool OpenMPTModule::setInterpolationLength(int length) const
{
    return call<&Api::set_render_param>(0, kRenderInterpolationFilterLength, int32_t(length)) != 0;
}

bool OpenMPTModule::setTempoFactor(double factor) const
{
    return call<&Api::ctl_set_floatingpoint>(0, "play.tempo_factor", factor) != 0;
}

QString OpenMPTModule::metadata(const QString &key) const
{
    const QByteArray bytes = key.toUtf8();
    return text<&Api::get_metadata>(bytes.constData());
}

QString OpenMPTModule::instrumentName(int index) const
{
    return text<&Api::get_instrument_name>(int32_t(index));
}

QString OpenMPTModule::sampleName(int index) const
{
    return text<&Api::get_sample_name>(int32_t(index));
}

QStringList OpenMPTModule::subsongNameList() const
{
    if (!handle_ || !lib_->api_->get_subsong_name)
        return {};
    QStringList names;
    for (int i = 0; i < numSubsongs(); ++i)
        names << text<&Api::get_subsong_name>(int32_t(i));
    return names; // preserve empty slots: names must retain native indices
}

QString OpenMPTModule::formatCommand(int pattern, int row, int channel, CommandType type) const
{
    return text<&Api::format_pattern_row_channel_command>(int32_t(pattern), int32_t(row),
                                                          int32_t(channel), int(type));
}

ModuleInfo OpenMPTModule::info(const QString &path) const { return collect(path, true); }
ModuleInfo OpenMPTModule::summary(const QString &path) const { return collect(path, false); }

ModuleInfo OpenMPTModule::collect(const QString &path, bool names) const
{
    ModuleInfo out;
    if (!handle_)
        return out;
    out.ok = true;
    out.path = path;
    const QStringList keys = text<&Api::get_metadata_keys>().split(QLatin1Char(';'), Qt::SkipEmptyParts);
    auto md = [&](const char *key) { return keys.contains(QLatin1String(key)) ? metadata(QLatin1String(key)) : QString(); };
    out.title = md("title");
    if (out.title.isEmpty() && !path.isEmpty())
        out.title = QFileInfo(path).fileName();
    out.format = md("type");
    out.formatLong = md("type_long");
    out.tracker = md("tracker");
    out.artist = md("artist");
    out.duration = durationSeconds();
    out.channels = numChannels();
    out.orders = numOrders();
    out.patterns = numPatterns();
    out.instruments = numInstruments();
    out.samples = numSamples();
    out.subsongs = numSubsongs();
    if (!names)
        return out;
    out.message = md("message");
    if (out.message.isEmpty())
        out.message = md("message_raw");
    out.subsongNames = subsongNameList();
    for (int i = 0; i < out.samples; ++i)
        out.sampleNames << sampleName(i);
    for (int i = 0; i < out.instruments; ++i)
        out.instrumentNames << instrumentName(i);
    return out;
}
