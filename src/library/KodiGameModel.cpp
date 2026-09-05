#include "library/KodiGameModel.h"

#include "app/AppSettings.h"
#include "library/GameRoles.h"

#include <functional>

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSqlError>
#include <QSqlQuery>
#include <QUrl>
#include <QtConcurrent>

namespace {
QColor colorFor(const QString& id, int offset) {
  const QByteArray hash = QCryptographicHash::hash(id.toUtf8(), QCryptographicHash::Sha256);
  return QColor::fromHsl((static_cast<unsigned char>(hash.at(offset)) * 359) / 255, 115,
                         offset == 0 ? 105 : 72);
}

QString localUrl(const QString& path) {
  return path.isEmpty() ? QString{} : QUrl::fromLocalFile(path).toString();
}

QString defaultKodiUrl() {
  const QString envUrl = qEnvironmentVariable("OMAKADE_KODI_URL");
  if (!envUrl.isEmpty()) {
    return envUrl;
  }
  return QStringLiteral("http://127.0.0.1:8080/jsonrpc");
}

void rewriteArtwork(QVector<KodiLibraryItem>& items, const QString& jsonRpcUrl) {
  for (KodiLibraryItem& item : items) {
    item.coverPath = KodiScanner::httpArtworkUrl(jsonRpcUrl, item.coverPath);
  }
}
} // namespace

KodiGameModel::KodiGameModel(const QString& omakadeDatabasePath, AppSettings* settings,
                             QObject* parent)
    : QAbstractListModel(parent),
      m_connectionName(QStringLiteral("omakade-kodi-%1").arg(reinterpret_cast<quintptr>(this))),
      m_settings(settings), m_browseItems(this) {
  connect(&m_scanWatcher, &QFutureWatcher<KodiScanResult>::finished, this, [this] {
    m_scanning = false;
    applyScan(m_scanWatcher.result());
    emit statusChanged();
  });

  if (openDatabase(omakadeDatabasePath) && ensureSchema()) {
    loadDatabase();
    loadSourceState();
  }
  loadOfflineLibrary();
}

KodiGameModel::~KodiGameModel() {
  if (m_scanWatcher.isRunning()) {
    m_scanWatcher.waitForFinished();
  }
  m_database.close();
  m_database = {};
  QSqlDatabase::removeDatabase(m_connectionName);
}

int KodiGameModel::rowCount(const QModelIndex& parent) const {
  return parent.isValid() ? 0 : static_cast<int>(m_games.size());
}

QVariant KodiGameModel::data(const QModelIndex& index, int role) const {
  if (!index.isValid() || index.row() < 0 || index.row() >= m_games.size()) {
    return {};
  }
  return valueForRole(m_games.at(index.row()), role);
}

QHash<int, QByteArray> KodiGameModel::roleNames() const {
  auto roles = GameRoles::names();
  roles.insert(GameRoles::LaunchTarget, "launchTarget");
  roles.insert(GameRoles::Installed, "installed");
  return roles;
}

bool KodiGameModel::kodiDetected() const { return m_kodiDetected; }
QString KodiGameModel::statusText() const { return m_statusText; }
QString KodiGameModel::errorText() const { return m_errorText; }
QStringList KodiGameModel::detectedPaths() const { return m_detectedPaths; }
qint64 KodiGameModel::lastScan() const { return m_lastScan; }

QString KodiGameModel::browseTitle() const {
  if (m_browseStack.isEmpty()) {
    return {};
  }
  QStringList titles{QStringLiteral("Kodi")};
  for (const BrowseFrame& frame : m_browseStack) {
    titles.append(frame.title);
  }
  return titles.join(QStringLiteral(" / "));
}

void KodiGameModel::toggleFavorite(int row) {
  if (row < 0 || row >= m_games.size()) {
    return;
  }
  Game& game = m_games[row];
  game.favorite = !game.favorite;
  QSqlQuery query(m_database);
  query.prepare(QStringLiteral("UPDATE kodi_games SET favorite = ? WHERE game_id = ?"));
  query.addBindValue(game.favorite ? 1 : 0);
  query.addBindValue(game.kodi.sourceId);
  query.exec();
  emit dataChanged(index(row), index(row), {GameRoles::Favorite});
}

void KodiGameModel::toggleHidden(int row) {
  if (row < 0 || row >= m_games.size()) {
    return;
  }
  Game& game = m_games[row];
  game.hidden = !game.hidden;
  QSqlQuery query(m_database);
  query.prepare(QStringLiteral("UPDATE kodi_games SET hidden = ? WHERE game_id = ?"));
  query.addBindValue(game.hidden ? 1 : 0);
  query.addBindValue(game.kodi.sourceId);
  query.exec();
  emit dataChanged(index(row), index(row), {GameRoles::Hidden});
}

void KodiGameModel::refresh() {
  if (m_scanning) {
    return;
  }
  m_scanning = true;
  setStatus(QStringLiteral("Scanning Kodi sources..."));
  tryJsonRpcRefresh();
}

void KodiGameModel::refreshFromRoots(const QStringList& roots) {
  if (m_scanning) {
    return;
  }
  m_scanning = true;
  setStatus(QStringLiteral("Scanning Kodi sources..."));
  runLocalScan(roots);
}

void KodiGameModel::applyLibrarySnapshot(const KodiLibrarySnapshot& snapshot) {
  m_snapshot = snapshot;
  if (!m_browseStack.isEmpty()) {
    refreshBrowse();
  }
}

bool KodiGameModel::openIfContainer(const QString& appId) {
  if (appId.startsWith(QStringLiteral("kodi:episode:")) ||
      appId.startsWith(QStringLiteral("kodi:movie:")) ||
      appId.startsWith(QStringLiteral("kodi:file:"))) {
    return false;
  }
  if (const Game* source = sourceById(appId)) {
    return openSource(source->kodi);
  }
  const KodiLibraryItem current = m_browseItems.itemById(appId);
  if (current.itemId.isEmpty() || !current.container) {
    return false;
  }
  return pushItem(current);
}

bool KodiGameModel::goBack() {
  if (m_browseStack.isEmpty()) {
    return false;
  }
  m_browseStack.removeLast();
  if (m_browseStack.isEmpty()) {
    m_browseItems.clear();
    m_browseEmptyText.clear();
    emit browseChanged();
    return true;
  }
  const BrowseFrame& frame = m_browseStack.constLast();
  showItems(frame.items, m_browseEmptyText);
  emit browseChanged();
  return true;
}

void KodiGameModel::refreshBrowse() {
  if (m_browseStack.isEmpty()) {
    return;
  }
  BrowseFrame& frame = m_browseStack.last();
  frame.items = childrenFor(frame);
  showItems(frame.items, m_browseEmptyText);
  emit browseChanged();
}

bool KodiGameModel::openDatabase(const QString& path) {
  if (path.isEmpty()) {
    return false;
  }
  if (path != QStringLiteral(":memory:")) {
    QDir().mkpath(QFileInfo(path).absolutePath());
  }
  m_database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
  m_database.setDatabaseName(path);
  if (!m_database.open()) {
    setStatus(QStringLiteral("Could not open Kodi database cache"), m_database.lastError().text());
    return false;
  }
  return true;
}

bool KodiGameModel::ensureSchema() {
  QSqlQuery query(m_database);
  if (!query.exec(QStringLiteral(
          "CREATE TABLE IF NOT EXISTS kodi_games (game_id TEXT PRIMARY KEY, name TEXT NOT NULL, "
          "path TEXT NOT NULL, media_type TEXT, cover_path TEXT, last_played INTEGER NOT NULL "
          "DEFAULT 0, playtime_seconds INTEGER NOT NULL DEFAULT 0, flatpak INTEGER NOT NULL "
          "DEFAULT 0, flatpak_app_id TEXT, favorite INTEGER NOT NULL DEFAULT 0, hidden INTEGER "
          "NOT NULL DEFAULT 0, observed_at INTEGER NOT NULL)"))) {
    setStatus(QStringLiteral("Could not initialize Kodi cache"), query.lastError().text());
    return false;
  }
  if (!query.exec(QStringLiteral(
          "CREATE TABLE IF NOT EXISTS source_state (source TEXT PRIMARY KEY, last_scan INTEGER, "
          "last_error TEXT, paths TEXT NOT NULL DEFAULT '')"))) {
    setStatus(QStringLiteral("Could not initialize Kodi cache"), query.lastError().text());
    return false;
  }
  return true;
}

void KodiGameModel::loadDatabase() {
  QVector<Game> loaded;
  QSqlQuery query(m_database);
  if (!query.exec(QStringLiteral(
          "SELECT game_id, name, path, media_type, cover_path, last_played, playtime_seconds, "
          "flatpak, flatpak_app_id, favorite, hidden FROM kodi_games WHERE observed_at > 0 "
          "ORDER BY name COLLATE NOCASE"))) {
    setStatus(QStringLiteral("Could not load cached Kodi games"), query.lastError().text());
    return;
  }
  while (query.next()) {
    KodiSourceRecord record{.sourceId = query.value(0).toString(),
                            .title = query.value(1).toString(),
                            .path = query.value(2).toString(),
                            .mediaType = query.value(3).toString(),
                            .coverPath = query.value(4).toString(),
                            .lastPlayed = query.value(5).toLongLong(),
                            .playtimeSeconds = query.value(6).toLongLong(),
                            .flatpak = query.value(7).toBool(),
                            .flatpakAppId = query.value(8).toString()};
    loaded.append({.kodi = record,
                   .favorite = query.value(9).toBool(),
                   .hidden = query.value(10).toBool(),
                   .accentStart = colorFor(record.sourceId, 0),
                   .accentEnd = colorFor(record.sourceId, 1)});
  }
  beginResetModel();
  m_games = loaded;
  endResetModel();
}

void KodiGameModel::loadSourceState() {
  QSqlQuery query(m_database);
  query.prepare(QStringLiteral(
      "SELECT last_scan, last_error, paths FROM source_state WHERE source = 'kodi'"));
  if (!query.exec() || !query.next()) {
    return;
  }
  m_lastScan = query.value(0).toLongLong();
  m_errorText = query.value(1).toString();
  m_detectedPaths = query.value(2).toString().split(QLatin1Char('\n'), Qt::SkipEmptyParts);
  m_kodiDetected = !m_detectedPaths.isEmpty();
  if (m_lastScan > 0) {
    m_statusText = QStringLiteral("Loaded cached Kodi sources");
  }
}

void KodiGameModel::tryJsonRpcRefresh() {
  const QString urlString = (m_settings != nullptr && !m_settings->kodiUrl().isEmpty())
                                ? m_settings->kodiUrl()
                                : defaultKodiUrl();
  const QUrl url(urlString);
  if (!url.isValid() || url.scheme().isEmpty()) {
    runLocalScan(KodiScanner::discoverRoots());
    return;
  }

  const QStringList mediaCategories = {QStringLiteral("video"), QStringLiteral("music"),
                                       QStringLiteral("pictures"), QStringLiteral("files"),
                                       QStringLiteral("programs")};
  QJsonArray batch;
  int requestId = 1;
  for (const QString& category : mediaCategories) {
    QJsonObject request;
    request.insert(QStringLiteral("jsonrpc"), QStringLiteral("2.0"));
    request.insert(QStringLiteral("method"), QStringLiteral("Files.GetSources"));
    QJsonObject params;
    params.insert(QStringLiteral("media"), category);
    request.insert(QStringLiteral("params"), params);
    request.insert(QStringLiteral("id"), requestId++);
    batch.append(request);
  }

  QJsonObject shows;
  shows.insert(QStringLiteral("jsonrpc"), QStringLiteral("2.0"));
  shows.insert(QStringLiteral("method"), QStringLiteral("VideoLibrary.GetTVShows"));
  QJsonObject showParams;
  showParams.insert(
      QStringLiteral("properties"),
      QJsonArray({QStringLiteral("title"), QStringLiteral("thumbnail"), QStringLiteral("art"),
                  QStringLiteral("year"), QStringLiteral("plot"), QStringLiteral("file"),
                  QStringLiteral("episode")}));
  shows.insert(QStringLiteral("params"), showParams);
  shows.insert(QStringLiteral("id"), requestId++);
  batch.append(shows);

  QJsonObject movies;
  movies.insert(QStringLiteral("jsonrpc"), QStringLiteral("2.0"));
  movies.insert(QStringLiteral("method"), QStringLiteral("VideoLibrary.GetMovies"));
  QJsonObject movieParams;
  movieParams.insert(
      QStringLiteral("properties"),
      QJsonArray({QStringLiteral("title"), QStringLiteral("thumbnail"), QStringLiteral("art"),
                  QStringLiteral("year"), QStringLiteral("plot"), QStringLiteral("file")}));
  movies.insert(QStringLiteral("params"), movieParams);
  movies.insert(QStringLiteral("id"), requestId++);
  batch.append(movies);

  QNetworkRequest request(url);
  request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
  request.setTransferTimeout(3000);
  QNetworkReply* reply =
      m_network.post(request, QJsonDocument(batch).toJson(QJsonDocument::Compact));
  connect(reply, &QNetworkReply::finished, this, [this, reply, mediaCategories, urlString] {
    reply->deleteLater();
    if (reply->error() == QNetworkReply::NoError) {
      const QByteArray data = reply->readAll();
      const QJsonDocument document = QJsonDocument::fromJson(data);
      QVector<KodiSourceRecord> allSources;
      KodiLibrarySnapshot snapshot;
      if (document.isArray()) {
        const QJsonArray responses = document.array();
        for (int index = 0; index < responses.size(); ++index) {
          const QJsonObject single = responses.at(index).toObject();
          const QByteArray payload = QJsonDocument(single).toJson();
          if (index < mediaCategories.size()) {
            allSources.append(
                KodiScanner::parseJsonRpcSources(payload, mediaCategories.at(index), false));
          } else if (single.value(QStringLiteral("result"))
                         .toObject()
                         .contains(QStringLiteral("tvshows"))) {
            snapshot.tvShows = KodiScanner::parseTvShows(payload);
            rewriteArtwork(snapshot.tvShows, urlString);
          } else if (single.value(QStringLiteral("result"))
                         .toObject()
                         .contains(QStringLiteral("movies"))) {
            snapshot.movies = KodiScanner::parseMovies(payload);
            rewriteArtwork(snapshot.movies, urlString);
          }
        }
      }
      if (!snapshot.tvShows.isEmpty() || !snapshot.movies.isEmpty()) {
        applyLibrarySnapshot(snapshot);
      }
      if (!allSources.isEmpty()) {
        m_scanning = false;
        KodiScanResult result;
        result.sources = allSources;
        result.roots = KodiScanner::discoverRoots();
        if (result.roots.isEmpty()) {
          result.roots.append(QStringLiteral("api:") + defaultKodiUrl());
        }
        applyScan(result);
        setStatus(
            QStringLiteral("Connected to Kodi API: imported %1 source(s)").arg(allSources.size()));
        return;
      }
    }
    runLocalScan(KodiScanner::discoverRoots());
  });
}

void KodiGameModel::runLocalScan(const QStringList& roots) {
  m_scanWatcher.setFuture(QtConcurrent::run([roots] { return KodiScanner::scanLocal(roots); }));
}

void KodiGameModel::loadOfflineLibrary() {
  if (!m_snapshot.tvShows.isEmpty() || !m_snapshot.movies.isEmpty()) {
    return;
  }
  const QStringList roots =
      m_detectedPaths.isEmpty() ? KodiScanner::discoverRoots() : m_detectedPaths;
  for (const QString& root : roots) {
    const QStringList databases = KodiScanner::findMyVideosDatabases(root);
    if (databases.isEmpty()) {
      continue;
    }
    applyLibrarySnapshot(KodiScanner::readMyVideos(databases.constLast()));
    if (!m_snapshot.tvShows.isEmpty() || !m_snapshot.movies.isEmpty()) {
      return;
    }
  }
}

void KodiGameModel::applyScan(const KodiScanResult& result) {
  m_kodiDetected = !result.roots.isEmpty() || !result.sources.isEmpty();
  if (result.incomplete ||
      (result.roots.isEmpty() && result.sources.isEmpty() && !m_games.isEmpty())) {
    setStatus(QStringLiteral("Kodi scan interrupted; kept the cached library"),
              result.warnings.join(QLatin1Char('\n')));
    return;
  }
  if (!m_database.transaction()) {
    setStatus(QStringLiteral("Could not update Kodi sources"), m_database.lastError().text());
    return;
  }
  const qint64 scanTimestamp = QDateTime::currentSecsSinceEpoch();
  QSqlQuery query(m_database);
  bool okay = query.exec(QStringLiteral("UPDATE kodi_games SET observed_at = 0"));
  for (const KodiSourceRecord& source : result.sources) {
    query.prepare(QStringLiteral(
        "INSERT INTO kodi_games(game_id, name, path, media_type, cover_path, last_played, "
        "playtime_seconds, flatpak, flatpak_app_id, observed_at) VALUES(?, ?, ?, ?, ?, ?, ?, ?, "
        "?, strftime('%s', 'now')) ON CONFLICT(game_id) DO UPDATE SET name = excluded.name, path "
        "= excluded.path, media_type = excluded.media_type, cover_path = excluded.cover_path, "
        "last_played = excluded.last_played, playtime_seconds = excluded.playtime_seconds, "
        "flatpak = excluded.flatpak, flatpak_app_id = excluded.flatpak_app_id, observed_at = "
        "excluded.observed_at"));
    query.addBindValue(source.sourceId);
    query.addBindValue(source.title);
    query.addBindValue(source.path);
    query.addBindValue(source.mediaType);
    query.addBindValue(source.coverPath);
    query.addBindValue(source.lastPlayed);
    query.addBindValue(source.playtimeSeconds);
    query.addBindValue(source.flatpak ? 1 : 0);
    query.addBindValue(source.flatpakAppId.isNull() ? QStringLiteral("") : source.flatpakAppId);
    okay = okay && query.exec();
  }
  query.prepare(QStringLiteral(
      "INSERT INTO source_state(source, last_scan, last_error, paths) VALUES('kodi', "
      "?, ?, ?) ON CONFLICT(source) DO UPDATE SET last_scan = "
      "excluded.last_scan, last_error = excluded.last_error, paths = excluded.paths"));
  query.addBindValue(scanTimestamp);
  query.addBindValue(result.warnings.join(QLatin1Char('\n')));
  query.addBindValue(result.roots.isEmpty() ? QStringLiteral("")
                                            : result.roots.join(QLatin1Char('\n')));
  okay = okay && query.exec();
  if (!okay || !m_database.commit()) {
    m_database.rollback();
    setStatus(QStringLiteral("Could not update Kodi sources"), query.lastError().text());
    return;
  }
  loadDatabase();
  m_detectedPaths = result.roots;
  m_lastScan = scanTimestamp;
  loadOfflineLibrary();
  setStatus(m_kodiDetected ? QStringLiteral("Imported %1 Kodi source(s)").arg(result.sources.size())
                           : QStringLiteral("Kodi was not found"),
            result.warnings.join(QLatin1Char('\n')));
}

const KodiGameModel::Game* KodiGameModel::sourceById(const QString& appId) const {
  for (const Game& game : m_games) {
    if (game.kodi.sourceId == appId) {
      return &game;
    }
  }
  return nullptr;
}

bool KodiGameModel::openSource(const KodiSourceRecord& source) {
  const bool series = KodiScanner::isSeriesLike(source.title, source.path, source.mediaType);
  const bool movies = KodiScanner::isMovieLike(source.title, source.path, source.mediaType);
  if (source.mediaType.compare(QStringLiteral("video"), Qt::CaseInsensitive) != 0 && !series &&
      !movies) {
    return false;
  }

  BrowseFrame frame;
  frame.title = source.title;
  frame.itemId = source.sourceId;
  frame.path = source.path;
  frame.kind = KodiItemKind::Source;
  if (series || (!movies && !m_snapshot.tvShows.isEmpty()) ||
      (!movies && !m_snapshot.directories.isEmpty())) {
    const QVector<KodiLibraryItem> shows = KodiScanner::tvShowsForSource(m_snapshot, source.path);
    frame.items = !shows.isEmpty() ? shows : m_snapshot.directories;
    if (frame.items.isEmpty() && m_snapshot.childrenByPath.contains(source.path)) {
      frame.items = m_snapshot.childrenByPath.value(source.path);
    }
    m_browseEmptyText =
        m_snapshot.tvShows.isEmpty() && m_snapshot.directories.isEmpty()
            ? QStringLiteral("Kodi has no TV shows for this source yet. Scrape the share in "
                             "Kodi, or keep Kodi running so Omakade can list folders.")
            : QStringLiteral("No series match this source.");
  } else {
    frame.items = m_snapshot.movies;
    if (frame.items.isEmpty() && m_snapshot.childrenByPath.contains(source.path)) {
      frame.items = m_snapshot.childrenByPath.value(source.path);
    }
    m_browseEmptyText =
        QStringLiteral("No movies found for this source. Scrape the library in Kodi first.");
  }
  m_browseStack = {frame};
  showItems(frame.items, m_browseEmptyText);
  emit browseChanged();
  if (frame.items.isEmpty()) {
    requestLibraryChildren(frame);
  }
  return true;
}

bool KodiGameModel::pushItem(const KodiLibraryItem& item) {
  BrowseFrame frame;
  frame.title = item.title;
  frame.itemId = item.itemId;
  frame.path = item.path;
  frame.kind = item.kind;
  frame.tvshowId = item.tvshowId;
  frame.season = item.season;
  frame.items = childrenFor(frame);
  if (item.kind == KodiItemKind::TvShow) {
    m_browseEmptyText = QStringLiteral("No seasons found for this series.");
  } else if (item.kind == KodiItemKind::Season) {
    m_browseEmptyText = QStringLiteral("No episodes found for this season.");
  } else {
    m_browseEmptyText = QStringLiteral("Nothing in this folder.");
  }
  if (frame.items.isEmpty() && !item.container) {
    return false;
  }
  m_browseStack.append(frame);
  showItems(frame.items, m_browseEmptyText);
  emit browseChanged();
  if (frame.items.isEmpty()) {
    requestLibraryChildren(frame);
  }
  return true;
}

void KodiGameModel::showItems(const QVector<KodiLibraryItem>& items, const QString& emptyText) {
  m_browseEmptyText = items.isEmpty() ? emptyText : QString{};
  m_browseItems.setItems(items);
}

QVector<KodiLibraryItem> KodiGameModel::childrenFor(const BrowseFrame& frame) const {
  if (frame.kind == KodiItemKind::TvShow) {
    return m_snapshot.seasonsByShow.value(frame.tvshowId);
  }
  if (frame.kind == KodiItemKind::Season) {
    return m_snapshot.episodesBySeason.value(KodiScanner::seasonKey(frame.tvshowId, frame.season));
  }
  if (frame.kind == KodiItemKind::Directory || frame.kind == KodiItemKind::Source) {
    if (m_snapshot.childrenByPath.contains(frame.path)) {
      return m_snapshot.childrenByPath.value(frame.path);
    }
    if (frame.kind == KodiItemKind::Source) {
      const QVector<KodiLibraryItem> shows = KodiScanner::tvShowsForSource(m_snapshot, frame.path);
      return !shows.isEmpty() ? shows : m_snapshot.directories;
    }
  }
  return {};
}

void KodiGameModel::postJson(const QJsonObject& body,
                             const std::function<void(QByteArray)>& finished) {
  const QString urlString = (m_settings != nullptr && !m_settings->kodiUrl().isEmpty())
                                ? m_settings->kodiUrl()
                                : defaultKodiUrl();
  const QUrl url(urlString);
  if (!url.isValid()) {
    return;
  }
  QNetworkRequest request(url);
  request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
  request.setTransferTimeout(4000);
  QNetworkReply* reply =
      m_network.post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
  connect(reply, &QNetworkReply::finished, this, [reply, finished] {
    reply->deleteLater();
    if (reply->error() == QNetworkReply::NoError) {
      finished(reply->readAll());
    }
  });
}

void KodiGameModel::requestLibraryChildren(const BrowseFrame& frame) {
  if (frame.kind == KodiItemKind::TvShow && frame.tvshowId > 0) {
    QJsonObject request;
    request.insert(QStringLiteral("jsonrpc"), QStringLiteral("2.0"));
    request.insert(QStringLiteral("method"), QStringLiteral("VideoLibrary.GetSeasons"));
    QJsonObject params;
    params.insert(QStringLiteral("tvshowid"), frame.tvshowId);
    params.insert(QStringLiteral("properties"),
                  QJsonArray({QStringLiteral("season"), QStringLiteral("episode"),
                              QStringLiteral("thumbnail"), QStringLiteral("art"),
                              QStringLiteral("showtitle"), QStringLiteral("tvshowid")}));
    request.insert(QStringLiteral("params"), params);
    request.insert(QStringLiteral("id"), 1);
    const int tvshowId = frame.tvshowId;
    postJson(request, [this, tvshowId](const QByteArray& data) {
      QVector<KodiLibraryItem> seasons = KodiScanner::parseSeasons(data, tvshowId);
      rewriteArtwork(seasons, (m_settings != nullptr && !m_settings->kodiUrl().isEmpty())
                                  ? m_settings->kodiUrl()
                                  : defaultKodiUrl());
      m_snapshot.seasonsByShow[tvshowId] = seasons;
      refreshBrowse();
    });
    return;
  }
  if (frame.kind == KodiItemKind::Season && frame.tvshowId > 0) {
    QJsonObject request;
    request.insert(QStringLiteral("jsonrpc"), QStringLiteral("2.0"));
    request.insert(QStringLiteral("method"), QStringLiteral("VideoLibrary.GetEpisodes"));
    QJsonObject params;
    params.insert(QStringLiteral("tvshowid"), frame.tvshowId);
    params.insert(QStringLiteral("season"), frame.season);
    params.insert(
        QStringLiteral("properties"),
        QJsonArray({QStringLiteral("title"), QStringLiteral("plot"), QStringLiteral("episode"),
                    QStringLiteral("season"), QStringLiteral("thumbnail"), QStringLiteral("file"),
                    QStringLiteral("tvshowid"), QStringLiteral("episodeid")}));
    request.insert(QStringLiteral("params"), params);
    request.insert(QStringLiteral("id"), 1);
    const int tvshowId = frame.tvshowId;
    const int season = frame.season;
    postJson(request, [this, tvshowId, season](const QByteArray& data) {
      QVector<KodiLibraryItem> episodes = KodiScanner::parseEpisodes(data);
      rewriteArtwork(episodes, (m_settings != nullptr && !m_settings->kodiUrl().isEmpty())
                                   ? m_settings->kodiUrl()
                                   : defaultKodiUrl());
      m_snapshot.episodesBySeason[KodiScanner::seasonKey(tvshowId, season)] = episodes;
      refreshBrowse();
    });
    return;
  }
  if ((frame.kind == KodiItemKind::Source || frame.kind == KodiItemKind::Directory) &&
      !frame.path.isEmpty()) {
    QJsonObject request;
    request.insert(QStringLiteral("jsonrpc"), QStringLiteral("2.0"));
    request.insert(QStringLiteral("method"), QStringLiteral("Files.GetDirectory"));
    QJsonObject params;
    params.insert(QStringLiteral("directory"), frame.path);
    params.insert(QStringLiteral("media"), QStringLiteral("video"));
    request.insert(QStringLiteral("params"), params);
    request.insert(QStringLiteral("id"), 1);
    const QString path = frame.path;
    postJson(request, [this, path](const QByteArray& data) {
      QVector<KodiLibraryItem> items = KodiScanner::parseDirectory(data);
      rewriteArtwork(items, (m_settings != nullptr && !m_settings->kodiUrl().isEmpty())
                                ? m_settings->kodiUrl()
                                : defaultKodiUrl());
      if (m_snapshot.tvShows.isEmpty() && m_snapshot.directories.isEmpty()) {
        m_snapshot.directories = items;
      }
      m_snapshot.childrenByPath[path] = items;
      refreshBrowse();
    });
  }
}

QVariant KodiGameModel::valueForRole(const Game& game, int role) const {
  switch (role) {
  case GameRoles::Title:
    return game.kodi.title;
  case GameRoles::Subtitle:
    return QStringLiteral("Kodi (%1)").arg(game.kodi.mediaType);
  case GameRoles::Description:
    return QStringLiteral("Kodi %1 source at %2.").arg(game.kodi.mediaType, game.kodi.path);
  case GameRoles::Hours:
    return static_cast<int>(game.kodi.playtimeSeconds / 3600);
  case GameRoles::Progress:
  case GameRoles::AchievementsUnlocked:
  case GameRoles::AchievementsTotal:
    return 0;
  case GameRoles::Favorite:
    return game.favorite;
  case GameRoles::Recent:
    return game.kodi.lastPlayed > 0;
  case GameRoles::LastPlayed:
    return game.kodi.lastPlayed;
  case GameRoles::AccentStart:
    return game.accentStart;
  case GameRoles::AccentEnd:
    return game.accentEnd;
  case GameRoles::CoverMark:
    return game.kodi.title.left(1).toUpper();
  case GameRoles::Year:
    return 0;
  case GameRoles::AppId:
    return game.kodi.sourceId;
  case GameRoles::CoverPath:
    return localUrl(game.kodi.coverPath);
  case GameRoles::HeroPath:
  case GameRoles::LogoPath:
    return QString{};
  case GameRoles::InstallPath:
    return game.kodi.path;
  case GameRoles::Source:
    return QStringLiteral("Kodi");
  case GameRoles::Runner:
    return game.kodi.flatpakAppId;
  case GameRoles::LaunchTarget:
    return game.kodi.path;
  case GameRoles::Flatpak:
    return game.kodi.flatpak;
  case GameRoles::Hidden:
    return game.hidden;
  case GameRoles::Installed:
    return true;
  default:
    return {};
  }
}

void KodiGameModel::setStatus(const QString& status, const QString& error) {
  m_statusText = status;
  m_errorText = error;
  emit statusChanged();
}
