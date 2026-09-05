#pragma once

#include "library/KodiBrowseModel.h"
#include "sources/kodi/KodiScanner.h"

#include <QAbstractListModel>
#include <QColor>
#include <QFutureWatcher>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QSqlDatabase>

#include <functional>

class AppSettings;

class KodiGameModel final : public QAbstractListModel {
  Q_OBJECT
  Q_PROPERTY(bool kodiDetected READ kodiDetected NOTIFY statusChanged)
  Q_PROPERTY(QString statusText READ statusText NOTIFY statusChanged)
  Q_PROPERTY(QString errorText READ errorText NOTIFY statusChanged)
  Q_PROPERTY(QStringList detectedPaths READ detectedPaths NOTIFY statusChanged)
  Q_PROPERTY(qint64 lastScan READ lastScan NOTIFY statusChanged)
  Q_PROPERTY(bool scanning READ scanning NOTIFY statusChanged)
  Q_PROPERTY(bool browsing READ browsing NOTIFY browseChanged)
  Q_PROPERTY(bool canGoBack READ canGoBack NOTIFY browseChanged)
  Q_PROPERTY(int browseDepth READ browseDepth NOTIFY browseChanged)
  Q_PROPERTY(QString browseTitle READ browseTitle NOTIFY browseChanged)
  Q_PROPERTY(QString browseEmptyText READ browseEmptyText NOTIFY browseChanged)
  Q_PROPERTY(KodiBrowseModel* browseItems READ browseItems CONSTANT)

public:
  explicit KodiGameModel(const QString& databasePath, AppSettings* settings = nullptr,
                         QObject* parent = nullptr);
  ~KodiGameModel() override;

  [[nodiscard]] int rowCount(const QModelIndex& parent = QModelIndex()) const override;
  [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
  [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

  [[nodiscard]] bool kodiDetected() const;
  [[nodiscard]] QString statusText() const;
  [[nodiscard]] QString errorText() const;
  [[nodiscard]] QStringList detectedPaths() const;
  [[nodiscard]] qint64 lastScan() const;
  [[nodiscard]] bool scanning() const { return m_scanning; }
  [[nodiscard]] bool browsing() const { return !m_browseStack.isEmpty(); }
  [[nodiscard]] bool canGoBack() const { return !m_browseStack.isEmpty(); }
  [[nodiscard]] int browseDepth() const { return m_browseStack.size(); }
  [[nodiscard]] QString browseTitle() const;
  [[nodiscard]] QString browseEmptyText() const { return m_browseEmptyText; }
  [[nodiscard]] KodiBrowseModel* browseItems() { return &m_browseItems; }

  Q_INVOKABLE void toggleFavorite(int row);
  Q_INVOKABLE void toggleHidden(int row);
  Q_INVOKABLE void refresh();
  Q_INVOKABLE bool openIfContainer(const QString& appId);
  Q_INVOKABLE bool goBack();
  Q_INVOKABLE void refreshBrowse();

  void applyScan(const KodiScanResult& result);
  void applyLibrarySnapshot(const KodiLibrarySnapshot& snapshot);
  void refreshFromRoots(const QStringList& roots);

signals:
  void statusChanged();
  void browseChanged();

private:
  struct Game {
    KodiSourceRecord kodi;
    bool favorite = false;
    bool hidden = false;
    QColor accentStart;
    QColor accentEnd;
  };

  struct BrowseFrame {
    QString title;
    QString itemId;
    QString path;
    KodiItemKind kind = KodiItemKind::Source;
    int tvshowId = 0;
    int season = -1;
    QVector<KodiLibraryItem> items;
  };

  bool openDatabase(const QString& path);
  bool ensureSchema();
  void loadDatabase();
  void loadSourceState();
  void tryJsonRpcRefresh();
  void runLocalScan(const QStringList& roots);
  void loadOfflineLibrary();
  [[nodiscard]] QVariant valueForRole(const Game& game, int role) const;
  void setStatus(const QString& status, const QString& error = {});
  [[nodiscard]] const Game* sourceById(const QString& appId) const;
  bool openSource(const KodiSourceRecord& source);
  bool pushItem(const KodiLibraryItem& item);
  void showItems(const QVector<KodiLibraryItem>& items, const QString& emptyText);
  [[nodiscard]] QVector<KodiLibraryItem> childrenFor(const BrowseFrame& frame) const;
  void requestLibraryChildren(const BrowseFrame& frame);
  void postJson(const QJsonObject& body, const std::function<void(QByteArray)>& finished);

  QVector<Game> m_games;
  QSqlDatabase m_database;
  QString m_connectionName;
  AppSettings* m_settings = nullptr;
  bool m_kodiDetected = false;
  QString m_statusText;
  QString m_errorText;
  QStringList m_detectedPaths;
  qint64 m_lastScan = 0;
  QNetworkAccessManager m_network;
  QFutureWatcher<KodiScanResult> m_scanWatcher;
  bool m_scanning = false;
  KodiBrowseModel m_browseItems;
  QVector<BrowseFrame> m_browseStack;
  KodiLibrarySnapshot m_snapshot;
  QString m_browseEmptyText;
};
