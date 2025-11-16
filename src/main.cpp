#include <locale.h>

#include <QGuiApplication>
#include <QApplication>
#include <QFileInfo>
#include <QIcon>
#include <QtQml>
#include <QtWebEngine/qtwebengineglobal.h>
#include <QtWebEngineWidgets/QWebEngineProfile>
#include <QErrorMessage>
#include <QCommandLineOption>
#include <QDebug>
#include <QSettings>

#include "shared/Names.h"
#include "system/SystemComponent.h"
#include "Paths.h"
#include "player/CodecsComponent.h"
#include "player/PlayerComponent.h"
#include "player/OpenGLDetect.h"
#include "Version.h"
#include "settings/SettingsComponent.h"
#include "settings/SettingsSection.h"
#include "ui/KonvergoWindow.h"
#include "Globals.h"
#include "ui/ErrorMessage.h"
#include "UniqueApplication.h"
#include "utils/Log.h"

// GPU-stable additions
#include <QSurfaceFormat>
#include <QQuickWindow>
#include <QtWebEngine/QtWebEngine>

#ifdef Q_OS_MAC
#include "PFMoveApplication.h"
#endif

#if defined(Q_OS_MAC) || defined(Q_OS_LINUX) || defined(Q_OS_FREEBSD)
#include "SignalManager.h"
#endif

/////////////////////////////////////////////////////////////////////////////////////////
static void preinitQt()
{
  QCoreApplication::setApplicationName(Names::MainName());
  QCoreApplication::setApplicationVersion(Version::GetVersionString());
  QCoreApplication::setOrganizationDomain("jellyfin.org");

#ifdef Q_OS_WIN32
  // 1) Share GL contexts to keep WebEngine + QML compositor in sync
  QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);

  // 2) Respect user setting, default to ANGLE (OpenGLES) for D3D11 stability
  QVariant useOpengl = SettingsComponent::readPreinitValue(SETTINGS_SECTION_MAIN, "useOpenGL");
  if (useOpengl.type() != QMetaType::Bool)
    useOpengl = false;

  if (useOpengl.toBool())
    QCoreApplication::setAttribute(Qt::AA_UseDesktopOpenGL);
  else
    QCoreApplication::setAttribute(Qt::AA_UseOpenGLES);

  // 3) Lock a compatible default surface format (GLES 3.0, double-buffer, vsync)
  QSurfaceFormat fmt;
  fmt.setRenderableType(QSurfaceFormat::OpenGLES);
  fmt.setVersion(3, 0);
  fmt.setProfile(QSurfaceFormat::NoProfile);
  fmt.setSwapBehavior(QSurfaceFormat::DoubleBuffer);
  fmt.setSwapInterval(1);
  QSurfaceFormat::setDefaultFormat(fmt);

  // 4) Hint ANGLE/Chromium to use D3D11 and keep GPU features on
  qputenv("QT_ANGLE_PLATFORM", "d3d11");
  qputenv("QTWEBENGINE_CHROMIUM_FLAGS",
          "--use-angle=d3d11 --use-gl=angle --enable-gpu-rasterization --enable-zero-copy --ignore-gpu-blocklist");
  // If you still encounter rare timing issues, uncomment the next line (still GPU, non-threaded scenegraph):
  // qputenv("QSG_RENDER_LOOP", "basic");
#endif
}

/////////////////////////////////////////////////////////////////////////////////////////
char** appendCommandLineArguments(int argc, char **argv, const QStringList& args)
{
  size_t newSize = (argc + args.length() + 1) * sizeof(char*);
  char** newArgv = (char**)calloc(1, newSize);
  memcpy(newArgv, argv, (size_t)(argc * sizeof(char*)));

  int pos = argc;
  for (const QString& str : args)
    newArgv[pos++] = qstrdup(str.toUtf8().data());

  return newArgv;
}

/////////////////////////////////////////////////////////////////////////////////////////
void ShowLicenseInfo()
{
  QFile licenses(":/misc/licenses.txt");
  licenses.open(QIODevice::ReadOnly | QIODevice::Text);
  QByteArray contents = licenses.readAll();
  printf("%.*s\n", contents.size(), contents.data());
}

/////////////////////////////////////////////////////////////////////////////////////////
QStringList g_qtFlags = {
  "--enable-gpu-rasterization"
};

/////////////////////////////////////////////////////////////////////////////////////////
int main(int argc, char *argv[])
{
  try
  {
    QCommandLineParser parser;
    parser.setApplicationDescription("Jellyfin Media Player");
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOptions({{{"l", "licenses"},         "Show license information"},
                       {"desktop",                 "Start in desktop mode"},
                       {"tv",                      "Start in TV mode"},
                       {"windowed",                "Start in windowed mode"},
                       {"fullscreen",              "Start in fullscreen"},
                       {"disable-gpu",             "Disable QtWebEngine gpu accel"},
                       {"force-external-webclient","Use webclient provided by server"}});

    auto scaleOption = QCommandLineOption("scale-factor",
        "Set to an integer or 'auto' to control the scale (DPI) of the desktop interface.");
    scaleOption.setValueName("scale");
    scaleOption.setDefaultValue("auto");

    auto platformOption = QCommandLineOption("platform", "Equivalent to QT_QPA_PLATFORM.");
    platformOption.setValueName("platform");
    platformOption.setDefaultValue("default");

    auto devOption = QCommandLineOption("remote-debugging-port", "Port number for devtools.");
    devOption.setValueName("port");

    auto configDirOption = QCommandLineOption("config-dir", "Override config directory path.");
    configDirOption.setValueName("path");

    auto logLevelOption = QCommandLineOption("log-level", "Log level: debug, info, warn, error, fatal (default: error)");
    logLevelOption.setValueName("level");

    parser.addOption(scaleOption);
    parser.addOption(devOption);
    parser.addOption(platformOption);
    parser.addOption(configDirOption);
    parser.addOption(logLevelOption);

    char **newArgv = appendCommandLineArguments(argc, argv, g_qtFlags);
    int newArgc = argc + g_qtFlags.size();

#ifdef Q_OS_UNIX
    // Avoid locale side-effects in mpv/ffmpeg on *nix
    qputenv("LC_ALL", "C");
    qputenv("LC_NUMERIC", "C");
#endif

    // GPU path setup and attributes first
    preinitQt();
    detectOpenGLEarly();

    QStringList arguments;
    for (int i = 0; i < argc; i++)
      arguments << QString::fromLatin1(argv[i]);

    // Parse CLI using a tiny QCoreApplication (scale flags need to be set before GUI)
    {
      QCoreApplication core(newArgc, newArgv);
      parser.process(arguments);
    }

    if (parser.isSet("licenses"))
    {
      ShowLicenseInfo();
      return EXIT_SUCCESS;
    }

    QString logLevel = parser.value("log-level");
    if (parser.isSet("log-level") && (logLevel.isEmpty() || Log::ParseLogLevel(logLevel) == -1))
    {
      fprintf(stderr, "Error: invalid log level '%s'. Valid levels: debug, info, warn, error, fatal\n", qPrintable(logLevel));
      return EXIT_FAILURE;
    }
    if (parser.isSet("log-level"))
      Log::SetLogLevel(logLevel);

    Log::Init();

    auto scale = parser.value("scale-factor");
    if (scale.isEmpty() || scale == "auto")
      QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    else if (scale != "none")
      qputenv("QT_SCALE_FACTOR", scale.toUtf8());

    auto platform = parser.value("platform");
    if (!(platform.isEmpty() || platform == "default"))
      qputenv("QT_QPA_PLATFORM", platform.toUtf8());

    // Optional runtime toggle for testing
    if (parser.isSet("disable-gpu")) {
      qputenv("QTWEBENGINE_CHROMIUM_FLAGS", "--disable-gpu --disable-gpu-compositing --ignore-gpu-blocklist");
    }

    // CRITICAL: Initialize QtWebEngine BEFORE QApplication to avoid deprecation warnings
    QtWebEngine::initialize();

    // Real GUI app
    QApplication app(newArgc, newArgv);

#if defined(Q_OS_WIN)
    app.setWindowIcon(QIcon(":/images/icon.png"));
#endif
#if defined(Q_OS_LINUX) || defined(Q_OS_FREEBSD)
    app.setWindowIcon(QIcon::fromTheme("com.github.iwalton3.jellyfin-media-player", QIcon(":/images/icon.png")));
    app.setDesktopFileName("com.github.iwalton3.jellyfin-media-player");
#endif
#if defined(Q_OS_MAC) && defined(NDEBUG)
    PFMoveToApplicationsFolderIfNecessary();
#endif

    UniqueApplication* uniqueApp = new UniqueApplication();
    if (!uniqueApp->ensureUnique())
    {
      Log::Cleanup();
      return EXIT_SUCCESS;
    }

    Log::RotateLog();
    qInfo() << "Config directory:" << qPrintable(Paths::dataDir());

#if defined(Q_OS_UNIX)
    SignalManager signalManager(&app);
    Q_UNUSED(signalManager);
#endif

    detectOpenGLLate();
    Codecs::preinitCodecs();

    // Initialize core components early
    ComponentManager::Get().initialize();
    Log::ApplyConfigLogLevel();
    SettingsComponent::Get().setCommandLineValues(parser.optionNames());

    // Configure QtWebEngine paths
    QString configDir = parser.value("config-dir");
    QString webEngineDataDir;
    if (!configDir.isEmpty()) {
      QFileInfo fi(configDir);
      QString absPath = fi.absoluteFilePath();
      QDir parentDir = fi.dir();
      if (!parentDir.exists())
        qFatal("Config directory parent does not exist: %s", qPrintable(parentDir.absolutePath()));
      Paths::setConfigDir(absPath);
      QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, absPath);
      webEngineDataDir = absPath + "/QtWebEngine";
    } else {
      QDir d(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation));
      d.mkpath(d.absolutePath() + "/" + Names::MainName());
      d.cd(Names::MainName());
      webEngineDataDir = d.absolutePath() + "/QtWebEngine";
    }

    QWebEngineProfile* defaultProfile = QWebEngineProfile::defaultProfile();
    defaultProfile->setCachePath(webEngineDataDir);
    defaultProfile->setPersistentStoragePath(webEngineDataDir);

    // QML engine + UI bootstrap
    QQmlApplicationEngine *engine = Globals::Engine();
    KonvergoWindow::RegisterClass();
    Globals::SetContextProperty("components", &ComponentManager::Get().getQmlPropertyMap());

    QObject::connect(engine, &QQmlApplicationEngine::objectCreated, [=](QObject* object, const QUrl& url)
    {
      Q_UNUSED(url);
      if (object == nullptr)
        throw FatalException(QObject::tr("Failed to parse application engine script."));

      KonvergoWindow* window = Globals::MainWindow();
      QObject* webChannel = qvariant_cast<QObject*>(window->property("webChannel"));
      Q_ASSERT(webChannel);
      ComponentManager::Get().setWebChannel(qobject_cast<QWebChannel*>(webChannel));
      QObject::connect(uniqueApp, &UniqueApplication::otherApplicationStarted, window, &KonvergoWindow::otherAppFocus);
    });

    engine->load(QUrl(QStringLiteral("qrc:/ui/webview.qml")));

    int ret = app.exec();

    delete uniqueApp;
    Globals::EngineDestroy();
    Codecs::Uninit();
    return ret;
  }
  catch (FatalException& e)
  {
    qFatal("Unhandled FatalException: %s", qPrintable(e.message()));
    QApplication errApp(argc, argv);
    auto msg = new ErrorMessage(e.message(), true);
    msg->show();
    errApp.exec();
    Codecs::Uninit();
    return 1;
  }
}
