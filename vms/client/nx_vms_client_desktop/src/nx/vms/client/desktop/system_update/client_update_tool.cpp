// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include "client_update_tool.h"

#include <QtCore/QCoreApplication>

#include <api/global_settings.h>
#include <api/server_rest_connection.h>
#include <client/client_startup_parameters.h>
#include <client_core/client_core_module.h>
#include <common/common_module.h>
#include <nx/build_info.h>
#include <nx/network/cloud/cloud_connect_controller.h>
#include <nx/network/socket_global.h>
#include <nx/network/url/url_builder.h>
#include <nx/utils/app_info.h>
#include <nx/utils/guarded_callback.h>
#include <nx/utils/log/log.h>
#include <nx/vms/api/data/media_server_data.h>
#include <nx/vms/client/core/network/certificate_verifier.h>
#include <nx/vms/client/core/network/network_module.h>
#include <nx/vms/client/core/network/remote_connection.h>
#include <nx/vms/client/desktop/ini.h>
#include <nx/vms/common/p2p/downloader/private/internet_only_peer_manager.h>
#include <nx/vms/common/p2p/downloader/private/resource_pool_peer_manager.h>
#include <nx/vms/update/update_check.h>
#include <utils/applauncher_utils.h>

#include "peer_state_tracker.h"
#include "requests.h"
#include "update_verification.h"

using namespace std::chrono;

namespace nx::vms::client::desktop {

using namespace nx::vms::common::p2p::downloader;
using namespace nx::vms::applauncher::api;

namespace {

const nx::utils::SoftwareVersion kSignaturesAppearedVersion(4, 2);

// Time that is given to process to exit. After that, applauncher (if present) will try to
// terminate it.
constexpr milliseconds kProcessTerminateTimeout = 15s;

} // namespace

bool requestInstalledVersions(QList<nx::utils::SoftwareVersion>* versions)
{
    // Try to run applauncher if it is not running.
    if (!checkOnline())
        return false;
    int kMaxTries = 5;
    do
    {
        if (getInstalledVersions(versions) == ResultType::ok)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    } while (--kMaxTries > 0);

    return false;
}

ClientUpdateTool::ClientUpdateTool(QObject *parent):
    base_type(parent),
    m_outputDir(QDir::temp().absoluteFilePath("nx_updates/client")),
    m_peerManager(new ResourcePoolPeerManager(commonModule())),
    m_proxyPeerManager(new ResourcePoolProxyPeerManager(commonModule()))
{
    qRegisterMetaType<State>();
    qRegisterMetaType<Error>();

    // Expecting m_outputDir to be like /temp/nx_updates/client

    if (ini().massSystemUpdateClearDownloads)
        clearDownloadFolder();

    m_downloader.reset(new Downloader(
        m_outputDir,
        commonModule(),
        {m_peerManager, new InternetOnlyPeerManager(), m_proxyPeerManager}));

    connect(m_downloader.get(), &Downloader::fileStatusChanged,
        this, &ClientUpdateTool::atDownloaderStatusChanged);

    connect(m_downloader.get(), &Downloader::fileInformationChanged,
        this, &ClientUpdateTool::atDownloaderStatusChanged);

    connect(m_downloader.get(), &Downloader::downloadFinished,
        this, &ClientUpdateTool::atDownloadFinished);

    connect(m_downloader.get(), &Downloader::chunkDownloadFailed,
        this, &ClientUpdateTool::atChunkDownloadFailed);

    connect(m_downloader.get(), &Downloader::downloadFailed,
        this, &ClientUpdateTool::atDownloadFailed);

    connect(m_downloader.get(), &Downloader::downloadStalledChanged,
        this, &ClientUpdateTool::atDownloadStallChanged);

    m_downloader->startDownloads();

    m_installedVersionsFuture = std::async(std::launch::async,
        []()
        {
            std::set<nx::utils::SoftwareVersion> output;
            QList<nx::utils::SoftwareVersion> versions;
            if (requestInstalledVersions(&versions))
            {
                for (const auto& version: versions)
                    output.insert(version);
            }
            return output;
        });
}

ClientUpdateTool::~ClientUpdateTool()
{
    NX_VERBOSE(this, "~ClientUpdateTool() enter");

    // Making sure attached thread is complete
    m_state = State::exiting;
    if (m_applauncherTask.valid())
        m_applauncherTask.get();

    // Forcing downloader to be destroyed before serverConnection.
    if (m_downloader)
    {
        m_downloader->disconnect(this);
        m_downloader.reset();
    }

    m_serverConnection.reset();
    NX_VERBOSE(this, "~ClientUpdateTool() done");
}

void ClientUpdateTool::setState(State newState)
{
    if (m_state == newState)
    {
        if (m_state != State::downloading && m_state != State::installing)
            emit updateStateChanged(m_state, 0, {});
        return;
    }

    m_state = newState;
    m_stateChanged = true;
    m_error = common::update::Status::ErrorCode::noError;
    emit updateStateChanged(m_state, 0, {});
}

void ClientUpdateTool::setError(const Error& error)
{
    m_state = State::error;
    m_error = error;
    emit updateStateChanged(m_state, 0, error);
}

void ClientUpdateTool::verifyUpdateFile()
{
    if (m_verificationResult.valid())
        return;

    if (m_updateVersion < kSignaturesAppearedVersion)
    {
        NX_VERBOSE(this, "Update file verification is not supported for version %1. Skipping...",
            m_updateVersion);

        std::promise<common::FileSignature::Result> promise;
        promise.set_value(common::FileSignature::Result::ok);
        m_verificationResult = promise.get_future();
        atVerificationFinished();
        return;
    }

    NX_VERBOSE(this, "Start verifying the update file %1", m_updateFile);

    setState(State::verifying);

    m_verificationResult = std::async(std::launch::async,
        [this]()
        {
            auto result = common::FileSignature::Result::failed;

            if (ini().skipUpdateFilesVerification)
            {
                result = common::FileSignature::Result::ok;
            }
            else
            {
                const QDir keysDir(":/update_verification_keys");
                for (const QString& file: keysDir.entryList({"*.pem"}, QDir::Files, QDir::Name))
                {
                    const QString& fileName = keysDir.absoluteFilePath(file);
                    const QByteArray& key = common::FileSignature::readKey(fileName);
                    if (key.isEmpty())
                        NX_WARNING(this, "Cannot load key from %1", fileName);

                    result = common::FileSignature::verify(
                        m_updateFile, key, m_clientPackage.signature);

                    if (result == common::FileSignature::Result::ok)
                        break;
                }
            }

            QMetaObject::invokeMethod(
                this, &ClientUpdateTool::atVerificationFinished, Qt::QueuedConnection);

            return result;
        });
}

std::future<UpdateContents> ClientUpdateTool::requestInternetUpdateInfo(
    const QString& updateUrl,
    const nx::vms::update::PublicationInfoParams& params)
{
    return std::async(
        [updateUrl, params, connection = m_serverConnection]()
        {
            return system_update::getUpdateContents(connection, updateUrl, params);
        });
}

std::future<UpdateContents> ClientUpdateTool::requestUpdateInfoFromServer(
    const common::update::UpdateInfoParams& params)
{
    if (!m_serverConnection)
    {
        NX_WARNING(this, "requestUpdateInfoFromServer() - No connection to the server");
        std::promise<UpdateContents> promise;
        promise.set_value(UpdateContents());
        return promise.get_future();
    }

    return std::async(
        [this, params]()
        {
            UpdateContents contents = system_update::getUpdateContents(
                m_serverConnection, nx::utils::Url(), params);
            contents.sourceType = UpdateSourceType::mediaservers;
            contents.source = "mediaserver";

            return contents;
        });
}

void ClientUpdateTool::setServerUrl(
    const QnUuid& serverId,
    const nx::vms::client::core::ConnectionInfo& connectionInfo,
    nx::vms::common::AbstractCertificateVerifier* certificateVerifier)
{
    m_serverConnection.reset(
        new rest::ServerConnection(
            serverId,
            /*auditId*/ commonModule()->runningInstanceGUID(),
            certificateVerifier,
            connectionInfo.address,
            connectionInfo.credentials));
    m_peerManager->setServerDirectConnection(serverId, m_serverConnection);
    m_proxyPeerManager->setServerDirectConnection(serverId, m_serverConnection);
}

void ClientUpdateTool::checkServersInSystem()
{
    if (!m_serverConnection)
    {
        NX_WARNING(this, "checkServersInSystem() - No connection to server");
        return;
    }
    m_serverConnection->getMediaServers(nx::utils::guarded(this,
        [this](bool success, [[maybe_unused]] int handle, const nx::vms::api::MediaServerDataList& mediaservers)
        {
            if (!success)
                return;
            QSet<QnUuid> withInternet;
            QSet<QnUuid> withoutInternet;

            for (const auto& server: mediaservers)
            {
                bool hasInternet = server.flags.testFlag(nx::vms::api::ServerFlag::SF_HasPublicIP);
                if (hasInternet)
                    withInternet.insert(server.id);
                else
                    withoutInternet.insert(server.id);
            }

            NX_VERBOSE(this,
                "checkServersInSystem() - got %1 servers with internet and %2 servers without",
                withInternet.size(), withoutInternet.size());
            m_proxyPeerManager->setPeersWithInternetAccess(withInternet);
        }), thread());
}

std::set<nx::utils::SoftwareVersion> ClientUpdateTool::getInstalledClientVersions(
    bool includeCurrentVersion) const
{
    if (m_installedVersionsFuture.valid())
        m_installedVersions = m_installedVersionsFuture.get();
    auto result = m_installedVersions;
    if (includeCurrentVersion)
    {
        QString clientVersion = nx::build_info::vmsVersion();
        result.insert(nx::utils::SoftwareVersion(clientVersion));
    }
    return result;
}

QString ClientUpdateTool::errorString(const ClientUpdateTool::Error& error)
{
    if (const auto code = std::get_if<common::update::Status::ErrorCode>(&error))
        return PeerStateTracker::errorString(*code);
    else
        return std::get<QString>(error);
}

bool ClientUpdateTool::isVersionInstalled(const nx::utils::SoftwareVersion& version) const
{
    auto versions = getInstalledClientVersions(/*includeCurrentVersion=*/true);
    return versions.count(version) != 0;
}

bool ClientUpdateTool::shouldInstallThis(const UpdateContents& contents) const
{
    if (!contents.clientPackage)
        return false;

    const auto& installedVersions = getInstalledClientVersions(true);

    return installedVersions.count(contents.info.version) == 0;
}

void ClientUpdateTool::setUpdateTarget(const UpdateContents& contents)
{
    if (!contents.clientPackage)
        return;

    m_clientPackage = *contents.clientPackage;
    m_updateVersion = contents.info.version;
    const QString& localFile = contents.packageProperties[m_clientPackage.file].localFile;

    if (isVersionInstalled(m_updateVersion))
    {
        if (shouldRestartTo(m_updateVersion))
        {
            NX_INFO(this, "setUpdateTarget(%1) client already has this version installed %2",
                contents.info.version, localFile);
            setState(State::readyRestart);
        }
        else
        {
            NX_INFO(this, "setUpdateTarget(%1) client is already at this version %2",
                contents.info.version, localFile);
            setState(State::complete);
        }
    }
    else if (!contents.needClientUpdate)
    {
        NX_INFO(this, "setUpdateTarget(%1) - no need to install this version",
            contents.info.version);
        setState(State::initial);
        return;
    }
    else if (contents.sourceType == UpdateSourceType::file)
    {
        // Expecting that file is stored at:
        NX_INFO(this, "setUpdateTarget(%1) this is an offline update from the file %2",
            contents.info.version, localFile);

        const QString path = contents.storageDir.filePath(localFile);
        if (!QFileInfo::exists(path))
        {
            NX_ERROR(this, "setUpdateTarget(%1) file %2 does not exist",
                contents.info.version, path);
            setError(tr("File %1 does not exist").arg(path));
        }
        else
        {
            m_updateFile = path;
            verifyUpdateFile();
        }
    }
    else
    {
        NX_INFO(this, "setUpdateTarget(%1) this is an internet update", contents.info.version);

        FileInformation info;
        info.md5 = QByteArray::fromHex(m_clientPackage.md5.toLatin1());
        info.size = m_clientPackage.size;
        info.name = m_clientPackage.file;
        info.url = m_clientPackage.url;

        if (!info.isValid())
        {
            setError(tr("There is no valid client package to download"));
            return;
        }

        setState(State::downloading);

        const auto code = m_downloader->addFile(info);
        NX_VERBOSE(this, "setUpdateTarget(%1) m_downloader->addFile code=%2",
            contents.info.version, code);

        if (code == common::p2p::downloader::ResultCode::fileAlreadyExists
            || code == common::p2p::downloader::ResultCode::fileAlreadyDownloaded)
        {
            // Forcing downloader to start processing this file.
            // It should call all the events and ClientUpdateTool will process its state.
            m_downloader->startDownloads();
        }
        else if (code != common::p2p::downloader::ResultCode::ok)
        {
            const QString error = downloaderErrorToString(code);
            NX_ERROR(this, "setUpdateTarget() failed to add client package %1 %2 (%3)",
                info.name, code, error);
            setError(error);
            return;
        }
    }
}

void ClientUpdateTool::atDownloaderStatusChanged(const FileInformation& fileInformation)
{
    if (fileInformation.name != m_clientPackage.file)
        return;

    if (m_state != State::downloading)
    {
        // TODO: #dklychkov What are we doing here?
        return;
    }

    switch (fileInformation.status)
    {
        case FileInformation::Status::notFound:
            setError(common::update::Status::ErrorCode::updatePackageNotFound);
            break;
        case FileInformation::Status::uploading:
            break;
        case FileInformation::Status::downloaded:
            atDownloadFinished(fileInformation.name);
            break;
        case FileInformation::Status::corrupted:
            setError(common::update::Status::ErrorCode::corruptedArchive);
            break;
        case FileInformation::Status::downloading:
            emit updateStateChanged(State::downloading,
                fileInformation.calculateDownloadProgress(), {});
            break;
        default:
            // Nothing to do here
            break;
    }
}

void ClientUpdateTool::atDownloadFinished(const QString& fileName)
{
    if (fileName != m_clientPackage.file)
        return;

    m_updateFile = m_downloader->filePath(fileName);

    NX_VERBOSE(this, "atDownloadFinished(%1) - finally downloaded file to %2",
        fileName, m_updateFile);

    verifyUpdateFile();
}

void ClientUpdateTool::atChunkDownloadFailed(const QString& /*fileName*/)
{
    // It is a breakpoint catcher.
    //NX_VERBOSE(this) << "atChunkDownloadFailed() failed to download chunk for" << fileName;
}

void ClientUpdateTool::atDownloadFailed(const QString& fileName)
{
    if (m_state == State::downloading)
    {
        NX_ERROR(this, "atDownloadFailed() failed to download file %1", fileName);
        setError(common::update::Status::ErrorCode::downloadFailed);
    }
}

void ClientUpdateTool::atDownloadStallChanged(const QString& fileName, bool stalled)
{
    if (m_state == State::downloading && stalled)
    {
        NX_ERROR(this, "atDownloadFailed() download of %1 has stalled", fileName);
        setError(common::update::Status::ErrorCode::downloadFailed);
    }
}

void ClientUpdateTool::atVerificationFinished()
{
    m_verificationResult.wait();
    const common::FileSignature::Result result = m_verificationResult.get();

    NX_VERBOSE(this, "Update file verification finished (%1). Result: %2", m_updateFile, result);

    if (result == common::FileSignature::Result::ok)
        setState(State::readyInstall);
    else
        setError(common::update::Status::ErrorCode::verificationError);
}

bool ClientUpdateTool::isDownloadComplete() const
{
    return !hasUpdate()
        || m_state == State::readyInstall
        || m_state == State::readyRestart
        || m_state == State::complete;
}

void ClientUpdateTool::checkInternalState()
{
    using nx::vms::applauncher::api::ResultType;

    auto kWaitTime = std::chrono::milliseconds(1);
    if (m_applauncherTask.valid()
        && m_applauncherTask.wait_for(kWaitTime) == std::future_status::ready)
    {
        const ResultType result = m_applauncherTask.get();
        bool shouldRestart = shouldRestartTo(m_updateVersion);

        switch (result)
        {
            case ResultType::alreadyInstalled:
            case ResultType::ok:
                if (shouldRestart)
                    setState(State::readyRestart);
                else
                    setState(State::complete);
                break;

            case ResultType::otherError:
            case ResultType::versionNotInstalled:
            case ResultType::invalidVersionFormat:
            case ResultType::notEnoughSpace:
            case ResultType::notFound:
            case ResultType::ioError:
            {
                QString error = applauncherErrorToString(result);
                NX_ERROR(this) << "Failed check installation:" << error;
                setError(error);
                break;
            }
            default:
                break;
        }
    }
}

bool ClientUpdateTool::installUpdateAsync()
{
    if (m_state != State::readyInstall)
        return false;
    // Try to run applauncher if it is not running.
    if (!applauncher::api::checkOnline())
    {
        NX_VERBOSE(this, "installUpdate cannot install update: applauncher is offline");
        setError(applauncherErrorToString(ResultType::otherError));
        return false;
    }

    NX_INFO(this, "installUpdateAsync() from file %1", m_updateFile);
    NX_ASSERT(!m_updateFile.isEmpty());

    setState(State::installing);

    m_applauncherTask = std::async(std::launch::async,
        [tool = QPointer(this)](
            QString updateFile,
            nx::utils::SoftwareVersion updateVersion) -> applauncher::api::ResultType
        {
            const auto guard = nx::utils::ScopeGuard(
                [tool]()
                {
                    QMetaObject::invokeMethod(
                        tool, &ClientUpdateTool::checkInternalState, Qt::QueuedConnection);
                });

            QString absolutePath = QFileInfo(updateFile).absoluteFilePath();

            int installationAttempts = 2;
            bool stopInstallationAttempts = false;
            // We will try several installation attempts. It will solve a problem, when
            // applauncher was restarted during zip installation.
            do
            {
                const ResultType result = installZipAsync(updateVersion, absolutePath);
                if (result != ResultType::ok)
                {
                    const QString message = applauncherErrorToString(result);
                    NX_ERROR(NX_SCOPE_TAG, "Failed to start async zip installation: %1", message);
                    // Other variants can be fixed by retrying installation, do they?
                    return result;
                }

                NX_VERBOSE(NX_SCOPE_TAG,
                    "Started client installation from file %2. Waiting for completion", absolutePath);

                // Checking state of installation until it becomes Result::ok.
                constexpr int kMaxTries = 60;
                for (int retries = 0; retries < kMaxTries; ++retries)
                {
                    bool stopCheck = false;
                    applauncher::api::InstallationProgress progress;
                    const ResultType result = applauncher::api::checkInstallationProgress(
                        updateVersion, progress);
                    QString message = applauncherErrorToString(result);

                    switch (result)
                    {
                        case ResultType::versionNotInstalled:
                            NX_VERBOSE(NX_SCOPE_TAG,
                            "checkInstallationProgress - installation failed. Retrying");
                            stopCheck = true;
                            break;
                        case ResultType::alreadyInstalled:
                        case ResultType::invalidVersionFormat:
                        case ResultType::notEnoughSpace:
                        case ResultType::notFound:
                        case ResultType::ok:
                            NX_VERBOSE(NX_SCOPE_TAG,
                                "checkInstallationProgress returned %1. Exiting", message);
                            return result;
                        case ResultType::unpackingZip:
                            NX_VERBOSE(NX_SCOPE_TAG, "checkInstallationProgress() %1 of %2 unpacked",
                                progress.extracted, progress.total);
                            break;
                        case ResultType::otherError:
                        case ResultType::connectError:
                        case ResultType::ioError:
                        default:
                            NX_ERROR(NX_SCOPE_TAG, "checkInstallationProgress() failed to check zip "
                                "installation status: %1", message);
                            break;
                    }

                    // We can spend a lot of time in this cycle. So we should be able to exit
                    // as early as possible.
                    if (!tool || tool->m_state != State::installing)
                    {
                        NX_ERROR(NX_SCOPE_TAG, "checkInstallationProgress() is interrupted. Exiting");
                        stopInstallationAttempts = true;
                        break;
                    }

                    if (stopCheck)
                        break;

                    int percent = progress.total != 0 ? 100 * progress.extracted / progress.total : 0;
                    emit tool->updateStateChanged(tool->m_state, percent, {});
                    std::this_thread::sleep_for(std::chrono::seconds(2));
                }
                if (stopInstallationAttempts)
                    break;
            } while (--installationAttempts > 0);

            return ResultType::otherError;
        }, m_updateFile, m_updateVersion);
    return false;
}

bool ClientUpdateTool::isInstallComplete() const
{
    switch (m_state)
    {
        case State::pendingUpdateInfo:
        case State::readyDownload:
        case State::readyInstall:
        case State::downloading:
        case State::verifying:
            return false;
        case State::installing:
            // We need to check applauncher to get a proper result
            break;
        case State::readyRestart:
        case State::complete:
        case State::initial:
        case State::exiting:
        // Though actual install is not successful, no further progress is possible.
        case State::error:
            return true;
    }

    bool installed = false;
    const ResultType result = applauncher::api::isVersionInstalled(
        m_updateVersion,
        &installed);

    switch (result)
    {
        case ResultType::alreadyInstalled:
            break;
        case ResultType::ok:
            return true;

        case ResultType::otherError:
        case ResultType::versionNotInstalled:
        case ResultType::invalidVersionFormat:
        {
            QString error = applauncherErrorToString(result);
            NX_ERROR(this) << "Failed to check installation:" << error;
            return false;
        }
        default:
            // Other variants can be fixed by retrying installation, do they?
            break;
    }

    return installed;
}

bool ClientUpdateTool::shouldRestartTo(const nx::utils::SoftwareVersion& version) const
{
    QString clientVersion = nx::build_info::vmsVersion();
    return version != nx::utils::SoftwareVersion(clientVersion);
}

QString ClientUpdateTool::getServerAuthString() const
{
    const auto connection = this->connection();
    if (!connection)
        return {};

    return QnStartupParameters::createAuthenticationString(
        {
            connection->address(),
            connection->credentials()
        },
        connection->moduleInformation().version);
}

bool ClientUpdateTool::restartClient()
{
    /* Try to run Applauncher if it is not running. */
    if (!applauncher::api::checkOnline())
        return false;

    const QString authString = getServerAuthString();

    int triesLeft = 5;
    do
    {
        --triesLeft;
        if (applauncher::api::restartClient(m_updateVersion, authString) == ResultType::ok)
        {
            applauncher::api::scheduleProcessKill(
                QCoreApplication::applicationPid(), kProcessTerminateTimeout.count());
            qApp->exit(0);
            return true;
        }
        QThread::msleep(200);
        qApp->processEvents();
    } while (triesLeft > 0);

    NX_ERROR(this, "restartClient() - failed to restart client to desired version %1",
        m_updateVersion);
    return false;
}

void ClientUpdateTool::resetState()
{
    NX_DEBUG(this, "resetState() from state %1", toString(m_state));
    switch (m_state)
    {
        case State::downloading:
            m_downloader->deleteFile(m_clientPackage.file, false);
            break;
        case State::readyInstall:
            break;
        case State::initial:
        default:
            // Nothing to do here.
            break;
    }

    m_state = State::initial;
    m_error = common::update::Status::ErrorCode::noError;
    m_updateFile = "";
    m_updateVersion = nx::utils::SoftwareVersion();
    m_clientPackage = nx::vms::update::Package();
}

void ClientUpdateTool::clearDownloadFolder()
{
    // Clear existing folder for downloaded update files.
    m_outputDir.removeRecursively();
    if (!m_outputDir.exists())
        m_outputDir.mkpath(".");
}

ClientUpdateTool::State ClientUpdateTool::getState() const
{
    return m_state;
}

bool ClientUpdateTool::hasUpdate() const
{
    return m_state != State::initial
        && m_state != State::error
        && m_state != State::complete;
}

QString ClientUpdateTool::toString(State state)
{
    switch (state)
    {
        case State::initial:
            return "Initial";
        case State::pendingUpdateInfo:
            return "PendingUpdateInfo";
        case State::readyDownload:
            return "ReadyDownload";
        case State::downloading:
            return "Downloading";
        case State::verifying:
            return "Verifying";
        case State::readyInstall:
            return "ReadyInstall";
        case State::installing:
            return "Installing";
        case State::readyRestart:
            return "ReadyRestart";
        case State::complete:
            return "Complete";
        case State::exiting:
            return "Exiting";
        case State::error:
            return "Error";
    }
    return QString();
}

QString ClientUpdateTool::applauncherErrorToString(ResultType value)
{
    switch (value)
    {
        case ResultType::alreadyInstalled:
            return tr("This update is already installed.");
        case ResultType::otherError:
            return tr("Internal error.");
        case ResultType::versionNotInstalled:
            return tr("This version is not installed.");
        case ResultType::invalidVersionFormat:
            return tr("Invalid version format.");
        case ResultType::brokenPackage:
            return tr("Broken update package.");
        case ResultType::notEnoughSpace:
            return tr("Not enough space on disk to install the client update.");
        case ResultType::notFound:
            // Installed package does not exists. Either we have broken the code and asking
            // for a wrong file, or this file had been removed somehow.
            return tr("Installation package has been lost.");
        default:
            return nx::vms::applauncher::api::toString(value);
    }
}

QString ClientUpdateTool::downloaderErrorToString(
    nx::vms::common::p2p::downloader::ResultCode result)
{
    using nx::vms::common::p2p::downloader::ResultCode;

    switch (result)
    {
        case ResultCode::ok:
            return {};
        case ResultCode::loadingDownloads:
        case ResultCode::ioError:
        case ResultCode::fileDoesNotExist:
        case ResultCode::fileAlreadyExists:
        case ResultCode::fileAlreadyDownloaded:
        case ResultCode::invalidChecksum:
        case ResultCode::invalidFileSize:
        case ResultCode::invalidChunkIndex:
        case ResultCode::invalidChunkSize:
            return tr("Cannot download update file");
        case ResultCode::noFreeSpace:
            return tr("There is no enough space to download update file");
    }

    return {};
}

} // namespace nx::vms::client::desktop
