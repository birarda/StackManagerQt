//
//  AppDelegate.cpp
//  StackManagerQt/src
//
//  Created by Mohammed Nafees on 06/27/14.
//  Copyright (c) 2014 High Fidelity. All rights reserved.
//

#include <csignal>

#include "AppDelegate.h"
#include "BackgroundProcess.h"
#include "GlobalData.h"
#include "DownloadManager.h"

#include <QDateTime>
#include <QDebug>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfoList>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrlQuery>
#include <QUuid>
#include <QCommandLineParser>
#include <QXmlStreamReader>

const QString HIGH_FIDELITY_API_URL = "https://metaverse.highfidelity.com/api/v1";

const QString CHECK_BUILDS_URL = "https://highfidelity.io/builds.xml";

// Use a custom User-Agent to avoid ModSecurity filtering, e.g. by hosting providers.
const QByteArray HIGH_FIDELITY_USER_AGENT = "Mozilla/5.0 (HighFidelity)";

const int VERSION_CHECK_INTERVAL_MS = 86400000; // a day

const int WAIT_FOR_CHILD_MSECS = 5000;

void signalHandler(int param) {
    AppDelegate* app = AppDelegate::getInstance();

    app->quit();
}

static QTextStream* outStream = NULL;

void myMessageHandler(QtMsgType type, const QMessageLogContext &context, const QString &msg) {
    Q_UNUSED(context);

    QString dateTime = QDateTime::currentDateTime().toString("dd/MM/yyyy hh:mm:ss");
    QString txt = QString("[%1] ").arg(dateTime);

    //in this function, you can write the message to any stream!
    switch (type) {
        case QtDebugMsg:
            fprintf(stdout, "Debug: %s\n", qPrintable(msg));
            txt += msg;
            break;
        case QtWarningMsg:
            fprintf(stdout, "Warning: %s\n", qPrintable(msg));
            txt += msg;
            break;
        case QtCriticalMsg:
            fprintf(stdout, "Critical: %s\n", qPrintable(msg));
            txt += msg;
            break;
        case QtFatalMsg:
            fprintf(stdout, "Fatal: %s\n", qPrintable(msg));
            txt += msg;
    }

    if (outStream) {
        *outStream << txt << endl;
    }
}

AppDelegate::AppDelegate(int argc, char* argv[]) :
    QApplication(argc, argv),
    _qtReady(false),
    _dsReady(false),
    _dsResourcesReady(false),
    _acReady(false),
    _domainServerProcess(NULL),
    _acMonitorProcess(NULL),
    _domainServerName("localhost")
{
    // be a signal handler for SIGTERM so we can stop child processes if we get it
    signal(SIGTERM, signalHandler);

    // look for command-line options
    parseCommandLine();

    setApplicationName("Stack Manager");
    setOrganizationName("High Fidelity");
    setOrganizationDomain("io.highfidelity.StackManager");

    QFile* logFile = new QFile("last_run_log", this);
    if (!logFile->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qDebug() << "Failed to open log file. Will not be able to write STDOUT/STDERR to file.";
    } else {
        outStream = new QTextStream(logFile);
    }


    qInstallMessageHandler(myMessageHandler);
    _domainServerProcess = new BackgroundProcess(GlobalData::getInstance().getDomainServerExecutablePath(), this);
    _acMonitorProcess = new BackgroundProcess(GlobalData::getInstance().getAssignmentClientExecutablePath(), this);

    _manager = new QNetworkAccessManager(this);

    _window = new MainWindow();

    createExecutablePath();
    downloadLatestExecutablesAndRequirements();

    _checkVersionTimer.setInterval(0);
    connect(&_checkVersionTimer, SIGNAL(timeout()), this, SLOT(checkVersion()));
    _checkVersionTimer.start();

    connect(this, &QApplication::aboutToQuit, this, &AppDelegate::stopStack);
}

AppDelegate::~AppDelegate() {
    QHash<QUuid, BackgroundProcess*>::iterator it = _scriptProcesses.begin();

    qDebug() << "Stopping scripted assignment-client processes prior to quit.";
    while (it != _scriptProcesses.end()) {
        BackgroundProcess* backgroundProcess = it.value();

        // remove from the script processes hash
        it = _scriptProcesses.erase(it);

        // make sure the process is dead
        backgroundProcess->terminate();
        backgroundProcess->waitForFinished();
        backgroundProcess->deleteLater();
    }

    qDebug() << "Stopping domain-server process prior to quit.";
    _domainServerProcess->terminate();
    _domainServerProcess->waitForFinished();

    qDebug() << "Stopping assignment-client process prior to quit.";
    _acMonitorProcess->terminate();
    _acMonitorProcess->waitForFinished();

    _domainServerProcess->deleteLater();
    _acMonitorProcess->deleteLater();

    _window->deleteLater();

    delete outStream;
    outStream = NULL;
}

void AppDelegate::parseCommandLine() {
    QCommandLineParser parser;
    parser.setApplicationDescription("High Fidelity Stack Manager");
    parser.addHelpOption();

    const QCommandLineOption helpOption = parser.addHelpOption();

    const QCommandLineOption hifiBuildDirectoryOption("b", "Path to build of hifi", "build-directory");
    parser.addOption(hifiBuildDirectoryOption);

    if (!parser.parse(QCoreApplication::arguments())) {
        qCritical() << parser.errorText() << endl;
        parser.showHelp();
        Q_UNREACHABLE();
    }

    if (parser.isSet(helpOption)) {
        parser.showHelp();
        Q_UNREACHABLE();
    }

    if (parser.isSet(hifiBuildDirectoryOption)) {
        const QString hifiBuildDirectory = parser.value(hifiBuildDirectoryOption);
        qDebug() << "hifiBuildDirectory=" << hifiBuildDirectory << "\n";
        GlobalData::getInstance().setHifiBuildDirectory(hifiBuildDirectory);
    }
}

void AppDelegate::toggleStack(bool start) {
    toggleDomainServer(start);
    toggleAssignmentClientMonitor(start);
    toggleScriptedAssignmentClients(start);
    emit stackStateChanged(start);
}

void AppDelegate::toggleDomainServer(bool start) {

    if (start) {
        _domainServerProcess->start(QStringList());

        _window->getLogsWidget()->addTab(_domainServerProcess->getLogViewer(), "Domain Server");

        if (_domainServerID.isEmpty()) {
            // after giving the domain server some time to set up, ask for its ID
            QTimer::singleShot(1000, this, SLOT(requestDomainServerID()));
        }
    } else {
        _domainServerProcess->terminate();
        _domainServerProcess->waitForFinished(WAIT_FOR_CHILD_MSECS);
        _domainServerProcess->kill();
    }
}

void AppDelegate::toggleAssignmentClientMonitor(bool start) {
    if (start) {
        _acMonitorProcess->start(QStringList() << "-n" << "4");
        _window->getLogsWidget()->addTab(_acMonitorProcess->getLogViewer(), "Assignment Clients");
    } else {
        _acMonitorProcess->terminate();
        _acMonitorProcess->waitForFinished(WAIT_FOR_CHILD_MSECS);
        _acMonitorProcess->kill();
    }
}

void AppDelegate::toggleScriptedAssignmentClients(bool start) {
    foreach(BackgroundProcess* scriptProcess, _scriptProcesses) {
        if (start) {
            scriptProcess->start(scriptProcess->getLastArgList());
        } else {
            scriptProcess->terminate();
            scriptProcess->waitForFinished(WAIT_FOR_CHILD_MSECS);
            scriptProcess->kill();
        }
    }
}

int AppDelegate::startScriptedAssignment(const QUuid& scriptID, const QString& pool) {

    BackgroundProcess* scriptProcess = _scriptProcesses.value(scriptID);

    if (!scriptProcess) {
        QStringList argList = QStringList() << "-t" << "2";
        if (!pool.isEmpty()) {
            argList << "--pool" << pool;
        }

        scriptProcess = new BackgroundProcess(GlobalData::getInstance().getAssignmentClientExecutablePath(),
                                              this);

        scriptProcess->start(argList);

        qint64 processID = scriptProcess->processId();
        _scriptProcesses.insert(scriptID, scriptProcess);

        _window->getLogsWidget()->addTab(scriptProcess->getLogViewer(), "Scripted Assignment "
                                        + QString::number(processID));
    } else {
        scriptProcess->QProcess::start();
    }

    return scriptProcess->processId();
}

void AppDelegate::stopScriptedAssignment(BackgroundProcess* backgroundProcess) {
    _window->getLogsWidget()->removeTab(_window->getLogsWidget()->indexOf(backgroundProcess->getLogViewer()));
    backgroundProcess->terminate();
    backgroundProcess->waitForFinished(WAIT_FOR_CHILD_MSECS);
    backgroundProcess->kill();
}

void AppDelegate::stopScriptedAssignment(const QUuid& scriptID) {
    BackgroundProcess* processValue = _scriptProcesses.take(scriptID);
    if (processValue) {
        stopScriptedAssignment(processValue);
    }
}


void AppDelegate::requestDomainServerID() {
    // ask the domain-server for its ID so we can update the accessible name
    emit domainAddressChanged();
    QUrl domainIDURL = GlobalData::getInstance().getDomainServerBaseUrl() + "/id";

    qDebug() << "Requesting domain server ID from" << domainIDURL.toString();

    QNetworkReply* idReply = _manager->get(QNetworkRequest(domainIDURL));

    connect(idReply, &QNetworkReply::finished, this, &AppDelegate::handleDomainIDReply);
}

const QString AppDelegate::getServerAddress() const {
    return "hifi://" + _domainServerName;
}

void AppDelegate::handleDomainIDReply() {
    QNetworkReply* reply = qobject_cast<QNetworkReply*>(sender());

    if (reply->error() == QNetworkReply::NoError
        && reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200) {
        _domainServerID = QString(reply->readAll());

        if (!_domainServerID.isEmpty()) {

            if (!QUuid(_domainServerID).isNull()) {
                qDebug() << "The domain server ID is" << _domainServerID;
                qDebug() << "Asking High Fidelity API for associated domain name.";

                // fire off a request to high fidelity API to see if this domain exists with them
                QUrl domainGetURL = HIGH_FIDELITY_API_URL + "/domains/" + _domainServerID;
                QNetworkReply* domainGetReply = _manager->get(QNetworkRequest(domainGetURL));
                connect(domainGetReply, &QNetworkReply::finished, this, &AppDelegate::handleDomainGetReply);
            } else {
                emit domainServerIDMissing();
            }
        }
    } else {
        qDebug() << "Error getting domain ID from domain-server - "
            << reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt()
            << reply->errorString();
    }
}

void AppDelegate::handleDomainGetReply() {
    QNetworkReply* reply = qobject_cast<QNetworkReply*>(sender());

    if (reply->error() == QNetworkReply::NoError
        && reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200) {
        QJsonDocument responseDocument = QJsonDocument::fromJson(reply->readAll());

        QJsonObject domainObject = responseDocument.object()["domain"].toObject();

        const QString DOMAIN_NAME_KEY = "name";
        const QString DOMAIN_OWNER_PLACES_KEY = "owner_places";

        if (domainObject.contains(DOMAIN_NAME_KEY)) {
            _domainServerName = domainObject[DOMAIN_NAME_KEY].toString();
        } else if (domainObject.contains(DOMAIN_OWNER_PLACES_KEY)) {
            QJsonArray ownerPlaces = domainObject[DOMAIN_OWNER_PLACES_KEY].toArray();
            if (ownerPlaces.size() > 0) {
                _domainServerName = ownerPlaces[0].toObject()[DOMAIN_NAME_KEY].toString();
            }
        }

        qDebug() << "This domain server's name is" << _domainServerName << "- updating address link.";

        emit domainAddressChanged();
    }
}

void AppDelegate::changeDomainServerIndexPath(const QString& newPath) {
    if (!newPath.isEmpty()) {
        QString pathsJSON = "{\"paths\": { \"/\": { \"viewpoint\": \"%1\" }}}";

        QNetworkRequest settingsRequest(GlobalData::getInstance().getDomainServerBaseUrl() + "/settings.json");
        settingsRequest.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

        QNetworkReply* settingsReply = _manager->post(settingsRequest, pathsJSON.arg(newPath).toLocal8Bit());
        connect(settingsReply, &QNetworkReply::finished, this, &AppDelegate::handleChangeIndexPathResponse);
    }
}

void AppDelegate::handleChangeIndexPathResponse() {
    QNetworkReply* reply = qobject_cast<QNetworkReply*>(sender());

    if (reply->error() == QNetworkReply::NoError
        && reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200) {

        qDebug() << "Successfully changed index path in domain-server.";
        emit indexPathChangeResponse(true);
    } else {
        qDebug() << "Error changing domain-server index path-" << reply->errorString();
        emit indexPathChangeResponse(false);
    }
}

void AppDelegate::downloadContentSet(const QUrl& contentSetURL) {
    // make sure this link was an svo
    if (contentSetURL.path().endsWith(".svo")) {
        // setup a request for this content set
        QNetworkRequest contentRequest(contentSetURL);
        QNetworkReply* contentReply = _manager->get(contentRequest);
        connect(contentReply, &QNetworkReply::finished, this, &AppDelegate::handleContentSetDownloadFinished);
    }
}

void AppDelegate::handleContentSetDownloadFinished() {
    QNetworkReply* reply = qobject_cast<QNetworkReply*>(sender());

    if (reply->error() == QNetworkReply::NoError
        && reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200) {

        QString modelFilename = GlobalData::getInstance().getClientsResourcesPath() + "models.svo";

        // write the model file
        QFile modelFile(modelFilename);
        modelFile.open(QIODevice::WriteOnly);

        // stop the base assignment clients before we try to write the new content
        toggleAssignmentClientMonitor(false);

        if (modelFile.write(reply->readAll()) == -1) {
            qDebug() << "Error writing content set to" << modelFilename;
            modelFile.close();
            toggleAssignmentClientMonitor(true);
        } else {
            qDebug() << "Wrote new content set to" << modelFilename;
            modelFile.close();

            // restart the assignment-client
            toggleAssignmentClientMonitor(true);

            emit contentSetDownloadResponse(true);

            // did we have a path in the query?
            // if so when we need to set the DS index path to that path
            QUrlQuery svoQuery(reply->url().query());
            changeDomainServerIndexPath(svoQuery.queryItemValue("path"));

            emit domainAddressChanged();

            return;
        }
    }

    // if we failed we need to emit our signal with a fail
    emit contentSetDownloadResponse(false);
    emit domainAddressChanged();
}

void AppDelegate::onFileSuccessfullyInstalled(const QUrl& url) {
    if (url == GlobalData::getInstance().getRequirementsURL()) {
        _qtReady = true;
    } else if (url == GlobalData::getInstance().getAssignmentClientURL()) {
        _acReady = true;
    } else if (url == GlobalData::getInstance().getDomainServerURL()) {
        _dsReady = true;
    } else if (url == GlobalData::getInstance().getDomainServerResourcesURL()) {
        _dsResourcesReady = true;
    }

    if (_qtReady && _acReady && _dsReady && _dsResourcesReady) {
        _window->setRequirementsLastChecked(QDateTime::currentDateTime().toString());
        _window->show();
    }
}

void AppDelegate::createExecutablePath() {
    QDir launchDir(GlobalData::getInstance().getClientsLaunchPath());
    QDir resourcesDir(GlobalData::getInstance().getClientsResourcesPath());
    QDir logsDir(GlobalData::getInstance().getLogsPath());
    if (!launchDir.exists()) {
        if (QDir().mkpath(launchDir.absolutePath())) {
            qDebug() << "Successfully created directory: "
                     << launchDir.absolutePath();
        } else {
            qCritical() << "Failed to create directory: "
                        << launchDir.absolutePath();
        }
    }
    if (!resourcesDir.exists()) {
        if (QDir().mkpath(resourcesDir.absolutePath())) {
            qDebug() << "Successfully created directory: "
                     << resourcesDir.absolutePath();
        } else {
            qCritical() << "Failed to create directory: "
                        << resourcesDir.absolutePath();
        }
    }
    if (!logsDir.exists()) {
        if (QDir().mkpath(logsDir.absolutePath())) {
            qDebug() << "Successfully created directory: "
                     << logsDir.absolutePath();
        } else {
            qCritical() << "Failed to create directory: "
                        << logsDir.absolutePath();
        }
    }
}

void AppDelegate::downloadLatestExecutablesAndRequirements() {
    // Check if Qt is already installed
    if (GlobalData::getInstance().getPlatform() == "mac") {
        if (QDir(GlobalData::getInstance().getClientsLaunchPath() + "QtCore.framework").exists()) {
            _qtReady = true;
        }
    } else if (GlobalData::getInstance().getPlatform() == "win") {
        if (QFileInfo(GlobalData::getInstance().getClientsLaunchPath() + "Qt5Core.dll").exists()) {
            _qtReady = true;
        }
    } else { // linux
        if (QFileInfo(GlobalData::getInstance().getClientsLaunchPath() + "libQt5Core.so.5").exists()) {
            _qtReady = true;
        }
    }


    QFile reqZipFile(GlobalData::getInstance().getRequirementsZipPath());
    QByteArray reqZipData;
    if (reqZipFile.open(QIODevice::ReadOnly)) {
        reqZipData = reqZipFile.readAll();
        reqZipFile.close();
    }
    QFile resZipFile(GlobalData::getInstance().getDomainServerResourcesZipPath());
    QByteArray resZipData;
    if (resZipFile.open(QIODevice::ReadOnly)) {
        resZipData = resZipFile.readAll();
        resZipFile.close();
    }

    QDir resourcesDir(GlobalData::getInstance().getClientsResourcesPath());
    if (!(resourcesDir.entryInfoList(QDir::AllEntries).size() < 3)) {
        _dsResourcesReady = true;
    }

    // if the user has set hifiBuildDirectory, don't attempt to download the domain-server or assignement-client
    if (GlobalData::getInstance().isGetHifiBuildDirectorySet()) {
        _dsReady = true;
        _acReady = true;
    } else {
        QByteArray dsData;
        QFile dsFile(GlobalData::getInstance().getDomainServerExecutablePath());
        if (dsFile.open(QIODevice::ReadOnly)) {
            dsData = dsFile.readAll();
            dsFile.close();
        }
        QByteArray acData;
        QFile acFile(GlobalData::getInstance().getAssignmentClientExecutablePath());
        if (acFile.open(QIODevice::ReadOnly)) {
            acData = acFile.readAll();
            acFile.close();
        }

        QNetworkRequest acReq(QUrl(GlobalData::getInstance().getAssignmentClientMD5URL()));
        QNetworkReply* acReply = _manager->get(acReq);
        QEventLoop acLoop;
        connect(acReply, SIGNAL(finished()), &acLoop, SLOT(quit()));
        acLoop.exec();
        QByteArray acMd5Data = acReply->readAll().trimmed();
        if (GlobalData::getInstance().getPlatform() == "win") {
            // fix for reading the MD5 hash from Windows-generated
            // binary data of the MD5 hash
            QTextStream stream(acMd5Data);
            stream >> acMd5Data;
        }

        // fix for Mac and Linux network accessibility
        if (acMd5Data.size() == 0) {
            // network is not accessible
            qDebug() << "Could not connect to the internet.";
            _window->show();
            return;
        }

        qDebug() << "AC MD5: " << acMd5Data;
        if (acMd5Data.toLower() == QCryptographicHash::hash(acData, QCryptographicHash::Md5).toHex()) {
            _acReady = true;
        }


        QNetworkRequest dsReq(QUrl(GlobalData::getInstance().getDomainServerMD5URL()));
        QNetworkReply* dsReply = _manager->get(dsReq);
        QEventLoop dsLoop;
        connect(dsReply, SIGNAL(finished()), &dsLoop, SLOT(quit()));
        dsLoop.exec();
        QByteArray dsMd5Data = dsReply->readAll().trimmed();
        if (GlobalData::getInstance().getPlatform() == "win") {
            // fix for reading the MD5 hash from Windows generated
            // binary data of the MD5 hash
            QTextStream stream(dsMd5Data);
            stream >> dsMd5Data;
        }
        qDebug() << "DS MD5: " << dsMd5Data;
        if (dsMd5Data.toLower() == QCryptographicHash::hash(dsData, QCryptographicHash::Md5).toHex()) {
            _dsReady = true;
        }
    }

    if (_qtReady) {
        // check MD5 of requirements.zip only if Qt is found
        QNetworkRequest reqZipReq(QUrl(GlobalData::getInstance().getRequirementsMD5URL()));
        QNetworkReply* reqZipReply = _manager->get(reqZipReq);
        QEventLoop reqZipLoop;
        connect(reqZipReply, SIGNAL(finished()), &reqZipLoop, SLOT(quit()));
        reqZipLoop.exec();
        QByteArray reqZipMd5Data = reqZipReply->readAll().trimmed();
        if (GlobalData::getInstance().getPlatform() == "win") {
            // fix for reading the MD5 hash from Windows generated
            // binary data of the MD5 hash
            QTextStream stream(reqZipMd5Data);
            stream >> reqZipMd5Data;
        }
        qDebug() << "Requirements ZIP MD5: " << reqZipMd5Data;
        if (reqZipMd5Data.toLower() != QCryptographicHash::hash(reqZipData, QCryptographicHash::Md5).toHex()) {
            _qtReady = false;
        }
    }

    if (_dsResourcesReady) {
        // check MD5 of resources.zip only if Domain Server
        // resources are installed
        QNetworkRequest resZipReq(QUrl(GlobalData::getInstance().getDomainServerResourcesMD5URL()));
        QNetworkReply* resZipReply = _manager->get(resZipReq);
        QEventLoop resZipLoop;
        connect(resZipReply, SIGNAL(finished()), &resZipLoop, SLOT(quit()));
        resZipLoop.exec();
        QByteArray resZipMd5Data = resZipReply->readAll().trimmed();
        if (GlobalData::getInstance().getPlatform() == "win") {
            // fix for reading the MD5 hash from Windows generated
            // binary data of the MD5 hash
            QTextStream stream(resZipMd5Data);
            stream >> resZipMd5Data;
        }
        qDebug() << "Domain Server Resources ZIP MD5: " << resZipMd5Data;
        if (resZipMd5Data.toLower() != QCryptographicHash::hash(resZipData, QCryptographicHash::Md5).toHex()) {
            _dsResourcesReady = false;
        }
    }

    DownloadManager* downloadManager = 0;
    if (!_qtReady || !_acReady || !_dsReady || !_dsResourcesReady) {
        // initialise DownloadManager
        downloadManager = new DownloadManager(_manager);
        downloadManager->setWindowModality(Qt::ApplicationModal);
        connect(downloadManager, SIGNAL(fileSuccessfullyInstalled(QUrl)),
                SLOT(onFileSuccessfullyInstalled(QUrl)));
        downloadManager->show();
    } else {
        _window->setRequirementsLastChecked(QDateTime::currentDateTime().toString());
        _window->show();
    }

    if (!_qtReady) {
        downloadManager->downloadFile(GlobalData::getInstance().getRequirementsURL());
    }

    if (!_acReady) {
        downloadManager->downloadFile(GlobalData::getInstance().getAssignmentClientURL());
    }

    if (!_dsReady) {
        downloadManager->downloadFile(GlobalData::getInstance().getDomainServerURL());
    }

    if (!_dsResourcesReady) {
        downloadManager->downloadFile(GlobalData::getInstance().getDomainServerResourcesURL());
    }
}

void AppDelegate::checkVersion() {
    QNetworkRequest latestVersionRequest((QUrl(CHECK_BUILDS_URL)));
    latestVersionRequest.setHeader(QNetworkRequest::UserAgentHeader, HIGH_FIDELITY_USER_AGENT);
    latestVersionRequest.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::PreferCache);
    QNetworkReply* reply = _manager->get(latestVersionRequest);
    connect(reply, &QNetworkReply::finished, this, &AppDelegate::parseVersionXml);

    _checkVersionTimer.setInterval(VERSION_CHECK_INTERVAL_MS);
    _checkVersionTimer.start();
}

struct VersionInformation {
    QString version;
    QUrl downloadUrl;
    QString timeStamp;
    QString releaseNotes;
};

void AppDelegate::parseVersionXml() {

#ifdef Q_OS_WIN32
    QString operatingSystem("windows");
#endif

#ifdef Q_OS_MAC
    QString operatingSystem("mac");
#endif

#ifdef Q_OS_LINUX
    QString operatingSystem("ubuntu");
#endif

    QNetworkReply* sender = qobject_cast<QNetworkReply*>(QObject::sender());
    QXmlStreamReader xml(sender);

    QHash<QString, VersionInformation> projectVersions;

    while (!xml.atEnd() && !xml.hasError()) {
        if (xml.tokenType() == QXmlStreamReader::StartElement && xml.name().toString() == "project") {
            QString projectName = "";
            foreach(const QXmlStreamAttribute &attr, xml.attributes()) {
                if (attr.name().toString() == "name") {
                    projectName = attr.value().toString();
                    break;
                }
            }
            while (!(xml.tokenType() == QXmlStreamReader::EndElement && xml.name().toString() == "project")) {
                if (projectName != "") {
                    if (xml.tokenType() == QXmlStreamReader::StartElement && xml.name().toString() == "platform") {
                        QString platformName = "";
                        foreach(const QXmlStreamAttribute &attr, xml.attributes()) {
                            if (attr.name().toString() == "name") {
                                platformName = attr.value().toString();
                                break;
                            }
                        }
                        int latestVersion = 0;
                        VersionInformation latestVersionInformation;
                        while (!(xml.tokenType() == QXmlStreamReader::EndElement && xml.name().toString() == "platform")) {
                            if (platformName == operatingSystem) {
                                if (xml.tokenType() == QXmlStreamReader::StartElement && xml.name().toString() == "build") {
                                    VersionInformation buildVersionInformation;
                                    while (!(xml.tokenType() == QXmlStreamReader::EndElement && xml.name().toString() == "build")) {
                                        if (xml.tokenType() == QXmlStreamReader::StartElement && xml.name().toString() == "version") {
                                            xml.readNext();
                                            buildVersionInformation.version = xml.text().toString();
                                        }
                                        if (xml.tokenType() == QXmlStreamReader::StartElement && xml.name().toString() == "url") {
                                            xml.readNext();
                                            buildVersionInformation.downloadUrl = QUrl(xml.text().toString());
                                        }
                                        if (xml.tokenType() == QXmlStreamReader::StartElement && xml.name().toString() == "timestamp") {
                                            xml.readNext();
                                            buildVersionInformation.timeStamp = xml.text().toString();
                                        }
                                        if (xml.tokenType() == QXmlStreamReader::StartElement && xml.name().toString() == "note") {
                                            xml.readNext();
                                            if (buildVersionInformation.releaseNotes != "") {
                                                buildVersionInformation.releaseNotes += "\n";
                                            }
                                            buildVersionInformation.releaseNotes += xml.text().toString();
                                        }
                                        xml.readNext();
                                    }
                                    if (latestVersion < buildVersionInformation.version.toInt()) {
                                        latestVersionInformation = buildVersionInformation;
                                        latestVersion = buildVersionInformation.version.toInt();
                                    }
                                }
                            }
                            xml.readNext();
                        }
                        if (latestVersion>0) {
                            projectVersions[projectName] = latestVersionInformation;
                        }
                    }
                }
                xml.readNext();
            }
        }
        xml.readNext();
    }

#ifdef WANT_DEBUG
    qDebug() << "parsed projects for OS" << operatingSystem;
    QHashIterator<QString, VersionInformation> projectVersion(projectVersions);
    while (projectVersion.hasNext()) {
        projectVersion.next();
        qDebug() << "project:" << projectVersion.key();
        qDebug() << "version:" << projectVersion.value().version;
        qDebug() << "downloadUrl:" << projectVersion.value().downloadUrl.toString();
        qDebug() << "timeStamp:" << projectVersion.value().timeStamp;
        qDebug() << "releaseNotes:" << projectVersion.value().releaseNotes;
    }
#endif

    if (projectVersions.contains("stackmanager")) {
        VersionInformation latestVersion = projectVersions["stackmanager"];
        if (QCoreApplication::applicationVersion() != latestVersion.version && QCoreApplication::applicationVersion() != "dev") {
            _window->setUpdateNotification("There is an update available. Please download and install version " + latestVersion.version + ".");
            _window->update();
        }
    }

    sender->deleteLater();
}
