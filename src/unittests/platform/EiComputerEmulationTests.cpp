/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "EiComputerEmulationTests.h"

#include "platform/EiComputer.h"

using Slot = deskflow::EiComputer::EmulationSlot;

namespace deskflow {

// A capture session can hold more devices than the three Deskflow tracks (a touchscreen, a
// second tablet, a device the compositor paused on its own).  libeis answers a
// start_emulating on a device it ALREADY holds in EMULATING with "Invalid device state N for a
// start_emulating event" and then disconnects the client, which rebuilds the whole capture -
// an input stall.  So the sender must know, per device, what it has started.

void EiComputerEmulationTests::pauseOfAnUntrackedDeviceClearsNothing()
{
  EiComputer::EmulationState state;
  state.setEmulating(Slot::Pointer, true);
  state.setEmulating(Slot::Keyboard, true);
  state.setEmulating(Slot::Absolute, true);

  // An EI device that is none of the three tracked ones: its pause says nothing about ours.
  state.pause(Slot::Untracked);

  QVERIFY(state.isEmulating(Slot::Pointer));
  QVERIFY(state.isEmulating(Slot::Keyboard));
  QVERIFY(state.isEmulating(Slot::Absolute));

  // ... and therefore the next injected input must start NOTHING.
  const auto toStart = state.toStart(true, true, true);
  QVERIFY(!toStart.pointer);
  QVERIFY(!toStart.keyboard);
  QVERIFY(!toStart.absolute);
}

void EiComputerEmulationTests::pauseOfACapturedDeviceClearsOnlyThatDevice()
{
  EiComputer::EmulationState state;
  state.setEmulating(Slot::Pointer, true);
  state.setEmulating(Slot::Keyboard, true);

  state.pause(Slot::Keyboard);

  QVERIFY(!state.isEmulating(Slot::Keyboard));
  QVERIFY(state.isEmulating(Slot::Pointer));

  // EIS reset the paused keyboard to neutral; the pointer is still ours to keep emulating, so a
  // start pass must touch the keyboard only.
  const auto toStart = state.toStart(true, true, false);
  QVERIFY(!toStart.pointer);
  QVERIFY(toStart.keyboard);
  QVERIFY(!toStart.absolute);
}

void EiComputerEmulationTests::aStartPassNeverRestartsADeviceStillEmulating()
{
  EiComputer::EmulationState state;
  state.setEmulating(Slot::Absolute, true);

  const auto toStart = state.toStart(true, true, true);

  QVERIFY(toStart.pointer);
  QVERIFY(toStart.keyboard);
  QVERIFY(!toStart.absolute);
}

void EiComputerEmulationTests::aStartPassStartsEachResumedDeviceOnce()
{
  EiComputer::EmulationState state;

  const auto first = state.toStart(true, false, true);
  QVERIFY(first.pointer);
  QVERIFY(!first.keyboard);
  QVERIFY(first.absolute);

  state.setEmulating(Slot::Pointer, true);
  state.setEmulating(Slot::Absolute, true);

  // Nothing resumed at all: EIS has not handed us a usable device, and the caller must not arm
  // the idle timer for an emulation it never started.
  const auto none = state.toStart(false, false, false);
  QVERIFY(!none.pointer);
  QVERIFY(!none.keyboard);
  QVERIFY(!none.absolute);

  const auto again = state.toStart(true, true, true);
  QVERIFY(!again.pointer);
  QVERIFY(again.keyboard);
  QVERIFY(!again.absolute);
}

} // namespace deskflow

QTEST_GUILESS_MAIN(deskflow::EiComputerEmulationTests)
