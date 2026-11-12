/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2024 Synergy App Ltd
 * SPDX-FileCopyrightText: (C) 2022 Red Hat, Inc.
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "deskflow/KeyState.h"
#include "platform/EiComputer.h"

#include <xkbcommon/xkbcommon.h>

#include <cstddef>
#include <string>

struct xkb_context;
struct xkb_keymap;
struct xkb_state;

namespace deskflow {

/// A key state for Ei
class EiKeyState : public KeyState
{
public:
  EiKeyState(EiComputer *computer, IEventQueue *events);
  ~EiKeyState() override;

  void init(int fd, std::size_t len);
  /// Ingest the keymap at fd/len only when it differs from the one already in use.
  /// Returns true when the key state changed, i.e. when the caller must rebuild its KeyMap.
  bool initIfChanged(int fd, std::size_t len);
  void initDefaultKeymap();

  // IKeyState overrides
  bool fakeCtrlAltDel() override;
  KeyModifierMask pollActiveModifiers() const override;
  std::int32_t pollActiveGroup() const override;
  void pollPressedKeys(KeyButtonSet &pressedKeys) const override;
  KeyID mapKeyFromKeyval(std::uint32_t keyval) const;
  void updateXkbState(std::uint32_t keyval, bool isPressed);
  void updateLockedModifiers(xkb_mod_mask_t lockedMods);
  void clearStaleModifiers() override;

protected:
  // KeyState overrides
  void getKeyMap(KeyMap &keyMap) override;
  void fakeKey(const Keystroke &keystroke) override;

private:
  std::uint32_t convertModMask(xkb_mod_mask_t xkbModMaskIn, bool mapMod2ToNumLock = false) const;
  void assignGeneratedModifiers(std::uint32_t keycode, KeyMap::KeyItem &item);
  /// Compiles text into the active keymap/state.  Returns false and falls back to the default
  /// keymap when the text does not compile.
  bool applyKeymapText(const std::string &text);
  /// True when two keymap buffers are the same keymap, ignoring trailing null bytes (libei may
  /// or may not count the terminating null in the size it reports).
  static bool sameKeymapText(const std::string &previous, const std::string &incoming);

  EiComputer *m_computer = nullptr;

  xkb_context *m_xkb = nullptr;
  xkb_keymap *m_xkbKeymap = nullptr;
  xkb_state *m_xkbState = nullptr;
  /// The keymap text currently applied, empty when the default keymap is in use.
  std::string m_keymapText;
};

} // namespace deskflow
