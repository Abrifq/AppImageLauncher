// system includes
#include <iostream>
#include <sstream>
extern "C" {
    #include <sys/stat.h>
    #include <libgen.h>
    #include <unistd.h>
}

// library includes
#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QPushButton>
#include <QMessageBox>
extern "C" {
    #include <appimage/appimage.h>
}

bool makeExecutable(const std::string& path) {
    struct stat fileStat{};

    if (stat(path.c_str(), &fileStat) != 0) {
        std::cerr << "Failed to call stat() on " << path << std::endl;
        return false;
    }

    return chmod(path.c_str(), fileStat.st_mode | 0111) == 0;
}

// Runs an AppImage. Returns suitable exit code for main application.
int runAppImage(const QString& pathToAppImage, int argc, char** argv) {
    // needs to be converted to std::string to be able to use c_str()
    // when using QString and then .toStdString().c_str(), the std::string instance will be an rvalue, and the
    // pointer returned by c_str() will be invalid
    auto x = pathToAppImage.toStdString();
    auto fullPathToAppImage = QFileInfo(pathToAppImage).absoluteFilePath().toStdString();

    // first of all, chmod +x the AppImage file, otherwise execv() will complain
    if (!makeExecutable(fullPathToAppImage))
        return 1;

    // build path to AppImage runtime
    // as it might error, check before fork()ing to be able to display an error message beforehand
    auto exeDir = QFileInfo(QFile("/proc/self/exe").symLinkTarget()).absoluteDir().absolutePath();

    // use external runtime _without_ magic bytes to run the AppImage
    // the original idea was to use /lib64/ld-linux-x86_64.so to run AppImages, but it did complain about some
    // violated ELF data, and refused to run the AppImage
    // alternatively, the AppImage would have to be mounted by this application, and AppRun would need to be called
    // however, this requires some process management (e.g., killing all processes inside the AppImage and also
    // the FUSE "mount" process, when this application is killed...)
    setenv("TARGET_APPIMAGE", fullPathToAppImage.c_str(), true);

    // first attempt: find runtime in expected installation directory
    auto pathToRuntime = exeDir.toStdString() + "/../lib/appimagelauncher/runtime";

    // next method: find runtime in expected build location
    if (!QFile(QString::fromStdString(pathToRuntime)).exists()) {
        pathToRuntime = exeDir.toStdString() + "/../lib/AppImageKit/src/runtime";
    }

    // if it can't be found in either location, display error and exit
    if (!QFile(QString::fromStdString(pathToRuntime)).exists()) {
        std::cerr << "runtime not found: no such file or directory: " << pathToRuntime << std::endl;
        return 1;
    }

    // need a char pointer instead of a const one, therefore can't use .c_str()
    std::vector<char> fullPathToAppImageBuf(pathToRuntime.size() + 1, '\0');
    strcpy(fullPathToAppImageBuf.data(), pathToRuntime.c_str());

    std::vector<char*> args;
    args.push_back(fullPathToAppImageBuf.data());

    // copy arguments
    for (int i = 1; i < (argc - 1); i++) {
        args.push_back(argv[i]);
    }

    // args need to be null terminated
    args.push_back(nullptr);

    execv(pathToRuntime.c_str(), args.data());

    const auto& error = errno;
    std::cout << "execv() failed: " << strerror(error) << std::endl;
}

bool integrateAppImage(const QString& pathToAppImage) {
    auto rv = appimage_register_in_system(pathToAppImage.toStdString().c_str(), false);

    return rv == 0;
}

int main(int argc, char** argv) {
    QCommandLineParser parser;
    parser.setApplicationDescription("Desktop integration helper for AppImages, for use by Linux distributions");

    QApplication app(argc, argv);
    app.setApplicationDisplayName("AppImageLauncher");

    std::ostringstream version;
    version << "version " << APPIMAGELAUNCHER_VERSION << " "
            << "(git commit " << APPIMAGELAUNCHER_GIT_COMMIT << "), built on "
            << APPIMAGELAUNCHER_BUILD_DATE;
    app.setApplicationVersion(QString::fromStdString(version.str()));

    parser.addHelpOption();
    parser.addVersionOption();

    parser.process(app);

    parser.addPositionalArgument("path", "Path to AppImage", "<path>");

    if (parser.positionalArguments().empty()) {
        parser.showHelp(1);
    }

    const auto pathToAppImage = parser.positionalArguments().first();

    if (!QFile(pathToAppImage).exists()) {
        std::cout << "Error: no such file or directory: " << pathToAppImage.toStdString() << std::endl;
        return 1;
    }

    if (appimage_is_registered_in_system(pathToAppImage.toStdString().c_str()))
        return runAppImage(pathToAppImage, argc, argv);

    std::ostringstream explanationStrm;
    explanationStrm << "Integrating it will move the AppImage into a predefined location, "
                    << "and include it in your application launcher." << "\n\n"
                    << "To remove or update the AppImage, please use the context menu of the application icon in "
                    << "your task bar.";

    auto explanation = explanationStrm.str();

    std::ostringstream messageStrm;
    messageStrm << pathToAppImage.toStdString() << " "
                << QObject::tr("has not been integrated into your system.").toStdString() << "\n\n"
                << QObject::tr(explanation.c_str()).toStdString();

    QMessageBox messageBox(
        QMessageBox::Question,
        "AppImage Desktop Integrator",
        QString::fromStdString(messageStrm.str())
    );

    auto* okButton = messageBox.addButton("Integrate and run", QMessageBox::AcceptRole);
    auto* runOnceButton = messageBox.addButton("Run once", QMessageBox::ApplyRole);

    // *whyever* Qt somehow needs a button with "RejectRole" or the X button won't close the current window...
    auto* cancelButton = messageBox.addButton("Cancel", QMessageBox::RejectRole);
    // ... but it is fine to hide that button after creating it, so it's not displayed
    cancelButton->hide();

    messageBox.setDefaultButton(QMessageBox::Ok);

    messageBox.exec();

    const auto* clickedButton = messageBox.clickedButton();

    if (clickedButton == okButton) {
        if (!integrateAppImage(pathToAppImage))
            return 1;
        return runAppImage(pathToAppImage, argc, argv);
    } else if (clickedButton == runOnceButton) {
        return runAppImage(pathToAppImage, argc, argv);
    } else if (clickedButton == cancelButton) {
        return 0;
    }

    // _should_ be unreachable
    return 1;
}
