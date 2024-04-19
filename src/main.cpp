/************************************************************************
**
**  Copyright (C) 2018-2024  Kevin B. Hendricks, Stratford Ontario Canada
**  Copyright (C) 2019-2024  Doug Massay
**  Copyright (C) 2009-2011  Strahinja Markovic  <strahinja.markovic@gmail.com>
**
**  This file is part of Sigil.
**
**  Sigil is free software: you can redistribute it and/or modify
**  it under the terms of the GNU General Public License as published by
**  the Free Software Foundation, either version 3 of the License, or
**  (at your option) any later version.
**
**  Sigil is distributed in the hope that it will be useful,
**  but WITHOUT ANY WARRANTY; without even the implied warranty of
**  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**  GNU General Public License for more details.
**
**  You should have received a copy of the GNU General Public License
**  along with Sigil.  If not, see <http://www.gnu.org/licenses/>.
**
*************************************************************************/

#include "EmbedPython/EmbeddedPython.h"
#include <iostream>

#include <QCoreApplication>
#include <QDir>
#include <QLibraryInfo>
#include <QTextCodec>
#include <QThreadPool>
#include <QTranslator>
#include <QStandardPaths>
#include <QApplication>
#include <QMessageBox>
#include <QResource>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>
#include <QFontMetrics>
#include <QStyle>
#include <QStyleFactory>
#include <QtWebEngineWidgets>
#include <QtWebEngineCore>

#if QT_VERSION >= QT_VERSION_CHECK(5, 12, 0)
#include <QWebEngineUrlScheme>
#endif

#include "Misc/PluginDB.h"
#include "Misc/UILanguage.h"
#include "MainUI/MainApplication.h"
#include "MainUI/MainWindow.h"
#include "Misc/AppEventFilter.h"
#include "Misc/SigilDarkStyle.h"
#include "Misc/SettingsStore.h"
#include "Misc/TempFolder.h"
#include "Misc/UpdateChecker.h"
#include "Misc/Utility.h"
#include "Misc/WebProfileMgr.h"
#include "Widgets/CaretStyle.h"
#include "sigil_constants.h"
#include "sigil_exception.h"

#ifdef Q_OS_WIN32
#include <QtWidgets/QPlainTextEdit>
static const QString WIN_CLIPBOARD_ERROR = "QClipboard::setMimeData: Failed to set data on clipboard";
static const int RETRY_DELAY_MS = 5;
#endif

#ifdef Q_OS_MAC
#include <QFileDialog>
#include <QKeySequence>
#include <QAction>
#include <QIcon>
#include "Dialogs/About.h"
#include "Dialogs/Preferences.h"
extern void disableWindowTabbing();
extern void removeMacosSpecificMenuItems();
#endif

#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    #define QT_ENUM_SKIPEMPTYPARTS Qt::SkipEmptyParts
    #define QT_ENUM_KEEPEMPTYPARTS Qt::KeepEmptyParts
#else
    #define QT_ENUM_SKIPEMPTYPARTS QString::SkipEmptyParts
    #define QT_ENUM_KEEPEMPTYPARTS QString::KeepEmptyParts
#endif

const QString MAC_DOCK_TITLEBAR_FIX =
    "QDockWidget { "
    "    titlebar-close-icon: url(:/dark/closedock-macstyle.svg);"
    "    titlebar-normal-icon: url(:/dark/dockdock-macstyle.svg);"
    "}";


// Allow Focus Highlight qss to be platform dependent
#if defined(Q_OS_MAC)
const QString FOCUS_HIGHLIGHT_QSS =
    "QTableWidget:focus, QTreeWidget:focus, "
    "QPlainTextEdit:focus, QTextEdit:focus,QTreeView::focus, "
    "QTabWidget:focus, QListView:focus, QScrollArea:focus, "
    "QTabBar:focus { "
    "    border: 3px solid HIGHLIGHT_COLOR;"
    "}"
    ".QLineEdit:focus, QToolButton:focus, QPushButton:focus { "
    "    border: 3px solid HIGHLIGHT_COLOR;"
    "}"
    "QComboWidget > QLineEdit:focus { "
    "    border: 1px solid HIGHLIGHT_COLOR;"
    "}"
    "QPlainTextEdit, QTableWidget, QTreeView { "
    "    padding-left:3px; padding-right:3px; padding-top:3px; padding-bottom:3px;"
    "}";
#elif defined(Q_OS_WIN32)
const QString FOCUS_HIGHLIGHT_QSS =
    "QTableWidget:focus, QTreeWidget:focus, QLineEdit:focus, "
    "QPlainTextEdit:focus, QTextEdit:focus,QTreeView::focus, "
    "QTabWidget:focus, QListView:focus, QScrollArea:focus, "
    "QTabBar:focus { "
    "    border: 1px solid HIGHLIGHT_COLOR;"
    "}";
#else // Linux
const QString FOCUS_HIGHLIGHT_QSS =
    "QTableWidget:focus, QTreeWidget:focus, QLineEdit:focus, "
    "QPlainTextEdit:focus, QTextEdit:focus,QTreeView::focus, "
    "QTabWidget:focus, QListView:focus, QScrollArea:focus, "
    "QTabBar:focus { "
    "    border: 1px solid HIGHLIGHT_COLOR;"
    "}";
#endif

// Creates a MainWindow instance depending
// on command line arguments
static MainWindow *GetMainWindow(const QStringList &arguments)
{
    // We use the first argument as the file to load after starting
    QString filepath;
    if (arguments.size() > 1 && Utility::IsFileReadable(arguments.at(1))) {
        filepath = arguments.at(1);
    }
    return new MainWindow(filepath);
}

#ifdef Q_OS_MAC

static void AboutDialog()
{
    About about;
    about.exec();
}

static void PreferencesDialog()
{
    Preferences prefers;
    prefers.exec();
}

static void AppExit()
{
    qApp->closeAllWindows();
    qApp->quit();
}

static void file_new()
{
    MainWindow *w = GetMainWindow(QStringList());
    w->show();
    w->activateWindow();
    QCoreApplication::processEvents();
}

static void file_open()
{
    const QMap<QString, QString> load_filters = MainWindow::GetLoadFiltersMap();
    QStringList filters(load_filters.values());
    filters.removeDuplicates();
    QString filter_string = "";
    foreach(QString filter, filters) {
        filter_string += filter + ";;";
    }
    // "All Files (*.*)" is the default
    QString default_filter = load_filters.value("epub");
    QString filename = QFileDialog::getOpenFileName(0,
                       "Open File",
                       "~",
                       filter_string,
                       &default_filter,
                       QFileDialog::DontUseNativeDialog
                                                   );

    if (!filename.isEmpty()) {
        MainWindow *w = GetMainWindow(QStringList() << "" << filename);
        w->show();
    }
}
#endif

#if !defined(Q_OS_WIN32) && !defined(Q_OS_MAC)
// Returns a QIcon with the Sigil "S" logo in various sizes
static QIcon GetApplicationIcon()
{
    QIcon app_icon;
    // This 16x16 one looks wrong for some reason
    //app_icon.addFile( ":/icon/app_icon_16.png", QSize( 16, 16 ) );
    app_icon.addFile(":/app_icons/app_icon_32.png",  QSize(32, 32));
    app_icon.addFile(":/app_icons/app_icon_48.png",  QSize(48, 48));
    app_icon.addFile(":/app_icons/app_icon_64.png",  QSize(64, 64));
    app_icon.addFile(":/app_icons/app_icon_128.png", QSize(128, 128));
    app_icon.addFile(":/app_icons/app_icon_256.png", QSize(256, 256));
    app_icon.addFile(":/app_icons/app_icon_512.png", QSize(512, 512));
    return app_icon;
}
#endif


// The message handler installed to handle Qt messages
void MessageHandler(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    QString error_message;
    QString context_file;
    QString qt_debug_message;

    switch (type) {
        // TODO: should go to a log
        case QtDebugMsg:
            qt_debug_message = QString("Debug: %1").arg(message.toLatin1().constData());
            fprintf(stderr, "Debug: %s\n", message.toLatin1().constData());
            break;
#if QT_VERSION >= 0x050600
        case QtInfoMsg:
            qt_debug_message = QString("Info: %1").arg(message.toLatin1().constData());
            fprintf(stderr, "Info: %s\n", message.toLatin1().constData());
            break;
#endif
        // TODO: should go to a log
        case QtWarningMsg:
            qt_debug_message = QString("Warning: %1").arg(message.toLatin1().constData());
            fprintf(stderr, "Warning: %s\n", message.toLatin1().constData());
            break;
        case QtCriticalMsg:
            qt_debug_message = QString("Critical: %1").arg(message.toLatin1().constData());
            error_message = QString(message.toLatin1().constData());
            if (context.file) context_file = QString(context.file);


#ifdef Q_OS_WIN32
            // On Windows there is a known issue with the clipboard that results in some copy
            // operations in controls being intermittently blocked. Rather than presenting
            // the user with an error dialog, we should simply retry the operation.
            // Hopefully this will be fixed in a future Qt version (still broken as of 4.8.3).
            if (error_message.startsWith(WIN_CLIPBOARD_ERROR)) {
                QWidget *widget = QApplication::focusWidget();

                if (widget) {
                    QPlainTextEdit *textEdit = qobject_cast<QPlainTextEdit *>(widget);

                    if (textEdit) {
                        QTimer::singleShot(RETRY_DELAY_MS, textEdit, SLOT(copy()));
                        break;
                    }

                    // Same issue can happen on a QLineEdit / QComboBox
                    QLineEdit *lineEdit = qobject_cast<QLineEdit *>(widget);

                    if (lineEdit) {
                        QTimer::singleShot(RETRY_DELAY_MS, lineEdit, SLOT(copy()));
                        break;
                    }

                    QComboBox *comboBox = qobject_cast<QComboBox *>(widget);

                    if (comboBox) {
                        QTimer::singleShot(RETRY_DELAY_MS, comboBox->lineEdit(), SLOT(copy()));
                        break;
                    }
                }
            }

#endif
            // screen out error messages from inspector / devtools
            if (!context_file.contains("devtools://devtools")) {
                Utility::DisplayExceptionErrorDialog(QString("Critical: %1").arg(error_message));
            }
            break;

        case QtFatalMsg:
            Utility::DisplayExceptionErrorDialog(QString("Fatal: %1").arg(QString(message)));
            abort();
    }
    
    // qDebug() prints to SIGIL_DEBUG_LOGFILE environment variable.
    // User must have permissions to write to the location or no file will be created.
    QString sigil_log_file;
    sigil_log_file = Utility::GetEnvironmentVar("SIGIL_DEBUG_LOGFILE");
    if (!sigil_log_file.isEmpty()) {
        QFile outFile(sigil_log_file);
        outFile.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text);
        QTextStream ts(&outFile);
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
        ts << qt_debug_message << Qt::endl;
#else
        ts << qt_debug_message << endl;
#endif  
    }
}


void VerifyPlugins()
{
    PluginDB *pdb = PluginDB::instance();
    pdb->load_plugins_from_disk();
}

#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
void setupHighDPI()
{
    bool has_env_setting = false;
    QStringList env_vars;
    env_vars << "QT_ENABLE_HIGHDPI_SCALING" << "QT_SCALE_FACTOR_ROUNDING_POLICY"
             << "QT_AUTO_SCREEN_SCALE_FACTOR" << "QT_SCALE_FACTOR"
             << "QT_SCREEN_SCALE_FACTORS" << "QT_DEVICE_PIXEL_RATIO";
    foreach(QString v, env_vars) {
        if (!Utility::GetEnvironmentVar(v).isEmpty()) {
            has_env_setting = true;
            break;
        }
    }

    SettingsStore ss;
    int highdpi = ss.highDPI();
    if (highdpi == 1 || (highdpi == 0 && !has_env_setting)) {
        // Turning on Automatic High DPI scaling
        QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling, true);
    } else if (highdpi == 2) {
        // Turning off Automatic High DPI scaling
        QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling, false);
        foreach(QString v, env_vars) {
            bool irrel = qunsetenv(v.toUtf8().constData());
        Q_UNUSED(irrel);
        }
    }
}
#endif


// utility routine for performing centralized ini versioning based on Qt version
void update_ini_file_if_needed(const QString oldfile, const QString newfile)
{
    QFileInfo nf(newfile);
    if (!nf.exists()) {
        QFileInfo of(oldfile);
        if (of.exists() && of.isFile()) QFile::copy(oldfile, newfile);
    }
}


// Application entry point
int main(int argc, char *argv[])
{
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
  #if !defined(Q_OS_WIN32) && !defined(Q_OS_MAC)
    QT_REQUIRE_VERSION(argc, argv, "5.10.0");
  #else
    QT_REQUIRE_VERSION(argc, argv, "5.12.3");
  #endif
#endif // version

#if !defined(Q_OS_WIN32) && !defined(Q_OS_MAC)
    // Unset platform theme plugins/styles environment variables immediately
    // when forcing Sigil's own darkmode palette on Linux
    if (!force_sigil_darkmode_palette.isEmpty()) {
        QStringList env_vars = {"QT_QPA_PLATFORMTHEME", "QT_STYLE_OVERRIDE"};
        foreach(QString v, env_vars) {
            bool irrel = qunsetenv(v.toUtf8().constData());
        Q_UNUSED(irrel);
        }
    }
#endif // Linux


#ifndef QT_DEBUG
    qInstallMessageHandler(MessageHandler);
#endif

    // Set application information for easier use of QSettings classes
    QCoreApplication::setOrganizationName("sigil-ebook");
    QCoreApplication::setOrganizationDomain("sigil-ebook.com");
    QCoreApplication::setApplicationName("sigil");
    QCoreApplication::setApplicationVersion(SIGIL_VERSION);

    // make sure the default Sigil workspace folder has been created
    QString workspace_path = Utility::DefinePrefsDir() + "/workspace";
    QDir workspace_dir(workspace_path);
    if (!workspace_dir.exists()) {
        workspace_dir.mkpath(workspace_path);
    }

    // handle all non-backwards compatible ini file changes
    update_ini_file_if_needed(Utility::DefinePrefsDir() + "/" + SEARCHES_SETTINGS_FILE,
                              Utility::DefinePrefsDir() + "/" + SEARCHES_V2_SETTINGS_FILE);

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    // Qt6 forced move to utf-8 settings values but Qt5 settings are broken for utf-8 codec
    // See QTBUG-40796 and QTBUG-54510 which never got fixed
    update_ini_file_if_needed(Utility::DefinePrefsDir() + "/" + SIGIL_SETTINGS_FILE,
                              Utility::DefinePrefsDir() + "/" + SIGIL_V6_SETTINGS_FILE);

    update_ini_file_if_needed(Utility::DefinePrefsDir() + "/" + CLIPS_SETTINGS_FILE,
                              Utility::DefinePrefsDir() + "/" + CLIPS_V6_SETTINGS_FILE);

    update_ini_file_if_needed(Utility::DefinePrefsDir() + "/" + INDEX_SETTINGS_FILE,
                              Utility::DefinePrefsDir() + "/" + INDEX_V6_SETTINGS_FILE);

    update_ini_file_if_needed(Utility::DefinePrefsDir() + "/" + SEARCHES_V2_SETTINGS_FILE,
                              Utility::DefinePrefsDir() + "/" + SEARCHES_V6_SETTINGS_FILE);
#endif // version

#if QT_VERSION >= QT_VERSION_CHECK(5, 12, 0)
    // register the our own url scheme (this is required since Qt 5.12)
    QWebEngineUrlScheme sigilScheme("sigil");
    sigilScheme.setFlags(QWebEngineUrlScheme::SecureScheme |
                         QWebEngineUrlScheme::LocalScheme |
                         QWebEngineUrlScheme::LocalAccessAllowed |
                         QWebEngineUrlScheme::ContentSecurityPolicyIgnored);
    // sigilScheme.setSyntax(QWebEngineUrlScheme::Syntax::Host);
    sigilScheme.setSyntax(QWebEngineUrlScheme::Syntax::Path);
    QWebEngineUrlScheme::registerScheme(sigilScheme);
#endif // version

#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
#ifndef Q_OS_MAC
    setupHighDPI();
#endif
    QCoreApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
#endif // version

    // many qtbugs related to mixing 32 and 64 bit qt apps when shader disk cache is used
    // Only use if using Qt5.9.0 or higher
#if QT_VERSION >= 0x050900
    QCoreApplication::setAttribute(Qt::AA_DisableShaderDiskCache);
#endif

    // Disable ? as Sigil does not use QWhatsThis
#if QT_VERSION >= QT_VERSION_CHECK(5, 10, 0) && QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    QCoreApplication::setAttribute(Qt::AA_DisableWindowContextHelpButton);
#endif

    // On recent processors with multiple cores this leads to over 40 threads at times
    // We prevent Qt from constantly creating and deleting threads.
    // Using a negative number forces the threads to stay around;
    // that way, we always have a steady number of threads ready to do work.
    // QThreadPool::globalInstance()->setExpiryTimeout(-1);

    // QtWebEngine may need this
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);

    // handle other startup based on current settings and environment variables
    SettingsStore settings;


#if defined(Q_OS_WIN32)
    // Insert altgr and/or darkmode window decorations as needed
    QString current_env_str = Utility::GetEnvironmentVar("QT_QPA_PLATFORM");
    QStringList current_platform_args;
    QString platform_prefix = "windows:";

    // Take into account current system QT_QPA_PLATFORM values
    if (!current_env_str.isEmpty()) {
        if (current_env_str.startsWith(platform_prefix, Qt::CaseInsensitive)) {
            current_platform_args = current_env_str.mid(platform_prefix.length()).split(':', QT_ENUM_SKIPEMPTYPARTS);
            qDebug() << "Current windows platform args: " << current_platform_args;
        }
    }

    // Woff/woff2 fonts can be more fully supported by setting SIGIL_USE_FREETYPE_FONTENGINE to anything.
    // See https://www.mobileread.com/forums/showthread.php?t=356351 for discussion.
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    QString font_backend_override = Utility::GetEnvironmentVar("SIGIL_USE_FREETYPE_FONTENGINE");
    // Don't change any global fontengine parameters a user may have set in QT_QPA_PLATFORM
    bool fontengine_arg_exists = false;
    foreach(QString arg, current_platform_args) {
        if (arg.startsWith("fontengine=", Qt::CaseInsensitive)) {
            fontengine_arg_exists = true;
        }
    }
    if (!fontengine_arg_exists) {
        if (!font_backend_override.isEmpty()) {
            current_platform_args.append("fontengine=freetype");
        }
    }
#endif // QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)

    // if altgr is not already in the list of windows platform options, add it
    if (settings.enableAltGr()) {
        if (!current_platform_args.contains("altgr", Qt::CaseInsensitive)) {
            current_platform_args.append("altgr");
        }
    }

    // if darkmode options are not already in the list of windows platform options,
    // Set it to 1 so that sigil title bars will be dark in Windows darkmode.
    // This is setting is assumed with a dark palette starting with Qt6.5.
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0) && QT_VERSION <= QT_VERSION_CHECK(6, 5, 0)
    if (Utility::WindowsShouldUseDarkMode()) {
        bool darkmode_arg_exists = false;
        foreach(QString arg, current_platform_args) {
            if (arg.startsWith("darkmode", Qt::CaseInsensitive)) {
                darkmode_arg_exists = true;
            }
        }
        if (!darkmode_arg_exists) {
            current_platform_args.append("darkmode=1");
        }
    }
#endif // version

    // Rewrite the new (if any) windows platform options to QT_QPA_PLATFORM
    if (!current_platform_args.isEmpty()) {
        QString new_args = platform_prefix + current_platform_args.join(':');
        qDebug() << "New windows platform args: " << new_args;
        qputenv("QT_QPA_PLATFORM", new_args.toUtf8());
    }
#endif // Q_OS_WIN32)

    // allow user to override the default Preview Timeout (integer in milliseconds)
    QString new_timeout = Utility::GetEnvironmentVar("SIGIL_PREVIEW_TIMEOUT");
    if (!new_timeout.isEmpty()) {
        bool okay;
        int timeout = new_timeout.toInt(&okay, 10);
        if (!okay) {
            timeout = 1000;
        } else {
            if (timeout < 1000) timeout = 1000;
            if (timeout > 10000)timeout = 1000;
        }
        settings.setUIPreviewTimeout(timeout);
    }

    // enable disabling of gpu acceleration for QtWebEngine.
    // append to current environment variable contents as numerous chromium 
    // switches exist that may be useful
    if (settings.disableGPU()) {
        QString current_flags = Utility::GetEnvironmentVar("QTWEBENGINE_CHROMIUM_FLAGS");
        if (current_flags.isEmpty()) {
            current_flags = "--disable-gpu";
        } else if (!current_flags.contains("--disable-gpu")) {
            current_flags += " --disable-gpu";
        }
        qputenv("QTWEBENGINE_CHROMIUM_FLAGS", current_flags.toUtf8());
    }

    MainApplication app(argc, argv);

#ifdef Q_OS_MAC
    disableWindowTabbing();
    removeMacosSpecificMenuItems();
#endif

    // Install an event filter for the application
    // so we can catch OS X's file open events
    // This needs to be done upfront to prevent events from
    // being missed
    AppEventFilter *filter = new AppEventFilter(&app);
    app.installEventFilter(filter);

    // Set up embedded python integration first thing
    EmbeddedPython* epython = EmbeddedPython::instance();
    epython->addToPythonSysPath(epython->embeddedRoot());
    epython->addToPythonSysPath(PluginDB::launcherRoot() + "/python");

    try {

        // Specify the plugin folders
        // (language codecs and image loaders)
        app.addLibraryPath("codecs");
        app.addLibraryPath("iconengines");
        app.addLibraryPath("imageformats");

        QTextCodec::setCodecForLocale(QTextCodec::codecForName("utf8"));

        // Setup the qtbase_ translator and load the translation for the selected language
        QTranslator qtbaseTranslator;
        const QString qm_name_qtbase = QString("qtbase_%1").arg(settings.uiLanguage());
        // Run though all locations and stop once we find and are able to load
        // an appropriate Qt base translation.
        foreach(QString path, UILanguage::GetPossibleTranslationPaths()) {
            if (QDir(path).exists()) {
                if (qtbaseTranslator.load(qm_name_qtbase, path)) {
                    break;
                }
            }
        }
        app.installTranslator(&qtbaseTranslator);

        // Setup the Sigil translator and load the translation for the selected language
        QTranslator sigilTranslator;
        const QString qm_name = QString("sigil_%1").arg(settings.uiLanguage());
        // Run though all locations and stop once we find and are able to load
        // an appropriate translation.
        foreach(QString path, UILanguage::GetPossibleTranslationPaths()) {
            if (QDir(path).exists()) {
                if (sigilTranslator.load(qm_name, path)) {
                    break;
                }
            }
        }
        app.installTranslator(&sigilTranslator);

#ifdef Q_OS_MAC
        // QApplication::setStyle("macOS");
        QStyle* astyle = QStyleFactory::create("macOS");
        app.setStyle(astyle);
#endif // Q_OS_MAC

#ifdef Q_OS_WIN32
        QStyle* astyle = QStyleFactory::create("Fusion");
        app.setStyle(astyle);
#endif // Q_OS_WIN32

        // Handle the new CaretStyle (double width cursor)
        if (settings.uiDoubleWidthTextCursor()) {
            QApplication::setStyle(new CaretStyle(QApplication::style()));
        }

#ifndef Q_OS_MAC // Linux and Win
        // Custom dark style/palette for Windows and Linux
#ifndef Q_OS_WIN32 //Linux
        // Use platform themes/styles on Linux unless FORCE_SIGIL_DARKMODE_PALETTE is set
        if (!force_sigil_darkmode_palette.isEmpty()) {
            // Apply custom dark style
            app.setStyle(new SigilDarkStyle);
#if QT_VERSION == QT_VERSION_CHECK(5, 15, 0)
            // Qt keeps breaking my custom dark theme.
            // This was apparently only necessary for Qt5.15.0!!
            app.setPalette(QApplication::style()->standardPalette());
#endif // version
        }
#else  // Win
        if (Utility::WindowsShouldUseDarkMode()) {
            // Apply custom dark style last on Windows
            QApplication::setStyle(new SigilDarkStyle(QApplication::style()));
            //app.setStyle(new SigilDarkStyle());
#if QT_VERSION <= QT_VERSION_CHECK(5, 15, 0) || QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
            // At this point, I have no idea where along the 5.15.x series this
            // being present will break dark mode. I only know the first official
            // official windows version that uses 5.15.9 needs it to be gone.
            app.setPalette(QApplication::style()->standardPalette());
#endif // version
        }
#endif // Win
#endif // Linux and Win

        // Set ui font from preferences after dark theming
        QFont f = QFont(QApplication::font());
#ifdef Q_OS_WIN32
        if (f.family() == "MS Shell Dlg 2" && f.pointSize() == 8) {
            // Microsoft's recommended UI defaults
            f.setFamily("Segoe UI");
            f.setPointSize(9);
            QApplication::setFont(f);
        }
#elif defined(Q_OS_MAC)
        // Just in case
#else
        if (f.family() == "Sans Serif" && f.pointSize() == 9) {
            f.setPointSize(10);
            QApplication::setFont(f);
        }
#endif // end of os switch

        settings.setOriginalUIFont(f.toString());
        if (!settings.uiFont().isEmpty()) {
            QFont font;
            if (font.fromString(settings.uiFont())) QApplication::setFont(font);
        }
#ifndef Q_OS_MAC
        // redo on a timer to ensure in all cases
        if (!settings.uiFont().isEmpty()) {
            QFont font;
            if (font.fromString(settings.uiFont())) {
                QTimer::singleShot(0, [=]() {
                    QApplication::setFont(font);
                } );
            }
        }
#endif // Linux and Win

        // drag and drop in main tab bar is too touchy and that can cause problems.
        // default drag distance limit is much too small especially for hpi displays
        // startDragDistance default is just 10 pixels
#ifdef Q_OS_MAC
        if (app.startDragDistance() < 30) app.setStartDragDistance(30);
#else
        QFontMetrics fm(app.font());
        int dragbase = fm.xHeight() * 2;
        bool ok;
        int dragtweak = qEnvironmentVariableIntValue("SIGIL_DRAG_DISTANCE_TWEAK", &ok);
        if (!ok) {
            dragtweak = 0;
        }
        // Use calculated base distance if tweak value not between -20 and 20px
        if (dragtweak >= -20 && dragtweak <= 20) {
            int newdrag = dragbase + dragtweak;
            if (newdrag < 10) {
                app.setStartDragDistance(10);  // 10px minimum
            } else if (newdrag > 60) {
                app.setStartDragDistance(60);  // 60px maximum
            } else {
                app.setStartDragDistance(newdrag);
            }
        } else {
            // Tweak value outside range. Use calculated distance.
            app.setStartDragDistance(dragbase);
        }
#endif
        // End of UI font stuff

#ifdef Q_OS_MAC
        // macOS need to fix broken QDockWidgets under dark mode
        app.setStyleSheet(app.styleSheet().append(MAC_DOCK_TITLEBAR_FIX));
#endif

      	// allow user to highlight the focus widget
        if (settings.uiHighlightFocusWidgetEnabled()) {
            QString focus_qss = FOCUS_HIGHLIGHT_QSS;
            QString hcolor = app.palette().color(QPalette::Highlight).name();
            QString user_color = Utility::GetEnvironmentVar("SIGIL_FOCUS_HIGHLIGHT_COLOR");
            if (!user_color.isEmpty() && user_color.startsWith("#") && user_color.length() == 7) {
#if QT_VERSION >= QT_VERSION_CHECK(6,4,0)
		if (QColor::isValidColorName(user_color)) {
#else
                if (QColor::isValidColor(user_color)) {
#endif
                    hcolor = user_color;
                }
            }
            focus_qss.replace("HIGHLIGHT_COLOR", hcolor);
            app.setStyleSheet(app.styleSheet().append(focus_qss));
	}
        
        // Check for existing qt_styles.qss in Prefs dir and load it if present
        QString qt_stylesheet_path = Utility::DefinePrefsDir() + "/qt_styles.qss";
        QFileInfo QtStylesheetInfo(qt_stylesheet_path);
        if (QtStylesheetInfo.exists() && QtStylesheetInfo.isFile() && QtStylesheetInfo.isReadable()) {
            QString qtstyles = Utility::ReadUnicodeTextFile(qt_stylesheet_path);
            app.setStyleSheet(app.styleSheet().append(qtstyles));
        }

#if defined(Q_OS_MAC) || defined(Q_OS_WIN32)
        // it seems that any time there is stylesheet used, system dark-light palette
        // changes are not propagated to widgets with stylesheets (See QTBUG-124268).
        // This in turn prevents some widgets from properly geting repainted with the new
        // dark or light theme palette (See paintEvent in BookBrowser.cpp for example.)
        // Because our style changes are not palette dependent they should be
        // properly propagated to all widgets including those with stylesheets
        // This is how to tell Qt to do that.  Perhaps any platforms need this as well.
        app. setAttribute(Qt::AA_UseStyleSheetPropagationInWidgetStyles);
#endif

        // Qt's setCursorFlashTime(msecs) (or the docs) are broken
        // According to the docs, setting a negative value should disable cursor blinking 
        // but instead just forces it to look for PlatformSpecific Themeable Hints to get 
        // a value which for Mac OS X is hardcoded to 1000 ms
        // This was the only way I could get Qt to disable cursor blinking on a Mac if desired
        if (qEnvironmentVariableIsSet("SIGIL_DISABLE_CURSOR_BLINK")) {
            app.setCursorFlashTime(0);
        }
        // We set the window icon explicitly on Linux.
        // On Windows this is handled by the RC file,
        // and on Mac by the ICNS file.
#if !defined(Q_OS_WIN32) && !defined(Q_OS_MAC)
        app.setWindowIcon(GetApplicationIcon());
#if QT_VERSION >= 0x050700
        // Wayland needs this clarified in order to propery assign the icon 
        app.setDesktopFileName(QStringLiteral("sigil.desktop"));
#endif // version
#endif //!defined(Q_OS_WIN32) && !defined(Q_OS_MAC)

        // Create the required QWebEngineProfiles, Initialize the settings
        // just once, installing both URLInterceptor and URLSchemeHandler as needed
        // to bypass 2mb url limit (singleton)
        WebProfileMgr* profile_mgr = WebProfileMgr::instance();
        Q_UNUSED(profile_mgr);
        
        // Needs to be created on the heap so that
        // the reply has time to return.
        // Skip if compile-time define or runtime env var is set.
        if ((!DONT_CHECK_FOR_UPDATES) && (!qEnvironmentVariableIsSet("SKIP_SIGIL_UPDATE_CHECK"))) {
            UpdateChecker *checker = new UpdateChecker(&app);
            checker->CheckForUpdate();
        }

        // select the icon theme to use
        QString RCCResourcePath;
#ifdef Q_OS_MAC
        QDir exedir(QCoreApplication::applicationDirPath());
        exedir.cdUp();
        RCCResourcePath = exedir.absolutePath() + "/Resources";
#elif defined(Q_OS_WIN32)
        RCCResourcePath = QCoreApplication::applicationDirPath() + "/iconthemes";
#else
        // user supplied environment variable to 'share/sigil' directory overrides everything
        if (!sigil_extra_root.isEmpty()) {
            RCCResourcePath = sigil_extra_root + "/iconthemes";
        } else {
            RCCResourcePath = sigil_share_root + "/iconthemes";
        }
#endif // OS switch

        QString icon_theme = settings.uiIconTheme();
        // First check if user wants the Custom Icon Theme
        if (icon_theme == "custom") {
            // it must exist and be loadable
            QString CustomRCCPath = Utility::DefinePrefsDir() + "/" + CUSTOM_ICON_THEME_FILENAME;
            bool loaded = false;
            if (QFileInfo(CustomRCCPath).exists()) {
                loaded = QResource::registerResource(Utility::DefinePrefsDir() + "/" + CUSTOM_ICON_THEME_FILENAME);
            }
            if (!loaded) {
                // revert to using main
                icon_theme = "main";
                settings.setUIIconTheme("main");
            }
        }
        // qDebug() << RCCResourcePath;
        QResource::registerResource(RCCResourcePath + "/" + icon_theme + ".rcc");

        QStringList arguments = QCoreApplication::arguments();

#ifdef Q_OS_MAC
        // now process main app events so that any startup
        // FileOpen event will be processed for macOS

        // Note: this is a race between when macOS
        // sends out a signal that there is a file being launched simultaneously
        // and us reaching this point to check if there is such a file

        // So give Qt lots of time to process the macOS signal and convert
        // it into its own Qt signal that passes along the file path
        QCoreApplication::processEvents();
        QCoreApplication::processEvents();
        QCoreApplication::processEvents();
        QCoreApplication::processEvents();
        QCoreApplication::processEvents();

        QString filepath = filter->getInitialFilePath();

        // if one found append it to argv for processing as normal
        if ((arguments.size() == 1) && !filepath.isEmpty()) {
            arguments << QFileInfo(filepath).absoluteFilePath();
        }

        if (filepath.isEmpty()) {
            filter->setInitialFilePath(QString("placeholder"));
        }
#endif // Q_OS_MAC

        if (arguments.contains("-t")) {
            std::cout  << TempFolder::GetPathToSigilScratchpad().toStdString() << std::endl;
            return 1;
        } else {
            // Normal startup

#ifdef Q_OS_MAC
            // Try to Work around QTBUG-62193 and QTBUG-65245 and others where menubar
            // menu items are lost under File and Sigil menus and where
            // Quit menu gets lost when deleting other windows first

            app.setQuitOnLastWindowClosed(false);

            // Create a viable Global MacOS QMenuBar
            QMenuBar *mac_bar = new QMenuBar(0);

            // Create the Application Menu
            QMenu *app_menu = new QMenu("Sigil");
            QIcon icon;

            // Quit
            QAction* appquit_action = new QAction(QObject::tr("Quit"));
            appquit_action->setMenuRole(QAction::QuitRole);
            appquit_action->setShortcut(QKeySequence("Ctrl+Q"));
            icon = appquit_action->icon();
            icon.addFile(QString::fromUtf8(":/main/process-stop.svg"));
            appquit_action->setIcon(icon);
            QObject::connect(appquit_action, &QAction::triggered, AppExit);
            app_menu->addAction(appquit_action);

            // About
            QAction* about_action = new QAction(QObject::tr("About"));
            about_action->setMenuRole(QAction::AboutRole);
            icon = about_action->icon();
            icon.addFile(QString::fromUtf8(":/main/help-browser.svg"));
            about_action->setIcon(icon);
            QObject::connect(about_action, &QAction::triggered, AboutDialog);
            app_menu->addAction(about_action);

            // Preferences
            QAction* prefs_action = new QAction(QObject::tr("Preferences"));
            prefs_action->setMenuRole(QAction::PreferencesRole);
            QObject::connect(prefs_action, &QAction::triggered, PreferencesDialog);
            app_menu->addAction(prefs_action);

            mac_bar->addMenu(app_menu);

            // now create a File Menu
            QMenu *file_menu = new QMenu("File");

            // New
            QAction * new_action = new QAction(QObject::tr("New"));
            new_action->setShortcut(QKeySequence("Ctrl+N"));
            icon = new_action->icon();
            icon.addFile(QString::fromUtf8(":/main/document-new.svg"));
            new_action->setIcon(icon);
            QObject::connect(new_action, &QAction::triggered, file_new);
            file_menu->addAction(new_action);

            // Open
            QAction* open_action = new QAction(QObject::tr("Open"));
            open_action->setShortcut(QKeySequence("Ctrl+O"));
            icon = open_action->icon();
            icon.addFile(QString::fromUtf8(":/main/document-open.svg"));
            open_action->setIcon(icon);
            QObject::connect(open_action, &QAction::triggered, file_open);
            file_menu->addAction(open_action);

            // Quit - force add of a secondary quit menu to the file menu
            QAction* quit_action = new QAction(QObject::tr("Quit"));
            quit_action->setMenuRole(QAction::NoRole);
            quit_action->setShortcut(QKeySequence("Ctrl+Q"));
            QObject::connect(quit_action, &QAction::triggered, qApp->quit);
            file_menu->addAction(quit_action);

            mac_bar->addMenu(file_menu);
#endif // Q_OS_MAC

            VerifyPlugins();
            MainWindow *widget = GetMainWindow(arguments);
            widget->show();
            widget->activateWindow();
            return app.exec();
        }
    } catch (std::exception &e) {
        Utility::DisplayExceptionErrorDialog(e.what());
        return 1;
    }
}
