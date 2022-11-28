// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include "remote_connection_factory.h"

#include <memory>

#include <QtCore/QPointer>

#include <common/common_module.h>
#include <common/common_module_aware.h>
#include <network/system_helpers.h>
#include <nx/network/address_resolver.h>
#include <nx/network/http/auth_tools.h>
#include <nx/network/nx_network_ini.h>
#include <nx/network/socket_global.h>
#include <nx/network/url/url_builder.h>
#include <nx/utils/guarded_callback.h>
#include <nx/utils/thread/thread_util.h>
#include <nx/vms/client/core/ini.h>
#include <nx/vms/client/core/settings/client_core_settings.h>
#include <nx/vms/common/network/server_compatibility_validator.h>
#include <nx/vms/common/resource/server_host_priority.h>
#include <nx_ec/ec_api_fwd.h>
#include <utils/common/delayed.h>
#include <utils/common/synctime.h>
#include <utils/email/email.h>
#include <watchers/cloud_status_watcher.h>

#include "certificate_verifier.h"
#include "cloud_connection_factory.h"
#include "network_manager.h"
#include "private/remote_connection_factory_requests.h"
#include "remote_connection.h"
#include "remote_connection_user_interaction_delegate.h"

using namespace ec2;
using ServerCompatibilityValidator = nx::vms::common::ServerCompatibilityValidator;

namespace nx::vms::client::core {

namespace {

static const nx::utils::SoftwareVersion kRestApiSupportVersion(5, 0);
static const nx::utils::SoftwareVersion kSimplifiedLoginSupportVersion(5, 1);

/**
 * Digest authentication requires username to be in lowercase.
 */
void ensureUserNameIsLowercaseIfDigest(nx::network::http::Credentials& credentials)
{
    if (credentials.authToken.isPassword())
    {
        const QString username = QString::fromStdString(credentials.username);
        credentials.username = username.toLower().toStdString();
    }
}

RemoteConnectionErrorCode toErrorCode(ServerCompatibilityValidator::Reason reason)
{
    switch (reason)
    {
        case ServerCompatibilityValidator::Reason::binaryProtocolVersionDiffers:
            return RemoteConnectionErrorCode::binaryProtocolVersionDiffers;

        case ServerCompatibilityValidator::Reason::cloudHostDiffers:
            return RemoteConnectionErrorCode::cloudHostDiffers;

        case ServerCompatibilityValidator::Reason::customizationDiffers:
            return RemoteConnectionErrorCode::customizationDiffers;

        case ServerCompatibilityValidator::Reason::versionIsTooLow:
            return RemoteConnectionErrorCode::versionIsTooLow;
    }

    NX_ASSERT(false, "Should never get here");
    return RemoteConnectionErrorCode::internalError;
}

std::optional<std::string> publicKey(const std::string& pem)
{
    if (pem.empty())
        return {};

    auto chain = nx::network::ssl::Certificate::parse(pem);
    if (chain.empty())
        return {};

    return chain[0].publicKey();
};

std::optional<QnUuid> deductServerId(const std::vector<nx::vms::api::ServerInformation>& info)
{
    if (info.size() == 1)
        return info.begin()->id;

    for (const auto& server: info)
    {
        if (server.collectedByThisServer)
            return server.id;
    }

    return std::nullopt;
}

nx::utils::Url mainServerUrl(const QSet<QString>& remoteAddresses, int port)
{
    std::vector<nx::utils::Url> addresses;
    for (const auto& addressString: remoteAddresses)
    {
        nx::network::SocketAddress sockAddr(addressString.toStdString());

        nx::utils::Url url = nx::network::url::Builder()
            .setScheme(nx::network::http::kSecureUrlSchemeName)
            .setEndpoint(sockAddr)
            .toUrl();
        if (url.port() == 0)
            url.setPort(port);

        addresses.push_back(url);
    }

    if (addresses.empty())
        return {};

    return *std::min_element(addresses.cbegin(), addresses.cend(),
        [](const auto& l, const auto& r)
        {
            using namespace nx::vms::common;
            return serverHostPriority(l.host()) < serverHostPriority(r.host());
        });
}

} // namespace

using WeakContextPtr = std::weak_ptr<RemoteConnectionFactory::Context>;

struct RemoteConnectionFactory::Private: public /*mixin*/ QnCommonModuleAware
{
    using RequestsManager = detail::RemoteConnectionFactoryRequestsManager;

    ec2::AbstractECConnectionFactory* q;
    const nx::vms::api::PeerType peerType;
    const Qn::SerializationFormat serializationFormat;
    QPointer<CertificateVerifier> certificateVerifier;
    std::unique_ptr<AbstractRemoteConnectionUserInteractionDelegate> userInteractionDelegate;
    std::unique_ptr<CloudConnectionFactory> cloudConnectionFactory;
    std::unique_ptr<nx::cloud::db::api::Connection> cloudConnection;

    std::unique_ptr<RequestsManager> requestsManager;

    Private(
        ec2::AbstractECConnectionFactory* q,
        nx::vms::api::PeerType peerType,
        Qn::SerializationFormat serializationFormat,
        CertificateVerifier* certificateVerifier,
        QnCommonModule* commonModule)
        :
        QnCommonModuleAware(commonModule),
        q(q),
        peerType(peerType),
        serializationFormat(serializationFormat),
        certificateVerifier(certificateVerifier)
    {
        requestsManager = std::make_unique<detail::RemoteConnectionFactoryRequestsManager>(
            certificateVerifier);

        cloudConnectionFactory = std::make_unique<CloudConnectionFactory>();
    }

    RemoteConnectionPtr makeRemoteConnectionInstance(ContextPtr context)
    {
        ConnectionInfo connectionInfo{
            .address = context->address(),
            .credentials = context->credentials(),
            .userType = context->userType()};

        return std::make_shared<RemoteConnection>(
            peerType,
            context->moduleInformation,
            connectionInfo,
            context->sessionTokenExpirationTime,
            certificateVerifier,
            context->certificateCache,
            serializationFormat,
            /*auditId*/ commonModule()->sessionId());
    }

    bool executeInUiThreadSync(std::function<bool()> handler)
    {
        std::promise<bool> isAccepted;
        executeInThread(userInteractionDelegate->thread(),
            [&isAccepted, handler]()
            {
                isAccepted.set_value(handler());
            });
        return isAccepted.get_future().get();
    }

    RequestsManager::ModuleInformationReply getModuleInformation(ContextPtr context)
    {
        if (context)
            return requestsManager->getModuleInformation(context);

        return {};
    }

    RequestsManager::ServersInfoReply getServersInfo(ContextPtr context)
    {
        if (context)
            return requestsManager->getServersInfo(context);

        return {};
    }

    bool checkCompatibility(ContextPtr context)
    {
        if (context)
        {
            NX_VERBOSE(this, "Checking compatibility");
            if (context->moduleInformation.isNewSystem())
            {
                context->error = RemoteConnectionErrorCode::factoryServer;
            }
            else if (const auto incompatibilityReason = ServerCompatibilityValidator::check(
                context->moduleInformation))
            {
                context->error = toErrorCode(*incompatibilityReason);
            }
        }
        return context && !context->failed();
    }

    void verifyExpectedServerId(ContextPtr context)
    {
        if (context && context->logonData.expectedServerId
            && *context->logonData.expectedServerId != context->moduleInformation.id)
        {
            context->error = RemoteConnectionErrorCode::unexpectedServerId;
        }
    }

    void verifyTargetCertificate(ContextPtr context)
    {
        if (!context || !nx::network::ini().verifyVmsSslCertificates)
            return;

        NX_VERBOSE(this, "Verify certificate");
        // Make sure factory is setup correctly.
        if (!NX_ASSERT(certificateVerifier) || !NX_ASSERT(userInteractionDelegate))
        {
            context->error = RemoteConnectionErrorCode::internalError;
            return;
        }

        const CertificateVerifier::Status status = certificateVerifier->verifyCertificate(
            context->moduleInformation.id,
            context->handshakeCertificate);

        if (status == CertificateVerifier::Status::ok)
            return;

        auto pinTargetServerCertificate =
            [this, context]
            {
                if (!NX_ASSERT(!context->handshakeCertificate.empty()))
                    return;

                // Server certificates are checked in checkServerCertificateEquality() using
                // REST API. After that we know exactly which type of certificate is used on
                // SSL handshake. For old servers that don't support REST API the certificate
                // is stored as auto-generated.
                certificateVerifier->pinCertificate(
                    context->moduleInformation.id,
                    context->handshakeCertificate[0].publicKey(),
                    context->targetHasUserProvidedCertificate
                        ? CertificateVerifier::CertificateType::connection
                        : CertificateVerifier::CertificateType::autogenerated);
            };

        const auto serverName = context->address().address.toString();
        NX_VERBOSE(
            this,
            "New certificate has been received from %1. "
            "Trying to verify it by system CA certificates.",
            serverName);

        std::string errorMessage;
        if (NX_ASSERT(!context->handshakeCertificate.empty())
            && nx::network::ssl::verifyBySystemCertificates(
                context->handshakeCertificate, serverName, &errorMessage))
        {
            NX_VERBOSE(this, "Certificate verification for %1 is successful.", serverName);
            pinTargetServerCertificate();
            return;
        }

        NX_VERBOSE(this, errorMessage);

        auto accept =
            [this, status, context]
            {
                if (status == CertificateVerifier::Status::notFound)
                {
                    return userInteractionDelegate->acceptNewCertificate(
                        context->moduleInformation,
                        context->address(),
                        context->handshakeCertificate);
                }
                else if (status == CertificateVerifier::Status::mismatch)
                {
                    return userInteractionDelegate->acceptCertificateAfterMismatch(
                        context->moduleInformation,
                        context->address(),
                        context->handshakeCertificate);
                }
                return false;
            };

        if (auto accepted = context->logonData.userInteractionAllowed
            && executeInUiThreadSync(accept))
        {
            pinTargetServerCertificate();
        }
        else
        {
            // User rejected the certificate.
            context->error = RemoteConnectionErrorCode::certificateRejected;
        }
    }

    void fillCloudConnectionCredentials(ContextPtr context,
        const nx::utils::SoftwareVersion& serverVersion)
    {
        if (!NX_ASSERT(context))
            return;

        if (!NX_ASSERT(context->userType() == nx::vms::api::UserType::cloud))
            return;

        auto& credentials = context->logonData.credentials;

        // Use refresh token to issue new session token if server supports OAuth cloud
        // authorization through the REST API.
        if (serverVersion >= kRestApiSupportVersion)
        {
            credentials = qnCloudStatusWatcher->remoteConnectionCredentials();
        }
        // Current or stored credentials will be passed to compatibility mode client.
        else if (credentials.authToken.empty())
        {
            // Developer mode code.
            context->logonData.credentials.username =
                qnCloudStatusWatcher->cloudLogin().toStdString();
            if (!context->logonData.credentials.authToken.isPassword()
                || context->logonData.credentials.authToken.value.empty())
            {
                context->logonData.credentials.authToken =
                    nx::network::http::PasswordAuthToken(settings()->digestCloudPassword());
            }
        }
    }

    // For cloud connections we can get url, containing only system id.
    // In that case each request may potentially be sent to another server, which may lead
    // to undefined behavior. So we are replacing generic url with the exact server's one.
    // Server address should be fixed as soon as possible.
    void fixCloudConnectionInfoIfNeeded(ContextPtr context)
    {
        if (!context)
            return;

        const std::string hostname = context->address().address.toString();
        if (nx::network::SocketGlobals::addressResolver().isCloudHostname(hostname))
        {
            const auto fullHostname = context->moduleInformation.id.toSimpleString()
                + '.'
                + context->moduleInformation.cloudSystemId;
            context->logonData.address.address = fullHostname;
            NX_DEBUG(this, "Fixed connection address: %1", fullHostname);
        }
    }

    void checkServerCloudConnection(ContextPtr context)
    {
        if (!context)
            return;

        requestsManager->getCurrentSession(context);

        // Assuming server cloud connection problems if fresh cloud token is unauthorized
        if (context->failed() && context->error == RemoteConnectionErrorCode::sessionExpired)
            context->error = RemoteConnectionErrorCode::cloudUnavailableOnServer;
    }

    bool isRestApiSupported(ContextPtr context)
    {
        if (!context)
            return false;

        if (!context->moduleInformation.version.isNull())
            return context->moduleInformation.version >= kRestApiSupportVersion;

        return context->logonData.expectedServerVersion
            && *context->logonData.expectedServerVersion >= kRestApiSupportVersion;
    }

    void loginWithDigest(ContextPtr context)
    {
        if (context)
        {
            ensureUserNameIsLowercaseIfDigest(context->logonData.credentials);
            requestsManager->checkDigestAuthentication(context);
        }
    }

    bool loginWithToken(ContextPtr context)
    {
        if (!context)
            return false;

        if (context->credentials().authToken.isBearerToken())
        {
            // In case token is outdated we will receive unauthorized here.
            nx::vms::api::LoginSession currentSession = requestsManager->getCurrentSession(context);
            if (!context->failed())
            {
                context->logonData.credentials.username = currentSession.username.toStdString();
                context->sessionTokenExpirationTime =
                    qnSyncTime->currentTimePoint() + currentSession.expiresInS;
            }
            return true;
        }
        return false;
    }

    nx::vms::api::LoginUser verifyLocalUserType(ContextPtr context)
    {
        if (context)
        {
            if (!NX_ASSERT(context->userType() == nx::vms::api::UserType::local))
            {
                context->error = RemoteConnectionErrorCode::internalError;
                return {};
            }

            nx::vms::api::LoginUser loginUserData = requestsManager->getUserType(context);
            if (context->failed())
                return {};

            // Check if expected user type does not match actual. Possible scenarios:
            // * Receive cloud user using the local tile - forbidden, throw error.
            // * Receive ldap user using the local tile - OK, updating actual user type.
            switch (loginUserData.type)
            {
                case nx::vms::api::UserType::cloud:
                    context->error = RemoteConnectionErrorCode::loginAsCloudUserForbidden;
                    break;
                case nx::vms::api::UserType::ldap:
                    context->logonData.userType = nx::vms::api::UserType::ldap;
                    break;
                default:
                    break;
            }
            return loginUserData;
        }
        return {};
    }

    std::future<RequestsManager::CloudTokenInfo> issueCloudToken(
        ContextPtr context,
        const nx::utils::SoftwareVersion& serverVersion,
        const QString& cloudSystemId)
    {
        if (!context)
            return {};

        fillCloudConnectionCredentials(context, serverVersion);
        cloudConnection = cloudConnectionFactory->createConnection();
        return requestsManager->issueCloudToken(context, cloudConnection.get(), cloudSystemId);
    }

    void processCloudToken(ContextPtr context, RequestsManager::CloudTokenInfo cloudTokenInfo)
    {
        if (!context || context->failed())
            return;

        if (cloudTokenInfo.error)
            context->error = *cloudTokenInfo.error;

        const auto& response = cloudTokenInfo.response;
        if (!context->failed())
        {
            NX_DEBUG(this, "Token response error: %1", response.error);
            if (response.error)
            {
                if (*response.error == nx::cloud::db::api::OauthManager::k2faRequiredError)
                {
                    auto credentials = context->credentials();
                    credentials.authToken =
                        nx::network::http::BearerAuthToken(response.access_token);

                    auto validate =
                        [this, credentials = std::move(credentials)]
                        {
                            return userInteractionDelegate->request2FaValidation(credentials);
                        };

                    const bool validated = context->logonData.userInteractionAllowed
                        && executeInUiThreadSync(validate);

                    if (!validated)
                        context->error = RemoteConnectionErrorCode::unauthorized;
                }
                else
                {
                    context->error = RemoteConnectionErrorCode::unauthorized;
                }
            }
        }

        if (!context->failed())
        {
            context->logonData.credentials.authToken =
                nx::network::http::BearerAuthToken(response.access_token);
            context->sessionTokenExpirationTime = response.expires_at;
        }
    }

    void issueLocalToken(ContextPtr context)
    {
        if (!context)
            return;

        nx::vms::api::LoginSession session = requestsManager->createLocalSession(context);
        if (!context->failed())
        {
            context->logonData.credentials.username = session.username.toStdString();
            context->logonData.credentials.authToken =
                nx::network::http::BearerAuthToken(session.token);
            context->sessionTokenExpirationTime =
                qnSyncTime->currentTimePoint() + session.expiresInS;
        }
    }

    void processCertificates(ContextPtr context,
        const std::vector<nx::vms::api::ServerInformation>& servers)
    {
        using CertificateType = CertificateVerifier::CertificateType;

        if (!context)
            return;

        NX_VERBOSE(this, "Process received certificates list.");
        context->certificateCache = std::make_shared<CertificateCache>();

        for (const auto& server: servers)
        {
            const auto& serverId = server.id;
            const auto serverUrl = mainServerUrl(server.remoteAddresses, server.port);

            auto storeCertificate =
                [&](const nx::network::ssl::CertificateChain& chain, CertificateType type)
                {
                    if (chain.empty())
                        return false;

                    const auto currentKey = chain[0].publicKey();
                    const auto pinnedKey = certificateVerifier->pinnedCertificate(serverId, type);

                    if (currentKey == pinnedKey)
                        return true;

                    if (!pinnedKey)
                    {
                        // A new certificate has been found. Pin it, since the system is trusted.
                        certificateVerifier->pinCertificate(serverId, currentKey, type);
                        return true;
                    }

                    auto accept =
                        [this, server, serverUrl, chain]()
                        {
                            return userInteractionDelegate->acceptCertificateOfServerInTargetSystem(
                                server,
                                nx::network::SocketAddress::fromUrl(serverUrl),
                                chain);
                        };
                    if (const auto accepted = context->logonData.userInteractionAllowed
                        && executeInUiThreadSync(accept))
                    {
                        certificateVerifier->pinCertificate(serverId, currentKey, type);
                        return true;
                    }

                    return false;
                };

            auto processCertificate =
                [&](const std::string& pem, CertificateType type)
                {
                    if (pem.empty())
                        return true; //< There is no certificate to process.

                    const auto chain = nx::network::ssl::Certificate::parse(pem);

                    if (!storeCertificate(chain, type))
                    {
                        context->error = RemoteConnectionErrorCode::certificateRejected;
                        return false;
                    }

                    // Certificate has been stored successfully. Add it into the cache.
                    context->certificateCache->addCertificate(
                        serverId,
                        chain[0].publicKey(),
                        type);
                    return true;
                };

            if (!processCertificate(server.certificatePem, CertificateType::autogenerated))
            {
                return;
            }

            if (!processCertificate(
                server.userProvidedCertificatePem,
                CertificateType::connection))
            {
                return;
            }
        }
    }

    void fixupCertificateCache(ContextPtr context)
    {
        if (!context)
            return;

        NX_VERBOSE(this, "Emulate certificate cache for System without REST API support.");
        context->certificateCache = std::make_shared<CertificateCache>();
        if (nx::network::ini().verifyVmsSslCertificates
            && NX_ASSERT(!context->handshakeCertificate.empty()))
        {
            context->certificateCache->addCertificate(
                context->moduleInformation.id,
                context->handshakeCertificate[0].publicKey(),
                CertificateVerifier::CertificateType::autogenerated);
        }
    }

    /**
     * This method run asynchronously in a separate thread.
     */
    void connectToServerAsync(WeakContextPtr contextPtr)
    {
        auto context =
            [contextPtr]() -> ContextPtr
            {
                if (auto context = contextPtr.lock(); context && !context->failed())
                    return context;
                return {};
            };

        std::optional<QnUuid> expectedServerId;
        std::optional<nx::utils::SoftwareVersion> expectedServerVersion;
        std::optional<QString> expectedCloudSystemId;
        bool isCloudConnection = false;
        std::future<RequestsManager::CloudTokenInfo> cloudToken;

        auto requestCloudTokenIfPossible =
            [&](const QString& logMessage)
            {
                if (isCloudConnection
                    && !cloudToken.valid()
                    && expectedServerVersion
                    && expectedCloudSystemId)
                {
                    NX_DEBUG(this, "Requesting Cloud access token (%1).", logMessage);
                    // GET /cdb/oauth2/token.
                    cloudToken = issueCloudToken(
                        context(),
                        *expectedServerVersion,
                        *expectedCloudSystemId);
                }
            };

        if (auto ctx = context())
        {
            expectedServerId = ctx->logonData.expectedServerId;
            expectedServerVersion = ctx->logonData.expectedServerVersion;
            expectedCloudSystemId = ctx->logonData.expectedCloudSystemId;
            isCloudConnection = ctx->userType() == nx::vms::api::UserType::cloud;
        }

        if (expectedServerId)
            NX_DEBUG(this, "Expecting Server ID %1.", *expectedServerId);
        else
            NX_DEBUG(this, "Server ID is not known.");

        if (expectedServerVersion)
            NX_DEBUG(this, "Expecting Server version %1.", *expectedServerVersion);
        else
            NX_DEBUG(this, "Server version is not known.");

        if (isCloudConnection)
        {
            if (expectedCloudSystemId)
                NX_DEBUG(this, "Expecting Cloud connect to %1.", *expectedCloudSystemId);
            else
                NX_DEBUG(this, "Expection Cloud connect but the System ID is not known yet.");
        }

        // Request cloud token asyncronously, as this request may go in parallel with Server api.
        requestCloudTokenIfPossible("all data present");
        if (!context())
            return;

        // If server version is not known, we should call api/moduleInformation first. Also send
        // this request for 4.2 and older systems as there is no other way to identify them.
        if (!expectedServerVersion || *expectedServerVersion < kRestApiSupportVersion)
        {
            // GET /api/moduleInformation.
            RequestsManager::ModuleInformationReply reply = getModuleInformation(context());
            if (auto ctx = context())
            {
                ctx->handshakeCertificate = reply.handshakeCertificate;
                ctx->moduleInformation = reply.moduleInformation;
                expectedServerVersion = reply.moduleInformation.version;

                // Check whether actual server id matches the one we expected to get (if any).
                if (expectedServerId)
                    verifyExpectedServerId(context());
                else
                    expectedServerId = reply.moduleInformation.id;

                NX_DEBUG(this, "Fill Cloud System ID from module information");
                expectedCloudSystemId = reply.moduleInformation.cloudSystemId;
            }
            requestCloudTokenIfPossible("module information received");
        }

        if (!expectedServerVersion || !context())
            return;

        // For Systems 5.0 and newer we may use /rest/v1/servers/*/info and receive all Servers'
        // certificates in one call. Offline Servers will not be listed if the System is 5.0, so
        // their certificates will be processed in the ServerCertificateWatcher class.
        if (*expectedServerVersion >= kRestApiSupportVersion)
        {
            // GET /rest/v1/servers/*/info.
            RequestsManager::ServersInfoReply reply = getServersInfo(context());
            if (auto ctx = context())
            {
                ctx->handshakeCertificate = reply.handshakeCertificate;

                if (!expectedServerId)
                {
                    // Try to deduct server id for the 5.1 Systems or Systems with one server.
                    expectedServerId = deductServerId(reply.serversInfo);
                }

                if (!expectedCloudSystemId && !reply.serversInfo.empty())
                {
                    NX_DEBUG(this, "Fill Cloud System ID from servers info");
                    expectedCloudSystemId = reply.serversInfo[0].cloudSystemId;
                    requestCloudTokenIfPossible("servers info received");
                }
            }

            // We can connect to 5.0 System without knowing actual server id, so we need to get
            // it somehow anyway.
            if (!expectedServerId)
            {
                NX_DEBUG(this, "Cannot deduct Server ID, requesting it additionally.");

                // GET /api/moduleInformation.
                RequestsManager::ModuleInformationReply reply = getModuleInformation(context());
                if (auto ctx = context())
                    expectedServerId = reply.moduleInformation.id;
            }

            if (!expectedServerId)
                return;

            if (auto ctx = context())
            {
                auto currentServer = std::find_if(
                    reply.serversInfo.cbegin(),
                    reply.serversInfo.cend(),
                    [id = *expectedServerId](const auto& server) { return server.id == id; });
                if (currentServer == reply.serversInfo.cend())
                {
                    NX_WARNING(
                        this, "Server info list does not contain Server %1.", *expectedServerId);
                    ctx->error = RemoteConnectionErrorCode::networkContentError;
                    return;
                }

                ctx->moduleInformation = *currentServer;

                // Check that the handshake certificate matches one of the targets's.
                [this, ctx, currentServer]
                {
                    if (!nx::network::ini().verifyVmsSslCertificates)
                        return;

                    if (NX_ASSERT(!ctx->handshakeCertificate.empty(),
                        "Handshake certificate chain is empty."))
                    {
                        const auto handshakeKey = ctx->handshakeCertificate[0].publicKey();

                        if (handshakeKey == publicKey(currentServer->userProvidedCertificatePem))
                        {
                            ctx->targetHasUserProvidedCertificate = true;
                            return;
                        }

                        if (handshakeKey == publicKey(currentServer->certificatePem))
                            return;

                        // Handshake certificate doesn't match target server certificates.
                        ctx->error = RemoteConnectionErrorCode::certificateRejected;

                        NX_WARNING(this,
                            "The handshake certificate doesn't match any certificate provided by server.\n"
                            "Handshake key: %1", handshakeKey);

                        if (const auto& pem = currentServer->certificatePem;
                            !pem.empty())
                        {
                            NX_VERBOSE(this,
                                "Server's certificate key: %1\nServer's certificate: %2",
                                publicKey(pem), pem);
                        }

                        if (const auto& pem = currentServer->userProvidedCertificatePem;
                            !pem.empty())
                        {
                            NX_VERBOSE(this,
                                "User provided certificate key: %1\nServer's certificate: %2",
                                publicKey(pem), pem);
                        }
                    }
                }();

            }

            verifyTargetCertificate(context()); //< User interaction.
            processCertificates(context(), reply.serversInfo);
        }
        else //< 4.2 and older servers.
        {
            verifyTargetCertificate(context()); //< User interaction.
            fixupCertificateCache(context());
        }

        fixCloudConnectionInfoIfNeeded(context());

        if (!checkCompatibility(context()))
            return;

        if (!isRestApiSupported(context()))
        {
            NX_DEBUG(this, "Login with Digest to the System with no REST API support.");
            // GET /api/moduleInformationAuthenticated.
            loginWithDigest(context());
            return;
        }

        if (peerType == nx::vms::api::PeerType::videowallClient)
        {
            NX_DEBUG(this, "Login as Video Wall.");
            // GET /rest/v1/login/sessions/current.
            loginWithToken(context());
            return;
        }

        if (isCloudConnection)
        {
            NX_DEBUG(this, "Connecting as Cloud User, waiting for the access token.");
            if (!NX_ASSERT(cloudToken.valid(), "Cloud token request must be sent already."))
            {
                if (auto ctx = context())
                    ctx->error = RemoteConnectionErrorCode::internalError;
                return;
            }
            processCloudToken(context(), cloudToken.get()); // User Interaction.

            NX_DEBUG(this, "Check whether Server is connected to the Cloud.");
            // GET /rest/v1/login/sessions/current.
            checkServerCloudConnection(context());
        }
        else
        {
            NX_DEBUG(this, "Connecting as Local User, checking whether LDAP is required.");
            // Step is performed for local users to upgrade them to LDAP if needed - or block
            // cloud login using a local system tile / login dialog.
            // GET /rest/v1/login/users/<username>.
            nx::vms::api::LoginUser userType = verifyLocalUserType(context());
            if (userType.methods.testFlag(nx::vms::api::LoginMethod::http))
            {
                NX_DEBUG(this, "Digest authentication is preferred for the User.");
                // Digest is the preferred method as it is the only way to use rtsp instead of
                // rtsps.
                // GET /api/moduleInformationAuthenticated.
                loginWithDigest(context());
            }
            else
            {
                NX_DEBUG(this, "Logging in with a token.");
                // Try to login with an already saved token if present.
                if (!loginWithToken(context())) //< GET /rest/v1/login/sessions/current
                    issueLocalToken(context()); //< GET /rest/v1/login/sessions
            }
        }
    }
};

RemoteConnectionFactory::RemoteConnectionFactory(
    QnCommonModule* commonModule,
    CertificateVerifier* certificateVerifier,
    nx::vms::api::PeerType peerType,
    Qn::SerializationFormat serializationFormat)
    :
    AbstractECConnectionFactory(),
    d(new Private(this, peerType, serializationFormat, certificateVerifier, commonModule))
{
}

RemoteConnectionFactory::~RemoteConnectionFactory()
{
    shutdown();
}

void RemoteConnectionFactory::setUserInteractionDelegate(
    std::unique_ptr<AbstractRemoteConnectionUserInteractionDelegate> delegate)
{
    d->userInteractionDelegate = std::move(delegate);
}

AbstractRemoteConnectionUserInteractionDelegate*
    RemoteConnectionFactory::userInteractionDelegate() const
{
    return d->userInteractionDelegate.get();
}

void RemoteConnectionFactory::shutdown()
{
    d->requestsManager.reset();
}

RemoteConnectionFactory::ProcessPtr RemoteConnectionFactory::connect(
    LogonData logonData,
    Callback callback)
{
    ensureUserNameIsLowercaseIfDigest(logonData.credentials);

    auto process = std::make_shared<RemoteConnectionProcess>();

    process->context->logonData = logonData;

    process->future = std::async(std::launch::async,
        [this, contextPtr = WeakContextPtr(process->context), callback]
        {
            nx::utils::setCurrentThreadName("RemoteConnectionFactoryThread");

            d->connectToServerAsync(contextPtr);

            if (!contextPtr.lock())
                return;

            QMetaObject::invokeMethod(
                this,
                [this, contextPtr, callback]()
                {
                    auto context = contextPtr.lock();
                    if (!context)
                        return;

                    if (context->error)
                        callback(*context->error);
                    else
                        callback(d->makeRemoteConnectionInstance(context));
                },
                Qt::QueuedConnection);
        });
    return process;
}

void RemoteConnectionFactory::destroyAsync(ProcessPtr&& process)
{
    NX_ASSERT(process.use_count() == 1);
    std::thread([process = std::move(process)]{ }).detach();
}

} // namespace nx::vms::client::core
