#include "openmptapi.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>

#include <cmath>
#include <cstring>
#include <algorithm>

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
    char *(*highlight_pattern_row_channel_command)(void *, int32_t, int32_t, int32_t, int) = nullptr;

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

QString g_loadError;

// Loader warnings would go to stderr through libopenmpt's default log
// function; errors still come back through the create call's message.
void silentLog(const char *, void *) {}

// Accessors of a closed or moved-from module see an API without functions.
const OpenMPTLib::Api kNoApi{};

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
    auto &L = library_;
    api->get_library_version = reinterpret_cast<decltype(api->get_library_version)>(
        L.resolve("openmpt_get_library_version"));
    api->is_extension_supported = reinterpret_cast<decltype(api->is_extension_supported)>(
        L.resolve("openmpt_is_extension_supported"));
    api->get_supported_extensions = reinterpret_cast<decltype(api->get_supported_extensions)>(
        L.resolve("openmpt_get_supported_extensions"));
    api->free_string = reinterpret_cast<decltype(api->free_string)>(
        L.resolve("openmpt_free_string"));
    api->module_create_from_memory2 = reinterpret_cast<decltype(api->module_create_from_memory2)>(
        L.resolve("openmpt_module_create_from_memory2"));
    api->module_destroy = reinterpret_cast<decltype(api->module_destroy)>(
        L.resolve("openmpt_module_destroy"));
    api->read_interleaved_float_stereo = reinterpret_cast<decltype(api->read_interleaved_float_stereo)>(
        L.resolve("openmpt_module_read_interleaved_float_stereo"));
    api->get_duration_seconds = reinterpret_cast<decltype(api->get_duration_seconds)>(
        L.resolve("openmpt_module_get_duration_seconds"));
    api->get_position_seconds = reinterpret_cast<decltype(api->get_position_seconds)>(
        L.resolve("openmpt_module_get_position_seconds"));
    api->set_position_seconds = reinterpret_cast<decltype(api->set_position_seconds)>(
        L.resolve("openmpt_module_set_position_seconds"));
    api->set_position_order_row = reinterpret_cast<decltype(api->set_position_order_row)>(
        L.resolve("openmpt_module_set_position_order_row"));
    api->get_current_order = reinterpret_cast<decltype(api->get_current_order)>(
        L.resolve("openmpt_module_get_current_order"));
    api->get_current_pattern = reinterpret_cast<decltype(api->get_current_pattern)>(
        L.resolve("openmpt_module_get_current_pattern"));
    api->get_current_row = reinterpret_cast<decltype(api->get_current_row)>(
        L.resolve("openmpt_module_get_current_row"));
    api->get_current_speed = reinterpret_cast<decltype(api->get_current_speed)>(
        L.resolve("openmpt_module_get_current_speed"));
    api->get_current_tempo = reinterpret_cast<decltype(api->get_current_tempo)>(
        L.resolve("openmpt_module_get_current_tempo"));
    api->get_current_playing_channels = reinterpret_cast<decltype(api->get_current_playing_channels)>(
        L.resolve("openmpt_module_get_current_playing_channels"));
    api->get_channel_vu_mono = reinterpret_cast<decltype(api->get_channel_vu_mono)>(
        L.resolve("openmpt_module_get_current_channel_vu_mono"));
    api->select_subsong = reinterpret_cast<decltype(api->select_subsong)>(
        L.resolve("openmpt_module_select_subsong"));
    api->get_num_channels = reinterpret_cast<decltype(api->get_num_channels)>(
        L.resolve("openmpt_module_get_num_channels"));
    api->get_num_orders = reinterpret_cast<decltype(api->get_num_orders)>(
        L.resolve("openmpt_module_get_num_orders"));
    api->get_num_patterns = reinterpret_cast<decltype(api->get_num_patterns)>(
        L.resolve("openmpt_module_get_num_patterns"));
    api->get_num_instruments = reinterpret_cast<decltype(api->get_num_instruments)>(
        L.resolve("openmpt_module_get_num_instruments"));
    api->get_num_samples = reinterpret_cast<decltype(api->get_num_samples)>(
        L.resolve("openmpt_module_get_num_samples"));
    api->get_num_subsongs = reinterpret_cast<decltype(api->get_num_subsongs)>(
        L.resolve("openmpt_module_get_num_subsongs"));
    api->get_metadata_keys = reinterpret_cast<decltype(api->get_metadata_keys)>(
        L.resolve("openmpt_module_get_metadata_keys"));
    api->get_metadata = reinterpret_cast<decltype(api->get_metadata)>(
        L.resolve("openmpt_module_get_metadata"));
    api->get_instrument_name = reinterpret_cast<decltype(api->get_instrument_name)>(
        L.resolve("openmpt_module_get_instrument_name"));
    api->get_sample_name = reinterpret_cast<decltype(api->get_sample_name)>(
        L.resolve("openmpt_module_get_sample_name"));
    api->get_subsong_name = reinterpret_cast<decltype(api->get_subsong_name)>(
        L.resolve("openmpt_module_get_subsong_name"));
    api->get_order_pattern = reinterpret_cast<decltype(api->get_order_pattern)>(
        L.resolve("openmpt_module_get_order_pattern"));
    api->get_pattern_num_rows = reinterpret_cast<decltype(api->get_pattern_num_rows)>(
        L.resolve("openmpt_module_get_pattern_num_rows"));
    api->format_pattern_row_channel_command =
        reinterpret_cast<decltype(api->format_pattern_row_channel_command)>(
            L.resolve("openmpt_module_format_pattern_row_channel_command"));
    api->highlight_pattern_row_channel_command =
        reinterpret_cast<decltype(api->highlight_pattern_row_channel_command)>(
            L.resolve("openmpt_module_highlight_pattern_row_channel_command"));
    api->set_repeat_count = reinterpret_cast<decltype(api->set_repeat_count)>(
        L.resolve("openmpt_module_set_repeat_count"));
    api->ctl_set_text = reinterpret_cast<decltype(api->ctl_set_text)>(
        L.resolve("openmpt_module_ctl_set_text"));
    api->ctl_set_floatingpoint = reinterpret_cast<decltype(api->ctl_set_floatingpoint)>(
        L.resolve("openmpt_module_ctl_set_floatingpoint"));
    api->set_render_param = reinterpret_cast<decltype(api->set_render_param)>(
        L.resolve("openmpt_module_set_render_param"));

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
        g_loadError = QStringLiteral("libopenmpt could not be found or is too old.\n")
                      + QLatin1String(hint)
                      + QStringLiteral("You can also set MODJUKE_LIBOPENMPT to the library's full path");
        if (errorOut)
            *errorOut = g_loadError;
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

bool OpenMPTLib::analyzeFile(const QString &path, ModuleInfo *out, QString *errorOut)
{
    QString loadError;
    OpenMPTLib *lib = instance(&loadError);
    if (!lib) {
        if (errorOut)
            *errorOut = loadError;
        return false;
    }
    // The same load-ctl optimization the Python analyzer uses.
    const QVector<QPair<QByteArray, QByteArray>> ctls = {
        {QByteArrayLiteral("load.skip_samples"), QByteArrayLiteral("1")},
        {QByteArrayLiteral("load.skip_plugins"), QByteArrayLiteral("1")},
        {QByteArrayLiteral("load.skip_subsongs_init"), QByteArrayLiteral("1")},
    };
    OpenMPTModule module = lib->openFile(path, ctls, &loadError);
    if (!module.isOpen()) {
        if (errorOut)
            *errorOut = loadError;
        if (out)
            *out = ModuleInfo{};
        return false;
    }
    module.setAtEndStop();
    if (out)
        *out = module.info(path);
    return true;
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
    if (handle_ && lib_ && lib_->api_ && lib_->api_->module_destroy)
        lib_->api_->module_destroy(handle_);
    handle_ = nullptr;
}

#define MJ_REQUIRE(fn)                                                      \
    const OpenMPTLib::Api &api = (lib_ ? *lib_->api_ : kNoApi);                                           \
    if (!handle_ || !api.fn)                                                \
        return {};

size_t OpenMPTModule::readFloatStereo(int sampleRate, size_t frames, float *out) const
{
    const OpenMPTLib::Api &api = (lib_ ? *lib_->api_ : kNoApi);
    if (!handle_ || !api.read_interleaved_float_stereo)
        return 0;
    return api.read_interleaved_float_stereo(handle_, uint32_t(sampleRate), frames, out);
}

double OpenMPTModule::durationSeconds() const
{
    const OpenMPTLib::Api &api = (lib_ ? *lib_->api_ : kNoApi);
    if (!handle_ || !api.get_duration_seconds)
        return 0.0;
    const double d = api.get_duration_seconds(handle_);
    return std::isfinite(d) ? d : 1e18;    // >= 1 day = endless
}

double OpenMPTModule::positionSeconds() const
{
    const OpenMPTLib::Api &api = (lib_ ? *lib_->api_ : kNoApi);
    if (!handle_ || !api.get_position_seconds)
        return 0.0;
    return api.get_position_seconds(handle_);
}

double OpenMPTModule::seekSeconds(double seconds) const
{
    const OpenMPTLib::Api &api = (lib_ ? *lib_->api_ : kNoApi);
    if (!handle_ || !api.set_position_seconds)
        return 0.0;
    return api.set_position_seconds(handle_, std::max(0.0, seconds));
}

double OpenMPTModule::seekOrderRow(int order, int row) const
{
    const OpenMPTLib::Api &api = (lib_ ? *lib_->api_ : kNoApi);
    if (!handle_ || !api.set_position_order_row)
        return 0.0;
    return api.set_position_order_row(handle_, int32_t(order), int32_t(row));
}

int OpenMPTModule::currentOrder() const
{
    const OpenMPTLib::Api &api = (lib_ ? *lib_->api_ : kNoApi);
    return handle_ && api.get_current_order ? int(api.get_current_order(handle_)) : 0;
}

int OpenMPTModule::currentPattern() const
{
    const OpenMPTLib::Api &api = (lib_ ? *lib_->api_ : kNoApi);
    return handle_ && api.get_current_pattern ? int(api.get_current_pattern(handle_)) : 0;
}

int OpenMPTModule::currentRow() const
{
    const OpenMPTLib::Api &api = (lib_ ? *lib_->api_ : kNoApi);
    return handle_ && api.get_current_row ? int(api.get_current_row(handle_)) : 0;
}

int OpenMPTModule::currentSpeed() const
{
    const OpenMPTLib::Api &api = (lib_ ? *lib_->api_ : kNoApi);
    return handle_ && api.get_current_speed ? int(api.get_current_speed(handle_)) : 0;
}

int OpenMPTModule::currentTempo() const
{
    const OpenMPTLib::Api &api = (lib_ ? *lib_->api_ : kNoApi);
    return handle_ && api.get_current_tempo ? int(api.get_current_tempo(handle_)) : 0;
}

int OpenMPTModule::playingChannels() const
{
    const OpenMPTLib::Api &api = (lib_ ? *lib_->api_ : kNoApi);
    return handle_ && api.get_current_playing_channels ? int(api.get_current_playing_channels(handle_)) : 0;
}

float OpenMPTModule::channelVu(int channel) const
{
    const OpenMPTLib::Api &api = (lib_ ? *lib_->api_ : kNoApi);
    if (!handle_ || !api.get_channel_vu_mono)
        return 0.0f;
    const float v = api.get_channel_vu_mono(handle_, int32_t(channel));
    return std::isfinite(v) ? float(std::clamp(v, 0.0f, 1.0f)) : 0.0f;
}

bool OpenMPTModule::selectSubsong(int index) const
{
    const OpenMPTLib::Api &api = (lib_ ? *lib_->api_ : kNoApi);
    return handle_ && api.select_subsong && api.select_subsong(handle_, int32_t(index));
}

int OpenMPTModule::numChannels() const
{
    const OpenMPTLib::Api &api = (lib_ ? *lib_->api_ : kNoApi);
    return handle_ && api.get_num_channels ? int(api.get_num_channels(handle_)) : 0;
}
int OpenMPTModule::numOrders() const
{
    const OpenMPTLib::Api &api = (lib_ ? *lib_->api_ : kNoApi);
    return handle_ && api.get_num_orders ? int(api.get_num_orders(handle_)) : 0;
}
int OpenMPTModule::numPatterns() const
{
    const OpenMPTLib::Api &api = (lib_ ? *lib_->api_ : kNoApi);
    return handle_ && api.get_num_patterns ? int(api.get_num_patterns(handle_)) : 0;
}
int OpenMPTModule::numInstruments() const
{
    const OpenMPTLib::Api &api = (lib_ ? *lib_->api_ : kNoApi);
    return handle_ && api.get_num_instruments ? int(api.get_num_instruments(handle_)) : 0;
}
int OpenMPTModule::numSamples() const
{
    const OpenMPTLib::Api &api = (lib_ ? *lib_->api_ : kNoApi);
    return handle_ && api.get_num_samples ? int(api.get_num_samples(handle_)) : 0;
}
int OpenMPTModule::numSubsongs() const
{
    const OpenMPTLib::Api &api = (lib_ ? *lib_->api_ : kNoApi);
    return handle_ && api.get_num_subsongs ? int(api.get_num_subsongs(handle_)) : 1;
}

int OpenMPTModule::orderPattern(int order) const
{
    const OpenMPTLib::Api &api = (lib_ ? *lib_->api_ : kNoApi);
    if (!handle_ || !api.get_order_pattern)
        return -1;
    // "+++" (skip) and "---" (end) order entries come back as 0xFFFE/0xFFFF
    const int pattern = int(api.get_order_pattern(handle_, int32_t(order)));
    return pattern >= 0xFFFE ? -1 : pattern;
}

int OpenMPTModule::patternRows(int pattern) const
{
    const OpenMPTLib::Api &api = (lib_ ? *lib_->api_ : kNoApi);
    return handle_ && api.get_pattern_num_rows ? int(api.get_pattern_num_rows(handle_, int32_t(pattern))) : 0;
}

bool OpenMPTModule::setRepeatCount(int count) const
{
    if (!handle_ || !lib_) return false;
    const auto &api = (lib_ ? *lib_->api_ : kNoApi);
    return handle_ && api.set_repeat_count && api.set_repeat_count(handle_, count) != 0;
}

bool OpenMPTModule::setCtlText(const char *ctl, const QString &value) const
{
    const OpenMPTLib::Api &api = (lib_ ? *lib_->api_ : kNoApi);
    if (!handle_ || !api.ctl_set_text)
        return false;
    const QByteArray bytes = value.toUtf8();
    return api.ctl_set_text(handle_, ctl, bytes.constData()) != 0;
}

bool OpenMPTModule::setCtlDouble(const char *ctl, double value) const
{
    const OpenMPTLib::Api &api = (lib_ ? *lib_->api_ : kNoApi);
    return handle_ && api.ctl_set_floatingpoint
        && api.ctl_set_floatingpoint(handle_, ctl, value) != 0;
}

bool OpenMPTModule::setInterpolationLength(int length) const
{
    const OpenMPTLib::Api &api = (lib_ ? *lib_->api_ : kNoApi);
    if (!handle_ || !api.set_render_param)
        return false;
    return api.set_render_param(handle_, kRenderInterpolationFilterLength, int32_t(length)) != 0;
}

bool OpenMPTModule::setTempoFactor(double factor) const
{
    return setCtlDouble("play.tempo_factor", factor);
}

void OpenMPTModule::setAtEndStop() const
{
    setCtlText("play.at_end", QStringLiteral("stop"));
}

QStringList OpenMPTModule::metadataKeys() const
{
    const OpenMPTLib::Api &api = (lib_ ? *lib_->api_ : kNoApi);
    if (!handle_ || !api.get_metadata_keys)
        return {};
    const QString all = takeString(api, api.get_metadata_keys(handle_));
    QStringList keys = all.split(QLatin1Char(';'), Qt::SkipEmptyParts);
    return keys;
}

QString OpenMPTModule::metadata(const QString &key) const
{
    const OpenMPTLib::Api &api = (lib_ ? *lib_->api_ : kNoApi);
    if (!handle_ || !api.get_metadata)
        return QString();
    const QByteArray bytes = key.toUtf8();
    return takeString(api, api.get_metadata(handle_, bytes.constData()));
}

QString OpenMPTModule::instrumentName(int index) const
{
    const OpenMPTLib::Api &api = (lib_ ? *lib_->api_ : kNoApi);
    if (!handle_ || !api.get_instrument_name)
        return QString();
    return takeString(api, api.get_instrument_name(handle_, int32_t(index)));
}

QString OpenMPTModule::sampleName(int index) const
{
    const OpenMPTLib::Api &api = (lib_ ? *lib_->api_ : kNoApi);
    if (!handle_ || !api.get_sample_name)
        return QString();
    return takeString(api, api.get_sample_name(handle_, int32_t(index)));
}

QStringList OpenMPTModule::subsongNameList() const
{
    const OpenMPTLib::Api &api = (lib_ ? *lib_->api_ : kNoApi);
    if (!handle_ || !api.get_subsong_name)
        return {};
    QStringList names;
    for (int i = 0; i < numSubsongs(); ++i)
        names << takeString(api, api.get_subsong_name(handle_, i));
    return names; // preserve empty slots: names must retain native indices
}

QString OpenMPTModule::formatCommand(int pattern, int row, int channel, CommandType type) const
{
    const OpenMPTLib::Api &api = (lib_ ? *lib_->api_ : kNoApi);
    if (!handle_ || !api.format_pattern_row_channel_command)
        return QString();
    char *text = api.format_pattern_row_channel_command(
        handle_, int32_t(pattern), int32_t(row), int32_t(channel), int(type));
    return takeString(api, text);
}

QString OpenMPTModule::highlightRow(int pattern, int row, int channel, CommandType type) const
{
    const OpenMPTLib::Api &api = (lib_ ? *lib_->api_ : kNoApi);
    if (!handle_ || !api.highlight_pattern_row_channel_command)
        return QString();
    char *text = api.highlight_pattern_row_channel_command(
        handle_, int32_t(pattern), int32_t(row), int32_t(channel), int(type));
    return takeString(api, text);
}

ModuleInfo OpenMPTModule::info(const QString &path) const
{
    ModuleInfo out;
    if (!handle_)
        return out;
    out.ok = true;
    out.path = path;
    const QStringList keys = metadataKeys();
    auto md = [&](const char *key) { return keys.contains(QLatin1String(key)) ? metadata(QLatin1String(key)) : QString(); };
    out.title = md("title");
    if (out.title.isEmpty() && !path.isEmpty())
        out.title = QFileInfo(path).fileName();
    out.format = md("type");
    out.formatLong = md("type_long");
    out.tracker = md("tracker");
    out.artist = md("artist");
    out.message = md("message");
    if (out.message.isEmpty())
        out.message = md("message_raw");
    out.duration = durationSeconds();
    out.channels = numChannels();
    out.orders = numOrders();
    out.patterns = numPatterns();
    out.instruments = numInstruments();
    out.samples = numSamples();
    out.subsongs = numSubsongs();
    out.subsongNames = subsongNameList();
    for (int i = 0; i < out.samples; ++i)
        out.sampleNames << sampleName(i);
    for (int i = 0; i < out.instruments; ++i)
        out.instrumentNames << instrumentName(i);
    return out;
}
