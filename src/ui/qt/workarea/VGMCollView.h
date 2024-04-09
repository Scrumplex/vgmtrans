/*
 * VGMTrans (c) 2002-2021
 * Licensed under the zlib license,
 * refer to the included LICENSE.txt file
 */

#pragma once

#include <QAbstractListModel>
#include <QGroupBox>
#include <QItemSelectionModel>
#include "VGMColl.h"

class VGMFile;
class QLabel;
class QListView;
class QLineEdit;

class VGMCollViewModel : public QAbstractListModel {
  Q_OBJECT
public:
  VGMCollViewModel(QItemSelectionModel *collListSelModel, QObject *parent = 0);

  int rowCount(const QModelIndex &parent = QModelIndex()) const override;
  QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const;

  [[nodiscard]] VGMFile *fileFromIndex(QModelIndex index) const;
  [[nodiscard]] QModelIndex indexFromFile(VGMFile* file) const;
  bool containsVGMFile(VGMFile* file);

public slots:
  void handleNewCollSelected(QModelIndex modelIndex);

private:
  VGMColl *m_coll;
};

class VGMCollView : public QGroupBox {
  Q_OBJECT
public:
  VGMCollView(QItemSelectionModel *collListSelModel, QWidget *parent = 0);

private slots:
  void doubleClickedSlot(QModelIndex);
  void handleSelectionChanged(const QModelIndex &current, const QModelIndex &previous);
  void selectRowForVGMFile(VGMFile *file);

private:
  VGMCollViewModel *vgmCollViewModel;
  QLineEdit *m_collection_title;
  QListView *m_listview;
  QLabel *m_title;
};