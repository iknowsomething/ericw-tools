/*  Copyright (C) 2017 Eric Wasylishen

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA

See file, 'COPYING', for details.
*/

#pragma once

#include <QMainWindow>
#include <QVBoxLayout>

#include <common/bspfile.hh>

class GLView;
class QFileSystemWatcher;
class QLineEdit;
class QCheckBox;

class MainWindow : public QMainWindow
{
    Q_OBJECT

private:
    QFileSystemWatcher *m_watcher = nullptr;
    std::unique_ptr<QTimer> m_fileReloadTimer;
    QString m_mapFile;
    bspdata_t m_bspdata;
    qint64 m_fileSize = -1;

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private:
    void setupMenu();
    void fileOpen();
    void takeScreenshot();
    void fileReloadTimerExpired();

protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void showEvent(QShowEvent *event) override;

private:
    void reload();
    void loadFile(const QString &file);
    void loadFileInternal(const QString &file, bool is_reload);
    void displayCameraPositionInfo();

private:
    GLView *glView;

    QCheckBox *vis_checkbox;

    QLineEdit *qbsp_options;
    QLineEdit *vis_options;
    QLineEdit *light_options;
    QVBoxLayout *lightstyles;
};
