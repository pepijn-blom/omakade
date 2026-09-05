#include "sources/kodi/KodiScanner.h"

#include "sources/FlatpakInstall.h"

#include <algorithm>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QUrl>
#include <QXmlStreamReader>

namespace {
QString artworkUrl(const QJsonObject& object) {
  const QString thumbnail = object.value(QStringLiteral("thumbnail")).toString().trimmed();
  if (!thumbnail.isEmpty()) {
    return thumbnail;
  }
  const QJsonObject art = object.value(QStringLiteral("art")).toObject();
  const QString poster = art.value(QStringLiteral("poster")).toString().trimmed();
  if (!poster.isEmpty()) {
    return poster;
  }
  return art.value(QStringLiteral("thumb")).toString().trimmed();
}

QString normalizePath(const QString& path) {
  QString value = path.trimmed();
  while (value.endsWith(QLatin1Char('/')) && value.size() > 1) {
    value.chop(1);
  }
  return value;
}

bool pathBelongsToSource(const QString& itemPath, const QString& sourcePath) {
  if (sourcePath.isEmpty() || itemPath.isEmpty()) {
    return true;
  }
  const QString item = normalizePath(itemPath);
  const QString source = normalizePath(sourcePath);
  return item.compare(source, Qt::CaseInsensitive) == 0 ||
         item.startsWith(source + QLatin1Char('/'), Qt::CaseInsensitive);
}

bool matchesKeyword(const QString& title, const QString& path, const QStringList& keywords) {
  const QString haystack = (title + QLatin1Char(' ') + path).toLower();
  for (const QString& keyword : keywords) {
    if (haystack.contains(keyword)) {
      return true;
    }
  }
  return false;
}

QJsonObject resultObject(const QByteArray& jsonResponse) {
  const QJsonDocument document = QJsonDocument::fromJson(jsonResponse);
  if (!document.isObject()) {
    return {};
  }
  return document.object().value(QStringLiteral("result")).toObject();
}
} // namespace

QString KodiScanner::generateSourceId(const QString& mediaType, const QString& path,
                                      const QString& title) {
  const QByteArray hash =
      QCryptographicHash::hash((path + QLatin1Char(':') + title).toUtf8(), QCryptographicHash::Sha1)
          .toHex();
  return QStringLiteral("kodi:%1:%2").arg(mediaType, QString::fromUtf8(hash));
}

QString KodiScanner::seasonKey(int tvshowId, int season) {
  return QStringLiteral("%1:%2").arg(tvshowId).arg(season);
}

QStringList KodiScanner::discoverRoots() {
  QStringList roots;
  const QString nativeUserdata = QDir::homePath() + QStringLiteral("/.kodi/userdata");
  const QString flatpakUserdata =
      QDir::homePath() + QStringLiteral("/.var/app/tv.kodi.Kodi/data/userdata");

  if (QFileInfo::exists(nativeUserdata + QStringLiteral("/sources.xml")) ||
      QDir(nativeUserdata).exists()) {
    roots.append(nativeUserdata);
  } else if (!QStandardPaths::findExecutable(QStringLiteral("kodi")).isEmpty()) {
    roots.append(nativeUserdata);
  }

  if (QFileInfo::exists(flatpakUserdata + QStringLiteral("/sources.xml")) ||
      QDir(flatpakUserdata).exists()) {
    roots.append(flatpakUserdata);
  } else if (flatpakAppInstalled(QStringLiteral("tv.kodi.Kodi"))) {
    roots.append(flatpakUserdata);
  }

  roots.removeDuplicates();
  return roots;
}

QVector<KodiSourceRecord> KodiScanner::parseSourcesXml(const QString& xmlContent,
                                                       const QString& rootPath, bool flatpak) {
  Q_UNUSED(rootPath);
  QVector<KodiSourceRecord> sources;
  QXmlStreamReader reader(xmlContent);

  QString currentMedia;
  QString currentName;
  QString currentPath;

  while (!reader.atEnd()) {
    reader.readNext();
    if (reader.isStartElement()) {
      const QString name = reader.name().toString();
      if (name == QStringLiteral("sources")) {
        continue;
      }
      if (currentMedia.isEmpty()) {
        currentMedia = name;
      } else if (name == QStringLiteral("source")) {
        currentName.clear();
        currentPath.clear();
      } else if (name == QStringLiteral("name")) {
        currentName = reader.readElementText().trimmed();
      } else if (name == QStringLiteral("path")) {
        currentPath = reader.readElementText().trimmed();
        if (!currentPath.isEmpty()) {
          const QString title = currentName.isEmpty() ? currentPath : currentName;
          KodiSourceRecord record;
          record.sourceId = generateSourceId(currentMedia, currentPath, title);
          record.title = title;
          record.path = currentPath;
          record.mediaType = currentMedia;
          record.flatpak = flatpak;
          record.flatpakAppId = flatpak ? QStringLiteral("tv.kodi.Kodi") : QString{};
          sources.append(record);
        }
      }
    } else if (reader.isEndElement()) {
      if (reader.name().toString() == currentMedia) {
        currentMedia.clear();
      }
    }
  }
  return sources;
}

QVector<KodiSourceRecord> KodiScanner::parseJsonRpcSources(const QByteArray& jsonResponse,
                                                           const QString& mediaType, bool flatpak) {
  QVector<KodiSourceRecord> sources;
  const QJsonArray sourcesArray =
      resultObject(jsonResponse).value(QStringLiteral("sources")).toArray();
  for (const QJsonValue& value : sourcesArray) {
    if (!value.isObject()) {
      continue;
    }
    const QJsonObject object = value.toObject();
    const QString file = object.value(QStringLiteral("file")).toString().trimmed();
    if (file.isEmpty()) {
      continue;
    }
    QString label = object.value(QStringLiteral("label")).toString().trimmed();
    if (label.isEmpty()) {
      label = file;
    }
    KodiSourceRecord record;
    record.sourceId = generateSourceId(mediaType, file, label);
    record.title = label;
    record.path = file;
    record.mediaType = mediaType;
    record.flatpak = flatpak;
    record.flatpakAppId = flatpak ? QStringLiteral("tv.kodi.Kodi") : QString{};
    sources.append(record);
  }
  return sources;
}

KodiScanResult KodiScanner::scanLocal(const QStringList& roots) {
  KodiScanResult result;
  result.roots = roots;
  for (const QString& root : roots) {
    const QString sourcesPath = root.endsWith(QStringLiteral("/sources.xml"))
                                    ? root
                                    : root + QStringLiteral("/sources.xml");
    QFile file(sourcesPath);
    if (!file.exists() || !file.open(QIODevice::ReadOnly | QIODevice::Text)) {
      continue;
    }
    const bool isFlatpak = root.contains(QStringLiteral("tv.kodi.Kodi"));
    result.sources.append(parseSourcesXml(QString::fromUtf8(file.readAll()), root, isFlatpak));
  }
  return result;
}

QVector<KodiLibraryItem> KodiScanner::parseTvShows(const QByteArray& jsonResponse) {
  QVector<KodiLibraryItem> shows;
  const QJsonArray array = resultObject(jsonResponse).value(QStringLiteral("tvshows")).toArray();
  for (const QJsonValue& value : array) {
    if (!value.isObject()) {
      continue;
    }
    const QJsonObject object = value.toObject();
    const int tvshowId = object.value(QStringLiteral("tvshowid")).toInt();
    if (tvshowId <= 0) {
      continue;
    }
    KodiLibraryItem item;
    item.kind = KodiItemKind::TvShow;
    item.tvshowId = tvshowId;
    item.itemId = QStringLiteral("kodi:tvshow:%1").arg(tvshowId);
    item.title = object.value(QStringLiteral("title")).toString().trimmed();
    if (item.title.isEmpty()) {
      item.title = object.value(QStringLiteral("label")).toString().trimmed();
    }
    item.plot = object.value(QStringLiteral("plot")).toString().trimmed();
    item.path = object.value(QStringLiteral("file")).toString().trimmed();
    item.coverPath = artworkUrl(object);
    item.year = object.value(QStringLiteral("year")).toInt();
    item.episodeCount = object.value(QStringLiteral("episode")).toInt();
    item.subtitle =
        item.year > 0 ? QStringLiteral("Kodi · %1").arg(item.year) : QStringLiteral("Kodi TV show");
    item.container = true;
    shows.append(item);
  }
  return shows;
}

QVector<KodiLibraryItem> KodiScanner::parseSeasons(const QByteArray& jsonResponse, int tvshowId) {
  QVector<KodiLibraryItem> seasons;
  const QJsonArray array = resultObject(jsonResponse).value(QStringLiteral("seasons")).toArray();
  for (const QJsonValue& value : array) {
    if (!value.isObject()) {
      continue;
    }
    const QJsonObject object = value.toObject();
    const int season = object.value(QStringLiteral("season")).toInt(-1);
    if (season < 0) {
      continue;
    }
    const int showId = object.value(QStringLiteral("tvshowid")).toInt(tvshowId);
    KodiLibraryItem item;
    item.kind = KodiItemKind::Season;
    item.tvshowId = showId;
    item.season = season;
    item.itemId = QStringLiteral("kodi:season:%1:%2").arg(showId).arg(season);
    item.title = object.value(QStringLiteral("label")).toString().trimmed();
    if (item.title.isEmpty()) {
      item.title =
          season == 0 ? QStringLiteral("Specials") : QStringLiteral("Season %1").arg(season);
    }
    const QString showTitle = object.value(QStringLiteral("showtitle")).toString().trimmed();
    item.subtitle = showTitle.isEmpty() ? QStringLiteral("Kodi season")
                                        : QStringLiteral("%1 · Season").arg(showTitle);
    item.coverPath = artworkUrl(object);
    item.episodeCount = object.value(QStringLiteral("episode")).toInt();
    item.container = true;
    seasons.append(item);
  }
  return seasons;
}

QVector<KodiLibraryItem> KodiScanner::parseEpisodes(const QByteArray& jsonResponse) {
  QVector<KodiLibraryItem> episodes;
  const QJsonArray array = resultObject(jsonResponse).value(QStringLiteral("episodes")).toArray();
  for (const QJsonValue& value : array) {
    if (!value.isObject()) {
      continue;
    }
    const QJsonObject object = value.toObject();
    const int episodeId = object.value(QStringLiteral("episodeid")).toInt();
    if (episodeId <= 0) {
      continue;
    }
    KodiLibraryItem item;
    item.kind = KodiItemKind::Episode;
    item.episodeId = episodeId;
    item.tvshowId = object.value(QStringLiteral("tvshowid")).toInt();
    item.season = object.value(QStringLiteral("season")).toInt();
    item.episodeNumber = object.value(QStringLiteral("episode")).toInt();
    item.itemId = QStringLiteral("kodi:episode:%1").arg(episodeId);
    item.title = object.value(QStringLiteral("title")).toString().trimmed();
    if (item.title.isEmpty()) {
      item.title = object.value(QStringLiteral("label")).toString().trimmed();
    }
    item.plot = object.value(QStringLiteral("plot")).toString().trimmed();
    item.path = object.value(QStringLiteral("file")).toString().trimmed();
    item.coverPath = artworkUrl(object);
    item.subtitle = QStringLiteral("S%1E%2")
                        .arg(item.season, 2, 10, QLatin1Char('0'))
                        .arg(item.episodeNumber, 2, 10, QLatin1Char('0'));
    item.container = false;
    episodes.append(item);
  }
  return episodes;
}

QVector<KodiLibraryItem> KodiScanner::parseMovies(const QByteArray& jsonResponse) {
  QVector<KodiLibraryItem> movies;
  const QJsonArray array = resultObject(jsonResponse).value(QStringLiteral("movies")).toArray();
  for (const QJsonValue& value : array) {
    if (!value.isObject()) {
      continue;
    }
    const QJsonObject object = value.toObject();
    const int movieId = object.value(QStringLiteral("movieid")).toInt();
    if (movieId <= 0) {
      continue;
    }
    KodiLibraryItem item;
    item.kind = KodiItemKind::Movie;
    item.movieId = movieId;
    item.itemId = QStringLiteral("kodi:movie:%1").arg(movieId);
    item.title = object.value(QStringLiteral("title")).toString().trimmed();
    if (item.title.isEmpty()) {
      item.title = object.value(QStringLiteral("label")).toString().trimmed();
    }
    item.plot = object.value(QStringLiteral("plot")).toString().trimmed();
    item.path = object.value(QStringLiteral("file")).toString().trimmed();
    item.coverPath = artworkUrl(object);
    item.year = object.value(QStringLiteral("year")).toInt();
    item.subtitle =
        item.year > 0 ? QStringLiteral("Kodi · %1").arg(item.year) : QStringLiteral("Kodi movie");
    item.container = false;
    movies.append(item);
  }
  return movies;
}

QVector<KodiLibraryItem> KodiScanner::parseDirectory(const QByteArray& jsonResponse) {
  QVector<KodiLibraryItem> items;
  const QJsonArray array = resultObject(jsonResponse).value(QStringLiteral("files")).toArray();
  for (const QJsonValue& value : array) {
    if (!value.isObject()) {
      continue;
    }
    const QJsonObject object = value.toObject();
    const QString file = object.value(QStringLiteral("file")).toString().trimmed();
    if (file.isEmpty()) {
      continue;
    }
    const QString fileType = object.value(QStringLiteral("filetype")).toString().trimmed();
    const bool folder = fileType.compare(QStringLiteral("directory"), Qt::CaseInsensitive) == 0;
    KodiLibraryItem item;
    item.kind = folder ? KodiItemKind::Directory : KodiItemKind::Episode;
    item.path = file;
    item.title = object.value(QStringLiteral("label")).toString().trimmed();
    if (item.title.isEmpty()) {
      item.title = QFileInfo(file).completeBaseName();
    }
    item.coverPath = artworkUrl(object);
    item.container = folder;
    const QByteArray hash =
        QCryptographicHash::hash(file.toUtf8(), QCryptographicHash::Sha1).toHex();
    item.itemId = folder ? QStringLiteral("kodi:dir:%1").arg(QString::fromUtf8(hash))
                         : QStringLiteral("kodi:file:%1").arg(QString::fromUtf8(hash));
    item.subtitle = folder ? QStringLiteral("Kodi folder") : QStringLiteral("Kodi file");
    items.append(item);
  }
  return items;
}

QString KodiScanner::parsePrepareDownload(const QByteArray& jsonResponse) {
  const QJsonObject result = resultObject(jsonResponse);
  const QJsonObject details = result.value(QStringLiteral("details")).toObject();
  const QString path = details.value(QStringLiteral("path")).toString().trimmed();
  if (path.isEmpty()) {
    return {};
  }
  const QString protocol = result.value(QStringLiteral("protocol")).toString().trimmed();
  if (protocol.isEmpty() || path.startsWith(QStringLiteral("http"))) {
    return path;
  }
  return protocol + QStringLiteral("://") + path;
}

QString KodiScanner::httpArtworkUrl(const QString& jsonRpcUrl, const QString& artwork) {
  if (artwork.isEmpty() || artwork.startsWith(QStringLiteral("http://")) ||
      artwork.startsWith(QStringLiteral("https://")) || artwork.startsWith(QStringLiteral("file:"))) {
    return artwork;
  }
  const QUrl base(jsonRpcUrl);
  if (!base.isValid() || base.scheme().isEmpty() || !artwork.startsWith(QStringLiteral("image://"))) {
    return artwork;
  }
  return QStringLiteral("%1://%2/image/%3")
      .arg(base.scheme(), base.authority(),
           QString::fromUtf8(QUrl::toPercentEncoding(artwork)));
}

QByteArray KodiScanner::playerOpenPayload(const QString& itemId) {
  QJsonObject item;
  if (itemId.startsWith(QStringLiteral("kodi:episode:"))) {
    item.insert(QStringLiteral("episodeid"),
                itemId.mid(QStringLiteral("kodi:episode:").size()).toInt());
  } else if (itemId.startsWith(QStringLiteral("kodi:movie:"))) {
    item.insert(QStringLiteral("movieid"),
                itemId.mid(QStringLiteral("kodi:movie:").size()).toInt());
  } else {
    return {};
  }
  QJsonObject params;
  params.insert(QStringLiteral("item"), item);
  QJsonObject request;
  request.insert(QStringLiteral("jsonrpc"), QStringLiteral("2.0"));
  request.insert(QStringLiteral("method"), QStringLiteral("Player.Open"));
  request.insert(QStringLiteral("params"), params);
  request.insert(QStringLiteral("id"), 1);
  return QJsonDocument(request).toJson(QJsonDocument::Compact);
}

bool KodiScanner::isSeriesLike(const QString& title, const QString& path,
                               const QString& mediaType) {
  if (mediaType.compare(QStringLiteral("video"), Qt::CaseInsensitive) != 0 &&
      mediaType.compare(QStringLiteral("tvshows"), Qt::CaseInsensitive) != 0) {
    return false;
  }
  return matchesKeyword(title, path,
                        {QStringLiteral("series"), QStringLiteral("tv"), QStringLiteral("show")});
}

bool KodiScanner::isMovieLike(const QString& title, const QString& path, const QString& mediaType) {
  if (mediaType.compare(QStringLiteral("video"), Qt::CaseInsensitive) != 0 &&
      mediaType.compare(QStringLiteral("movies"), Qt::CaseInsensitive) != 0) {
    return false;
  }
  return matchesKeyword(title, path, {QStringLiteral("movie"), QStringLiteral("film")});
}

QVector<KodiLibraryItem> KodiScanner::tvShowsForSource(const KodiLibrarySnapshot& snapshot,
                                                       const QString& sourcePath) {
  if (sourcePath.isEmpty()) {
    return snapshot.tvShows;
  }
  QVector<KodiLibraryItem> filtered;
  for (const KodiLibraryItem& show : snapshot.tvShows) {
    if (show.path.isEmpty() || pathBelongsToSource(show.path, sourcePath)) {
      filtered.append(show);
    }
  }
  return filtered;
}

QStringList KodiScanner::findMyVideosDatabases(const QString& userdataRoot) {
  QStringList matches;
  const QDir directory(userdataRoot + QStringLiteral("/Database"));
  const QFileInfoList files =
      directory.entryInfoList({QStringLiteral("MyVideos*.db")}, QDir::Files, QDir::Name);
  for (const QFileInfo& info : files) {
    matches.append(info.absoluteFilePath());
  }
  return matches;
}

KodiLibrarySnapshot KodiScanner::readMyVideos(const QString& databasePath) {
  KodiLibrarySnapshot snapshot;
  if (databasePath.isEmpty() || !QFileInfo::exists(databasePath)) {
    return snapshot;
  }
  const QString connection =
      QStringLiteral("omakade-kodi-myvideos-%1")
          .arg(QString::fromUtf8(
              QCryptographicHash::hash(databasePath.toUtf8(), QCryptographicHash::Sha1)
                  .toHex()
                  .left(12)));
  {
    QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
    database.setDatabaseName(databasePath);
    database.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
    if (!database.open()) {
      QSqlDatabase::removeDatabase(connection);
      return snapshot;
    }

    QHash<int, QString> paths;
    QSqlQuery pathQuery(database);
    if (pathQuery.exec(QStringLiteral("SELECT idPath, strPath FROM path"))) {
      while (pathQuery.next()) {
        paths.insert(pathQuery.value(0).toInt(), pathQuery.value(1).toString());
      }
    }

    QHash<int, QString> files;
    QSqlQuery fileQuery(database);
    if (fileQuery.exec(QStringLiteral("SELECT idFile, idPath, strFilename FROM files"))) {
      while (fileQuery.next()) {
        const QString directory = paths.value(fileQuery.value(1).toInt());
        files.insert(fileQuery.value(0).toInt(), directory + fileQuery.value(2).toString());
      }
    }

    QHash<QString, QString> art;
    QSqlQuery artQuery(database);
    if (artQuery.exec(QStringLiteral(
            "SELECT media_id, media_type, type, url FROM art WHERE type IN ('poster','thumb')"))) {
      while (artQuery.next()) {
        const QString key =
            artQuery.value(1).toString() + QLatin1Char(':') + artQuery.value(0).toString();
        const QString type = artQuery.value(2).toString();
        if (type == QStringLiteral("poster") || !art.contains(key)) {
          art.insert(key, artQuery.value(3).toString());
        }
      }
    }

    QSqlQuery showQuery(database);
    if (showQuery.exec(QStringLiteral("SELECT idShow, c00, c01 FROM tvshow"))) {
      while (showQuery.next()) {
        KodiLibraryItem item;
        item.kind = KodiItemKind::TvShow;
        item.tvshowId = showQuery.value(0).toInt();
        item.itemId = QStringLiteral("kodi:tvshow:%1").arg(item.tvshowId);
        item.title = showQuery.value(1).toString().trimmed();
        item.plot = showQuery.value(2).toString().trimmed();
        item.coverPath = art.value(QStringLiteral("tvshow:%1").arg(item.tvshowId));
        item.subtitle = QStringLiteral("Kodi TV show");
        item.container = true;
        snapshot.tvShows.append(item);
      }
    }

    QSqlQuery seasonQuery(database);
    if (seasonQuery.exec(QStringLiteral("SELECT idShow, season, name FROM seasons"))) {
      while (seasonQuery.next()) {
        KodiLibraryItem item;
        item.kind = KodiItemKind::Season;
        item.tvshowId = seasonQuery.value(0).toInt();
        item.season = seasonQuery.value(1).toInt();
        item.itemId = QStringLiteral("kodi:season:%1:%2").arg(item.tvshowId).arg(item.season);
        item.title = seasonQuery.value(2).toString().trimmed();
        if (item.title.isEmpty()) {
          item.title = item.season == 0 ? QStringLiteral("Specials")
                                        : QStringLiteral("Season %1").arg(item.season);
        }
        item.coverPath = art.value(QStringLiteral("season:%1").arg(item.tvshowId));
        item.subtitle = QStringLiteral("Kodi season");
        item.container = true;
        snapshot.seasonsByShow[item.tvshowId].append(item);
      }
    }

    QSqlQuery episodeQuery(database);
    if (episodeQuery.exec(
            QStringLiteral("SELECT idEpisode, idShow, c00, c01, c12, c13, idFile FROM episode"))) {
      while (episodeQuery.next()) {
        KodiLibraryItem item;
        item.kind = KodiItemKind::Episode;
        item.episodeId = episodeQuery.value(0).toInt();
        item.tvshowId = episodeQuery.value(1).toInt();
        item.title = episodeQuery.value(2).toString().trimmed();
        item.plot = episodeQuery.value(3).toString().trimmed();
        item.season = episodeQuery.value(4).toString().toInt();
        item.episodeNumber = episodeQuery.value(5).toString().toInt();
        item.path = files.value(episodeQuery.value(6).toInt());
        item.itemId = QStringLiteral("kodi:episode:%1").arg(item.episodeId);
        item.coverPath = art.value(QStringLiteral("episode:%1").arg(item.episodeId));
        item.subtitle = QStringLiteral("S%1E%2")
                            .arg(item.season, 2, 10, QLatin1Char('0'))
                            .arg(item.episodeNumber, 2, 10, QLatin1Char('0'));
        item.container = false;
        snapshot.episodesBySeason[seasonKey(item.tvshowId, item.season)].append(item);
        if (!snapshot.seasonsByShow.contains(item.tvshowId) ||
            std::none_of(
                snapshot.seasonsByShow.value(item.tvshowId).cbegin(),
                snapshot.seasonsByShow.value(item.tvshowId).cend(),
                [&item](const KodiLibraryItem& season) { return season.season == item.season; })) {
          KodiLibraryItem season;
          season.kind = KodiItemKind::Season;
          season.tvshowId = item.tvshowId;
          season.season = item.season;
          season.itemId = QStringLiteral("kodi:season:%1:%2").arg(item.tvshowId).arg(item.season);
          season.title = item.season == 0 ? QStringLiteral("Specials")
                                          : QStringLiteral("Season %1").arg(item.season);
          season.subtitle = QStringLiteral("Kodi season");
          season.container = true;
          snapshot.seasonsByShow[item.tvshowId].append(season);
        }
      }
    }

    QSqlQuery movieQuery(database);
    if (movieQuery.exec(QStringLiteral("SELECT idMovie, c00, c01, idFile FROM movie"))) {
      while (movieQuery.next()) {
        KodiLibraryItem item;
        item.kind = KodiItemKind::Movie;
        item.movieId = movieQuery.value(0).toInt();
        item.itemId = QStringLiteral("kodi:movie:%1").arg(item.movieId);
        item.title = movieQuery.value(1).toString().trimmed();
        item.plot = movieQuery.value(2).toString().trimmed();
        item.path = files.value(movieQuery.value(3).toInt());
        item.coverPath = art.value(QStringLiteral("movie:%1").arg(item.movieId));
        item.subtitle = QStringLiteral("Kodi movie");
        item.container = false;
        snapshot.movies.append(item);
      }
    }
  }
  QSqlDatabase::removeDatabase(connection);
  return snapshot;
}
