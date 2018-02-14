//
//  DomainContentBackupManager.cpp
//  libraries/octree/src
//
//  Created by Brad Hefta-Gaub on 8/21/13.
//  Copyright 2013 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include <chrono>
#include <thread>

#include <cstdio>
#include <fstream>
#include <time.h>

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonDocument>

#include <NumericalConstants.h>
#include <PerfStat.h>
#include <PathUtils.h>

#include "DomainServer.h"
#include "DomainContentBackupManager.h"
const int DomainContentBackupManager::DEFAULT_PERSIST_INTERVAL = 1000 * 30;  // every 30 seconds

// Backup format looks like: daily_backup-TIMESTAMP.zip
const static QString DATETIME_FORMAT { "yyyy-MM-dd_HH-mm-ss" };
const static QString DATETIME_FORMAT_RE("\\d{4}-\\d{2}-\\d{2}_\\d{2}-\\d{2}-\\d{2}");

void DomainContentBackupManager::addBackupHandler(BackupHandler handler) {
    _backupHandlers.push_back(std::move(handler));
}

DomainContentBackupManager::DomainContentBackupManager(const QString& backupDirectory,
                                                       const QJsonObject& settings,
                                                       int persistInterval,
                                                       bool debugTimestampNow)
    : _backupDirectory(backupDirectory),
    _persistInterval(persistInterval),
    _lastCheck(usecTimestampNow())
{
    // Make sure the backup directory exists.
    QDir(_backupDirectory).mkpath(".");

    parseSettings(settings);
}

void DomainContentBackupManager::parseSettings(const QJsonObject& settings) {
    qDebug() << settings << settings["backups"] << settings["backups"].isArray();
    if (settings["backups"].isArray()) {
        const QJsonArray& backupRules = settings["backups"].toArray();
        qCDebug(domain_server) << "BACKUP RULES:";

        for (const QJsonValue& value : backupRules) {
            QJsonObject obj = value.toObject();

            int interval = 0;
            int count = 0;

            QJsonValue intervalVal = obj["backupInterval"];
            if (intervalVal.isString()) {
                interval = intervalVal.toString().toInt();
            } else {
                interval = intervalVal.toInt();
            }

            QJsonValue countVal = obj["maxBackupVersions"];
            if (countVal.isString()) {
                count = countVal.toString().toInt();
            } else {
                count = countVal.toInt();
            }

            auto name = obj["Name"].toString();
            auto format = obj["format"].toString();
            format = name.replace(" ", "_").toLower() + "-";

            qCDebug(domain_server) << "    Name:" << name;
            qCDebug(domain_server) << "        format:" << format;
            qCDebug(domain_server) << "        interval:" << interval;
            qCDebug(domain_server) << "        count:" << count;

            BackupRule newRule = { name, interval, format, count, 0 };

            newRule.lastBackupSeconds = getMostRecentBackupTimeInSecs(format);

            if (newRule.lastBackupSeconds > 0) {
                auto now = QDateTime::currentSecsSinceEpoch();
                auto sinceLastBackup = now - newRule.lastBackupSeconds;
                qCDebug(domain_server).noquote() << "        lastBackup:" <<  formatSecTime(sinceLastBackup) << "ago";
            } else {
                qCDebug(domain_server) << "        lastBackup: NEVER";
            }

            _backupRules.push_back(newRule);
        }
    } else {
        qCDebug(domain_server) << "BACKUP RULES: NONE";
    }
}

int64_t DomainContentBackupManager::getMostRecentBackupTimeInSecs(const QString& format) {
    int64_t mostRecentBackupInSecs = 0;

    QString mostRecentBackupFileName;
    QDateTime mostRecentBackupTime;

    bool recentBackup = getMostRecentBackup(format, mostRecentBackupFileName, mostRecentBackupTime);

    if (recentBackup) {
        mostRecentBackupInSecs = mostRecentBackupTime.toSecsSinceEpoch();
    }

    return mostRecentBackupInSecs;
}

void DomainContentBackupManager::setup() {
    load();
}

bool DomainContentBackupManager::process() {
    if (isStillRunning()) {
        constexpr int64_t MSECS_TO_USECS = 1000;
        constexpr int64_t USECS_TO_SLEEP = 10 * MSECS_TO_USECS;  // every 10ms
        std::this_thread::sleep_for(std::chrono::microseconds(USECS_TO_SLEEP));

        int64_t now = usecTimestampNow();
        int64_t sinceLastSave = now - _lastCheck;
        int64_t intervalToCheck = _persistInterval * MSECS_TO_USECS;

        if (sinceLastSave > intervalToCheck) {
            _lastCheck = now;
            persist();
        }
    }

    return isStillRunning();
}

void DomainContentBackupManager::aboutToFinish() {
    qCDebug(domain_server) << "Persist thread about to finish...";
    persist();
}

void DomainContentBackupManager::persist() {
    QDir backupDir { _backupDirectory };
    backupDir.mkpath(".");

    // create our "lock" file to indicate we're saving.
    QString lockFileName = _backupDirectory + "/running.lock";

    std::ofstream lockFile(qPrintable(lockFileName), std::ios::out | std::ios::binary);
    if (lockFile.is_open()) {
        backup();

        lockFile.close();
        remove(qPrintable(lockFileName));
    }
}

bool DomainContentBackupManager::getMostRecentBackup(const QString& format,
                                                     QString& mostRecentBackupFileName,
                                                     QDateTime& mostRecentBackupTime) {
    QRegExp formatRE { QRegExp::escape(format) + "(" + DATETIME_FORMAT_RE + ")" + "\\.zip" };

    QStringList filters;
    filters << format + "*.zip";

    bool bestBackupFound = false;
    QString bestBackupFile;
    QDateTime bestBackupFileTime;

    // Iterate over all of the backup files in the persist location
    QDirIterator dirIterator(_backupDirectory, filters, QDir::Files | QDir::NoSymLinks, QDirIterator::NoIteratorFlags);
    while (dirIterator.hasNext()) {
        dirIterator.next();
        auto fileName = dirIterator.fileInfo().fileName();

        if (formatRE.exactMatch(fileName)) {
            auto datetime = formatRE.cap(1);
            auto createdAt = QDateTime::fromString(datetime, DATETIME_FORMAT);

            if (!createdAt.isValid()) {
                qDebug() << "Skipping backup with invalid timestamp: " << datetime;
                continue;
            }

            qDebug() << "Checking " << dirIterator.fileInfo().filePath();

            // Based on last modified date, track the most recently modified file as the best backup
            if (createdAt > bestBackupFileTime) {
                bestBackupFound = true;
                bestBackupFile = dirIterator.filePath();
                bestBackupFileTime = createdAt;
            }
        } else {
            qDebug() << "NO match: " << fileName << formatRE;
        }
    }

    // If we found a backup then return the results
    if (bestBackupFound) {
        mostRecentBackupFileName = bestBackupFile;
        mostRecentBackupTime = bestBackupFileTime;
    }
    return bestBackupFound;
}

void DomainContentBackupManager::removeOldBackupVersions(const BackupRule& rule) {
    QDir backupDir { _backupDirectory };
    if (backupDir.exists() && rule.maxBackupVersions > 0) {
        qCDebug(domain_server) << "Rolling old backup versions for rule" << rule.name << "...";

        auto matchingFiles =
                backupDir.entryInfoList({ rule.extensionFormat + "*.zip" }, QDir::Files | QDir::NoSymLinks, QDir::Name);

        int backupsToDelete = matchingFiles.length() - rule.maxBackupVersions;
        for (int i = 0; i < backupsToDelete; ++i) {
            auto fileInfo = matchingFiles[i].absoluteFilePath();
            QFile backupFile(fileInfo);
            if (backupFile.remove()) {
                qCDebug(domain_server) << "Removed old backup: " << backupFile.fileName();
            } else {
                qCDebug(domain_server) << "Failed to remove old backup: " << backupFile.fileName();
            }
        }

        qCDebug(domain_server) << "Done rolling old backup versions...";
    } else {
        qCDebug(domain_server) << "Rolling backups for rule" << rule.name << "."
                                << " Max Rolled Backup Versions less than 1 [" << rule.maxBackupVersions << "]."
                                << " No need to roll backups...";
    }
}

void DomainContentBackupManager::load() {
    QDir backupDir { _backupDirectory };
    if (backupDir.exists()) {

        auto matchingFiles = backupDir.entryInfoList({ "backup-*.zip" }, QDir::Files | QDir::NoSymLinks, QDir::Name);

        for (const auto& file : matchingFiles) {
            QFile backupFile { file.absoluteFilePath() };
            if (!backupFile.open(QIODevice::ReadOnly)) {
                qCritical() << "Could not open file:" << file.absoluteFilePath();
                qCritical() << "    ERROR:" << backupFile.errorString();
                continue;
            }

            QuaZip zip { &backupFile };
            if (!zip.open(QuaZip::mdUnzip)) {
                qCritical() << "Could not open backup archive:" << file.absoluteFilePath();
                qCritical() << "    ERROR:" << zip.getZipError();
                continue;
            }

            for (auto& handler : _backupHandlers) {
                handler.loadBackup(zip);
            }

            zip.close();
        }
    }
}

void DomainContentBackupManager::backup() {
    auto nowDateTime = QDateTime::currentDateTime();
    auto nowSeconds = nowDateTime.toSecsSinceEpoch();

    for (BackupRule& rule : _backupRules) {
        auto secondsSinceLastBackup = nowSeconds - rule.lastBackupSeconds;

        qCDebug(domain_server) << "Checking [" << rule.name << "] - Time since last backup [" << secondsSinceLastBackup
                                << "] "
                                << "compared to backup interval [" << rule.intervalSeconds << "]...";

        if (secondsSinceLastBackup > rule.intervalSeconds) {
            qCDebug(domain_server) << "Time since last backup [" << secondsSinceLastBackup << "] for rule [" << rule.name
                                    << "] exceeds backup interval [" << rule.intervalSeconds << "] doing backup now...";

            auto timestamp = QDateTime::currentDateTime().toString(DATETIME_FORMAT);
            auto fileName = "backup-" + rule.extensionFormat + timestamp + ".zip";
            QuaZip zip(_backupDirectory + "/" + fileName);
            if (!zip.open(QuaZip::mdAdd)) {
                qDebug() << "Could not open backup archive:" << zip.getZipName();
                qDebug() << "    ERROR:" << zip.getZipError();
            }

            for (auto& handler : _backupHandlers) {
                handler.createBackup(zip);
            }

            zip.close();

            qDebug() << "Created backup: " << fileName;

            rule.lastBackupSeconds = nowSeconds;

            removeOldBackupVersions(rule);
        } else {
            qCDebug(domain_server) << "Backup not needed for this rule [" << rule.name << "]...";
        }
    }
}

void DomainContentBackupManager::consolidate(QString fileName) {
    QDir backupDir { _backupDirectory };
    if (backupDir.exists()) {
        auto filePath = backupDir.absoluteFilePath(fileName);

        auto copyFilePath = QDir::tempPath() + "/" + fileName;

        auto copySuccess = QFile::copy(filePath, copyFilePath);
        if (!copySuccess) {
            qCritical() << "Failed to create full backup.";
            return;
        }

        QuaZip zip(copyFilePath);
        if (!zip.open(QuaZip::mdAdd)) {
            qCritical() << "Could not open backup archive:" << filePath;
            qCritical() << "    ERROR:" << zip.getZipError();
            return;
        }

        for (auto& handler : _backupHandlers) {
            handler.consolidateBackup(zip);
        }

        zip.close();
    }
}
