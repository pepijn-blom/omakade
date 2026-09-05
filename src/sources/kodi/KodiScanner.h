#pragma once

#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

enum class KodiItemKind { Source, TvShow, Season, Episode, Movie, Directory };

struct KodiSourceRecord {
  QString sourceId;
  QString title;
  QString path;
  QString mediaType;
  QString coverPath;
  qint64 lastPlayed = 0;
  qint64 playtimeSeconds = 0;
  bool flatpak = false;
  QString flatpakAppId;
};

struct KodiLibraryItem {
  KodiItemKind kind = KodiItemKind::Directory;
  QString itemId;
  QString title;
  QString subtitle;
  QString plot;
  QString path;
  QString coverPath;
  int tvshowId = 0;
  int season = -1;
  int episodeNumber = 0;
  int episodeId = 0;
  int movieId = 0;
  int year = 0;
  qint64 lastPlayed = 0;
  int episodeCount = 0;
  bool container = true;
};

struct KodiLibrarySnapshot {
  QVector<KodiLibraryItem> tvShows;
  QVector<KodiLibraryItem> movies;
  QVector<KodiLibraryItem> directories;
  QHash<int, QVector<KodiLibraryItem>> seasonsByShow;
  QHash<QString, QVector<KodiLibraryItem>> episodesBySeason;
  QHash<QString, QVector<KodiLibraryItem>> childrenByPath;
};

struct KodiScanResult {
  QVector<KodiSourceRecord> sources;
  QStringList roots;
  QStringList warnings;
  bool incomplete = false;
};

class KodiScanner final {
public:
  [[nodiscard]] static QStringList discoverRoots();
  [[nodiscard]] static KodiScanResult scanLocal(const QStringList& roots);
  [[nodiscard]] static QVector<KodiSourceRecord>
  parseSourcesXml(const QString& xmlContent, const QString& rootPath, bool flatpak);
  [[nodiscard]] static QVector<KodiSourceRecord>
  parseJsonRpcSources(const QByteArray& jsonResponse, const QString& mediaType, bool flatpak);
  [[nodiscard]] static QString generateSourceId(const QString& mediaType, const QString& path,
                                                const QString& title);
  [[nodiscard]] static QVector<KodiLibraryItem> parseTvShows(const QByteArray& jsonResponse);
  [[nodiscard]] static QVector<KodiLibraryItem> parseSeasons(const QByteArray& jsonResponse,
                                                             int tvshowId);
  [[nodiscard]] static QVector<KodiLibraryItem> parseEpisodes(const QByteArray& jsonResponse);
  [[nodiscard]] static QVector<KodiLibraryItem> parseMovies(const QByteArray& jsonResponse);
  [[nodiscard]] static QVector<KodiLibraryItem> parseDirectory(const QByteArray& jsonResponse);
  [[nodiscard]] static QString parsePrepareDownload(const QByteArray& jsonResponse);
  [[nodiscard]] static QString httpArtworkUrl(const QString& jsonRpcUrl, const QString& artwork);
  [[nodiscard]] static QByteArray playerOpenPayload(const QString& itemId);
  [[nodiscard]] static bool isSeriesLike(const QString& title, const QString& path,
                                         const QString& mediaType);
  [[nodiscard]] static bool isMovieLike(const QString& title, const QString& path,
                                        const QString& mediaType);
  [[nodiscard]] static QStringList findMyVideosDatabases(const QString& userdataRoot);
  [[nodiscard]] static KodiLibrarySnapshot readMyVideos(const QString& databasePath);
  [[nodiscard]] static QVector<KodiLibraryItem>
  tvShowsForSource(const KodiLibrarySnapshot& snapshot, const QString& sourcePath);
  [[nodiscard]] static QString seasonKey(int tvshowId, int season);
};
