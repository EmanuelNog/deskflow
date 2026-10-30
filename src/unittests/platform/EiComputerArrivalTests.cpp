/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "EiComputerArrivalTests.h"

#include "platform/EiComputer.h"

namespace deskflow {

void EiComputerArrivalTests::canStartEmulatingOnArrival_data()
{
  QTest::addColumn<bool>("isPrimary");
  QTest::addColumn<bool>("resumed");
  QTest::addColumn<bool>("expected");

  // The primary is an ei RECEIVER context.  libei documents ei_device_start_emulating() as
  // "only available on an ei sender context", and libeis answers a start from a receiver with
  // "Bug: Invalid event from receiver ei context. Disconnecting client": the captured device
  // then flaps PAUSED <-> RESUMED and none of its events are delivered, so a mouse or keyboard
  // that appears while the capture session is running is dead until the session is rebuilt.
  QTest::newRow("primary, resumed") << true << true << false;
  QTest::newRow("primary, paused") << true << false << false;

  // The secondary is the sender: there a late device must be started, but only once EIS has
  // handed it over in RESUMED state - starting a paused device is the same invalid event.
  QTest::newRow("secondary, resumed") << false << true << true;
  QTest::newRow("secondary, paused") << false << false << false;
}

void EiComputerArrivalTests::canStartEmulatingOnArrival()
{
  QFETCH(bool, isPrimary);
  QFETCH(bool, resumed);
  QFETCH(bool, expected);

  QCOMPARE(EiComputer::canStartEmulatingOnArrival(isPrimary, resumed), expected);
}

} // namespace deskflow

QTEST_GUILESS_MAIN(deskflow::EiComputerArrivalTests)
