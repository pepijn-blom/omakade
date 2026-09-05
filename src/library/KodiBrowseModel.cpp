#include "library/KodiBrowseModel.h"

#include "library/GameRoles.h"

#include <QCryptographicHash>
#include <QUrl>

namespace {
QColor colorFor(const QString& id, int offset) {
  const QByteArray hash = QCryptographicHash::hash(id.toUtf8(), QCryptographicHash::Sha256);
  return QColor::fromHsl((static_cast<unsigned char>(hash.at(offset)) * 359) / 255, 115,
                         offset == 0 ? 105 : 72);
}

QString coverUrl(const QString& path) {
  if (path.isEmpty()) {
    return {};
  }
  if (path.startsWith(QStringLiteral("http://")) || path.startsWith(QStringLiteral("https://")) ||
      path.startsWith(QStringLiteral("image://")) || path.startsWith(QStringLiteral("file:"))) {
    return path;
  }
  return QUrl::fromLocalFile(path).toString();
}
} // namespace

KodiBrowseModel::KodiBrowseModel(QObject* parent) : QAbstractListModel(parent) {}

int KodiBrowseModel::rowCount(const QModelIndex& parent) const {
  return parent.isValid() ? 0 : visibleRows().size();
}

QVariant KodiBrowseModel::data(const QModelIndex& index, int role) const {
  const QVector<int> rows = visibleRows();
  if (!index.isValid() || index.row() < 0 || index.row() >= rows.size()) {
    return {};
  }
  return valueForRole(m_items.at(rows.at(index.row())), role);
}

QHash<int, QByteArray> KodiBrowseModel::roleNames() const {
  auto roles = GameRoles::names();
  roles.insert(GameRoles::LaunchTarget, "launchTarget");
  roles.insert(GameRoles::Installed, "installed");
  roles.insert(GameRoles::Description, "description");
  return roles;
}

void KodiBrowseModel::setSearchText(const QString& value) {
  if (m_searchText == value) {
    return;
  }
  beginResetModel();
  m_searchText = value;
  endResetModel();
  emit searchTextChanged();
}

void KodiBrowseModel::setItems(const QVector<KodiLibraryItem>& items) {
  beginResetModel();
  m_items = items;
  endResetModel();
}

void KodiBrowseModel::clear() {
  beginResetModel();
  m_items.clear();
  endResetModel();
}

KodiLibraryItem KodiBrowseModel::itemAt(int row) const {
  const QVector<int> rows = visibleRows();
  if (row < 0 || row >= rows.size()) {
    return {};
  }
  return m_items.at(rows.at(row));
}

KodiLibraryItem KodiBrowseModel::itemById(const QString& itemId) const {
  for (const KodiLibraryItem& item : m_items) {
    if (item.itemId == itemId) {
      return item;
    }
  }
  return {};
}

bool KodiBrowseModel::hasItem(const QString& itemId) const {
  return !itemById(itemId).itemId.isEmpty();
}

QVariantMap KodiBrowseModel::get(int row) const {
  const KodiLibraryItem item = itemAt(row);
  if (item.itemId.isEmpty()) {
    return {};
  }
  return mapForItem(item);
}

int KodiBrowseModel::indexOf(const QString& source, const QString& runner,
                             const QString& appId) const {
  Q_UNUSED(runner);
  if (source.compare(QStringLiteral("Kodi"), Qt::CaseInsensitive) != 0 || appId.isEmpty()) {
    return -1;
  }
  const QVector<int> rows = visibleRows();
  for (int row = 0; row < rows.size(); ++row) {
    if (m_items.at(rows.at(row)).itemId == appId) {
      return row;
    }
  }
  return -1;
}

void KodiBrowseModel::toggleFavorite(int row) { Q_UNUSED(row); }

QVariantList KodiBrowseModel::installations(int row) const {
  const QVariantMap game = get(row);
  return game.isEmpty() ? QVariantList{} : QVariantList{game};
}

QVector<int> KodiBrowseModel::visibleRows() const {
  QVector<int> rows;
  const QString needle = m_searchText.trimmed();
  for (int index = 0; index < m_items.size(); ++index) {
    if (needle.isEmpty() || m_items.at(index).title.contains(needle, Qt::CaseInsensitive) ||
        m_items.at(index).subtitle.contains(needle, Qt::CaseInsensitive)) {
      rows.append(index);
    }
  }
  return rows;
}

QVariant KodiBrowseModel::valueForRole(const KodiLibraryItem& item, int role) const {
  switch (role) {
  case GameRoles::Title:
    return item.title;
  case GameRoles::Subtitle:
    return item.subtitle;
  case GameRoles::Description:
    return item.plot.isEmpty() ? item.subtitle : item.plot;
  case GameRoles::Hours:
  case GameRoles::Progress:
  case GameRoles::AchievementsUnlocked:
  case GameRoles::AchievementsTotal:
    return 0;
  case GameRoles::Favorite:
  case GameRoles::Hidden:
    return false;
  case GameRoles::Recent:
    return item.lastPlayed > 0;
  case GameRoles::LastPlayed:
    return item.lastPlayed;
  case GameRoles::AccentStart:
    return colorFor(item.itemId, 0);
  case GameRoles::AccentEnd:
    return colorFor(item.itemId, 1);
  case GameRoles::CoverMark:
    return item.title.left(1).toUpper();
  case GameRoles::Year:
    return item.year;
  case GameRoles::AppId:
    return item.itemId;
  case GameRoles::CoverPath:
    return coverUrl(item.coverPath);
  case GameRoles::HeroPath:
  case GameRoles::LogoPath:
    return QString{};
  case GameRoles::InstallPath:
    return item.path;
  case GameRoles::Source:
    return QStringLiteral("Kodi");
  case GameRoles::Runner:
    return QString{};
  case GameRoles::LaunchTarget:
    return item.itemId;
  case GameRoles::Flatpak:
    return false;
  case GameRoles::Installed:
    return true;
  default:
    return {};
  }
}

QVariantMap KodiBrowseModel::mapForItem(const KodiLibraryItem& item) const {
  QVariantMap result;
  const auto roles = roleNames();
  for (auto iterator = roles.cbegin(); iterator != roles.cend(); ++iterator) {
    result.insert(QString::fromUtf8(iterator.value()), valueForRole(item, iterator.key()));
  }
  return result;
}
