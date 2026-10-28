/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 - 2026 Deskflow Developers
 * SPDX-FileCopyrightText: (C) 2012 - 2016 Synergy App Ltd
 * SPDX-FileCopyrightText: (C) 2002 Chris Schoeneman
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "base/LogOutputters.h"
#include "arch/Arch.h"

#include <iostream>

#include <QFile>
#include <QString>
#include <QTextStream>

constexpr auto s_logFileSizeLimit = 1024 * 1024; //!< Max Log size before rotating (1Mb)

//
// StopLogOutputter
//

void StopLogOutputter::open(const QString &)
{
  // do nothing
}

void StopLogOutputter::close()
{
  // do nothing
}

bool StopLogOutputter::write(LogLevel::Level, const QString &)
{
  return false;
}

//
// ConsoleLogOutputter
//

void ConsoleLogOutputter::open(const QString &title)
{
  // do nothing
}

void ConsoleLogOutputter::close()
{
  // do nothing
}

bool ConsoleLogOutputter::write(LogLevel::Level level, const QString &msg)
{
  using enum LogLevel::Level;
  if ((level >= Fatal) && (level <= Warning))
    std::cerr << qPrintable(msg) << std::endl;
  else
    std::cout << qPrintable(msg) << std::endl;
  std::cout.flush();
  return true;
}

void ConsoleLogOutputter::flush() const
{
  // do nothing
}

//
// SystemLogOutputter
//

void SystemLogOutputter::open(const QString &title)
{
  ARCH->openLog(title);
}

void SystemLogOutputter::close()
{
  ARCH->closeLog();
}

bool SystemLogOutputter::write(LogLevel::Level level, const QString &msg)
{
  ARCH->writeLog(level, msg);
  return true;
}

//
// SystemLogger
//

SystemLogger::SystemLogger(const QString &title, bool blockConsole)
{
  // redirect log messages
  if (blockConsole) {
    m_stop = new StopLogOutputter; // NOSONAR - Adopted by `Log`
    CLOG->insert(m_stop);
  }
  m_syslog = new SystemLogOutputter; // NOSONAR - Adopted by `Log`
  m_syslog->open(title);
  CLOG->insert(m_syslog);
}

SystemLogger::~SystemLogger()
{
  CLOG->remove(m_syslog);
  delete m_syslog;
  if (m_stop != nullptr) {
    CLOG->remove(m_stop);
    delete m_stop;
  }
}

//
// FileLogOutputter
//

FileLogOutputter::FileLogOutputter(const QString &logFile)
{
  setLogFilename(logFile);
}

void FileLogOutputter::setLogFilename(const QString &logFile)
{
  assert(logFile != nullptr);
  if (m_file.isOpen())
    m_file.close();
  m_fileName = logFile;
  m_size = 0;
}

bool FileLogOutputter::write(LogLevel::Level, const QString &message)
{
  // Keep the file open across lines.  The previous implementation opened, flushed and closed
  // the file (then queried its size) for EVERY line.  At VERBOSE rates that is hundreds of
  // file create/close pairs per second, each of which a real-time AV scanner inspects on
  // Windows - measured at ~400 us per line on NTFS versus ~15 us on Linux.  That cost lands on
  // the input event thread and is enough to miss the 3x3 s keep-alive and drop the peer.
  if (!m_file.isOpen()) {
    m_file.setFileName(m_fileName);
    if (!m_file.open(QFile::WriteOnly | QFile::Append))
      return false;
    m_size = m_file.size();
  }

  const QByteArray line = message.toUtf8() + '\n';
  const qint64 written = m_file.write(line);
  if (written != line.size())
    return false;

  // flush to the OS, not to the platter: a line survives a crash, not a power cut.  This keeps
  // the log useful while removing the per-line open/close/stat cost.
  m_file.flush();
  m_size += written;

  if (m_size > s_logFileSizeLimit)
    rotate();

  return true;
}

void FileLogOutputter::rotate()
{
  if (m_file.isOpen())
    m_file.close();

  const auto oldFile = QStringLiteral("%1.1").arg(m_fileName);
  QFile::remove(oldFile);
  QFile::rename(m_fileName, oldFile);

  m_file.setFileName(m_fileName);
  m_size = 0;
  if (m_file.open(QFile::WriteOnly | QFile::Append))
    m_size = m_file.size();
}

void FileLogOutputter::open(const QString &title)
{
  // do nothing
}

void FileLogOutputter::close()
{
  if (m_file.isOpen()) {
    m_file.flush();
    m_file.close();
  }
}
