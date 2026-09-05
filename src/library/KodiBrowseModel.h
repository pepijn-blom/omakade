#pragma once

#include "sources/kodi/KodiScanner.h"

#include <QAbstractListModel>
#include <QColor>
#include <QVariant>

class KodiBrowseModel final : public QAbstractListModel {
  Q_OBJECT
  Q_PROPERTY(QString searchText READ searchText WRITE setSearchText NOTIFY searchTextChanged)

public:
  explicit KodiBrowseModel(QObject* parent = nullptr);

  [[nodiscard]] int rowCount(const QModelIndex& parent = QModelIndex()) const override;
  [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
  [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

  [[nodiscard]] QString searchText() const { return m_searchText; }
  void setSearchText(const QString& value);

  void setItems(const QVector<KodiLibraryItem>& items);
  void clear();
  [[nodiscard]] KodiLibraryItem itemAt(int row) const;
  [[nodiscard]] KodiLibraryItem itemById(const QString& itemId) const;
  [[nodiscard]] bool hasItem(const QString& itemId) const;

  Q_INVOKABLE QVariantMap get(int row) const;
  Q_INVOKABLE int indexOf(const QString& source, const QString& runner, const QString& appId) const;
  Q_INVOKABLE void toggleFavorite(int row);
  Q_INVOKABLE QVariantList installations(int row) const;

signals:
  void searchTextChanged();

private:
  [[nodiscard]] QVector<int> visibleRows() const;
  [[nodiscard]] QVariant valueForRole(const KodiLibraryItem& item, int role) const;
  [[nodiscard]] QVariantMap mapForItem(const KodiLibraryItem& item) const;

  QVector<KodiLibraryItem> m_items;
  QString m_searchText;
};
