/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "EiKeyStateTests.h"

#include "base/EventQueue.h"
#include "deskflow/AppUtil.h"
#include "deskflow/KeyTypes.h"
#include "platform/EiKeyState.h"

#include <QByteArray>
#include <QTemporaryFile>

#include <cstdint>

namespace {
class TestAppUtil : public AppUtil
{
public:
  int run() override
  {
    return 0;
  }

  std::vector<std::string> getKeyboardLayoutList() override
  {
    return {"en"};
  }

  std::string getCurrentLanguageCode() override
  {
    return "en";
  }
};

const char TestKeymap[] = R"XKB(xkb_keymap {
xkb_keycodes "test" {
    minimum = 8;
    maximum = 255;
    <LFSH> = 50;
    <NMLK> = 77;
};
xkb_types "test" {
    type "ONE_LEVEL" {
        modifiers = none;
        level_name[Level1] = "Any";
    };
};
xkb_compat "test" {
    interpret Shift_L+AnyOfOrNone(all) {
        action = SetMods(modifiers=Shift);
    };
    interpret Num_Lock+AnyOfOrNone(all) {
        action = LockMods(modifiers=Mod2);
    };
};
xkb_symbols "test" {
    key <LFSH> { [ Shift_L ] };
    key <NMLK> { [ Num_Lock ] };
    modifier_map Shift { <LFSH> };
    modifier_map Mod2 { <NMLK> };
};
};)XKB";

// XKB keycodes for TestKeymap.
constexpr std::uint32_t LeftShiftKeycode = 50;
constexpr std::uint32_t NumLockKeycode = 77;

// Same shape as TestKeymap with one more symbol, so the text differs while staying compilable.
const char TestKeymap2[] = R"XKB(xkb_keymap {
xkb_keycodes "test" {
    minimum = 8;
    maximum = 255;
    <LFSH> = 50;
    <NMLK> = 77;
    <ESC> = 9;
};
xkb_types "test" {
    type "ONE_LEVEL" {
        modifiers = none;
        level_name[Level1] = "Any";
    };
};
xkb_compat "test" {
    interpret Shift_L+AnyOfOrNone(all) {
        action = SetMods(modifiers=Shift);
    };
    interpret Num_Lock+AnyOfOrNone(all) {
        action = LockMods(modifiers=Mod2);
    };
};
xkb_symbols "test" {
    key <LFSH> { [ Shift_L ] };
    key <NMLK> { [ Num_Lock ] };
    key <ESC> { [ Escape ] };
    modifier_map Shift { <LFSH> };
    modifier_map Mod2 { <NMLK> };
};
};)XKB";

// Writes keymapText into a fresh temp file and hands its fd to the key state.
void ingestKeymap(deskflow::EiKeyState &keyState, QTemporaryFile &file, const QByteArray &keymapText)
{
  QVERIFY(file.open());
  QCOMPARE(file.write(keymapText), keymapText.size());
  QVERIFY(file.flush());
  keyState.init(file.handle(), static_cast<std::size_t>(file.size()));
}
} // namespace


void EiKeyStateTests::initTestCase()
{
  m_arch.init();
  m_log.setFilter(LogLevel::Level::Verbose);
}

void EiKeyStateTests::clearStaleModifiers_shiftDownAndNumLockOn_shiftClearedAndNumLockPreserved()
{
  TestAppUtil appUtil;
  EventQueue eventQueue;
  deskflow::EiKeyState keyState(nullptr, &eventQueue);

  QTemporaryFile keymapFile;
  QVERIFY(keymapFile.open());
  const QByteArray keymapData = QByteArray::fromRawData(TestKeymap, sizeof(TestKeymap) - 1);
  QCOMPARE(keymapFile.write(keymapData), keymapData.size());
  QVERIFY(keymapFile.flush());
  keyState.init(keymapFile.handle(), keymapFile.size());

  keyState.updateXkbState(LeftShiftKeycode, true);
  keyState.updateXkbState(NumLockKeycode, true);
  keyState.updateXkbState(NumLockKeycode, false);

  QVERIFY((keyState.pollActiveModifiers() & KeyModifierShift) != 0);
  QVERIFY((keyState.pollActiveModifiers() & KeyModifierNumLock) != 0);

  keyState.clearStaleModifiers();

  QVERIFY((keyState.pollActiveModifiers() & KeyModifierShift) == 0);
  QVERIFY((keyState.pollActiveModifiers() & KeyModifierNumLock) != 0);
}

void EiKeyStateTests::updateLockedModifiers_compositorLockState_numLockFollowsCompositor()
{
  // Real modifier Mod2 is index 4 (Shift, Lock, Control, Mod1, Mod2, ...).
  constexpr std::uint32_t xkbMod2Mask = 1 << 4;

  TestAppUtil appUtil;
  EventQueue eventQueue;
  deskflow::EiKeyState keyState(nullptr, &eventQueue);

  QTemporaryFile keymapFile;
  QVERIFY(keymapFile.open());
  const QByteArray keymapData = QByteArray::fromRawData(TestKeymap, sizeof(TestKeymap) - 1);
  QCOMPARE(keymapFile.write(keymapData), keymapData.size());
  QVERIFY(keymapFile.flush());
  keyState.init(keymapFile.handle(), keymapFile.size());

  keyState.updateXkbState(LeftShiftKeycode, true);
  QVERIFY((keyState.pollActiveModifiers() & KeyModifierNumLock) == 0);

  // NumLock toggled on locally, reported by the compositor
  keyState.updateLockedModifiers(xkbMod2Mask);
  QVERIFY((keyState.pollActiveModifiers() & KeyModifierNumLock) != 0);
  QVERIFY((keyState.pollActiveModifiers() & KeyModifierShift) != 0);

  keyState.clearStaleModifiers();
  QVERIFY((keyState.pollActiveModifiers() & KeyModifierNumLock) != 0);

  // NumLock toggled off locally
  keyState.updateLockedModifiers(0);
  QVERIFY((keyState.pollActiveModifiers() & KeyModifierNumLock) == 0);
}

void EiKeyStateTests::initIfChanged_sameKeymapAgain_modifierStateSurvivesAndNoChangeReported()
{
  // The compositor destroys its EIS keyboard and re-creates it with the SAME keymap
  // (measured 2026-11-12: 1089 rebuilds in 27h, each re-ingesting a byte-identical
  // 320-entry keymap).  A held modifier must survive that, and the caller must be told
  // there is nothing to rebuild.
  TestAppUtil appUtil;
  EventQueue eventQueue;
  deskflow::EiKeyState keyState(nullptr, &eventQueue);

  const QByteArray keymapData = QByteArray::fromRawData(TestKeymap, sizeof(TestKeymap) - 1);
  QTemporaryFile keymapFile;
  ingestKeymap(keyState, keymapFile, keymapData);

  keyState.updateXkbState(LeftShiftKeycode, true);
  QVERIFY((keyState.pollActiveModifiers() & KeyModifierShift) != 0);

  // Second delivery of the same keymap, over the same fd.
  QVERIFY(!keyState.initIfChanged(keymapFile.handle(), static_cast<std::size_t>(keymapFile.size())));
  QVERIFY((keyState.pollActiveModifiers() & KeyModifierShift) != 0);
}

void EiKeyStateTests::initIfChanged_differentKeymap_ingestsAndResetsModifierState()
{
  TestAppUtil appUtil;
  EventQueue eventQueue;
  deskflow::EiKeyState keyState(nullptr, &eventQueue);

  QTemporaryFile firstFile;
  ingestKeymap(keyState, firstFile, QByteArray::fromRawData(TestKeymap, sizeof(TestKeymap) - 1));
  keyState.updateXkbState(LeftShiftKeycode, true);
  QVERIFY((keyState.pollActiveModifiers() & KeyModifierShift) != 0);

  QTemporaryFile secondFile;
  QVERIFY(secondFile.open());
  const QByteArray changed = QByteArray::fromRawData(TestKeymap2, sizeof(TestKeymap2) - 1);
  QCOMPARE(secondFile.write(changed), changed.size());
  QVERIFY(secondFile.flush());

  QVERIFY(keyState.initIfChanged(secondFile.handle(), static_cast<std::size_t>(secondFile.size())));
  // A new keymap is a new xkb state: what was held under the old one no longer holds.
  QVERIFY((keyState.pollActiveModifiers() & KeyModifierShift) == 0);
}

void EiKeyStateTests::initIfChanged_trailingNullOnly_notTreatedAsAChangedKeymap()
{
  // libei may or may not include the terminating null byte in the keymap size
  // (see the libxkbcommon#307 note in init()), so the two spellings of one keymap
  // must compare equal - otherwise every alternate rebuild re-ingests for nothing.
  TestAppUtil appUtil;
  EventQueue eventQueue;
  deskflow::EiKeyState keyState(nullptr, &eventQueue);

  QTemporaryFile withNull;
  ingestKeymap(keyState, withNull, QByteArray(TestKeymap));

  QTemporaryFile withoutNull;
  QVERIFY(withoutNull.open());
  const QByteArray bare = QByteArray::fromRawData(TestKeymap, sizeof(TestKeymap) - 1);
  QCOMPARE(withoutNull.write(bare), bare.size());
  QVERIFY(withoutNull.flush());

  QVERIFY(!keyState.initIfChanged(withoutNull.handle(), static_cast<std::size_t>(withoutNull.size())));
}

void EiKeyStateTests::initIfChanged_afterDefaultKeymapFallback_ingestsTheSameKeymapAgain()
{
  // The EIS implementation may hand us a device with no keymap at all, in which case we
  // fall back to a default.  That fallback is NOT the keymap the server sends us later, so
  // the first real delivery must not be mistaken for a repeat.
  TestAppUtil appUtil;
  EventQueue eventQueue;
  deskflow::EiKeyState keyState(nullptr, &eventQueue);

  const QByteArray keymapData = QByteArray::fromRawData(TestKeymap, sizeof(TestKeymap) - 1);
  QTemporaryFile keymapFile;
  ingestKeymap(keyState, keymapFile, keymapData);

  keyState.initDefaultKeymap();

  QVERIFY(keyState.initIfChanged(keymapFile.handle(), static_cast<std::size_t>(keymapFile.size())));
}

QTEST_MAIN(EiKeyStateTests)
