// Copyright 2018-present Network Optix, Inc. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/

#include "user_list_model.h"

#include <boost/container/flat_set.hpp>

#include <client/client_globals.h>
#include <core/resource/device_dependent_strings.h>
#include <core/resource/user_resource.h>
#include <core/resource_access/access_rights_manager.h>
#include <core/resource_access/resource_access_subject.h>
#include <core/resource_management/resource_pool.h>
#include <nx/branding.h>
#include <nx/utils/qt_helpers.h>
#include <nx/utils/string.h>
#include <nx/vms/api/data/ldap.h>
#include <nx/vms/api/data/user_group_data.h>
#include <nx/vms/client/core/skin/skin.h>
#include <nx/vms/client/desktop/system_context.h>
#include <nx/vms/client/core/skin/color_theme.h>
#include <nx/vms/common/system_settings.h>
#include <nx/vms/common/user_management/user_management_helpers.h>

namespace nx::vms::client::desktop {

namespace {

bool isCustomUser(const QnUserResourcePtr& user)
{
    if (!NX_ASSERT(user))
        return false;

    return user->getRawPermissions() != GlobalPermission::none
        || !user->systemContext()->accessRightsManager()->ownResourceAccessMap(
            user->getId()).isEmpty();
}

nx::vms::api::UserType userTypeForSort(nx::vms::api::UserType userType)
{
    switch (userType) {
        case nx::vms::api::UserType::temporaryLocal:
            return nx::vms::api::UserType::local;

        default:
            return userType;
    }
}

} // namespace

class UserListModel::Private:
    public QObject,
    public SystemContextAware
{
    Q_DECLARE_TR_FUNCTIONS(UserListModel)
    using base_type = QObject;

    UserListModel* const model;

public:
    QString syncId;
    boost::container::flat_set<QnUserResourcePtr> users;
    QHash<QString, QSet<QnUuid>> sameNameUsers; //< Used for detection of non-unique user names.
    QHash<QnUuid, QString> prevName; //< Cache for user names before change.
    QSet<QnUserResourcePtr> checkedUsers;
    QHash<QnUserResourcePtr, bool> enableChangedUsers;
    QHash<QnUserResourcePtr, bool> digestChangedUsers;

    Private(UserListModel* q):
        SystemContextAware(q->systemContext()),
        model(q),
        syncId(globalSettings()->ldap().syncId())
    {
        connect(resourcePool(), &QnResourcePool::resourceAdded, this,
            [this](const QnResourcePtr& resource)
            {
                if (auto user = resource.dynamicCast<QnUserResource>())
                {
                    connect(user.get(), &QnUserResource::attributesChanged,
                        [this](const QnResourcePtr& resource)
                        {
                            auto user = resource.dynamicCast<QnUserResource>();
                            if (!NX_ASSERT(user))
                                return;

                            if (!user->attributes().testFlag(nx::vms::api::UserAttribute::hidden))
                                addUser(user);
                            else
                                removeUser(user);
                        });

                    if (!user->attributes().testFlag(nx::vms::api::UserAttribute::hidden))
                        addUser(user);
                }
            });

        connect(resourcePool(), &QnResourcePool::resourceRemoved, this,
            [this](const QnResourcePtr& resource)
            {
                if (auto user = resource.dynamicCast<QnUserResource>())
                {
                    disconnect(user.get(), nullptr, this, nullptr);
                    removeUser(user);
                }
            });

        connect(globalSettings(), &common::SystemSettings::ldapSettingsChanged, this,
            [this]() { syncId = globalSettings()->ldap().syncId(); });
    }

    void at_resourcePool_resourceChanged(const QnResourcePtr& resource);

    void handleUserChanged(const QnUserResourcePtr& user);

    QnUserResourcePtr user(const QModelIndex& index) const;
    bool isUnique(const QnUserResourcePtr& user) const;

    Qt::CheckState checkState() const;
    void setCheckState(Qt::CheckState state, const QnUserResourcePtr& user = QnUserResourcePtr());

    void resetUsers(const QnUserResourceList& value);
    void addUser(const QnUserResourcePtr& user);
    void removeUser(const QnUserResourcePtr& user);

private:
    void addUserInternal(const QnUserResourcePtr& user);
    void removeUserInternal(const QnUserResourcePtr& user);
};

void UserListModel::Private::at_resourcePool_resourceChanged(const QnResourcePtr& resource)
{
    QnUserResourcePtr user = resource.dynamicCast<QnUserResource>();
    if (!user)
        return;
    handleUserChanged(user);
}

void UserListModel::Private::handleUserChanged(const QnUserResourcePtr& user)
{
    const auto it = users.find(user);
    if (it == users.end())
        return;

    // Update map for non-unique name detection.

    // Remove old name.
    const auto prevLowercaseName = prevName.value(user->getId());
    auto& usersWithName = sameNameUsers[prevLowercaseName];
    usersWithName.remove(user->getId());
    if (usersWithName.isEmpty())
        sameNameUsers.remove(prevLowercaseName);

    // Insert new name.
    const auto lowercaseName = user->getName().toLower();
    sameNameUsers[lowercaseName].insert(user->getId());
    prevName[user->getId()] = lowercaseName;

    const auto row = users.index_of(it);
    QModelIndex index = model->index(row);
    emit model->dataChanged(index, index.sibling(row, UserListModel::ColumnCount - 1));
}

QnUserResourcePtr UserListModel::Private::user(const QModelIndex& index) const
{
    if (!index.isValid() || index.row() >= users.size())
        return QnUserResourcePtr();

    return *users.nth(index.row());
}

bool UserListModel::Private::isUnique(const QnUserResourcePtr& user) const
{
    return sameNameUsers[user->getName().toLower()].size() <= 1;
}

Qt::CheckState UserListModel::Private::checkState() const
{
    if (checkedUsers.isEmpty())
        return Qt::Unchecked;

    if (checkedUsers.size() == users.size())
        return Qt::Checked;

    return Qt::PartiallyChecked;
}

void UserListModel::Private::setCheckState(Qt::CheckState state, const QnUserResourcePtr& user)
{
    if (!user)
    {
        if (state == Qt::Checked)
            checkedUsers = nx::utils::toQSet(users);
        else if (state == Qt::Unchecked)
            checkedUsers.clear();
    }
    else
    {
        if (state == Qt::Checked)
            checkedUsers.insert(user);
        else if (state == Qt::Unchecked)
            checkedUsers.remove(user);
    }
}

void UserListModel::Private::resetUsers(const QnUserResourceList& value)
{
    model->beginResetModel();
    for (const auto& user: users)
        removeUserInternal(user);
    users.clear();

    for (const auto& user: value)
    {
        if (!user->attributes().testFlag(nx::vms::api::UserAttribute::hidden))
            users.insert(user);
    }

    for (const auto& user: users)
        addUserInternal(user);
    model->endResetModel();
}

void UserListModel::Private::addUser(const QnUserResourcePtr& user)
{
    if (users.contains(user))
        return;

    const auto row = users.index_of(users.lower_bound(user));

    model->beginInsertRows(QModelIndex(), row, row);
    users.insert(user);
    model->endInsertRows();

    addUserInternal(user);
}

void UserListModel::Private::removeUser(const QnUserResourcePtr& user)
{
    const auto it = users.find(user);
    if (it == users.end())
        return;

    const auto row = users.index_of(it);
    model->beginRemoveRows(QModelIndex(), row, row);
    users.erase(user);
    model->endRemoveRows();

    removeUserInternal(user);
}

void UserListModel::Private::addUserInternal(const QnUserResourcePtr& user)
{
    const auto lowercaseName = user->getName().toLower();
    sameNameUsers[lowercaseName].insert(user->getId());
    prevName[user->getId()] = lowercaseName;

    connect(user.get(), &QnUserResource::nameChanged, this,
        &UserListModel::Private::at_resourcePool_resourceChanged);
    connect(user.get(), &QnUserResource::fullNameChanged, this,
        &UserListModel::Private::at_resourcePool_resourceChanged);
    connect(user.get(), &QnUserResource::permissionsChanged, this,
        &UserListModel::Private::handleUserChanged);
    connect(user.get(), &QnUserResource::userGroupsChanged, this,
        &UserListModel::Private::handleUserChanged);
    connect(user.get(), &QnUserResource::enabledChanged, this,
        [this](const QnUserResourcePtr &user)
        {
            enableChangedUsers.remove(user);
            handleUserChanged(user);
        });
    connect(user.get(), &QnUserResource::digestChanged,
        this,
        [this](const QnUserResourcePtr& user)
        {
            digestChangedUsers.remove(user);
            handleUserChanged(user);
        });
}

void UserListModel::Private::removeUserInternal(const QnUserResourcePtr& user)
{
    const auto lowercaseName = user->getName().toLower();
    auto& usersWithName = sameNameUsers[lowercaseName];
    usersWithName.remove(user->getId());
    if (usersWithName.isEmpty())
        sameNameUsers.remove(lowercaseName);

    prevName.remove(user->getId());

    disconnect(user.get(), &QnUserResource::nameChanged, this,
        &UserListModel::Private::at_resourcePool_resourceChanged);
    disconnect(user.get(), &QnUserResource::fullNameChanged, this,
        &UserListModel::Private::at_resourcePool_resourceChanged);
    disconnect(user.get(), &QnUserResource::permissionsChanged, this,
        &UserListModel::Private::handleUserChanged);
    disconnect(user.get(), &QnUserResource::userGroupsChanged, this,
        &UserListModel::Private::handleUserChanged);
    disconnect(user.get(), &QnUserResource::enabledChanged, this, nullptr);
    disconnect(user.get(), &QnUserResource::digestChanged, this, nullptr);

    checkedUsers.remove(user);
    enableChangedUsers.remove(user);
    digestChangedUsers.remove(user);
}

UserListModel::UserListModel(QObject* parent):
    base_type(parent),
    QnWorkbenchContextAware(parent, QnWorkbenchContextAware::InitializationMode::lazy),
    d(new UserListModel::Private(this))
{
}

UserListModel::~UserListModel()
{
    // Required here for forward-declared scoped pointer destruction.
}

int UserListModel::rowCount(const QModelIndex& parent) const
{
    if (!parent.isValid())
        return d->users.size();

    return 0;
}

int UserListModel::columnCount(const QModelIndex &parent) const
{
    if (!parent.isValid())
        return ColumnCount;

    return 0;
}

QVariant UserListModel::data(const QModelIndex& index, int role) const
{
    if (!hasIndex(index.row(), index.column(), index.parent()))
        return QVariant();

    QnUserResourcePtr user = *d->users.nth(index.row());

    switch (role)
    {
        case Qt::DisplayRole:
        {
            switch (index.column())
            {
                case LoginColumn:
                    return user->getName();
                case FullNameColumn:
                    return user->fullName();
                case EmailColumn:
                    return user->getEmail();
                case UserGroupsColumn:
                    return nx::vms::common::userGroupNames(user).join(", ");
                default:
                    break;
            }
            break;
        }

        case Qt::ToolTipRole:
        {
            switch (index.column())
            {
                case UserTypeColumn:
                {
                    switch (user->userType())
                    {
                        case nx::vms::api::UserType::local:
                            return tr("Local user");
                        case nx::vms::api::UserType::temporaryLocal:
                            return tr("Temporary user");
                        case nx::vms::api::UserType::cloud:
                            return tr("%1 user", "%1 is the short cloud name (like Cloud)")
                                .arg(nx::branding::shortCloudName());
                        case nx::vms::api::UserType::ldap:
                            return tr("LDAP user");
                        default:
                            break;
                    }

                    break;
                }

                case LoginColumn:
                    return user->getName().toHtmlEscaped();

                case FullNameColumn:
                    return user->fullName().toHtmlEscaped();

                case EmailColumn:
                    return user->getEmail();

                case UserGroupsColumn:
                    return nx::vms::common::userGroupNames(user).join("<br>");

                default:
                    return QString(); // not QVariant() because we want to hide a tooltip if shown.

            } // switch (column)
            break;
        }

        case Qn::DecorationPathRole:
        {
            switch (index.column())
            {
                case UserWarningColumn:
                {
                    if (user->externalId().dn.isEmpty() || user->externalId().syncId == d->syncId)
                        return {};

                    return QString("user_settings/user_alert.svg");
                }

                case UserTypeColumn:
                {
                    switch (user->userType())
                    {
                        case nx::vms::api::UserType::cloud:
                            return QString("user_settings/user_cloud.svg");

                        case nx::vms::api::UserType::ldap:
                            return QString("user_settings/user_ldap.svg");

                        case nx::vms::api::UserType::local:
                            return QString("user_settings/user_local.svg");

                        case nx::vms::api::UserType::temporaryLocal:
                            return QString("user_settings/user_local_temp.svg");

                        default:
                            break;
                    }

                    break;
                }

                case IsCustomColumn:
                {
                    if (isCustomUser(user))
                        return QString("text_buttons/ok_20.svg");

                    break;
                }

                default:
                    break;
            }

            break;
        }

        case Qt::DecorationRole:
        {
            const auto path = data(index, Qn::DecorationPathRole).toString();
            return path.isEmpty() ? QVariant() : QVariant::fromValue(qnSkin->icon(path));
        }

        case Qt::ForegroundRole:
        {
            // Always use default color for checkboxes.
            if (index.column() == CheckBoxColumn)
                return QVariant();

            // Highlight conflicting users.
            if (user->isLdap() && !d->isUnique(user))
                return core::colorTheme()->color("red_l2");

            break;
        }

        case Qn::UserResourceRole:
            return QVariant::fromValue(user);

        case Qt::TextAlignmentRole:
        {
            if (index.column() == UserTypeColumn)
                return Qt::AlignCenter;
            break;
        }

        case Qn::DisabledRole:
            return index.column() != CheckBoxColumn && !isUserEnabled(user);

        case Qt::CheckStateRole:
        {
            if (index.column() == CheckBoxColumn)
                return d->checkedUsers.contains(user) ? Qt::Checked : Qt::Unchecked;
            break;
        }

        default:
            break;
    }

    return QVariant();
}

QVariant UserListModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation == Qt::Vertical)
        return QVariant();

    if (section >= ColumnCount)
        return QVariant();

    if (role != Qt::DisplayRole)
        return base_type::headerData(section, orientation, role);

    switch (section)
    {
        case LoginColumn:
            return tr("Login");
        case FullNameColumn:
            return tr("Full Name");
        case EmailColumn:
            return tr("Email");
        case UserGroupsColumn:
            return tr("Groups");
        case IsCustomColumn:
            return tr("Custom");
        default:
            return QString();
    }
}

Qt::ItemFlags UserListModel::flags(const QModelIndex& index) const
{
    Qt::ItemFlags flags = Qt::NoItemFlags;

    QnUserResourcePtr user = d->user(index);
    if (!user)
        return flags;

    flags |= Qt::ItemIsSelectable | Qt::ItemIsEnabled;

    if (index.column() == CheckBoxColumn)
        flags |= Qt::ItemIsUserCheckable;

    return flags;
}

QHash<int, QByteArray> UserListModel::roleNames() const
{
    auto roleNames = base_type::roleNames();
    roleNames[Qt::CheckStateRole] = "checkState";
    roleNames[Qn::DecorationPathRole] = "decorationPath";
    return roleNames;
}

Qt::CheckState UserListModel::checkState() const
{
    return d->checkState();
}

void UserListModel::setCheckState(Qt::CheckState state, const QnUserResourcePtr& user)
{
    if (state == Qt::PartiallyChecked)
        return;

    auto roles = QVector<int>() << Qt::CheckStateRole << Qt::BackgroundRole << Qt::ForegroundRole;

    d->setCheckState(state, user);
    if (!user)
    {
        emit dataChanged(index(0, CheckBoxColumn), index(d->users.size() - 1, ColumnCount - 1), roles);
    }
    else
    {
        if (const auto it = d->users.find(user); it != d->users.end())
        {
            const auto row = d->users.index_of(it);
            emit dataChanged(index(row, CheckBoxColumn), index(row, ColumnCount - 1), roles);
        }
    }
}

int UserListModel::userRow(const QnUserResourcePtr& user) const
{
    if (const auto it = d->users.find(user); it != d->users.end())
        return d->users.index_of(it);

    return -1;
}

bool UserListModel::isUserEnabled(const QnUserResourcePtr& user) const
{
    if (!d->enableChangedUsers.contains(user))
        return user->isEnabled();

    return d->enableChangedUsers[user];
}

void UserListModel::setUserEnabled(const QnUserResourcePtr& user, bool enabled)
{
    NX_ASSERT(user->resourcePool());
    if (!user->resourcePool())
        return;

    d->enableChangedUsers[user] = enabled;
    d->handleUserChanged(user);
}

bool UserListModel::isDigestEnabled(const QnUserResourcePtr& user) const
{
    return d->digestChangedUsers.contains(user)
        ? d->digestChangedUsers[user]
        : user->shouldDigestAuthBeUsed();
}

void UserListModel::setDigestEnabled(const QnUserResourcePtr& user, bool enabled)
{
    NX_ASSERT(!user->isCloud());
    d->digestChangedUsers[user] = enabled;
}

QnUserResourceList UserListModel::users() const
{
    QnUserResourceList result;
    for (const auto& user: d->users)
        result << user;
    return result;
}

void UserListModel::resetUsers()
{
    d->resetUsers(resourcePool()->getResources<QnUserResource>());
}

void UserListModel::addUser(const QnUserResourcePtr& user)
{
    d->addUser(user);
}

void UserListModel::removeUser(const QnUserResourcePtr& user)
{
    d->removeUser(user);
}

bool UserListModel::contains(const QnUserResourcePtr& user) const
{
    return d->users.contains(user);
}

bool UserListModel::isInteractiveColumn(int column)
{
    return column == CheckBoxColumn;
}

SortedUserListModel::SortedUserListModel(QObject* parent): base_type(parent)
{
}

void SortedUserListModel::setDigestFilter(std::optional<bool> value)
{
    m_digestFilter = value;
    invalidateFilter();
}

void SortedUserListModel::setSyncId(const QString& syncId)
{
    m_syncId = syncId;
}

bool SortedUserListModel::lessThan(const QModelIndex& left, const QModelIndex& right) const
{
    const auto leftUser = left.data(Qn::UserResourceRole).value<QnUserResourcePtr>();
    const auto rightUser = right.data(Qn::UserResourceRole).value<QnUserResourcePtr>();

    if (!NX_ASSERT(rightUser))
        return true;

    if (!NX_ASSERT(leftUser))
        return false;

    switch (sortColumn())
    {
        case UserListModel::UserWarningColumn:
        {
            const bool l =
                leftUser->externalId().dn.isEmpty() || leftUser->externalId().syncId == m_syncId;
            const bool r =
                rightUser->externalId().dn.isEmpty() || rightUser->externalId().syncId == m_syncId;
            if (l != r)
                return r;

            break;
        }

        case UserListModel::UserTypeColumn:
        {
            const auto leftType = userTypeForSort(leftUser->userType());
            const auto rightType = userTypeForSort(rightUser->userType());
            if (leftType != rightType)
                return leftType < rightType;

            break;
        }

        case UserListModel::FullNameColumn:
        case UserListModel::EmailColumn:
        case UserListModel::UserGroupsColumn:
        {
            const QString leftText = left.data(Qt::DisplayRole).toString();
            const QString rightText = right.data(Qt::DisplayRole).toString();

            if (leftText != rightText)
                return leftText < rightText;

            break;
        }

        case UserListModel::IsCustomColumn:
        {
            const bool leftCustom = isCustomUser(leftUser);
            const bool rightCustom = isCustomUser(rightUser);
            if (leftCustom != rightCustom)
                return leftCustom;

            break;
        }

        default:
            break;
    }

    // Otherwise sort by login (which is unique):
    return nx::utils::naturalStringLess(leftUser->getName(), rightUser->getName());
}

bool SortedUserListModel::filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const
{
    QModelIndex sourceIndex = sourceModel()->index(sourceRow, 0, sourceParent);
    const auto user = sourceIndex.data(Qn::UserResourceRole).value<QnUserResourcePtr>();

    return (!m_digestFilter || m_digestFilter == user->shouldDigestAuthBeUsed())
        && base_type::filterAcceptsRow(sourceRow, sourceParent);
}

} // namespace nx::vms::client::desktop
